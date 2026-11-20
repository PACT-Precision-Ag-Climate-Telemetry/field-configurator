// web.h - WiFi hotspot + HTTP configurator that relays to the mainboard console
#pragma once
#include <Arduino.h>

namespace Web {

void begin();   // start AP, DNS (captive portal) and HTTP server
void loop();    // service DNS + HTTP; call often from loop()

const char *ssid();
const char *password();
String ip();
uint8_t clients();
uint32_t requestCount();
bool busy();    // a console command is in flight

}  // namespace Web
