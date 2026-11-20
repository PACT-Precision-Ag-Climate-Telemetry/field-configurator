// web.cpp - Web configurator. The page (web_page.h) talks to one endpoint:
//   POST /api/cmd   c=<console line>  [t=<timeout seconds>]
//   -> {"ok":true,"text":"<console reply, echo/prompt stripped>"}
// Settings are discovered from the mainboard's own "list" reply, so new
// mainboard settings show up without touching this firmware.
#include "web.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "bridge.h"
#include "config.h"
#include "web_page.h"

namespace Web {
namespace {

WebServer g_http(80);
DNSServer g_dns;
volatile uint32_t g_reqs = 0;
volatile bool g_busy = false;

String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<uint8_t>(c) >= 32) o += c;  // drop other control bytes
    }
  }
  return o;
}

// Send one console line and collect the reply. The console echoes the line,
// prints the reply, then a "\r\n> " prompt, which is our end-of-reply marker.
// Falls back to a quiet-period / timeout so a stuck mainboard can't hang us.
String consoleExec(const String &cmd, uint32_t timeoutMs, bool &gotPrompt) {
  Bridge::uiFlush();
  String out = cmd + "\r";
  Bridge::uiWrite(reinterpret_cast<const uint8_t *>(out.c_str()), out.length());

  String buf;
  uint32_t start = millis(), lastRx = start;
  gotPrompt = false;
  uint8_t tmp[128];
  while (millis() - start < timeoutMs) {
    size_t n = Bridge::uiRead(tmp, sizeof(tmp));
    if (n) {
      lastRx = millis();
      for (size_t i = 0; i < n && buf.length() < 4096; i++) buf += static_cast<char>(tmp[i]);
      int nl = buf.indexOf('\n');  // reply can only end after the echoed line
      if (nl >= 0 && buf.endsWith("\n> ")) { gotPrompt = true; break; }
    } else {
      if (buf.length() && millis() - lastRx > 1500 && buf.indexOf('\n') >= 0 && !buf.endsWith("\n> ")) {
        // reply arrived but no prompt (e.g. mainboard busy logging): accept
        if (millis() - lastRx > 1500) break;
      }
      delay(2);
    }
  }

  // Clean: drop echo line, prompts, and control bytes.
  String clean, line;
  for (unsigned i = 0; i <= buf.length(); i++) {
    char c = i < buf.length() ? buf[i] : '\n';
    if (c == '\n') {
      line.trim();
      while (line.startsWith(">")) { line.remove(0, 1); line.trim(); }
      if (line.length() && line != cmd) {
        if (clean.length()) clean += '\n';
        clean += line;
      }
      line = "";
    } else if (c >= 32 && c < 127) {
      line += c;
    }
  }
  return clean;
}

void handleCmd() {
  String cmd = g_http.arg("c");
  cmd.trim();
  bool valid = cmd.length() > 0 && cmd.length() <= 128;
  for (unsigned i = 0; valid && i < cmd.length(); i++) {
    if (cmd[i] < 32 || cmd[i] > 126) valid = false;
  }
  if (!valid) {
    g_http.send(400, "application/json", "{\"ok\":false,\"text\":\"bad command\"}");
    return;
  }
  uint32_t t = g_http.hasArg("t") ? g_http.arg("t").toInt() : 6;
  t = constrain(t, 1, 40);

  g_busy = true;
  bool prompt = false;
  String text = consoleExec(cmd, t * 1000UL, prompt);
  g_busy = false;
  g_reqs = g_reqs + 1;

  bool ok = text.length() > 0 && !text.startsWith("ERR");
  String json = String("{\"ok\":") + (ok ? "true" : "false") + ",\"done\":" + (prompt ? "true" : "false") +
                ",\"text\":\"" + jsonEscape(text.length() ? text : String("No reply from mainboard")) + "\"}";
  g_http.send(200, "application/json", json);
}

// GET /api/serial?since=N -> {"next":N2,"text":"..."}  (raw mainboard output)
void handleSerial() {
  uint32_t cursor = static_cast<uint32_t>(g_http.arg("since").toInt());
  static char buf[1024];
  size_t n = Bridge::streamRead(cursor, buf, sizeof(buf));
  String raw;
  raw.reserve(n);
  for (size_t i = 0; i < n; i++) raw += buf[i];
  g_http.sendHeader("Cache-Control", "no-store");
  g_http.send(200, "application/json",
              String("{\"next\":") + cursor + ",\"text\":\"" + jsonEscape(raw) + "\"}");
}

void redirectHome() {
  g_http.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  g_http.send(302, "text/plain", "");
}

}  // namespace

void begin() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(Bridge::deviceName(), WEB_AP_PASS, WEB_AP_CHANNEL, 0, WEB_AP_MAX_CLIENTS);
  WiFi.setSleep(false);

  g_dns.start(53, "*", WiFi.softAPIP());  // captive portal: every name -> us

  g_http.on("/", HTTP_GET, []() {
    g_http.sendHeader("Cache-Control", "no-cache");
    g_http.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
  });
  g_http.on("/api/cmd", HTTP_POST, handleCmd);
  g_http.on("/api/serial", HTTP_GET, handleSerial);
  g_http.onNotFound(redirectHome);  // /generate_204, /hotspot-detect.html, ...
  g_http.begin();
}

void loop() {
  g_dns.processNextRequest();
  g_http.handleClient();
}

const char *ssid() { return Bridge::deviceName(); }
const char *password() { return WEB_AP_PASS; }
String ip() { return WiFi.softAPIP().toString(); }
uint8_t clients() { return WiFi.softAPgetStationNum(); }
uint32_t requestCount() { return g_reqs; }
bool busy() { return g_busy; }

}  // namespace Web
