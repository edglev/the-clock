# Agent Viewer

A BLE-connected desktop AI agent status monitor for the **Waveshare ESP32-S3-Touch-AMOLED-1.75**.

Displays your AI agent's current working state (IDLE, THINKING, WAITING, SUCCESS) on a 1.75" circular AMOLED display with animated visuals, status ring, and touch interaction.

## Hardware

- **Board:** [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm)
- **MCU:** ESP32-S3 R8 (dual-core LX7, 240MHz)
- **RAM:** 8MB Octal PSRAM
- **Flash:** 16MB
- **Display:** 1.75" 466x466 circular AMOLED (CO5300 QSPI driver)
- **Touch:** Capacitive CST92xx (I2C)
- **PMIC:** AXP2101 (battery management)
- **Connectivity:** Bluetooth 5 LE, Wi-Fi

## Features

- **Animated status dial** — breathing/pulsing animations for each agent state
- **Cyan status ring** — color-coded ring around the screen edge
- **Touch interaction** — tap to acknowledge alerts
- **Swipe navigation** — swipe left to access Settings
- **Settings screen** — display brightness control, BLE connection status
- **BLE GATT server** — receives state updates from a PC daemon
- **Battery monitoring** — via AXP2101 PMIC (battery % top-right)

### Status States

| State | Color | Visual |
|-------|-------|--------|
| IDLE (0) | Cyan | Breathing circle + slow rotating line |
| THINKING (1) | Purple | Fast rotating vortex + pulsing core |
| WAITING (2) | Amber/Red | Blinking alert triangle |
| SUCCESS (3) | Green | Checkmark badge (auto-reverts to IDLE) |

## Display Constraints

The screen is a **466×466 circular AMOLED**. The corners of the square coordinate system are outside the visible circle. Place UI elements within the safe area:

![Coordinate system: 466×466 square with circle radius 233 centered at (233, 233)]

### Visible area calculations
- **Center:** `(233, 233)`, **Radius:** `233`
- At Y offset from center, visible X range: `center.x ± sqrt(R² − y²)`

| Y (from top) | Visible X range | Notes |
|---|---|---|
| 0 | 233 ± 0 (single pixel) | Very top of circle |
| 20 | 233 ± 94 (139–327) | Top bar — keep labels centered, not at edges |
| 50 | 233 ± 104 (129–337) | |
| 100 | 233 ± 132 (101–365) | Safe for wider elements |
| 150 | 233 ± 175 (58–408) | |
| 200 | 233 ± 217 (16–450) | |
| 233 (center) | 0–466 (full width) | Widest point |
| 300 | 233 ± 217 (16–450) | |
| 400 | 233 ± 132 (101–365) | |
| 446 | 233 ± 94 (139–327) | Bottom of visible area |

### Rules of thumb
- **Avoid `LV_ALIGN_TOP_LEFT` / `LV_ALIGN_TOP_RIGHT`** — positions fall outside visible circle
- **Use `LV_ALIGN_TOP_MID` or center-based positioning** with offsets ≤ ±100 for top/bottom content
- **Keep safe margin of 30–50px** from the screen edge at top and bottom
- **Center content vertically** — the circular shape clips the top and bottom ~33px

## Architecture

```
┌─────────────────┐   Unix socket    ┌──────────────────┐    BLE GATT    ┌──────────────────┐
│  AI Agent CLI   │ ────────────────▶  │  agent-ble-daemon│ ────────────▶ │  ESP32-S3        │
│  (lifecycle     │   /tmp/agent-     │  (Go, Linux)     │               │  NimBLE Server   │
│   hooks)        │   viewer.sock     │                  │               │  +               │
│                 │                    │  reads            │               │  LVGL UI         │
│  agent-hook     │                    │  stats/           │               │  + AXP2101 PMIC  │
│  (Go, Unix)     │                    │  codex data       │               │                  │
└─────────────────┘                    └──────────────────┘               └──────────────────┘
```

### BLE GATT Protocol

| Characteristic | UUID | Direction | Payload | Purpose |
|---|---|---|---|---|
| **State** | `00000000-0000-a359-42f0-4467de900002` | Host → ESP32 | 1 byte: 0=IDLE, 1=THINKING, 2=WAITING, 3=SUCCESS | Agent status |
| **Stats** | `00000000-0000-a359-42f0-4467de900003` | Host → ESP32 | UTF-8 string ≤24 chars | Token/cost HUD |
| **Action** | `00000000-0000-a359-42f0-4467de900004` | ESP32 → Host | 1 byte: 1=ACK | Touch acknowledgment |

## Build & Flash

### Prerequisites

- **ESP-IDF v6.0.1** ([setup guide](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/get-started/index.html))

### Firmware

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```



## Project Structure

```
waveshare-clock/
├── CMakeLists.txt              # ESP-IDF project root
├── sdkconfig.defaults          # Board configuration
├── partitions.csv              # Partition table
├── main/main.cpp               # Entry point
├── components/
│   ├── agent_ble/              # NimBLE GATT server
│   ├── agent_pmic/             # AXP2101 PMIC driver
│   └── agent_viewer/           # LVGL UI (main + settings screens)
├── host/                       # PC-side utilities (Go daemon, BLE test)
└── managed_components/         # BSP, LVGL, etc. (auto-managed)
```
