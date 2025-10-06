# ESP32‑S3 Motion Camera Trap (Discord Photo Alerts)

A lightweight motion‑triggered camera trap for ESP32‑S3 that captures a color JPEG and posts it to a Discord channel via webhook. Designed for Arduino IDE with minimal dependencies and a focus on robustness and clarity.

---

## Features

- Motion detection by block‑wise frame differencing (grayscale averages per grid cell)
- Color JPEG capture (VGA if PSRAM is available, QVGA otherwise)
- Discord webhook delivery (text + image multipart upload)
- Captive‑portal configuration via **WiFiManager** (no hard‑coded secrets)
- Runtime tunables stored in NVS (Preferences)
- Long‑press button to re‑enter setup portal
- Early‑return/guard‑based loop to keep timing predictable

---

## Hardware

- **MCU:** ESP32‑S3 board with camera connector (e.g., ESP32‑S3 CAM variants)
- **Camera:** Supported by `esp_camera` (OV2640/OV3660 etc.)
- **Button (optional but recommended):** momentary switch to **GPIO 0** → GND (for config portal)
- **PSRAM:** Strongly recommended; project auto‑detects and uses it when present

> The provided pin map in `initConfig()` targets an ESP32‑S3 camera board with specific wiring. If your module differs, adjust the `camera_config_t` pins accordingly.

---

## Software Requirements

- **Arduino IDE** with **ESP32 board package by Espressif Systems**
- **Libraries:**
  - `WiFiManager` (single third‑party dependency)
  - Built‑ins from the ESP32 Arduino core: `WiFi.h`, `HTTPClient.h`, `esp_camera.h`, `Preferences.h`, `img_converters.h`

### Installing WiFiManager (Arduino IDE)

- Sketch → Include Library → Manage Libraries… → search **WiFiManager** (by tzapu) → Install

### Optional: PlatformIO

If you later port to PlatformIO, you can declare dependencies as:

```ini
[env:esp32s3]
platform = espressif32
board = esp32s3dev
framework = arduino
lib_deps =
  tzapu/WiFiManager
board_build.partitions = default.csv
build_flags =
  -DARDUINO_USB_CDC_ON_BOOT=1
  -DBOARD_HAS_PSRAM
monitor_speed = 115200
```

---

## Build & Flash (Arduino IDE)

1. Boards Manager: install **esp32** by Espressif Systems.
2. Select your **ESP32‑S3** board (e.g., “ESP32S3 Dev Module”).
3. Ensure **PSRAM** is enabled in board options (if available).
4. Connect via USB, choose the serial port.
5. Compile & Upload.

---

## First‑Run Setup (Captive Portal)

On boot, the device tries to connect using stored credentials. If none are present (or connection fails), it launches a Wi‑Fi AP and portal:

- **AP SSID:** `ESP32-Setup`
- **AP Password:** `12345678`

Connect to the AP, open the captive page, and fill in both Wi‑Fi and the custom parameters described below. After you save, the device reboots, applies the config, and runs.

### Re‑entering the Portal Later

Hold the button on **GPIO 0** low for about **1 second** during normal operation. The device enters config mode and reboots after saving.

---

## Runtime Configuration (Custom Parameters)

All values persist in NVS under the `config` namespace and are also applied to the running sketch when saved.

| Field (Portal)        | NVS Key     | Type   | Default | Description                                                                               |
| --------------------- | ----------- | ------ | ------- | ----------------------------------------------------------------------------------------- |
| Discord Webhook URL   | `webhook`   | string | ""      | Full Discord webhook URL where messages and images are posted. Keep this secret.          |
| Capture Interval (ms) | `interval`  | int    | 1000    | Minimum time between capture attempts. Larger values reduce CPU load and traffic.         |
| Motion Threshold      | `threshold` | int    | 3       | Per‑block grayscale difference threshold. Higher = less sensitive.                        |
| Pixel Step            | `pixelstep` | int    | 1       | Sampling stride inside each block. 1 = most accurate, >1 = fewer sampled pixels (faster). |
| Blocks X              | `blocksX`   | int    | 12      | Grid columns for block‑wise brightness. Higher = more granular comparison (slower).       |
| Blocks Y              | `blocksY`   | int    | 8       | Grid rows for block‑wise brightness. Higher = more granular comparison (slower).          |

**Sanity clamps:**

- `interval >= 100`
- `threshold >= 1`
- `pixelstep >= 1`
- `blocksX >= 1`, `blocksY >= 1`

---

## How Motion Detection Works

1. A frame is captured as JPEG and converted to **RGB888** using `fmt2rgb888`.
2. The image is divided into a **grid** (`blocksX` × `blocksY`).
3. For each cell, the code samples pixels with stride `pixelstep` and computes an **average grayscale value**.
4. The current grid is compared to the **reference** grid from the previous frame. If any cell’s absolute difference exceeds `threshold`, motion is flagged.
5. The reference grid is then updated to the current grid.

This approach is resilient to small noise but responsive to actual changes. Your tuning levers are:

- **Sensitivity:** `threshold` (lower → more sensitive)
- **Load vs. fidelity:** `pixelstep`, `blocksX`, `blocksY`
- **Event rate:** `interval`

---

## Camera Behavior

- If **PSRAM** is detected:
  - `frame_size = FRAMESIZE_VGA`
  - `jpeg_quality = 10`
  - frame buffer in PSRAM
- If PSRAM is not present:
  - `frame_size = FRAMESIZE_QVGA`
  - `jpeg_quality = 15`
  - frame buffer in DRAM

Pin mapping and clock are defined in `initConfig()`. Adjust for your board if preview images are garbled or if init fails.

---

## Discord Delivery

- On successful network connection, the device sends a **“ESP32 is online and connected!”** text message once.
- When motion is detected, the device posts a text notification and the **JPEG image** via a single **multipart/form‑data** HTTP POST to the Webhook.
- A short `delay(10)` follows each HTTP call to let the TCP stack breathe.

**Notes and limits**

- Webhook URLs grant write access to the channel. Treat as a secret; rotate if exposed.
- Discord has rate limits; this sketch naturally throttles by `interval`, but consider increasing it for busy scenes.

---

## Button & Safety

- **GPIO 0** is configured as `INPUT_PULLUP`. A long press (~1 s) pulls it low and triggers config mode.
- The main loop uses early returns and a tiny `delay(1)` during idle cycles to reduce busy‑wait heat and watchdog risk.
- Wi‑Fi connect has a **20 s** timeout; if it fails, the portal is started automatically.

---

## Typical Serial Log (Example)

```
Calling esp_camera_init...
Camera init OK
....
Connected to Wi‑Fi!
IP: 192.168.1.42
Webhook: https://discord.com/api/webhooks/...
Interval: 1000
Threshold: 3
Pixel Step: 1
Blocks X: 12
Blocks Y: 8
Photo captured! Size: 51234 bytes (640x480)
Image upload response code: 204
Motion detected! #1
Image upload response code: 204
```

---

## Troubleshooting

- **No portal appears:** Ensure your phone/laptop reconnects to the AP `ESP32-Setup`. Some OSes require manually opening a browser.
- **Camera init fails:** Verify pin map and supply voltage. Try QVGA + higher `jpeg_quality` first.
- **False positives:** Increase `threshold` or `pixelstep`; reduce `blocksX`/`blocksY`.
- **Missed motion:** Decrease `threshold` and/or `interval`.
- **Out‑of‑memory:** Prefer PSRAM‑equipped boards. Lower frame size to QVGA.
- **Discord not receiving:** Check that the webhook URL is correct and the device has Internet connectivity.

---

## Security Considerations

- Webhook URL is stored in plain text in NVS for convenience. If the device is shared or physically accessible, consider re‑entering the portal to clear/rotate the webhook.
- For sensitive deployments, proxy the webhook through a server you control.

---

## Design Notes

This project began from a design that considered SMTP e‑mail delivery and a local web UI for controls. The current implementation favors **Discord webhooks** for simpler, faster delivery and easier testing on mobile and desktop.

---

## License

MIT (or your preferred permissive license). Add a `LICENSE` file if you plan to distribute.

---

## Acknowledgments

- ESP32 Arduino core by Espressif Systems
- WiFiManager by tzapu
- Discord Webhooks API
