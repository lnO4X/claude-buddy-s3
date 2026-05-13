# Contributing to claude-buddy-s3

Thank you for your interest! A few notes before you open an issue or PR:

## This is an unofficial personal port

- For **upstream firmware bugs** (logic, wire protocol, state machine, UI design): please report to [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) directly. We pull bug-fix patches from upstream when applicable.
- For **port-specific issues** (M5StickS3 hardware, M5Unified API adaptation, AXP2101/M5PM1 PMIC, BMI270 IMU, ES8311 audio, USB-CDC): open an issue here.

## Bug reports

Open a [bug-report issue](https://github.com/lnO4X/claude-buddy-s3/issues/new?template=bug-report.yml) with:

- **Device batch**: sticker on back (`K150` confirms M5StickS3)
- **Firmware version**: `git describe --tags` or the tag flashed
- **PMIC**: AXP2101 or M5PM1 (visible on `DEVICE` info page: `reg06=0x??`)
- **Reproducer**: minimal steps + observed vs expected
- **Serial diag**: paste `[diag]` lines from `pio device monitor`, redact MAC if you wish

## Pull requests

Welcome for:
- Hardware adaptation fixes (port-specific GPIO, PMIC, audio, IMU)
- Documentation improvements (`docs/UPSTREAM-DELTA.md`, README, gotchas)
- Internationalization additions (more language strings)
- Easter eggs / experience polish (preserve upstream behavior parity)

Please don't PR:
- Changes to wire protocol JSON shapes (that's upstream's contract — see gotcha G15)
- Removal of `Claude-XXXX` BLE advert filter (Hardware Buddy app requires this — see G12)
- Breaking changes to character pack format / GIF decode

## Development setup

```bash
git clone https://github.com/lnO4X/claude-buddy-s3.git
cd claude-buddy-s3
pio run -e clawstick-s3                # build
pio run -e clawstick-s3 -t upload      # flash
pio device monitor -b 115200           # debug serial
```

## Code style

- Comments in English (preferred) or 中文 (acceptable, esp. for user-facing translations)
- Reference gotcha IDs (`G15`, `G29`, ...) when fixing known issues
- Document file:line cross-references for cross-module flow
- Keep upstream-verbatim files untouched where possible (modifications break sync)

## License

By contributing, you agree your changes are licensed under MIT (same as the
repo). Anthropic's original copyright on upstream-derived files is preserved.

## Code of conduct

Be kind. This is a hobby port — patience appreciated.
