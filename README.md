# claude-buddy-s3

> ⚠️ **非官方个人项目** — 不是 Anthropic 官方产品
> Unofficial personal port, not affiliated with Anthropic.

A community port of [Anthropic's Claude Desktop Hardware Buddy firmware](https://github.com/anthropics/claude-desktop-buddy) to the **M5StickS3** (ESP32-S3) hardware. The upstream firmware targets M5StickC Plus 1 (ESP32). This port adapts it for the newer S3 SoC with 8 MB PSRAM, ES8311 audio codec, and USB-OTG support.

**Maintainer**: eashe@qq.com
**License**: MIT (dual copyright — see [LICENSE](LICENSE))

---

## Status

- ✅ **D6 (D-phase end-to-end)** — Hardware Buddy → Claude Desktop approval flow working
- ✅ **F1-phase 体验优先 ship** — i18n EN/中, audio chimes, welcome ritual, Easter egg, accurate Li-Po SoC
- ⏳ **F2 polish** — USB-HID buddy → host action / TinyUSB / wake-word / etc (待 use case 确认)

Latest stable tag: `v0.7.x`.

---

## Hardware

| Component | Spec |
|---|---|
| Board | [M5StickS3](https://docs.m5stack.com/en/core/StickS3) (K150) |
| SoC | ESP32-S3-PICO-1-N8R8, dual-core LX7 240 MHz |
| Flash | 8 MB |
| PSRAM | 8 MB Octal |
| Display | ST7789P3, 135×240 |
| IMU | BMI270 |
| Audio | ES8311 codec + AW8737 amp + 8Ω speaker + MEMS mic |
| PMIC | AXP2101 or M5PM1 (depends on batch) |
| BLE | ESP32-S3 native, Nordic UART Service (NUS) |

---

## Quick start

### Prerequisites

- PlatformIO Core ≥ 6.1 (`pip install platformio`)
- USB-C cable
- M5StickS3 device
- Claude Desktop with [Hardware Buddy patcher](https://github.com/githubesson/claude-hardware-buddy) installed

### Build & flash

```bash
git clone https://github.com/lnO4X/claude-buddy-s3.git
cd claude-buddy-s3
pio run -e clawstick-s3 -t upload
```

If `Failed to connect to ESP32-S3: No serial data received`:
1. Hold **BtnA** (front button)
2. Tap **PWR** (left side small button)
3. Release BtnA → screen blank = ROM bootloader entered
4. Retry `pio run -t upload`

### Pair with Claude Desktop

1. Patch Claude Desktop with [githubesson/claude-hardware-buddy](https://github.com/githubesson/claude-hardware-buddy) → enables Hardware Buddy menu
2. Open Claude Desktop → Help → Troubleshooting → Enable Developer Mode → Hardware Buddy
3. Device advertises as `Claude-XXXX` (last 4 hex of MAC)
4. Select + 6-digit passkey pair
5. Buddy now displays Claude session status; press **BtnA** to approve / **BtnB** to deny prompts

---

## Features

### Core (matches upstream Plus 1)
- 18 ASCII pet species (capybara, cat, octopus, etc.) — 7 animation states each
- GIF character pack upload via BLE xfer protocol
- Approval prompt with 24s countdown
- Pet stats (mood/fed/energy), token tracking, level progression
- Settings menu (brightness/sound/BT/clock rotation/etc.)

### S3-specific additions
- **Bilingual UI** — English / 中文 toggle via settings (efontCN_12 GB18030 subset)
- **Accurate Li-Po SoC** — piecewise battery curve, not linear (industry-standard)
- **Audio chimes** — completion tip, approval color-shift tick chime, welcome ritual
- **Fireworks Easter egg** — hold BtnA + BtnB ≥ 2s for surprise
- **Info page scroll** — BtnA scrolls long pages, then advances mode

### Known limitations
- LED feedback (GPIO10 = EX_SDA on S3, gated via `ledIsSafe()`)
- Face-down nap disabled (BMI270 axis convention unverified, see gotchas G21)
- Battery current reading returns 0 (M5Unified limitation on M5PM1, see gotchas G34)

---

## Architecture

See [`docs/UPSTREAM-DELTA.md`](docs/UPSTREAM-DELTA.md) for provenance breakdown:
- §1 Upstream modules (wire protocol / state machine / 18 species — verbatim)
- §2 Hardware port adaptations (M5Unified, pin map, partition, USB-CDC)
- §3 Known gotchas G15–G34 (bug fixes with grounded sources)
- §4 clawstick-original experience design (F1.1–F1.16)
- §5 Reverse PR candidates → upstream

---

## Contributing

Bug reports + PRs welcome. Per [CLAUDE.md L34](https://github.com/anthropics/claude-desktop-buddy/blob/main/CLAUDE.md) (Anthropic's contribution guidelines for upstream), this port follows MIT-license downstream conventions.

**Issues**: Open a GitHub issue with:
- Device batch identifier (look at sticker on back: K150 = M5StickS3)
- Firmware tag / commit (`git describe --tags`)
- Reproducer steps

---

## License

[MIT](LICENSE) — preserves Anthropic's original copyright, adds clawstick port copyright.

**Disclaimer**: This is an unofficial personal port. Anthropic does not endorse, support, or maintain this firmware. For official Hardware Buddy on Plus 1: https://github.com/anthropics/claude-desktop-buddy
