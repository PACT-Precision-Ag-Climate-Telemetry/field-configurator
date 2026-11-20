// bridge.h - BLE Nordic-UART <-> mainboard UART bridge
#pragma once
#include <Arduino.h>

#include "config.h"

namespace Bridge {

struct Stats {
  uint32_t uartToBle;      // bytes forwarded UART -> BLE
  uint32_t bleToUart;      // bytes forwarded BLE -> UART
  uint32_t dropUartToBle;  // bytes dropped (ring full / no subscriber)
  uint32_t dropBleToUart;
  uint32_t notifyRetries;
};

// Start UART, ring buffers, tasks and BLE advertising. Call once from setup().
// enableBle=false: UART + ring buffers only (web mode); BLE stack stays off.
void begin(bool enableBle = true, uint32_t baud = MB_UART_BAUD);
void setBaud(uint32_t baud);  // live change of the mainboard link speed
uint32_t baud();

bool isConnected();          // a central is connected AND subscribed to TX
uint16_t mtu();              // negotiated ATT MTU (23 if none)
uint16_t connIntervalUnits(); // current interval in 1.25 ms units (0 if none)
const char *deviceName();
Stats stats();

// UI -> mainboard. Non-blocking; returns bytes queued (0 while BLE owns link).
size_t uiWrite(const uint8_t *data, size_t len);
// Mainboard -> UI (only populated while no BLE central is connected).
size_t uiRead(uint8_t *dst, size_t max);
void uiFlush();

uint32_t rxTotal();  // total bytes ever received from the mainboard

// Raw mainboard output stream (last 8 KB, always collected, any mode).
// Copies bytes after `cursor` into dst and advances cursor. Start with
// cursor=0 to get the backlog; a cursor that fell too far behind is skipped ahead.
size_t streamRead(uint32_t &cursor, char *dst, size_t max);

// Rolling text log of everything the mainboard prints (always collected).
#define LOG_COLS 25
// Copies up to maxLines most recent lines, oldest first. Returns the count.
size_t logCopy(char (*out)[LOG_COLS + 1], size_t maxLines);

}  // namespace Bridge
