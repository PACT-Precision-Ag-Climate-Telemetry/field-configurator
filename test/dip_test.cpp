// dip_test.cpp - standalone hardware test for the 4-way DIP bank, the AB024 rotary
// encoder (CLK/DT/SW) and the OLED.
// Build/flash:  pio run -e dip_test -t upload   then   pio device monitor
//
// DIP: switches pull their pin to GND when ON (internal pull-ups), as in the firmware.
// Encoder: turn CW, turn CCW, press the button. Serial prints every event, the OLED
// shows live state. PASS once every DIP switch was seen ON and OFF AND the encoder
// was seen turning both ways AND the button was pressed.
// Pin map comes from include/config.h so it can't drift from the real firmware.
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "config.h"

#if OLED_CTRL == 1
U8G2_SSD1327_WS_128X128_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SH1107_PIMORONI_128X128_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#endif

// ---------------------------------------------------------------- DIP bank
struct Sw {
  const char* name;
  uint8_t pin;
  bool state;      // true = ON (pin LOW)
  bool lastRaw;
  uint32_t since;  // debounce timer
  bool seenOn, seenOff;
};

static Sw sw[4] = {
    {"SW1 mode", DIP_MODE_PIN, false, false, 0, false, false},
    {"SW2 log ", DIP_LOG_PIN, false, false, 0, false, false},
    {"SW3 baud", DIP_BAUD_PIN, false, false, 0, false, false},
    {"SW4 flip", DIP_FLIP_PIN, false, false, 0, false, false},
};

// ----------------------------------------------------------------- encoder
static uint8_t encPrev = 0;
static int8_t encAcc = 0;       // quarter-steps accumulated toward one detent
static int32_t encPos = 0;      // detents, + = CW
static bool seenCW = false, seenCCW = false;
static bool btnDown = false, btnLastRaw = false, btnSeen = false;
static uint32_t btnSince = 0, btnPressMs = 0;
static uint32_t btnCount = 0;

static bool g_oled = false;
static bool g_passed = false;

static bool allDipOk() {
  for (auto& s : sw) if (!(s.seenOn && s.seenOff)) return false;
  return true;
}

static void draw() {
  if (!g_oled) return;
  char buf[32];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "DIP + ENCODER TEST");
  for (int i = 0; i < 4; i += 2) {  // two switches per row; +/- = seen ON/OFF
    snprintf(buf, sizeof buf, "S%d %-3s%c%c  S%d %-3s%c%c", i + 1, sw[i].state ? "ON" : "OFF",
             sw[i].seenOn ? '+' : '.', sw[i].seenOff ? '-' : '.', i + 2,
             sw[i + 1].state ? "ON" : "OFF", sw[i + 1].seenOn ? '+' : '.',
             sw[i + 1].seenOff ? '-' : '.');
    u8g2.drawStr(0, 21 + (i / 2) * 10, buf);
  }
  snprintf(buf, sizeof buf, "ENC %+ld CW%c CCW%c", (long)encPos, seenCW ? '+' : '.',
           seenCCW ? '+' : '.');
  u8g2.drawStr(0, 43, buf);
  snprintf(buf, sizeof buf, "CLK%d DT%d BTN %s n=%lu", digitalRead(ENC_CLK_PIN),
           digitalRead(ENC_DT_PIN), btnDown ? "DOWN" : "up", (unsigned long)btnCount);
  u8g2.drawStr(0, 53, buf);
  u8g2.drawStr(0, SCR_H - 2, g_passed ? "ALL PASS" : "toggle/turn/press all");
  u8g2.sendBuffer();
}

static void checkPass() {
  if (!g_passed && allDipOk() && seenCW && seenCCW && btnSeen) {
    g_passed = true;
    Serial.println(">>> ALL PASS: 4 DIP switches, encoder CW + CCW, button <<<");
  }
}

static void reportDip() {
  Serial.printf("[%lu ms] ", millis());
  for (auto& s : sw) Serial.printf("%s=%s  ", s.name, s.state ? "ON " : "OFF");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB CDC monitor attach
  Serial.println("\n=== DIP + rotary encoder test ===");

  for (auto& s : sw) {
    pinMode(s.pin, INPUT_PULLUP);
    s.state = s.lastRaw = (digitalRead(s.pin) == LOW);
    (s.state ? s.seenOn : s.seenOff) = true;
    Serial.printf("%s on GPIO%u\n", s.name, s.pin);
  }

  pinMode(ENC_CLK_PIN, INPUT_PULLUP);
  pinMode(ENC_DT_PIN, INPUT_PULLUP);
  pinMode(ENC_SW_PIN, INPUT_PULLUP);
  encPrev = (digitalRead(ENC_CLK_PIN) << 1) | digitalRead(ENC_DT_PIN);
  btnDown = btnLastRaw = (digitalRead(ENC_SW_PIN) == LOW);
  Serial.printf("Encoder CLK=GPIO%d DT=GPIO%d SW=GPIO%d (ENC_INVERT=%d)\n", ENC_CLK_PIN,
                ENC_DT_PIN, ENC_SW_PIN, ENC_INVERT);

  Wire.setPins(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.begin();
  Wire.beginTransmission(0x3C);
  g_oled = (Wire.endTransmission() == 0);
  if (g_oled) {
    u8g2.begin();
    u8g2.setBusClock(OLED_I2C_HZ);
  } else {
    Serial.println("(OLED not found - serial output only)");
  }

  Serial.println("Toggle every DIP switch ON and OFF, turn encoder both ways, press button.");
  reportDip();
  draw();
}

void loop() {
  bool changed = false;

  // --- DIP bank (30 ms debounce)
  for (auto& s : sw) {
    bool raw = (digitalRead(s.pin) == LOW);
    if (raw != s.lastRaw) {
      s.lastRaw = raw;
      s.since = millis();
    }
    if (raw != s.state && millis() - s.since > 30) {
      s.state = raw;
      (s.state ? s.seenOn : s.seenOff) = true;
      reportDip();
      changed = true;
    }
  }

  // --- Encoder: Gray-code quadrature decode (4 quarter-steps per detent)
  static const int8_t kTable[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t cur = (digitalRead(ENC_CLK_PIN) << 1) | digitalRead(ENC_DT_PIN);
  if (cur != encPrev) {
    int8_t step = kTable[(encPrev << 2) | cur];
    encPrev = cur;
    if (ENC_INVERT) step = -step;
    encAcc += step;
    if (encAcc >= 4 || encAcc <= -4) {
      int8_t dir = encAcc > 0 ? 1 : -1;
      encAcc = 0;
      encPos += dir;
      (dir > 0 ? seenCW : seenCCW) = true;
      Serial.printf("[%lu ms] ENC %s  pos=%ld\n", millis(), dir > 0 ? "CW " : "CCW", (long)encPos);
      changed = true;
    } else {
      changed = true;  // refresh CLK/DT levels on screen
    }
  }

  // --- Encoder button (30 ms debounce)
  bool braw = (digitalRead(ENC_SW_PIN) == LOW);
  if (braw != btnLastRaw) {
    btnLastRaw = braw;
    btnSince = millis();
  }
  if (braw != btnDown && millis() - btnSince > 30) {
    btnDown = braw;
    if (btnDown) {
      btnCount++;
      btnSeen = true;
      btnPressMs = millis();
      Serial.printf("[%lu ms] BTN pressed (n=%lu)\n", btnPressMs, (unsigned long)btnCount);
    } else {
      Serial.printf("[%lu ms] BTN released after %lu ms%s\n", millis(),
                    (unsigned long)(millis() - btnPressMs),
                    (millis() - btnPressMs >= ENC_LONG_PRESS_MS) ? " (long press)" : "");
    }
    changed = true;
  }

  if (changed) {
    checkPass();
    draw();
  }
  delayMicroseconds(300);
}
