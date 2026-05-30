# Development

This document contains implementation details and setup notes for Agent Viewer.

## Build And Flash

### Prerequisites

- **ESP-IDF v6.0.1** ([setup guide](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/get-started/index.html))
- Go 1.21 or newer for the host utility

### Firmware

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

### Host Tool

Build the single host binary:

```bash
cd host
go build -o agent-viewer .
```

Run the daemon while using an AI coding CLI:

```bash
# From the repository root
./host/agent-viewer daemon
```

## Hook Setup

### Claude Code

Install Claude Code hooks into `~/.claude/settings.json`:

```bash
./host/agent-viewer install-claude-hooks
```

Alternatively, copy `host/settings_snippet.json` into your Claude Code settings and replace `/absolute/path/to/agent-viewer` with the built binary path.

### Codex CLI

Install global Codex hooks:

```bash
./host/agent-viewer install-codex-hooks
```

The installer updates `~/.codex/hooks.json` for `SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PermissionRequest`, and `Stop`. In Codex, run `/hooks` and trust the new hook commands before expecting events to reach the daemon.

Codex token stats are read from `~/.codex/state_5.sqlite` when available. If the database is missing or its schema changes, the daemon falls back to lifecycle status text.

## Architecture

```
┌─────────────────┐   Unix socket    ┌──────────────────┐    BLE GATT    ┌──────────────────┐
│ Claude/Codex CLI│ ────────────────▶  │ agent-viewer     │ ────────────▶ │ ESP32-S3         │
│ lifecycle hooks │   /tmp/agent-     │ Go, Linux        │               │ NimBLE Server    │
│                 │   viewer.sock     │                  │               │ + LVGL UI        │
│ agent-viewer    │                    │ Claude/Codex     │               │ + AXP2101 PMIC   │
│ Go, Unix        │                    │ stats            │               │                  │
└─────────────────┘                    └──────────────────┘               └──────────────────┘
```

The `agent-viewer hook` subcommand sends JSON to `/tmp/agent-viewer.sock` with `event`, `cwd`, `provider`, optional `session_id`, optional `label`, optional `model`, and `timestamp_ms`. The `agent-viewer daemon` subcommand aggregates by provider, canonical worktree path, and session id when available, then derives an 8-character instance id for BLE updates.

## BLE GATT Protocol

| Characteristic | UUID | Direction | Payload | Purpose |
|---|---|---|---|---|
| **State** | `00000000-0000-a359-42f0-4467de900002` | Host -> ESP32 | 1 byte: 0=IDLE, 1=THINKING, 2=WAITING, 3=SUCCESS | Agent status |
| **Stats** | `00000000-0000-a359-42f0-4467de900003` | Host -> ESP32 | UTF-8 string <=24 chars | Token/cost HUD |
| **Action** | `00000000-0000-a359-42f0-4467de900004` | ESP32 -> Host | 1 byte: 1=ACK | Touch acknowledgment |
| **Name** | `00000000-0000-a359-42f0-4467de900005` | Host -> ESP32 | UTF-8 string <=32 chars | Paired host display name |
| **Multi** | `00000000-0000-a359-42f0-4467de900006` | Host -> ESP32 | `U\tid\tstate\tlabel\tstatus\tprovider` or `D\tid` | Multi-agent updates |

## Display Constraints

The screen is a **466x466 circular AMOLED**. The corners of the square coordinate system are outside the visible circle. Place UI elements within the safe area.

### Visible Area Calculations

- **Center:** `(233, 233)`, **Radius:** `233`
- At Y offset from center, visible X range: `center.x +/- sqrt(R^2 - y^2)`

| Y from top | Visible X range | Notes |
|---|---|---|
| 0 | 233 +/- 0, single pixel | Very top of circle |
| 20 | 139-327 | Top bar, keep labels centered |
| 50 | 129-337 | |
| 100 | 101-365 | Safe for wider elements |
| 150 | 58-408 | |
| 200 | 16-450 | |
| 233 | 0-466 | Widest point |
| 300 | 16-450 | |
| 400 | 101-365 | |
| 446 | 139-327 | Bottom of visible area |

### Rules Of Thumb

- Avoid `LV_ALIGN_TOP_LEFT` and `LV_ALIGN_TOP_RIGHT`; positions fall outside the visible circle.
- Use `LV_ALIGN_TOP_MID` or center-based positioning with offsets no larger than about +/-100 for top/bottom content.
- Keep a 30-50px safe margin from the screen edge at top and bottom.
- Center content vertically; the circular shape clips the top and bottom roughly 33px.

## Status Ring Scroll UX

Selected implementation: **tile-local ring hide/show**. The ring hides while page navigation is in progress and reappears only when the tileview settles back on the main Agent Viewer tile.

| Option | Behavior | Difficulty |
|---|---|---|
| 1. Quick fade | Fade the status ring out as tile scrolling starts, then fade it back in only on the main Agent Viewer tile. | Low |
| 2. Directional tuck | Compress or shift the ring edge opposite the swipe direction so it appears to tuck into the circular viewport. | Medium |
| 3. Directional wipe | Clip the ring with a moving mask based on swipe direction, hiding the side that would otherwise sweep across the screen. | Medium |
| 4. Tile-local ring | Keep the ring visually owned by the main tile and hide it whenever the tile is no longer active or settled. | Low |
| 5. Rebuilt overlay | Draw the ring as a custom canvas layer with scroll-aware clipping and custom transition states. | High |

## Verification

Host tests:

```bash
cd host
go test ./...
```

Firmware build:

```bash
idf.py build
```
