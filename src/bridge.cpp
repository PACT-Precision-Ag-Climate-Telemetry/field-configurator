// bridge.cpp - BLE Nordic-UART <-> mainboard UART bridge (NimBLE-Arduino 2.x)
//
// Data path (no BLE callback ever blocks, no UART call ever happens in a BLE
// callback):
//
//   mainboard --UART--> uartRxTask --ring u2b--> bleTxTask --notify--> central
//   central --write--> onWrite() --ring b2u--> uartTxTask --UART--> mainboard
//   (no central)       uartRxTask --ring u2ui--> UI (menu / console parser)
//
// Rings are FreeRTOS byte buffers. Producers use a 0-tick timeout: if a ring is
// full the bytes are counted as dropped rather than stalling the producer.
#include "bridge.h"

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>

#include "config.h"

#ifndef BLE_HCI_LE_PHY_2M_PREF_MASK
#define BLE_HCI_LE_PHY_2M_PREF_MASK 0x02
#endif

#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // central -> us
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // us -> central

namespace Bridge {
namespace {

RingbufHandle_t g_u2b = nullptr;
RingbufHandle_t g_b2u = nullptr;
RingbufHandle_t g_u2ui = nullptr;

NimBLEServer *g_server = nullptr;
NimBLECharacteristic *g_txChar = nullptr;
char g_name[24] = {0};

// Rolling log of mainboard output (for the OLED live-log switch).
constexpr int kLogLines = 12;
char g_log[kLogLines][LOG_COLS + 1];
int g_logHead = 0, g_logLen = 0;
bool g_logCR = false;
SemaphoreHandle_t g_logMux = nullptr;
uint32_t g_baud = MB_UART_BAUD;

void logFeed(const uint8_t *d, size_t n) {
  xSemaphoreTake(g_logMux, portMAX_DELAY);
  for (size_t i = 0; i < n; i++) {
    char c = static_cast<char>(d[i]);
    if (c == '\n' || c == '\r') {
      if (c == '\n' && g_logCR) { g_logCR = false; continue; }
      g_logCR = (c == '\r');
      if (g_logLen > 0) {
        g_logHead = (g_logHead + 1) % kLogLines;
        g_log[g_logHead][0] = 0;
        g_logLen = 0;
      }
      continue;
    }
    g_logCR = false;
    if (c < 32 || c > 126) continue;
    if (g_logLen >= LOG_COLS) {  // wrap long lines
      g_logHead = (g_logHead + 1) % kLogLines;
      g_logLen = 0;
    }
    g_log[g_logHead][g_logLen++] = c;
    g_log[g_logHead][g_logLen] = 0;
  }
  xSemaphoreGive(g_logMux);
}

// Raw capture of everything the mainboard prints, for the web Serial tab.
// Circular byte buffer addressed by a monotonic byte counter so each client
// can poll with its own cursor.
constexpr size_t kStreamSize = 8192;
char g_stream[kStreamSize];
uint32_t g_streamTotal = 0;  // total bytes ever captured

void streamFeed(const uint8_t *d, size_t n) {
  xSemaphoreTake(g_logMux, portMAX_DELAY);
  for (size_t i = 0; i < n; i++) g_stream[(g_streamTotal + i) % kStreamSize] = static_cast<char>(d[i]);
  g_streamTotal += n;
  xSemaphoreGive(g_logMux);
}

volatile bool g_subscribed = false;
volatile uint16_t g_connHandle = BLE_HS_CONN_HANDLE_NONE;
volatile uint16_t g_mtu = 23;
volatile uint16_t g_interval = 0;
volatile bool g_newConn = false;

volatile uint32_t g_u2bBytes = 0, g_b2uBytes = 0, g_dropU2b = 0, g_dropB2u = 0, g_retries = 0;

inline bool linkActive() { return g_subscribed && g_connHandle != BLE_HS_CONN_HANDLE_NONE; }

// ------------------------------------------------------------------ BLE ----
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, NimBLEConnInfo &info) override {
    if (g_connHandle != BLE_HS_CONN_HANDLE_NONE) {
      // Single-central bridge: refuse a second one.
      srv->disconnect(info.getConnHandle());
      return;
    }
    g_connHandle = info.getConnHandle();
    g_mtu = 23;
    g_interval = info.getConnInterval();
    g_newConn = true;  // parameter negotiation is done by connTask, not here
  }

  void onDisconnect(NimBLEServer *srv, NimBLEConnInfo &info, int reason) override {
    if (info.getConnHandle() != g_connHandle) return;  // a refused 2nd central
    g_subscribed = false;
    g_connHandle = BLE_HS_CONN_HANDLE_NONE;
    g_mtu = 23;
    g_interval = 0;
    g_newConn = false;
    // Advertising restarts via advertiseOnDisconnect(true); connTask also
    // polices it, so a failed restart cannot leave us invisible.
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo &info) override {
    if (info.getConnHandle() == g_connHandle) g_mtu = mtu;
  }

  void onConnParamsUpdate(NimBLEConnInfo &info) override {
    if (info.getConnHandle() == g_connHandle) g_interval = info.getConnInterval();
  }
};

class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
    const NimBLEAttValue &v = chr->getValue();
    size_t n = v.size();
    if (n == 0) return;
    if (xRingbufferSend(g_b2u, v.data(), n, 0) == pdTRUE) {
      g_b2uBytes += n;
    } else {
      g_dropB2u += n;  // never block the BLE host task
    }
  }
};

class TxCb : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *chr, NimBLEConnInfo &info, uint16_t subValue) override {
    if (info.getConnHandle() == g_connHandle) g_subscribed = (subValue & 1) != 0;
  }
};

ServerCb g_serverCb;
RxCb g_rxCb;
TxCb g_txCb;

void startAdvertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (!adv->isAdvertising()) adv->start();
}

// Negotiates link parameters and keeps advertising alive. Low priority.
void connTask(void *) {
  uint32_t connectedAt = 0;
  bool fallbackDone = false;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));

    uint16_t h = g_connHandle;
    if (h == BLE_HS_CONN_HANDLE_NONE) {
      startAdvertising();
      continue;
    }

    if (g_newConn) {
      g_newConn = false;
      fallbackDone = false;
      connectedAt = millis();
      // Stage 1: 7.5-15 ms. Android/Windows grant it; iOS may ignore it.
      g_server->updateConnParams(h, CONN_FAST_MIN, CONN_FAST_MAX, CONN_LATENCY, CONN_TIMEOUT);
      g_server->setDataLen(h, 251);  // data length extension: 251-byte PDUs
      g_server->updatePhy(h, BLE_HCI_LE_PHY_2M_PREF_MASK, BLE_HCI_LE_PHY_2M_PREF_MASK, 0);
    }

    NimBLEConnInfo ci = g_server->getPeerInfoByHandle(h);
    g_interval = ci.getConnInterval();

    // Stage 2: central didn't honour stage 1 (iOS requires >= 15 ms and
    // max >= min + 15 ms) -> ask again with iOS-compliant values.
    if (!fallbackDone && millis() - connectedAt > CONN_FALLBACK_MS) {
      fallbackDone = true;
      if (g_interval > CONN_FAST_MAX) {
        g_server->updateConnParams(h, CONN_IOS_MIN, CONN_IOS_MAX, CONN_LATENCY, CONN_TIMEOUT);
      }
    }
  }
}

// ------------------------------------------------------------ data tasks ---
void uartRxTask(void *) {
  static uint8_t buf[512];
  for (;;) {
    int avail = MB_UART.available();
    if (avail <= 0) {
      vTaskDelay(1);
      continue;
    }
    size_t n = MB_UART.read(buf, min<size_t>(avail, sizeof(buf)));
    if (n == 0) continue;

    logFeed(buf, n);
    streamFeed(buf, n);
    if (linkActive()) {
      if (xRingbufferSend(g_u2b, buf, n, 0) == pdTRUE) {
        g_u2bBytes += n;
      } else {
        g_dropU2b += n;
      }
    } else {
      xRingbufferSend(g_u2ui, buf, n, 0);  // UI may be idle: loss is fine
    }
  }
}

void uartTxTask(void *) {
  for (;;) {
    size_t n = 0;
    void *item = xRingbufferReceiveUpTo(g_b2u, &n, pdMS_TO_TICKS(20), 256);
    if (!item) continue;
    MB_UART.write(static_cast<const uint8_t *>(item), n);  // may block: this task only
    vRingbufferReturnItem(g_b2u, item);
  }
}

void bleTxTask(void *) {
  static uint8_t buf[BLE_MTU_MAX];
  size_t have = 0, off = 0;

  for (;;) {
    if (!linkActive()) {
      // No subscriber: discard stale data so it never leaks into the next session.
      size_t n = 0;
      void *item = xRingbufferReceiveUpTo(g_u2b, &n, pdMS_TO_TICKS(20), sizeof(buf));
      if (item) vRingbufferReturnItem(g_u2b, item);
      have = off = 0;
      continue;
    }

    size_t payload = g_mtu > 3 ? g_mtu - 3 : 20;
    if (payload > sizeof(buf)) payload = sizeof(buf);

    if (have == off) {
      have = off = 0;
      size_t n = 0;
      void *item = xRingbufferReceiveUpTo(g_u2b, &n, pdMS_TO_TICKS(20), payload);
      if (!item) continue;
      memcpy(buf, item, n);
      vRingbufferReturnItem(g_u2b, item);
      have = n;
      // Coalesce: a short wait lets small bursts ride one notification.
      if (have < payload) {
        item = xRingbufferReceiveUpTo(g_u2b, &n, pdMS_TO_TICKS(2), payload - have);
        if (item) {
          memcpy(buf + have, item, n);
          vRingbufferReturnItem(g_u2b, item);
          have += n;
        }
      }
    }

    size_t chunk = min(have - off, payload);
    if (g_txChar->notify(buf + off, chunk, g_connHandle)) {
      off += chunk;
    } else {
      g_retries = g_retries + 1;  // controller out of buffers (congestion): back off
      vTaskDelay(1);     // 1 tick; ring buffers absorb the UART meanwhile
    }
  }
}

}  // namespace

// ------------------------------------------------------------- public API --
void begin(bool enableBle, uint32_t baud) {
  g_logMux = xSemaphoreCreateMutex();
  configASSERT(g_logMux);
  g_baud = baud;
  g_u2b = xRingbufferCreate(RB_UART_TO_BLE, RINGBUF_TYPE_BYTEBUF);
  g_b2u = xRingbufferCreate(RB_BLE_TO_UART, RINGBUF_TYPE_BYTEBUF);
  g_u2ui = xRingbufferCreate(RB_UART_TO_UI, RINGBUF_TYPE_BYTEBUF);
  configASSERT(g_u2b && g_b2u && g_u2ui);

  MB_UART.setRxBufferSize(UART_RX_BUF);
  MB_UART.setTxBufferSize(UART_TX_BUF);
  MB_UART.begin(baud, SERIAL_8N1, MB_UART_RX_PIN, MB_UART_TX_PIN);

  uint64_t mac = ESP.getEfuseMac();
  snprintf(g_name, sizeof(g_name), "%s-%02X%02X", BLE_NAME_PREFIX,
           (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));

  if (enableBle) {
  NimBLEDevice::init(g_name);
  NimBLEDevice::setMTU(BLE_MTU_MAX);
  NimBLEDevice::setPower(BLE_TX_POWER_DBM);
  NimBLEDevice::setDefaultPhy(BLE_HCI_LE_PHY_2M_PREF_MASK | 0x01, BLE_HCI_LE_PHY_2M_PREF_MASK | 0x01);

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(&g_serverCb, false);  // static storage: don't let NimBLE delete
  g_server->advertiseOnDisconnect(true);

  NimBLEService *svc = g_server->createService(NUS_SERVICE_UUID);
  NimBLECharacteristic *rx = svc->createCharacteristic(
      NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(&g_rxCb);
  g_txChar = svc->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  g_txChar->setCallbacks(&g_txCb);

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);  // 128-bit UUID in the ADV packet
  NimBLEAdvertisementData scan;
  scan.setName(g_name);                   // name in the scan response
  adv->setScanResponseData(scan);
  adv->enableScanResponse(true);
  adv->start();
  }

  xTaskCreatePinnedToCore(uartRxTask, "uartRx", 4096, nullptr, 5, nullptr, TASK_CORE);
  xTaskCreatePinnedToCore(uartTxTask, "uartTx", 3072, nullptr, 5, nullptr, TASK_CORE);
  if (enableBle) {
    xTaskCreatePinnedToCore(bleTxTask, "bleTx", 4096, nullptr, 6, nullptr, TASK_CORE);
    xTaskCreatePinnedToCore(connTask, "bleConn", 4096, nullptr, 2, nullptr, TASK_CORE);
  }
}

bool isConnected() { return linkActive(); }
uint16_t mtu() { return g_mtu; }
uint16_t connIntervalUnits() { return g_interval; }
const char *deviceName() { return g_name; }

Stats stats() { return {g_u2bBytes, g_b2uBytes, g_dropU2b, g_dropB2u, g_retries}; }

size_t uiWrite(const uint8_t *data, size_t len) {
  if (linkActive() || len == 0) return 0;
  return xRingbufferSend(g_b2u, data, len, 0) == pdTRUE ? len : 0;
}

void setBaud(uint32_t b) {
  if (b == g_baud) return;
  g_baud = b;
  MB_UART.updateBaudRate(b);
}

uint32_t baud() { return g_baud; }

size_t logCopy(char (*out)[LOG_COLS + 1], size_t maxLines) {
  if (maxLines > (size_t)kLogLines) maxLines = kLogLines;
  xSemaphoreTake(g_logMux, portMAX_DELAY);
  size_t count = 0;
  int idx[kLogLines];
  for (int k = 0; k < kLogLines; k++) {  // newest -> oldest, stop at first empty
    int i = (g_logHead - k + kLogLines) % kLogLines;
    if (g_log[i][0] == 0) break;
    idx[count++] = i;
  }
  if (count > maxLines) count = maxLines;
  for (size_t k = 0; k < count; k++) strcpy(out[k], g_log[idx[count - 1 - k]]);
  xSemaphoreGive(g_logMux);
  return count;
}

uint32_t rxTotal() { return g_streamTotal; }

size_t streamRead(uint32_t &cursor, char *dst, size_t max) {
  xSemaphoreTake(g_logMux, portMAX_DELAY);
  if (g_streamTotal - cursor > kStreamSize) cursor = g_streamTotal - kStreamSize;  // fell behind (or cursor ahead after reboot)
  if (cursor > g_streamTotal) cursor = g_streamTotal;
  size_t n = g_streamTotal - cursor;
  if (n > max) n = max;
  for (size_t i = 0; i < n; i++) dst[i] = g_stream[(cursor + i) % kStreamSize];
  cursor += n;
  xSemaphoreGive(g_logMux);
  return n;
}

void uiFlush() {
  uint8_t tmp[128];
  while (uiRead(tmp, sizeof(tmp)) > 0) {}
}

size_t uiRead(uint8_t *dst, size_t max) {
  size_t n = 0;
  void *item = xRingbufferReceiveUpTo(g_u2ui, &n, 0, max);
  if (!item) return 0;
  memcpy(dst, item, n);
  vRingbufferReturnItem(g_u2ui, item);
  return n;
}

}  // namespace Bridge
