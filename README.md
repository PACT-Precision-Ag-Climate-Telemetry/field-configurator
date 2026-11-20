# PACT Field Configurator

Handheld configurator for the PACT mainboard. An ESP32-S3 Zero with a 1.5" I2C OLED and an AB024 rotary encoder talks to the mainboard's serial console (`get NAME` / `set NAME VALUE`, see `pact/master/lib/setting.h`). A 4-way DIP switch sets how you reach it. Power has its own dedicated switch and is not part of the bank.

| Switch | Function | OFF | ON |
|---|---|---|---|
| **SW1** | Mode | **BLE bridge**: Nordic UART Service bridged to the mainboard UART; also the OLED + encoder menu while no phone is connected | **Web configurator**: WiFi hotspot + mobile page at `http://192.168.4.1` |
| **SW2** | Live log | Normal screens | OLED shows the mainboard's live output (last 12 lines), in both modes |
| **SW3** | Baud | 115200 | 9600 (applied live) |
| **SW4** | Screen flip | Normal | Display rotated 180 degrees (applied live) |

Switches pull their pin to GND when ON. SW1 reboots into the other mode after a stable 300 ms; SW2-SW4 take effect immediately. WiFi and BLE are never active together. The mainboard's baud must match SW3.

## Hardware

- Waveshare ESP32-S3 Zero (target `waveshare_esp32_s3_zero`)
- 1.5" I2C OLED, 128x128 (SSD1327 by default; set `OLED_CTRL` to 2 for SH1107 in `include/config.h`)
- Rotary encoder module AB024 (CLK / DT / SW)
- 1x DIP switch (mode select)
- Wires to the mainboard: TX, RX, GND

## Pin definition

All pins are defined in [include/config.h](include/config.h).

| Function | ESP32-S3 Zero GPIO | Connects to | Notes |
|---|---|---|---|
| UART TX | **GPIO11** | Mainboard **GPIO44** (RX0) | `Serial1`, 3.3 V logic |
| UART RX | **GPIO12** | Mainboard **GPIO43** (TX0) | Cross TX/RX |
| UART GND | GND | Mainboard GND | **Required** common ground |
| OLED SDA | **GPIO1** | OLED SDA | I2C, 400 kHz, address 0x3C |
| OLED SCL | **GPIO2** | OLED SCL | |
| OLED VCC / GND | 3V3 / GND | OLED VCC / GND | |
| Encoder CLK (A) | **GPIO4** | AB024 `CLK` | Internal pull-up |
| Encoder DT (B) | **GPIO5** | AB024 `DT` | Internal pull-up |
| Encoder SW | **GPIO6** | AB024 `SW` | Internal pull-up, active low |
| Encoder + / GND | 3V3 / GND | AB024 `+` / `GND` | |
| DIP SW1 (mode) | **GPIO7** | Other side to GND | Internal pull-up. ON = Web, OFF = BLE |
| DIP SW2 (live log) | **GPIO8** | Other side to GND | ON = log on OLED |
| DIP SW3 (baud) | **GPIO9** | Other side to GND | ON = 9600, OFF = 115200 |
| DIP SW4 (flip) | **GPIO10** | Other side to GND | ON = screen rotated 180 degrees |
| USB | native USB | PC | Flashing + debug serial (`Serial`) |

Reserved on the Zero and not used: GPIO0 (BOOT button), GPIO21 (on-board RGB LED), GPIO19/20 (USB).

### Wiring diagram

```
 ESP32-S3 Zero                           PACT mainboard
 ─────────────                           ──────────────
 GPIO11 (TX) ───────────────────────────► GPIO44 (RX0)
 GPIO12 (RX) ◄─────────────────────────── GPIO43 (TX0)
 GND         ──────────────────────────── GND

 GPIO1  ── SDA ┐
 GPIO2  ── SCL ├── 1.5" OLED (3V3, GND)
               ┘
 GPIO4  ── CLK ┐
 GPIO5  ── DT  ├── AB024 encoder (+ = 3V3, GND)
 GPIO6  ── SW  ┘
 GPIO7  ── DIP SW1 ── GND   (mode: ON = Web, OFF = BLE)
 GPIO8  ── DIP SW2 ── GND   (live log on OLED)
 GPIO9  ── DIP SW3 ── GND   (baud: ON = 9600)
 GPIO10 ── DIP SW4 ── GND   (screen flip)
```

The mainboard console is on its `Serial` at 115200 baud. This assumes `Serial` is UART0 (GPIO43/44) on the mainboard, which is the default for its `esp32-s3-devkitc-1` profile with USB-CDC-on-boot off. If the mainboard routes its console elsewhere, change `MB_UART_TX_PIN` / `MB_UART_RX_PIN`.

## Build and flash

Requires [PlatformIO](https://platformio.org/). On Windows, run it from PowerShell or cmd, not Git Bash (the ESP toolchain installer rejects MSys).

```
pio run -t upload
pio device monitor
```

Libraries (installed automatically): NimBLE-Arduino 2.x, U8g2. Arduino IDE: rename `src/main.cpp` to an `.ino`, copy the other files into the sketch folder, install those two libraries, and pick a partition scheme with at least 1.5 MB of app space (for example Minimal SPIFFS).

## Using it

### BLE mode (SW1 OFF)

- Advertises as `PACT-CFG-XXXX` with Nordic UART Service:
  - Service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
  - RX (write / write without response) `6E400002-…`
  - TX (notify) `6E400003-…`
- Negotiates MTU 517, 251-byte data length, 2M PHY. Connection interval is requested at 7.5-15 ms first, falling back after 3 s to 15-30 ms for iOS. Latency 0, timeout 4 s.
- Connect with a BLE serial app and type console commands (`help`, `get ID`, `set SERVER_PORT 5000`).
- OLED menu (no BLE client connected): rotate to scroll, click to read, click again to edit. In the editor, rotate to choose a character, click to add it, and long press to save. While a BLE client is subscribed the OLED shows MTU, interval and byte counters, and the local menu is locked.

### Web mode (SW1 ON)

1. Join the WiFi `PACT-CFG-XXXX` (shown on the OLED). Default password: `pactconfig`.
2. Open `http://192.168.4.1` (the phone's sign-in prompt should also open it).
3. **Settings** tab: read any setting, edit and save the editable ones. **Console** tab: send raw commands.

The settings list comes from the mainboard's `help` reply, so new mainboard settings appear automatically. Slow reads (GPS, ultrasonic debug/monitor) can take up to 30 s.

## Configuration

Everything tunable is in [include/config.h](include/config.h):

| Define | Default | Meaning |
|---|---|---|
| `MB_UART_BAUD` | 115200 | Baud with SW3 OFF. Must match the mainboard's `SERIAL_BAUD_RATE`. The bridge handles up to 921600 if you change both. |
| `BAUD_ALT` | 9600 | Baud with SW3 ON |
| `OLED_CTRL` | 1 | 1 = SSD1327 (grayscale), 2 = SH1107 (mono) for the 1.5" 128x128 panel |
| `ENC_INVERT` | 0 | Swap encoder direction |
| `ENC_LONG_PRESS_MS` | 600 | Long-press threshold |
| `DIP_MODE_PIN` / `DIP_LOG_PIN` / `DIP_BAUD_PIN` / `DIP_FLIP_PIN` | 7 / 8 / 9 / 10 | DIP switch pins (ON = LOW) |
| `WEB_AP_PASS` | `pactconfig` | Hotspot password (8+ characters). **Change before deploying.** |
| `CONN_*` | see file | BLE connection parameter stages |
| `RB_*` | 16 K / 8 K / 4 K | Ring buffer sizes |

## Architecture

```
mainboard ──UART── uartRxTask ──ring──► bleTxTask ──notify──► BLE central
mainboard ◄─UART── uartTxTask ◄─ring──  onWrite() ◄──write──  BLE central
                       │ (no central / web mode)
                       └─ui ring──► OLED menu  or  Web /api/cmd
```

BLE callbacks only copy into FreeRTOS ring buffers and return, so they never block UART streaming. A full ring drops and counts bytes rather than stalling.

## Known limitations

- BLE and the web page are unauthenticated apart from the WiFi password. Anyone in range who can connect can change mainboard settings.
- BLE write-without-response has no flow control. A fast sender can overrun the 8 KB BLE-to-UART ring; drops are counted on the OLED. Use write-with-response for bulk data.
- The OLED menu's setting list is hard-coded in `kItems` (`src/main.cpp`). The web page discovers it dynamically.
- Slow reads block the web server and OLED redraw until the mainboard answers.
- Not yet tested on hardware.

## Files

| File | Purpose |
|---|---|
| `include/config.h` | Pins and tunables |
| `include/bridge.h`, `src/bridge.cpp` | UART + ring buffers + BLE NUS bridge |
| `include/web.h`, `src/web.cpp` | WiFi hotspot, captive portal, HTTP API |
| `include/web_page.h` | The mobile web page (served from flash) |
| `src/main.cpp` | Mode switch, OLED menu, encoder |
