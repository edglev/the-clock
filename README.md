# Agent Viewer

A BLE-connected desktop status monitor for the **Waveshare ESP32-S3-Touch-AMOLED-1.75**.

It displays AI coding agent state on a 1.75" circular AMOLED screen with animated visuals, a status ring, touch interaction, a multi-instance roster for Claude and Codex sessions, GitLab merge requests that need review, and Bambu P2S printer and AMS status pages.

## Hardware

- **Board:** [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm)
- **MCU:** ESP32-S3 R8, dual-core LX7, 240MHz
- **RAM:** 8MB Octal PSRAM
- **Flash:** 16MB
- **Display:** 1.75" 466x466 circular AMOLED, CO5300 QSPI driver
- **Touch:** Capacitive CST92xx over I2C
- **PMIC:** AXP2101 battery management
- **Connectivity:** Bluetooth 5 LE, Wi-Fi

## Features

- **Animated status dial** - breathing/pulsing animations for each agent state
- **Status ring** - color-coded ring around the screen edge
- **Multi-instance dashboard** - one focused newest agent plus a scrollable roster
- **Bambu P2S status** - display-only cloud printer state with a PrintSphere-inspired progress ring, ETA, layers, temps, material, and AMS tray view
- **GitLab review monitor** - device-side HTTPS polling with a provisioned read-only PAT, a needs-review list, and a blinking ring when a new review appears
- **Bambu Wi-Fi fallback** - ESP32 can use provisioned Wi-Fi plus Bambu Cloud config when BLE drops, keeping printer cloud mode enabled
- **Claude/Codex labels** - main and Agents screens identify the coding CLI for each session
- **Touch interaction** - tap to acknowledge alerts
- **Swipe navigation** - swipe down for Agents, then right through Merge Requests, Printer, and Settings
- **Settings screen** - display brightness, BLE/Wi-Fi status, battery status, orientation lock, and persistent screen toggles
- **BLE GATT server** - receives state updates from a PC daemon
- **Battery monitoring** - via AXP2101 PMIC

## Agent States

| State | Color | Visual |
|-------|-------|--------|
| IDLE (0) | Cyan | Breathing circle + slow rotating line |
| THINKING (1) | Purple | Fast rotating vortex + pulsing core |
| WAITING (2) | Amber/Red | Blinking alert triangle |
| SUCCESS (3) | Green | Checkmark badge, then auto-idle |

## Architecture

```
Claude/Codex hooks -> agent-viewer daemon -> ESP32-S3 BLE --\
GitLab HTTPS API -----------------------> ESP32-S3 Wi-Fi ----+-> LVGL UI
Bambu Cloud MQTT ----------------------> ESP32-S3 Wi-Fi ----/
```

Wi-Fi is provisioned separately with `wifi-setup`. The host binary's `gitlab-setup` command sends only the GitLab URL and PAT to the paired device over encrypted BLE. The ESP32 stores them in NVS and polls GitLab directly every 60 seconds, whether or not the host daemon is connected. Existing GitLab reviews establish the baseline; a newly appearing needs-review ID blinks the ring until the Merge Requests screen is opened. A rejected or insufficiently scoped PAT produces a **Reauthenticate** state on the screen. The daemon still aggregates Claude/Codex sessions and supplies Bambu state while BLE is connected; the device-side Bambu fallback uses the same Wi-Fi connection when BLE is absent.

The Settings > Screens page can disable the primary agent dial and Agents roster, Merge Requests, or Printer + AMS pages. Changing a toggle restarts the device so the disabled pages and their background processing are omitted cleanly. Settings always remains available.

## Development

See [DEVELOPMENT.md](DEVELOPMENT.md) for build/flash instructions, host hook setup, BLE protocol details, display constraints, and implementation notes.

## License

This project is licensed under the [Federation Non-Commercial License (FNCL) v1.1](LICENSE). Commercial use requires a separate written license from the copyright holder.

## Acknowledgements

This project is inspired by [PrintSphere](https://github.com/cptkirki/PrintSphere) and borrows some PrintSphere-derived printer UI/assets code. See [`components/agent_printer/assets/NOTICE.md`](components/agent_printer/assets/NOTICE.md) for attribution and license details.

## Project Structure

```
waveshare-clock/
├── CMakeLists.txt              # ESP-IDF project root
├── sdkconfig.defaults          # Board configuration
├── partitions.csv              # Partition table
├── main/main.cpp               # Entry point
├── components/
│   ├── agent_ble/              # NimBLE GATT server
│   ├── agent_ams/              # Bambu AMS tray screen
│   ├── agent_pmic/             # AXP2101 PMIC driver
│   ├── agent_bambu/            # ESP32 Wi-Fi/Bambu Cloud fallback
│   ├── agent_features/         # Persistent optional-screen feature flags
│   ├── agent_gitlab/           # Device-side GitLab HTTPS client
│   ├── agent_merge_requests/   # GitLab review screen
│   ├── agent_printer/          # Bambu P2S printer screen
│   └── agent_viewer/           # LVGL tile shell and agent screens
├── host/                       # PC-side agent-viewer utility and helpers
└── managed_components/         # BSP, LVGL, etc. generated by ESP-IDF
```
