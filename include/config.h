// config.h - PACT field configurator: pin map and tunables
#pragma once

// ---------------------------------------------------------------- UART link
// Wire to the PACT mainboard console (the board's `Serial`, 115200 8N1 by
// default -> UART0: mainboard GPIO43 = TX0, GPIO44 = RX0). Cross TX/RX, share GND.
//   configurator GPIO11 (TX)  ->  mainboard GPIO44 (RX0)
//   configurator GPIO12 (RX)  <-  mainboard GPIO43 (TX0)
#define MB_UART            Serial1
#define MB_UART_TX_PIN     11
#define MB_UART_RX_PIN     12
// Must match SERIAL_BAUD_RATE in pact/master/lib/config.h (115200 today). The
// bridge itself is fine up to 921600: raise both sides together.
#define MB_UART_BAUD       115200

// ------------------------------------------- On-board RGB status LED (WS2812)
#define LED_PIN            21
#define LED_BRIGHT         24     // 0-255 peak brightness (full is painfully bright)

// ----------------------------------------------------------- OLED (I2C 1.5" 128x128)
#define OLED_SDA_PIN       1
#define OLED_SCL_PIN       2
#define OLED_I2C_HZ        400000
// Controller of the 1.5" 128x128 module:
//   1 = SSD1327 (grayscale, e.g. Waveshare 1.5" OLED)   2 = SH1107 (monochrome)
#define OLED_CTRL          2
#define SCR_W              128
#define SCR_H              128
#define SCR_ROWS           12     // 9 px text rows between header and footer

// ---------------------------------------------- Rotary encoder module AB024
#define ENC_CLK_PIN        4
#define ENC_DT_PIN         5
#define ENC_SW_PIN         6
#define ENC_INVERT         0      // 1 = swap rotation direction
#define ENC_LONG_PRESS_MS  600

// ---------------------------------------------------------------- BLE (NUS)
#define BLE_NAME_PREFIX    "PACT-CFG"
#define BLE_MTU_MAX        517
#define BLE_TX_POWER_DBM   9

// Connection parameters (units: interval 1.25 ms, timeout 10 ms)
// Stage 1 (Android / Windows / anything that honours it): 7.5-15 ms.
#define CONN_FAST_MIN      6      // 7.5 ms
#define CONN_FAST_MAX      12     // 15 ms
// Stage 2 (iOS-compliant, used if the central ignored stage 1): 15-30 ms.
#define CONN_IOS_MIN       12     // 15 ms
#define CONN_IOS_MAX       24     // 30 ms (>= min + 15 ms, per Apple guidelines)
#define CONN_LATENCY       0
#define CONN_TIMEOUT       400    // 4000 ms
#define CONN_FALLBACK_MS   3000   // wait this long before falling back to stage 2

// ----------------------------------------------------- Data path (FreeRTOS)
#define RB_UART_TO_BLE     (16 * 1024)
#define RB_BLE_TO_UART     (8 * 1024)
#define RB_UART_TO_UI      (4 * 1024)
#define UART_RX_BUF        (8 * 1024)   // driver ring (set before begin)
#define UART_TX_BUF        (4 * 1024)
#define TASK_CORE          1            // NimBLE host runs on core 0

// ------------------------------------------------------------ 4-way DIP switch
// Each switch pulls its pin to GND when ON (internal pull-ups). Power has its
// own dedicated switch and is NOT part of this bank. Switches are read live.
//   SW1  mode      OFF = BLE bridge            ON = Web configurator (reboots)
//   SW2  log       OFF = normal screens        ON = live mainboard log on OLED
//   SW3  baud      OFF = MB_UART_BAUD (115200) ON = BAUD_ALT (9600)
//   SW4  flip      OFF = normal                ON = screen rotated 180 deg
#define DIP_MODE_PIN       7      // SW1
#define DIP_WEB_LEVEL      LOW
#define DIP_LOG_PIN        8      // SW2
#define DIP_BAUD_PIN       9      // SW3
#define DIP_FLIP_PIN       10     // SW4
#define BAUD_ALT           9600

// Web mode: the device hosts its own WiFi hotspot + configurator page.
// CHANGE THE PASSWORD before deploying (WPA2 needs >= 8 characters).
#define WEB_AP_PASS        "pactconfig"
#define WEB_AP_CHANNEL     1
#define WEB_AP_MAX_CLIENTS 3
