# Security Policy

## Reporting a Vulnerability

If you discover a security issue in `claude-buddy-s3`, please report it
**privately** before public disclosure:

- **Email**: eashe@qq.com (Subject: `claude-buddy-s3 SECURITY`)
- Or use [GitHub Security Advisories](https://github.com/lnO4X/claude-buddy-s3/security/advisories/new)

Please **do not** open a public GitHub issue for security vulnerabilities.

We aim to acknowledge reports within 7 days.

## Scope

In scope:
- Firmware buffer overflows / memory safety bugs
- BLE protocol parsing vulnerabilities (NUS payload handling)
- LittleFS path traversal via `xfer` protocol
- NVS injection via `owner` / `name` commands
- Privacy leaks (info pages, Serial diag output)

Out of scope:
- Vulnerabilities in upstream `anthropics/claude-desktop-buddy` (report to Anthropic)
- Vulnerabilities in M5Stack `M5Unified` library (report to m5stack/M5Unified)
- Vulnerabilities in `githubesson/claude-hardware-buddy` patcher (report to githubesson)
- Hardware-layer issues with M5StickS3 / M5PM1 / ESP32-S3 (report to M5Stack / Espressif)

## When sharing logs / device info

If your report includes Serial diagnostic output:
- The `[diag]` line includes ESP runtime info but **no secrets**
- BT MAC address (printed during pairing) is public-air-broadcast; redact if you wish
- Owner name / pet name (stored in NVS) may be personal — redact before pasting

## Responsible disclosure timeline

We aim for a 90-day coordinated disclosure window after acknowledgement,
extendable on mutual agreement.
