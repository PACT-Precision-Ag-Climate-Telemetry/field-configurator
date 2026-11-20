// PACT field configurator - ESP32-S3 Zero + 1.5" 128x128 I2C OLED + AB024 rotary encoder
//
//  * Local UI: browse / read / edit the PACT mainboard settings over its serial
//    console ("get NAME" / "set NAME VALUE", see pact/master/lib/setting.h).
//  * BLE: Nordic UART Service bridged transparently to the same UART, so a phone
//    or PC terminal can talk to the mainboard console. While a BLE central is
//    subscribed, BLE owns the link and the local UI shows bridge statistics.
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "bridge.h"
#include "config.h"
#include "web.h"

// ------------------------------------------------------------------ display
#if OLED_CTRL == 1
U8G2_SSD1327_WS_128X128_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SH1107_PIMORONI_128X128_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#endif
static const int kFootY = SCR_H - 2;      // footer text baseline
static const int kFootLine = SCR_H - 11;  // footer separator

// ------------------------------------------------------------------ encoder
enum EvType : uint8_t { EV_CW, EV_CCW, EV_CLICK, EV_LONG };
static QueueHandle_t g_evq;

static void encoderTask(void *) {
  pinMode(ENC_CLK_PIN, INPUT_PULLUP);
  pinMode(ENC_DT_PIN, INPUT_PULLUP);
  pinMode(ENC_SW_PIN, INPUT_PULLUP);

  static const int8_t kTable[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t prev = (digitalRead(ENC_CLK_PIN) << 1) | digitalRead(ENC_DT_PIN);
  int8_t acc = 0;

  bool btn = false, longSent = false;
  uint32_t btnSince = 0;
  uint8_t btnDebounce = 0;

  TickType_t last = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last, 1);  // 1 ms

    uint8_t cur = (digitalRead(ENC_CLK_PIN) << 1) | digitalRead(ENC_DT_PIN);
    if (cur != prev) {
      acc += kTable[(prev << 2) | cur];
      prev = cur;
      if (acc >= 4 || acc <= -4) {
        bool cw = acc > 0;
        acc = 0;
#if ENC_INVERT
        cw = !cw;
#endif
        EvType e = cw ? EV_CW : EV_CCW;
        xQueueSend(g_evq, &e, 0);
      }
    }

    bool pressed = digitalRead(ENC_SW_PIN) == LOW;
    if (pressed != btn) {
      if (++btnDebounce >= 20) {  // 20 ms stable
        btnDebounce = 0;
        btn = pressed;
        if (btn) {
          btnSince = millis();
          longSent = false;
        } else if (!longSent) {
          EvType e = EV_CLICK;
          xQueueSend(g_evq, &e, 0);
        }
      }
    } else {
      btnDebounce = 0;
      if (btn && !longSent && millis() - btnSince >= ENC_LONG_PRESS_MS) {
        longSent = true;
        EvType e = EV_LONG;
        xQueueSend(g_evq, &e, 0);
      }
    }
  }
}

// ----------------------------------------------------------------- settings
// Mirrors the mainboard console in docs/field-configurator.md (sections 3 and 4).
// `type` decides how the value is shown and which keys the editor offers.
enum Type : uint8_t {
  T_INT,     // whole number, range-checked
  T_HEX,     // byte shown and typed in hex (0x1A), sent as 0x1A
  T_FLOAT,   // decimal number, range-checked
  T_BOOL,    // 0 / 1, toggled with one click
  T_HOST,    // IPv4 address or hostname
  T_TEXT,    // free text (may be empty)
  T_RO,      // read-only number (big readout)
  T_STRRO,   // read-only text
  T_ACTION,  // press to run (MODEM_WAKE)
  T_DANGER,  // press, then hold to confirm (reset ALL)
  T_CALIB    // guided moisture calibration wizard
};
enum Group : uint8_t { G_SET, G_SENS, G_ESP, G_SYS, G_COUNT };

struct Item {
  const char *name;
  Group grp;
  Type type;
  bool rw;
  double lo, hi;      // valid range (T_INT / T_FLOAT)
  const char *unit;
  bool bar;           // draw a 0..100 % bar
};

static const Item kItems[] = {
    // ---- writable settings (saved in flash)
    {"SERVER_IP", G_SET, T_HOST, true, 0, 0, "", false},
    {"SERVER_PORT", G_SET, T_INT, true, 1, 65535, "", false},
    {"TELEMETRY_MAGIC_BYTE_1", G_SET, T_HEX, true, 0, 255, "", false},
    {"TELEMETRY_MAGIC_BYTE_2", G_SET, T_HEX, true, 0, 255, "", false},
    {"TELEMETRY_RAW", G_SET, T_BOOL, true, 0, 1, "", false},
    {"OFFSET", G_SET, T_FLOAT, true, 0, 65535, "mm", false},
    {"MOISTURE_SLOPE", G_SET, T_FLOAT, true, -1e9, 1e9, "", false},
    {"MOISTURE_INTERCEPT", G_SET, T_FLOAT, true, -1e9, 1e9, "", false},
    {"BATTERY_DIVIDER_R1", G_SET, T_FLOAT, true, 1, 1e9, "ohm", false},
    {"BATTERY_DIVIDER_R2", G_SET, T_FLOAT, true, 1, 1e9, "ohm", false},
    {"GPS_LATITUDE", G_SET, T_FLOAT, true, -90, 90, "deg", false},
    {"GPS_LONGITUDE", G_SET, T_FLOAT, true, -180, 180, "deg", false},
    {"MOISTURE_CALIB", G_SET, T_CALIB, false, 0, 0, "", false},
    {"MODEM_APN", G_SET, T_TEXT, true, 0, 0, "", false},
    // ---- live sensors (read-only)
    {"SHT30_TEMPERATURE", G_SENS, T_RO, false, 0, 0, "C", false},
    {"SHT30_HUMIDITY", G_SENS, T_RO, false, 0, 0, "%RH", true},
    {"CO2_PPM", G_SENS, T_RO, false, 0, 0, "ppm", false},
    {"METHANE_LEL", G_SENS, T_RO, false, 0, 0, "%LEL", true},
    {"DISTANCE_MM", G_SENS, T_RO, false, 0, 0, "mm", false},
    {"MOISTURE_PERCENT", G_SENS, T_RO, false, 0, 0, "%", true},
    {"MOISTURE_VOLTS", G_SENS, T_RO, false, 0, 0, "V", false},
    {"MOISTURE_RAW", G_SENS, T_RO, false, 0, 0, "", false},
    {"BATTERY_MV", G_SENS, T_RO, false, 0, 0, "mV", false},
    {"BATTERY_PERCENT", G_SENS, T_RO, false, 0, 0, "%", true},
    {"ADC_0", G_SENS, T_RO, false, 0, 0, "V", false},
    {"ADC_1", G_SENS, T_RO, false, 0, 0, "V", false},
    {"ADC_2", G_SENS, T_RO, false, 0, 0, "V", false},
    {"ADC_3", G_SENS, T_RO, false, 0, 0, "V", false},
    {"GPS_FIX", G_SENS, T_STRRO, false, 0, 0, "", false},
    // ---- ESP32 chip info (read-only)
    {"ESP_CHIP", G_ESP, T_STRRO, false, 0, 0, "", false},
    {"ESP_CPU_MHZ", G_ESP, T_RO, false, 0, 0, "MHz", false},
    {"ESP_MAC", G_ESP, T_STRRO, false, 0, 0, "", false},
    {"ESP_SDK", G_ESP, T_STRRO, false, 0, 0, "", false},
    {"ESP_UPTIME", G_ESP, T_RO, false, 0, 0, "s", false},
    {"ESP_RESET_REASON", G_ESP, T_STRRO, false, 0, 0, "", false},
    {"ESP_TEMPERATURE", G_ESP, T_RO, false, 0, 0, "C", false},
    {"ESP_HEAP", G_ESP, T_STRRO, false, 0, 0, "", false},
    {"ESP_PSRAM", G_ESP, T_STRRO, false, 0, 0, "", false},
    {"ESP_FLASH", G_ESP, T_STRRO, false, 0, 0, "", false},
    {"ESP_FS", G_ESP, T_STRRO, false, 0, 0, "", false},
    // ---- system
    {"ID", G_SYS, T_STRRO, false, 0, 0, "", false},
    {"FW_VERSION", G_SYS, T_STRRO, false, 0, 0, "", false},
    {"MODEM_STATE", G_SYS, T_STRRO, false, 0, 0, "", false},
    {"MODEM_CELL", G_SYS, T_STRRO, false, 0, 0, "", false},
    {"PAYLOAD_HEX", G_SYS, T_STRRO, false, 0, 0, "", false},
    {"MODEM_WAKE", G_SYS, T_ACTION, false, 0, 0, "", false},
    {"LOG", G_SYS, T_BOOL, true, 0, 1, "", false},
    {"DEV_MODE", G_SYS, T_BOOL, true, 0, 1, "", false},
    {"RESET_ALL", G_SYS, T_DANGER, false, 0, 0, "", false},
};
static const int kItemCount = sizeof(kItems) / sizeof(kItems[0]);
static String g_cache[kItemCount];  // last value seen per item (shown in the menu)

static const char *kGroupName[G_COUNT] = {"SETTINGS", "SENSORS", "ESP32", "SYSTEM"};
static const char *kGroupHint[G_COUNT] = {"edit & save", "live readings", "chip, heap, uptime", "modem, log, reset"};

static const char *typeTag(const Item *it) {
  switch (it->type) {
    case T_INT: return "int";
    case T_HEX: return "hex";
    case T_FLOAT: return "number";
    case T_BOOL: return "on/off";
    case T_HOST: return "ip/host";
    case T_TEXT: return "text";
    case T_ACTION: return "action";
    case T_DANGER: return "danger";
    case T_CALIB: return "wizard";
    default: return "read-only";
  }
}

// ------------------------------------------------------------- value helpers
static String firstLine(const String &s) {
  int n = s.indexOf('\n');
  return n < 0 ? s : s.substring(0, n);
}

static bool parseNum(const String &s, double &out) {
  if (!s.length()) return false;
  char *e;
  out = strtod(s.c_str(), &e);
  return e != s.c_str() && *e == 0;
}

// "350.000000" -> "350", "0.500000" -> "0.5"
static String prettyNum(const String &s) {
  double d;
  if (s.indexOf('.') < 0 || !parseNum(s, d)) return s;
  String r = s;
  while (r.endsWith("0")) r.remove(r.length() - 1);
  if (r.endsWith(".")) r.remove(r.length() - 1);
  if (r.length() == 0 || r == "-") r = "0";
  return r;
}

// "12410 (age 14 s)" -> main "12410", extra "(age 14 s)"
static void splitAge(const String &v, String &main, String &extra) {
  int p = v.indexOf(" (");
  if (p < 0) { main = v; extra = ""; }
  else { main = v.substring(0, p); extra = v.substring(p + 1); }
}

// "26" (decimal from the mainboard) -> "0x1A"
static String hexOf(const String &dec) {
  double d;
  if (!parseNum(dec, d) || d < 0 || d > 255) return dec;
  char b[8];
  snprintf(b, sizeof(b), "0x%02X", (int)d);
  return b;
}

static bool boolOf(const String &v) {
  String m = firstLine(v);
  m.trim();
  m.toLowerCase();
  return m == "1" || m == "true" || m == "on";
}

static const char *fmtNum(double d) {
  static char b[24];
  if (d == floor(d) && fabs(d) < 1e9) snprintf(b, sizeof(b), "%.0f", d);
  else snprintf(b, sizeof(b), "%g", d);
  return b;
}

static String rangeText(const Item *it) {
  String r = fmtNum(it->lo);
  r += "..";
  r += fmtNum(it->hi);
  if (it->unit[0]) { r += " "; r += it->unit; }
  return r;
}

// Is every character a digit or a dot (so it should be a dotted-quad)?
static bool looksIpv4(const String &s) {
  if (!s.length()) return false;
  for (unsigned i = 0; i < s.length(); i++)
    if (!isDigit(s[i]) && s[i] != '.') return false;
  return true;
}

static bool validIpv4(const String &s) {
  int parts = 0, val = 0, digits = 0;
  for (unsigned i = 0; i <= s.length(); i++) {
    char c = i < s.length() ? s[i] : '.';
    if (c == '.') {
      if (!digits || val > 255) return false;
      parts++; val = 0; digits = 0;
    } else {
      val = val * 10 + (c - '0');
      if (++digits > 3) return false;
    }
  }
  return parts == 4;
}

// Live check of the text being typed against the item's data type.
static bool validate(const Item *it, const String &s, const char **why) {
  *why = "ok";
  switch (it->type) {
    case T_INT:
    case T_FLOAT: {
      double d;
      if (!s.length()) { *why = "type a number"; return false; }
      if (!parseNum(s, d)) { *why = "not a number yet"; return false; }
      if (it->type == T_INT && s.indexOf('.') >= 0) { *why = "whole numbers only"; return false; }
      static char m[28];
      if (d < it->lo) { snprintf(m, sizeof(m), "too low, min %s", fmtNum(it->lo)); *why = m; return false; }
      if (d > it->hi) { snprintf(m, sizeof(m), "too high, max %s", fmtNum(it->hi)); *why = m; return false; }
      return true;
    }
    case T_HEX: {
      if (!s.length()) { *why = "type 1-2 hex digits"; return false; }
      if (s.length() > 2) { *why = "max 2 hex digits"; return false; }
      for (unsigned i = 0; i < s.length(); i++)
        if (!isxdigit(s[i])) { *why = "hex digits only"; return false; }
      static char m[20];
      snprintf(m, sizeof(m), "0x%s = %ld", s.c_str(), strtol(s.c_str(), nullptr, 16));
      *why = m;
      return true;
    }
    case T_HOST:
      if (!s.length()) { *why = "type an address"; return false; }
      if (s.length() > 253) { *why = "too long"; return false; }
      if (looksIpv4(s) && !validIpv4(s)) { *why = "need a.b.c.d (0-255)"; return false; }
      *why = looksIpv4(s) ? "valid IPv4" : "hostname";
      return true;
    case T_TEXT:
      if (s.length() > 63) { *why = "max 63 chars"; return false; }
      if (!s.length()) *why = "empty (clears it)";
      return true;
    default:
      return true;
  }
}

// ----------------------------------------------- mainboard request/response
// One request in flight. The console echoes our keystrokes, so only lines that
// start with "OK <prefix>" or "ERR" are treated as the reply.
struct Request {
  bool active = false;
  bool done = false;
  bool ok = false;
  bool gotOk = false;
  String name;
  String prefix;  // reply prefix to match, e.g. "OK NAME=" or "OK reset NAME"
  String value;   // reply payload (OK) or error text
  String line;
  uint32_t deadline = 0;
  uint32_t lastRx = 0;
};
static Request g_req;

static bool startsWithNoCase(const String &s, const String &prefix) {
  return s.length() >= prefix.length() && s.substring(0, prefix.length()).equalsIgnoreCase(prefix);
}

static void reqStart(const String &name, const String &cmd, uint32_t timeoutMs, const String &prefix = "") {
  uint8_t drop[64];
  while (Bridge::uiRead(drop, sizeof(drop)) > 0) {}  // discard stale console output
  g_req = Request();
  g_req.active = true;
  g_req.name = name;
  g_req.prefix = prefix.length() ? prefix : "OK " + name + "=";
  g_req.deadline = millis() + timeoutMs;
  String out = "\r" + cmd + "\r";  // leading CR terminates any half-typed line
  Bridge::uiWrite(reinterpret_cast<const uint8_t *>(out.c_str()), out.length());
}

static void reqCancel() { g_req = Request(); }

static void reqHandleLine(String l) {
  while (l.startsWith(">")) { l.remove(0, 1); l.trim(); }
  if (l.length() == 0) return;
  if (!g_req.gotOk) {
    if (startsWithNoCase(l, g_req.prefix)) {
      g_req.gotOk = true;
      g_req.value = l.substring(g_req.prefix.length());
      g_req.value.trim();
      g_req.lastRx = millis();
    } else if (startsWithNoCase(l, "ERR")) {
      g_req.value = l;
      g_req.ok = false;
      g_req.done = true;
    }
  } else if (g_req.value.length() < 400) {  // multi-line reply
    g_req.value += "\n" + l;
    g_req.lastRx = millis();
  }
}

static void reqPoll() {
  if (!g_req.active || g_req.done) return;
  uint8_t b[64];
  size_t n;
  while ((n = Bridge::uiRead(b, sizeof(b))) > 0) {
    for (size_t i = 0; i < n; i++) {
      char c = static_cast<char>(b[i]);
      if (c == '\r' || c == '\n') {
        if (g_req.line.length()) reqHandleLine(g_req.line);
        g_req.line = "";
      } else if (c >= 32 && c < 127 && g_req.line.length() < 400) {
        g_req.line += c;
      }
    }
  }
  uint32_t now = millis();
  if (g_req.gotOk && now - g_req.lastRx > 250) {  // reply settled
    g_req.ok = true;
    g_req.done = true;
  } else if (!g_req.done && (int32_t)(now - g_req.deadline) > 0) {
    g_req.value = "No reply (timeout)";
    g_req.ok = false;
    g_req.done = true;
  }
}

// ----------------------------------------------------------------------- UI
enum Screen { SCR_HOME, SCR_MENU, SCR_VIEW, SCR_EDIT, SCR_CONFIRM, SCR_CALIB };
enum Pend : uint8_t { P_NONE, P_PROBE, P_VIEW, P_SET, P_RESET, P_RESETALL, P_CAL_TRIG, P_CAL_READ, P_CAL_SET1, P_CAL_SET2 };
static Screen g_scr = SCR_HOME;
static Pend g_pend = P_NONE;
static int g_hsel = 0;                // home selection (group)
static int g_sel = 0, g_top = 0;      // menu selection within g_list
static int g_list[kItemCount], g_listN = 0, g_listPos = 0;
static Group g_group = G_SET;
static const Item *g_item = nullptr;
static int g_itemIdx = 0;

// view state
static String g_viewText;
static bool g_viewOk = false, g_viewBusy = false;
static int g_scroll = 0;
static uint32_t g_viewAt = 0;

// edit state
enum Spec : uint8_t { S_DEL, S_CLR, S_CASE, S_SAVE, S_DFLT, S_ESC };
static char g_cs[48];
static int g_csN = 0;
static Spec g_spec[6];
static int g_specN = 0;
static String g_editBuf;
static int g_cursor = 0;
static bool g_upper = false;
static String g_toast;
static uint32_t g_toastUntil = 0;

// link state
static String g_mbId;  // empty = unknown
static uint32_t g_nextProbe = 0;
static bool g_wasBle = false;
static bool g_dirty = true;

static void toast(const String &m, uint32_t ms = 1800) {
  g_toast = m;
  g_toastUntil = millis() + ms;
  g_dirty = true;
}

static void buildList(Group g) {
  g_group = g;
  g_listN = 0;
  for (int i = 0; i < kItemCount; i++)
    if (kItems[i].grp == g) g_list[g_listN++] = i;
}

static int groupCount(Group g) {
  int n = 0;
  for (int i = 0; i < kItemCount; i++) n += kItems[i].grp == g;
  return n;
}

// ---- moisture calibration wizard
// Percent = SLOPE x volts + INTERCEPT. Read the sensor's volts at 0 % (dry) and
// 100 % (wet), then slope = 100 / (v100 - v0) and intercept = -slope x v0.
static int g_calStep = 0;  // 0 = dry, 1 = wet, 2 = review, 3 = saving
static double g_calV0 = 0, g_calV100 = 0, g_calVolts = 0, g_calSlope = 0, g_calInt = 0;
static bool g_calHave = false, g_calTrig = false;
static uint32_t g_calAt = 0;
static String g_calErr;

static void startCalib() {
  g_scr = SCR_CALIB;
  g_calStep = 0;
  g_calHave = g_calTrig = false;
  g_calErr = "";
  g_calAt = millis();
  g_dirty = true;
}

// Live sampling: asking for MOISTURE_PERCENT starts a fresh ADC reading; the
// volts from it are fetched ~1.5 s later with MOISTURE_VOLTS.
static void calibUpdate() {
  if (g_scr != SCR_CALIB || g_calStep > 1 || g_req.active || g_pend != P_NONE) return;
  if (millis() < g_calAt) return;
  if (g_calTrig) {
    g_pend = P_CAL_READ;
    reqStart("MOISTURE_VOLTS", "get MOISTURE_VOLTS", 3000);
  } else {
    g_pend = P_CAL_TRIG;
    reqStart("MOISTURE_PERCENT", "get MOISTURE_PERCENT", 3000);
  }
}

static void calibClick() {
  if (g_calStep == 0 || g_calStep == 1) {
    if (!g_calHave) { toast("No reading yet"); return; }
    if (g_calStep == 0) {
      g_calV0 = g_calVolts;
    } else {
      g_calV100 = g_calVolts;
      if (fabs(g_calV100 - g_calV0) < 0.05) { toast("Dry = wet? too close"); return; }
      g_calSlope = 100.0 / (g_calV100 - g_calV0);
      g_calInt = -g_calSlope * g_calV0;
    }
    reqCancel();  // drop any sample taken before the click
    g_pend = P_NONE;
    g_calTrig = false;
    g_calHave = false;
    g_calAt = millis();
    g_calStep++;
  } else if (g_calStep == 2) {
    char b[48];
    snprintf(b, sizeof(b), "set MOISTURE_SLOPE %.4f", g_calSlope);
    g_calStep = 3;
    g_pend = P_CAL_SET1;
    reqStart("MOISTURE_SLOPE", b, 6000);
  }
}

static void enterView(int idx, bool quiet = false) {
  if (kItems[idx].type == T_CALIB) { g_itemIdx = idx; g_item = &kItems[idx]; startCalib(); return; }
  g_itemIdx = idx;
  g_item = &kItems[idx];
  g_scr = SCR_VIEW;
  g_dirty = true;
  if (g_item->type == T_DANGER) {
    g_viewBusy = false;
    g_viewOk = true;
    g_viewText = "Restore EVERY setting to its factory default and clear saved values.";
    return;
  }
  if (!quiet) {
    g_viewBusy = true;
    g_viewText = "";
    g_scroll = 0;
  }
  g_pend = P_VIEW;
  reqStart(g_item->name, String("get ") + g_item->name, 4000);
}

static void sendSet(const String &val) {
  g_pend = P_SET;
  String v = val.length() ? val : String("\"\"");
  reqStart(g_item->name, String("set ") + g_item->name + " " + v, 6000);
  g_dirty = true;
}

static void sendReset() {
  g_pend = P_RESET;
  reqStart(g_item->name, String("reset ") + g_item->name, 6000, String("OK reset ") + g_item->name);
  g_dirty = true;
}

static void sendResetAll() {
  g_pend = P_RESETALL;
  reqStart("ALL", "reset ALL", 8000, "OK reset ALL");
  g_dirty = true;
}

static void enterEdit() {
  const Item *it = g_item;
  const char *cs;
  switch (it->type) {
    case T_HEX: cs = "0123456789ABCDEF"; break;
    case T_INT: cs = it->lo < 0 ? "0123456789-" : "0123456789"; break;
    case T_FLOAT: cs = it->lo < 0 ? "0123456789.-" : "0123456789."; break;
    case T_HOST: cs = "0123456789.-abcdefghijklmnopqrstuvwxyz"; break;
    default: cs = "abcdefghijklmnopqrstuvwxyz0123456789.-_"; break;
  }
  strncpy(g_cs, cs, sizeof(g_cs) - 1);
  g_csN = strlen(g_cs);
  g_specN = 0;
  g_spec[g_specN++] = S_DEL;
  g_spec[g_specN++] = S_CLR;
  if (it->type == T_TEXT) g_spec[g_specN++] = S_CASE;
  g_spec[g_specN++] = S_SAVE;
  g_spec[g_specN++] = S_DFLT;
  g_spec[g_specN++] = S_ESC;

  String v = firstLine(g_viewText);
  if (it->type == T_INT || it->type == T_FLOAT || it->type == T_HEX) {
    String m, x;
    splitAge(v, m, x);
    v = prettyNum(m);
    if (it->type == T_HEX) v = hexOf(v).substring(2);
  }
  g_editBuf = v;
  g_cursor = 0;
  g_upper = false;
  g_toast = "";
  g_scr = SCR_EDIT;
}

static int maxLen(const Item *it) {
  switch (it->type) {
    case T_HEX: return 2;
    case T_INT: return 11;
    case T_FLOAT: return 14;
    case T_HOST: return 64;
    default: return 63;
  }
}

static const char *specLabel(Spec s) {
  switch (s) {
    case S_DEL: return "DEL";
    case S_CLR: return "CLR";
    case S_CASE: return g_upper ? "abc" : "ABC";
    case S_SAVE: return "SAVE";
    case S_DFLT: return "DFLT";
    default: return "ESC";
  }
}

static void tokLabel(int i, char *out) {
  if (i < g_csN) {
    char c = g_cs[i];
    if (g_upper && c >= 'a' && c <= 'z') c -= 32;
    out[0] = c;
    out[1] = 0;
  } else {
    strcpy(out, specLabel(g_spec[i - g_csN]));
  }
}

// ----------------------------------------------------------------- drawing
static void header(const char *left, const char *right = nullptr) {
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 7, left);
  if (right) u8g2.drawStr(128 - 5 * strlen(right), 7, right);
  u8g2.drawHLine(0, 9, 128);
}

static void footer(const char *hint) {
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawHLine(0, kFootLine, 128);
  u8g2.drawStr(0, kFootY, hint);
}

static int wrapRows(const String &txt, int cols) {
  int rows = 1, c = 0;
  for (unsigned i = 0; i < txt.length(); i++) {
    if (txt[i] == '\n') { rows++; c = 0; continue; }
    if (++c >= cols && i + 1 < txt.length()) { rows++; c = 0; }
  }
  return rows;
}

static void drawWrapped(const String &txt, int x, int y, int cols, int rows, int skip) {
  int r = 0, c = 0;
  char buf[40];
  int bi = 0;
  auto flush = [&]() {
    buf[bi] = 0;
    if (r >= skip && r - skip < rows) u8g2.drawStr(x, y + (r - skip) * 9, buf);
    bi = 0; c = 0; r++;
  };
  for (unsigned i = 0; i < txt.length() && r - skip < rows; i++) {
    char ch = txt[i];
    if (ch == '\n') { flush(); continue; }
    buf[bi++] = ch;
    if (++c >= cols) flush();
  }
  if (bi && r - skip < rows) flush();
}

static void centerText(const char *s, int y) {
  u8g2.drawStr((128 - u8g2.getStrWidth(s)) / 2, y, s);
}

static void spinner(int cx, int cy) {
  static const int8_t dx[8] = {0, 5, 7, 5, 0, -5, -7, -5}, dy[8] = {-7, -5, 0, 5, 7, 5, 0, -5};
  int p = (millis() / 90) % 8;
  for (int k = 0; k < 8; k++) {
    if (k == p) u8g2.drawDisc(cx + dx[k], cy + dy[k], 2);
    else u8g2.drawPixel(cx + dx[k], cy + dy[k]);
  }
}

static void bar(int y, double pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  u8g2.drawRFrame(8, y, 112, 9, 3);
  int w = (int)(pct * 108 / 100);
  if (w > 0) u8g2.drawRBox(10, y + 2, w, 5, 1);
}

static void drawIcon(int g, int x, int y) {
  if (g == G_SET) {  // gear
    u8g2.drawCircle(x + 8, y + 8, 6);
    u8g2.drawDisc(x + 8, y + 8, 2);
    for (int k = 0; k < 4; k++) {
      static const int8_t ox[4] = {0, 8, 0, -8}, oy[4] = {-8, 0, 8, 0};
      u8g2.drawBox(x + 7 + ox[k], y + 7 + oy[k], 3, 3);
    }
  } else if (g == G_SENS) {  // bar graph
    u8g2.drawBox(x + 1, y + 9, 4, 7);
    u8g2.drawBox(x + 7, y + 4, 4, 12);
    u8g2.drawBox(x + 13, y + 0, 4, 16);
  } else if (g == G_SYS) {  // power symbol
    u8g2.drawCircle(x + 8, y + 9, 7);
    u8g2.setDrawColor(u8g2.getDrawColor() ? 0 : 1);
    u8g2.drawBox(x + 5, y, 7, 7);
    u8g2.setDrawColor(u8g2.getDrawColor() ? 0 : 1);
    u8g2.drawVLine(x + 8, y, 9);
  } else {  // chip
    u8g2.drawFrame(x + 3, y + 3, 11, 11);
    for (int k = 0; k < 3; k++) {
      u8g2.drawVLine(x + 5 + k * 3, y, 3);
      u8g2.drawVLine(x + 5 + k * 3, y + 14, 3);
    }
  }
}

static void drawHome() {
  header("PACT CONFIG", nullptr);
  bool probing = g_pend == P_PROBE;
  String id = g_mbId.length() ? g_mbId.substring(0, 14)
              : probing ? String("linking") + String("...").substring(0, (millis() / 300) % 4)
                        : String("NO LINK");
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(128 - 5 * id.length(), 7, id.c_str());
  if (g_mbId.length()) u8g2.drawDisc(60, 4, 2);  // link LED

  for (int g = 0; g < G_COUNT; g++) {
    int y = 12 + g * 26;
    bool sel = g == g_hsel;
    if (sel) u8g2.drawRBox(1, y, 126, 24, 4);
    u8g2.setDrawColor(sel ? 0 : 1);
    drawIcon(g, 8, y + 4);
    u8g2.setFont(u8g2_font_7x13B_tf);
    u8g2.drawStr(34, y + 11, kGroupName[g]);
    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.drawStr(34, y + 21, kGroupHint[g]);
    char n[8];
    snprintf(n, sizeof(n), "%d", groupCount((Group)g));
    u8g2.drawStr(122 - 5 * strlen(n), y + 11, n);
    u8g2.setDrawColor(1);
  }
  footer("turn:pick  click:open");
}

static void drawMenu() {
  char h[24];
  snprintf(h, sizeof(h), "%d/%d", g_sel + 1, g_listN);
  header(kGroupName[g_group], h);
  u8g2.setFont(u8g2_font_5x8_tf);
  for (int row = 0; row < SCR_ROWS - 1; row++) {
    int i = g_top + row;
    if (i >= g_listN) break;
    const Item &it = kItems[g_list[i]];
    int y = 18 + row * 9;
    bool sel = i == g_sel;
    if (sel) {
      u8g2.drawBox(0, y - 8, 128, 9);
      u8g2.setDrawColor(0);
    }
    u8g2.drawStr(2, y, it.name);
    // cached value, right-aligned, when it fits beside the name
    String v = firstLine(g_cache[g_list[i]]);
    String m, x;
    splitAge(v, m, x);
    m = prettyNum(m);
    if (it.type == T_HEX) m = hexOf(m);
    if (it.type == T_BOOL && v.length()) m = boolOf(v) ? "ON" : "OFF";
    if (it.type == T_DANGER || it.type == T_ACTION) m = "";
    if (m.length() && strlen(it.name) + m.length() <= 23) u8g2.drawStr(126 - 5 * m.length(), y, m.c_str());
    u8g2.setDrawColor(1);
  }
  footer("click:open  hold:home");
}

static void drawView() {
  const Item *it = g_item;
  header(it->name);
  // live dot while auto-refreshing a sensor
  if (g_pend == P_VIEW && !g_viewBusy) u8g2.drawDisc(124, 4, 2);

  if (g_pend == P_SET || g_pend == P_RESET) {
    spinner(64, 52);
    centerText(g_pend == P_SET ? "saving..." : "resetting...", 80);
  } else if (g_viewBusy) {
    spinner(64, 50);
    centerText("reading", 78);
  } else if (!g_viewOk) {
    u8g2.setFont(u8g2_font_5x8_tf);
    drawWrapped(g_viewText, 0, 22, 25, 8, 0);
  } else if (it->type == T_BOOL) {
    bool on = boolOf(g_viewText);
    u8g2.drawRFrame(40, 26, 48, 24, 12);
    if (on) { u8g2.drawRBox(40, 26, 48, 24, 12); u8g2.setDrawColor(0); u8g2.drawDisc(76, 38, 8); u8g2.setDrawColor(1); }
    else u8g2.drawDisc(52, 38, 8);
    u8g2.setFont(u8g2_font_logisoso20_tf);
    centerText(on ? "ON" : "OFF", 84);
    u8g2.setFont(u8g2_font_5x8_tf);
    if (!strcmp(it->name, "LOG")) centerText("not saved, resets at boot", 98);
    if (!strcmp(it->name, "DEV_MODE")) centerText("1 min reads, 5 min upload", 98);
  } else if (it->type == T_DANGER) {
    u8g2.setFont(u8g2_font_5x8_tf);
    drawWrapped(g_viewText, 0, 24, 25, 6, 0);
    u8g2.setFont(u8g2_font_7x13B_tf);
    centerText("! DANGER !", 90);
  } else {
    String v = g_viewText, m, extra;
    splitAge(firstLine(v), m, extra);
    bool numeric = (it->type == T_INT || it->type == T_HEX || it->type == T_FLOAT || it->type == T_RO);
    double d;
    if (numeric && parseNum(m, d) && m.length() <= 9) {
      m = prettyNum(m);
      String sub = it->unit;
      if (it->type == T_HEX) { sub = String("dec ") + m; m = hexOf(m); }
      u8g2.setFont(u8g2_font_logisoso20_tf);
      centerText(m.c_str(), 52);
      u8g2.setFont(u8g2_font_6x10_tf);
      if (sub.length()) centerText(sub.c_str(), 68);
      if (it->bar) bar(74, d);
      u8g2.setFont(u8g2_font_5x8_tf);
      if (extra.length()) centerText(extra.c_str(), 96);
      if (it->rw && it->type != T_BOOL) centerText(it->type == T_HEX ? "range 0x00..0xFF" : (String("range ") + rangeText(it)).c_str(), 108);
    } else {
      u8g2.setFont(u8g2_font_5x8_tf);
      if (!v.length()) {
        centerText("(empty)", 52);
      } else {
        int rows = wrapRows(v, 25), vis = 11;
        int maxScroll = rows > vis ? rows - vis : 0;
        if (g_scroll > maxScroll) g_scroll = maxScroll;
        drawWrapped(v, 0, 19, 25, vis, g_scroll);
        if (maxScroll) {  // scroll thumb
          int th = max(4, 100 * vis / rows);
          int ty = 12 + (100 - th) * g_scroll / maxScroll;
          u8g2.drawVLine(127, ty, th);
        }
      }
    }
  }

  const char *hint;
  if (g_pend == P_SET || g_pend == P_RESET) hint = "please wait...";
  else if (g_viewBusy) hint = "hold:cancel";
  else if (it->type == T_BOOL) hint = "click:toggle hold:back";
  else if (it->type == T_ACTION) hint = "click:run  hold:back";
  else if (it->type == T_DANGER) hint = "click:continue hold:back";
  else if (it->rw) hint = "click:edit  hold:back";
  else hint = "turn:next hold:back";
  footer(hint);
}

static void drawEdit() {
  const Item *it = g_item;
  header(it->name, typeTag(it));

  // current text (tail if long) with a blinking cursor
  String shown = it->type == T_HEX ? "0x" + g_editBuf : g_editBuf;
  if (shown.length() > 17) shown = "~" + shown.substring(shown.length() - 16);
  u8g2.setFont(u8g2_font_7x13_tf);
  bool blink = (millis() / 400) % 2;
  u8g2.drawStr(2, 24, (shown + (blink ? "_" : " ")).c_str());
  u8g2.drawHLine(0, 28, 128);

  if (g_pend == P_SET || g_pend == P_RESET) {
    spinner(64, 62);
    u8g2.setFont(u8g2_font_5x8_tf);
    centerText("saving...", 90);
  } else {
    // pick wheel: previous / [current] / next
    int n = g_csN + g_specN;
    char a[6], b[6], c[6];
    tokLabel((g_cursor + n - 1) % n, a);
    tokLabel(g_cursor, b);
    tokLabel((g_cursor + 1) % n, c);
    u8g2.setFont(u8g2_font_7x13_tf);
    u8g2.drawStr(18 - u8g2.getStrWidth(a) / 2, 61, a);
    u8g2.drawStr(110 - u8g2.getStrWidth(c) / 2, 61, c);
    u8g2.drawRBox(40, 38, 48, 30, 5);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_logisoso16_tf);
    u8g2.drawStr(64 - u8g2.getStrWidth(b) / 2, 61, b);
    u8g2.setDrawColor(1);

    // live validity against the data type
    const char *why;
    bool good = validate(it, g_editBuf, &why);
    u8g2.setFont(u8g2_font_5x8_tf);
    String st = String(good ? "[+] " : "[x] ") + why;
    u8g2.drawStr(0, 84, st.c_str());
    if (it->type == T_HEX) {
      u8g2.drawStr(0, 94, "range 00..FF (hex)");
    } else if (it->type == T_INT || it->type == T_FLOAT) {
      u8g2.drawStr(0, 94, (String("range ") + rangeText(it)).c_str());
    } else if (it->type == T_HOST) {
      u8g2.drawStr(0, 94, "ip like 10.0.0.5 or host");
    } else {
      u8g2.drawStr(0, 94, (String(g_editBuf.length()) + "/63 chars").c_str());
    }
  }

  if (g_toast.length() && millis() < g_toastUntil) {
    u8g2.drawRBox(0, 100, 128, 13, 3);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.drawStr(4, 110, g_toast.c_str());
    u8g2.setDrawColor(1);
  }
  footer("click:add  hold:save");
}

static void drawConfirm() {
  header("RESET ALL");
  u8g2.setFont(u8g2_font_5x8_tf);
  if (g_pend == P_RESETALL) {
    spinner(64, 56);
    centerText("resetting...", 90);
  } else {
    u8g2.setFont(u8g2_font_7x13B_tf);
    centerText("Are you sure?", 36);
    u8g2.setFont(u8g2_font_5x8_tf);
    centerText("Every setting returns", 54);
    centerText("to its default.", 63);
    centerText("This cannot be undone.", 76);
    // hold-to-confirm hint
    u8g2.drawRFrame(14, 88, 100, 16, 4);
    centerText("HOLD = yes", 99);
  }
  footer("click:no, keep settings");
}

static void drawCalib() {
  char h[16];
  snprintf(h, sizeof(h), "step %d/3", g_calStep >= 2 ? 3 : g_calStep + 1);
  header("MOISTURE CALIB", h);
  u8g2.setFont(u8g2_font_5x8_tf);
  char l[40];
  if (g_calStep == 3) {
    spinner(64, 56);
    centerText("saving...", 90);
    footer("please wait...");
  } else if (g_calStep == 2) {
    snprintf(l, sizeof(l), "dry  0%%  = %.3f V", g_calV0);
    u8g2.drawStr(0, 21, l);
    snprintf(l, sizeof(l), "wet 100%% = %.3f V", g_calV100);
    u8g2.drawStr(0, 31, l);
    u8g2.drawHLine(0, 36, 128);
    u8g2.drawStr(0, 48, "SLOPE");
    u8g2.drawStr(0, 72, "INTERCEPT");
    u8g2.setFont(u8g2_font_7x13B_tf);
    snprintf(l, sizeof(l), "%.4f", g_calSlope);
    u8g2.drawStr(0, 60, l);
    snprintf(l, sizeof(l), "%.4f", g_calInt);
    u8g2.drawStr(0, 85, l);
    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.drawStr(0, 100, "% = slope x V + intercept");
    footer("click:save  turn:redo");
  } else {
    bool dry = g_calStep == 0;
    centerText(dry ? "Sensor DRY (0% moisture)" : "Sensor WET (100% moisture)", 20);
    centerText(dry ? "dry air / dry soil" : "in water / soaked soil", 30);
    if (!dry) {
      snprintf(l, sizeof(l), "dry saved: %.3f V", g_calV0);
      centerText(l, 42);
    }
    if (g_calHave) {
      snprintf(l, sizeof(l), "%.3f", g_calVolts);
      u8g2.setFont(u8g2_font_logisoso20_tf);
      centerText(l, 78);
      u8g2.setFont(u8g2_font_6x10_tf);
      centerText("volts (live)", 92);
      if ((millis() / 400) % 2) u8g2.drawDisc(122, 14, 2);
    } else {
      spinner(64, 70);
    }
    u8g2.setFont(u8g2_font_5x8_tf);
    if (g_calErr.length()) centerText(g_calErr.c_str(), 106);
    footer(dry ? "wait to settle, click" : "settle, click  turn:back");
  }
}

static void drawBle() {
  Bridge::Stats s = Bridge::stats();
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 7, "BLE BRIDGE ACTIVE");
  u8g2.drawHLine(0, 9, 128);
  char l[40];
  u8g2.drawStr(0, 19, Bridge::deviceName());
  snprintf(l, sizeof(l), "MTU %u  CI %.1f ms", Bridge::mtu(), Bridge::connIntervalUnits() * 1.25f);
  u8g2.drawStr(0, 28, l);
  snprintf(l, sizeof(l), "mb>BLE %lu B", (unsigned long)s.uartToBle);
  u8g2.drawStr(0, 37, l);
  snprintf(l, sizeof(l), "BLE>mb %lu B", (unsigned long)s.bleToUart);
  u8g2.drawStr(0, 46, l);
  snprintf(l, sizeof(l), "drop %lu/%lu  retry %lu", (unsigned long)s.dropUartToBle,
           (unsigned long)s.dropBleToUart, (unsigned long)s.notifyRetries);
  u8g2.drawStr(0, 55, l);
}

static inline bool dipOn(int pin);
static void drawLogScreen();

static void redraw() {
  u8g2.clearBuffer();
  if (dipOn(DIP_LOG_PIN)) drawLogScreen();
  else if (Bridge::isConnected()) drawBle();
  else {
    switch (g_scr) {
      case SCR_HOME: drawHome(); break;
      case SCR_MENU: drawMenu(); break;
      case SCR_VIEW: drawView(); break;
      case SCR_EDIT: drawEdit(); break;
      case SCR_CONFIRM: drawConfirm(); break;
      case SCR_CALIB: drawCalib(); break;
    }
    // toast banner (the editor draws its own above the footer)
    if (g_scr != SCR_EDIT && g_toast.length() && millis() < g_toastUntil) {
      u8g2.setFont(u8g2_font_6x10_tf);
      int w = u8g2.getStrWidth(g_toast.c_str()) + 12;
      if (w > 126) w = 126;
      int x = (128 - w) / 2;
      u8g2.setDrawColor(0);
      u8g2.drawRBox(x - 2, 50, w + 4, 22, 5);
      u8g2.setDrawColor(1);
      u8g2.drawRFrame(x, 52, w, 18, 4);
      u8g2.drawStr(x + 6, 65, g_toast.c_str());
    }
  }
  u8g2.sendBuffer();
}

// ------------------------------------------------------------------ events
static void viewMove(int dir) {  // turn the knob in a view: next / previous item
  int p = g_listPos + dir;
  if (p < 0 || p >= g_listN) return;
  reqCancel();
  g_pend = P_NONE;
  g_listPos = p;
  g_sel = p;
  if (g_sel < g_top) g_top = g_sel;
  if (g_sel > g_top + SCR_ROWS - 2) g_top = g_sel - (SCR_ROWS - 2);
  enterView(g_list[p]);
}

static void editSave() {
  const char *why;
  if (!validate(g_item, g_editBuf, &why)) { toast(String("x ") + why); return; }
  sendSet(g_item->type == T_HEX ? "0x" + g_editBuf : g_editBuf);
}

static void handleEvent(EvType e) {
  g_dirty = true;
  if (Bridge::isConnected()) return;  // BLE owns the link

  switch (g_scr) {
    case SCR_HOME:
      if (e == EV_CW && g_hsel < G_COUNT - 1) g_hsel++;
      else if (e == EV_CCW && g_hsel > 0) g_hsel--;
      else if (e == EV_CLICK) {
        buildList((Group)g_hsel);
        g_sel = g_top = 0;
        g_scr = SCR_MENU;
      }
      break;

    case SCR_MENU:
      if (e == EV_CW && g_sel < g_listN - 1) g_sel++;
      else if (e == EV_CCW && g_sel > 0) g_sel--;
      else if (e == EV_CLICK) { g_listPos = g_sel; enterView(g_list[g_sel]); }
      else if (e == EV_LONG) g_scr = SCR_HOME;
      if (g_sel < g_top) g_top = g_sel;
      if (g_sel > g_top + SCR_ROWS - 2) g_top = g_sel - (SCR_ROWS - 2);
      break;

    case SCR_VIEW:
      if (e == EV_LONG) {
        reqCancel();
        g_pend = P_NONE;
        g_viewBusy = false;
        g_scr = SCR_MENU;
      } else if (g_viewBusy || g_pend == P_SET || g_pend == P_RESET) {
        // wait for the reply
      } else if (e == EV_CW || e == EV_CCW) {
        int rows = wrapRows(g_viewText, 25);
        bool scrolls = g_viewOk && g_item->type == T_STRRO && rows > 11;
        if (scrolls) {
          int ms = rows - 11;
          g_scroll = constrain(g_scroll + (e == EV_CW ? 1 : -1), 0, ms);
        } else {
          viewMove(e == EV_CW ? 1 : -1);
        }
      } else if (e == EV_CLICK) {
        if (!g_viewOk && g_item->type != T_DANGER) {
          enterView(g_itemIdx);
        } else if (g_item->type == T_BOOL) {
          sendSet(boolOf(g_viewText) ? "0" : "1");
        } else if (g_item->type == T_ACTION) {
          sendSet("1");
        } else if (g_item->type == T_DANGER) {
          g_scr = SCR_CONFIRM;
        } else if (g_item->rw) {
          enterEdit();
        } else {
          enterView(g_itemIdx);
        }
      }
      break;

    case SCR_EDIT: {
      if (g_pend == P_SET || g_pend == P_RESET) {
        if (e == EV_LONG) { reqCancel(); g_pend = P_NONE; }
        break;
      }
      int n = g_csN + g_specN;
      if (e == EV_CW) g_cursor = (g_cursor + 1) % n;
      else if (e == EV_CCW) g_cursor = (g_cursor + n - 1) % n;
      else if (e == EV_LONG) editSave();
      else if (e == EV_CLICK) {
        if (g_cursor < g_csN) {
          if ((int)g_editBuf.length() < maxLen(g_item)) {
            char c = g_cs[g_cursor];
            if (g_upper && c >= 'a' && c <= 'z') c -= 32;
            g_editBuf += c;
          }
        } else {
          switch (g_spec[g_cursor - g_csN]) {
            case S_DEL: if (g_editBuf.length()) g_editBuf.remove(g_editBuf.length() - 1); break;
            case S_CLR: g_editBuf = ""; break;
            case S_CASE: g_upper = !g_upper; break;
            case S_SAVE: editSave(); break;
            case S_DFLT: sendReset(); break;
            case S_ESC: enterView(g_itemIdx); break;
          }
        }
      }
      break;
    }

    case SCR_CALIB:
      if (g_calStep == 3) break;  // saving
      if (e == EV_LONG) {
        reqCancel();
        g_pend = P_NONE;
        g_scr = SCR_MENU;
      } else if (e == EV_CLICK) {
        calibClick();
      } else if ((e == EV_CW || e == EV_CCW) && g_calStep > 0) {  // go back a step
        reqCancel();
        g_pend = P_NONE;
        g_calStep--;
        g_calHave = g_calTrig = false;
        g_calAt = millis();
      }
      break;

    case SCR_CONFIRM:
      if (g_pend == P_RESETALL) break;
      if (e == EV_LONG) sendResetAll();
      else if (e == EV_CLICK) g_scr = SCR_VIEW;
      break;
  }
}

static void serviceRequests() {
  reqPoll();
  if (!g_req.active || !g_req.done) return;
  Request r = g_req;
  Pend p = g_pend;
  reqCancel();
  g_pend = P_NONE;
  g_dirty = true;

  switch (p) {
    case P_PROBE:  // background "get ID"
      g_mbId = r.ok ? firstLine(r.value) : "";
      break;

    case P_VIEW:
      if (g_scr != SCR_VIEW) break;
      g_viewBusy = false;
      g_viewAt = millis();
      if (r.ok || !g_viewOk || g_viewText.length() == 0) {  // keep old reading on a failed refresh
        g_viewOk = r.ok;
        g_viewText = r.value;
      }
      if (r.ok) {
        g_cache[g_itemIdx] = r.value;
        if (!strcmp(g_item->name, "ID")) g_mbId = firstLine(r.value);
      }
      break;

    case P_SET:
      if (r.ok) {
        g_cache[g_itemIdx] = r.value;
        g_viewText = r.value;
        g_viewOk = true;
        g_viewBusy = false;
        g_viewAt = millis();
        g_scr = SCR_VIEW;
        if (g_item->type == T_ACTION) toast("Modem waking...");
        else toast("Saved!");
      } else {
        toast(r.value.substring(0, 24));
      }
      break;

    case P_RESET:
      if (r.ok) { toast("Default restored"); enterView(g_itemIdx); }
      else toast(r.value.substring(0, 24));
      break;

    case P_RESETALL:
      if (r.ok) {
        for (int i = 0; i < kItemCount; i++) g_cache[i] = "";
        g_scr = SCR_HOME;
        toast("All reset to defaults", 2500);
      } else {
        g_scr = SCR_VIEW;
        toast(r.value.substring(0, 24));
      }
      break;

    case P_CAL_TRIG:
      g_calAt = millis() + (r.ok ? 1500 : 2000);
      g_calTrig = r.ok;
      g_calErr = r.ok ? "" : r.value.substring(0, 24);
      break;

    case P_CAL_READ: {
      g_calTrig = false;
      g_calAt = millis() + 300;
      String m, x;
      splitAge(firstLine(r.value), m, x);
      double d;
      if (r.ok && parseNum(m, d)) { g_calVolts = d; g_calHave = true; g_calErr = ""; }
      else g_calErr = r.ok ? "bad volts reply" : r.value.substring(0, 24);
      break;
    }

    case P_CAL_SET1:
      if (r.ok) {
        char b[48];
        snprintf(b, sizeof(b), "set MOISTURE_INTERCEPT %.4f", g_calInt);
        g_pend = P_CAL_SET2;
        reqStart("MOISTURE_INTERCEPT", b, 6000);
      } else {
        g_calStep = 2;
        toast(r.value.substring(0, 24));
      }
      break;

    case P_CAL_SET2:
      if (r.ok) {
        for (int i = 0; i < kItemCount; i++)
          if (!strcmp(kItems[i].name, "MOISTURE_SLOPE") || !strcmp(kItems[i].name, "MOISTURE_INTERCEPT")) g_cache[i] = "";
        g_scr = SCR_MENU;
        toast("Calibrated & saved!", 2500);
      } else {
        g_calStep = 2;
        toast(r.value.substring(0, 24));
      }
      break;

    default: break;
  }
}

// Sensors re-read themselves every few seconds so the view stays live.
static void viewAutoRefresh() {
  if (g_scr != SCR_VIEW || g_req.active || g_pend != P_NONE || !g_item) return;
  if (g_item->type != T_RO && g_item->type != T_STRRO) return;
  if (g_item->type == T_STRRO && strcmp(g_item->name, "MODEM_STATE") && strcmp(g_item->name, "GPS_FIX") && strcmp(g_item->name, "ESP_HEAP")) return;
  if (millis() - g_viewAt >= 3000) enterView(g_itemIdx, true);
}

// ----------------------------------------------------------- mode selection
static bool g_webMode = false;

static bool readDipWeb() { return digitalRead(DIP_MODE_PIN) == DIP_WEB_LEVEL; }

static void drawWeb() {
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 7, "WEB CONFIGURATOR");
  u8g2.drawHLine(0, 9, 128);
  char l[40];
  snprintf(l, sizeof(l), "WiFi: %s", Web::ssid());
  u8g2.drawStr(0, 19, l);
  snprintf(l, sizeof(l), "Pass: %s", Web::password());
  u8g2.drawStr(0, 28, l);
  snprintf(l, sizeof(l), "http://%s", Web::ip().c_str());
  u8g2.drawStr(0, 37, l);
  snprintf(l, sizeof(l), "Clients: %u  Cmds: %lu", Web::clients(), (unsigned long)Web::requestCount());
  u8g2.drawStr(0, 46, l);
  u8g2.drawStr(0, 55, Web::busy() ? "Talking to mainboard..." : "Ready");
  u8g2.drawStr(0, kFootY, "SW1: on=web off=BLE");
}

static inline bool dipOn(int pin) { return digitalRead(pin) == LOW; }

static void drawLogScreen() {
  u8g2.setFont(u8g2_font_5x8_tf);
  char h[32];
  snprintf(h, sizeof(h), "LOG %s %lu", g_webMode ? "WEB" : "BLE", (unsigned long)Bridge::baud());
  u8g2.drawStr(0, 7, h);
  u8g2.drawHLine(0, 9, 128);
  char lines[SCR_ROWS][LOG_COLS + 1];
  size_t n = Bridge::logCopy(lines, SCR_ROWS);
  if (n == 0) u8g2.drawStr(0, 24, "(no data from mainboard)");
  for (size_t i = 0; i < n; i++) u8g2.drawStr(0, 18 + (SCR_ROWS - n + i) * 9, lines[i]);
}

// ------------------------------------------------------------- status LED
// Blue = BLE mode, green = web mode. Slow blink = waiting for a client,
// solid = client connected, yellow = web talking to the mainboard, brief
// white flash = bytes received from the mainboard.
static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t last = 0xFFFFFFFF;
  uint32_t v = (r << 16) | (g << 8) | b;
  if (v == last) return;
  last = v;
  neopixelWrite(LED_PIN, r, g, b);
}

static void ledUpdate() {
  static uint32_t lastTotal = 0, flashUntil = 0;
  uint32_t now = millis(), total = Bridge::rxTotal();
  if (total != lastTotal) { lastTotal = total; flashUntil = now + 40; }
  const uint8_t L = LED_BRIGHT;
  if (now < flashUntil) { ledSet(L, L, L); return; }
  bool blinkOn = (now % 2000) < 150;
  if (g_webMode) {
    if (Web::busy()) ledSet(L, L * 2 / 3, 0);
    else if (Web::clients() > 0) ledSet(0, L, 0);
    else if (blinkOn) ledSet(0, L, 0);
    else ledSet(0, 0, 0);
  } else {
    if (Bridge::isConnected()) ledSet(0, 0, L);
    else if (blinkOn) ledSet(0, 0, L);
    else ledSet(0, 0, 0);
  }
}

// --------------------------------------------------------------------- main
void setup() {
  ledSet(LED_BRIGHT, 0, LED_BRIGHT);  // magenta while booting
  Serial.begin(115200);  // USB CDC debug only
  pinMode(DIP_MODE_PIN, INPUT_PULLUP);
  pinMode(DIP_LOG_PIN, INPUT_PULLUP);
  pinMode(DIP_BAUD_PIN, INPUT_PULLUP);
  pinMode(DIP_FLIP_PIN, INPUT_PULLUP);
  delay(5);
  g_webMode = readDipWeb();

  g_evq = xQueueCreate(16, sizeof(EvType));
  xTaskCreatePinnedToCore(encoderTask, "encoder", 3072, nullptr, 3, nullptr, TASK_CORE);

  Wire.setPins(OLED_SDA_PIN, OLED_SCL_PIN);

  Wire.begin();

  u8g2.begin();
  u8g2.setDisplayRotation(dipOn(DIP_FLIP_PIN) ? U8G2_R2 : U8G2_R0);
  u8g2.setBusClock(OLED_I2C_HZ);
  // Boot splash: a little loading bar that fills up.
  for (int i = 0; i <= 16; i++) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_logisoso20_tf);
    u8g2.drawStr(26, 52, "PACT");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(22, 70, "field configurator");
    u8g2.drawStr(34, 86, g_webMode ? "mode: WEB" : "mode: BLE");
    u8g2.drawRFrame(14, 98, 100, 10, 3);
    u8g2.drawRBox(16, 100, i * 96 / 16, 6, 1);
    u8g2.sendBuffer();
    delay(25);
  }

  Bridge::begin(!g_webMode, dipOn(DIP_BAUD_PIN) ? BAUD_ALT : MB_UART_BAUD);
  if (g_webMode) Web::begin();
  g_nextProbe = millis() + 500;
}

void loop() {
  ledUpdate();
  EvType e;
  bool logOn = dipOn(DIP_LOG_PIN);
  while (xQueueReceive(g_evq, &e, 0) == pdTRUE) {
    if (!g_webMode && !logOn) handleEvent(e);
  }

  // SW3: baud rate, applied live once stable for 200 ms.
  static uint32_t baudSince = 0;
  uint32_t wantBaud = dipOn(DIP_BAUD_PIN) ? BAUD_ALT : MB_UART_BAUD;
  if (wantBaud != Bridge::baud()) {
    if (baudSince == 0) baudSince = millis();
    if (millis() - baudSince > 200) { Bridge::setBaud(wantBaud); baudSince = 0; g_dirty = true; }
  } else {
    baudSince = 0;
  }

  // SW4: screen flip, applied live.
  static bool flipped = dipOn(DIP_FLIP_PIN);
  bool wantFlip = dipOn(DIP_FLIP_PIN);
  if (wantFlip != flipped) {
    flipped = wantFlip;
    u8g2.setDisplayRotation(flipped ? U8G2_R2 : U8G2_R0);
    g_dirty = true;
  }

  // The DIP switch is sampled live: a stable change reboots into the other mode
  // (WiFi and BLE are never brought up together).
  static uint32_t dipSince = 0;
  if (readDipWeb() != g_webMode) {
    if (dipSince == 0) dipSince = millis();
    if (millis() - dipSince > 300) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(0, 30, "Switching mode...");
      u8g2.sendBuffer();
      delay(200);
      ESP.restart();
    }
  } else {
    dipSince = 0;
  }

  if (g_webMode) {
    Web::loop();
    static uint32_t lastWeb = 0;
    if (millis() - lastWeb > (logOn ? 150u : 400u) || g_dirty) {
      lastWeb = millis();
      g_dirty = false;
      u8g2.clearBuffer();
      if (logOn) drawLogScreen(); else drawWeb();
      u8g2.sendBuffer();
    }
    delay(2);
    return;
  }

  bool ble = Bridge::isConnected();
  if (ble != g_wasBle) {  // hand the link over cleanly
    g_wasBle = ble;
    reqCancel();
    g_pend = P_NONE;
    g_viewBusy = false;
    if (!ble) { g_scr = SCR_HOME; g_nextProbe = millis() + 500; }
    g_dirty = true;
  }

  if (!ble) {
    // Background link probe while idle in the menu (and until we know the ID).
    if (!logOn && !g_req.active && (g_scr == SCR_HOME || g_scr == SCR_MENU) && g_pend == P_NONE && millis() >= g_nextProbe) {
      g_pend = P_PROBE;
      reqStart("ID", "get ID", 1500);
      g_nextProbe = millis() + (g_mbId.length() ? 30000 : 4000);
    }
    serviceRequests();
    viewAutoRefresh();
    calibUpdate();
    static uint32_t lastAnim = 0;  // animations: spinner, cursor blink, toast
    bool animating = g_viewBusy || g_pend != P_NONE || g_scr == SCR_EDIT || g_scr == SCR_HOME || g_scr == SCR_CALIB || millis() < g_toastUntil;
    if (animating && millis() - lastAnim > 120) { lastAnim = millis(); g_dirty = true; }
  } else {
    static uint32_t lastBle = 0;  // keep stats fresh
    if (millis() - lastBle > 250) { lastBle = millis(); g_dirty = true; }
  }
  if (logOn) {
    static uint32_t lastLog = 0;  // faster refresh for the live log
    if (millis() - lastLog > 150) { lastLog = millis(); g_dirty = true; }
  }

  static uint32_t lastDraw = 0;
  if ((g_dirty && millis() - lastDraw >= 30) || millis() - lastDraw >= 500) {
    g_dirty = false;
    lastDraw = millis();
    redraw();
  }
  delay(5);
}
