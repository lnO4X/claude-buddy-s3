# clawstick — Upstream Delta + Design Provenance

> **Purpose**: For future maintenance + bug triage, identify what's **inherited from upstream** ([anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)) vs **clawstick-original**. When debugging "why does X behave this way?", check this file first to know whether to consult upstream behavior or clawstick design notes.
>
> **Last sync**: upstream commit `a280c64` (2026-04-16, "Add screenshots to README and bridge-enable steps to REFERENCE")
>
> **License**: upstream MIT preserved; clawstick additions also MIT (per [LICENSE](../LICENSE) dual-author).

---

## 0. 怎么用这文档

- 找代码出处 → grep filename + section here → 标记 [UPSTREAM] / [PORT] / [CLAWSTICK]
- 改代码前 → 看是否 [UPSTREAM] (动它要谨慎: upstream parity 可能 break)
- 修 bug → 见 §3 G15-G27 known issues, 大概率已记录
- Reverse PR upstream 候选 → 见 §5

## 1. 上游模块 [UPSTREAM] — verbatim 或近 verbatim

These modules / behaviors come directly from upstream. **Don't modify without understanding upstream impact**.

### 1.1 Wire protocol (BLE NUS JSON)

| 项 | 上游来源 | 文件 |
|---|---|---|
| Service UUID `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | upstream verbatim | [ble_bridge.cpp](../src/ble_bridge.cpp) |
| Heartbeat snapshot JSON shape (`total/running/waiting/msg/entries/tokens/prompt`) | upstream verbatim | [data.h](../src/data.h) `_applyJson` |
| Permission decision TX shape `{"cmd":"permission","id":...,"decision":"once|deny"}` | upstream verbatim (per G15 fix — was guessed wrong initially) | [main.cpp:1255-1310](../src/main.cpp) sendPermission |
| `time / owner / name / unpair / status` cmds | upstream verbatim | [data.h](../src/data.h) `_applyJson` + [xfer.h](../src/xfer.h) |
| Folder push protocol (`char_begin/file/chunk/file_end/char_end`) | upstream verbatim | [xfer.h](../src/xfer.h) |
| LE Secure Connections + 6-digit passkey + DisplayOnly IO | upstream verbatim | [ble_bridge.cpp](../src/ble_bridge.cpp) |

### 1.2 State machine + UI structure

| 项 | 上游来源 | 文件 |
|---|---|---|
| 7 PersonaStates (sleep/idle/busy/attention/celebrate/dizzy/heart) | upstream | [main.cpp:42](../src/main.cpp) |
| derive() function (state from tama snapshot) | upstream verbatim per G16 audit | [main.cpp](../src/main.cpp) |
| Screen-off auto after 30s idle | upstream | [main.cpp](../src/main.cpp) `SCREEN_OFF_MS` |
| Approval prompt UI 24s elapsed countdown | upstream (color shift = clawstick add per F1.1 M1.1) | [main.cpp drawApproval](../src/main.cpp) |
| Settings menu structure (10 items + back) | upstream | [main.cpp:221](../src/main.cpp) (clawstick added "language" → 11) |
| Main menu (settings/turn off/help/about/demo/close) | upstream | [main.cpp:217](../src/main.cpp) |
| Reset menu (delete char / factory reset / back) | upstream | [main.cpp:226](../src/main.cpp) |
| Info pages (about/buttons/claude/device/ble/credits) | upstream skeleton | [main.cpp drawInfo](../src/main.cpp) (clawstick i18n + credits update per F1.6) |
| Pet / stats display | upstream | [main.cpp drawPet drawPetStats drawPetHowTo](../src/main.cpp) |
| Landscape clock face | upstream | [main.cpp drawClock](../src/main.cpp) |

### 1.3 Character system

| 项 | 上游来源 | 文件 |
|---|---|---|
| 18 ASCII species (capybara/axolotl/cat/octopus/...) — 7 states each | upstream verbatim | [buddies/*.cpp](../src/buddies/) (18 files) |
| GIF character pack format + manifest.json schema | upstream | [character.cpp](../src/character.cpp) |
| AnimatedGIF lib integration | upstream | [character.cpp](../src/character.cpp) |
| Character peek mode (1× scale for PET/INFO) vs home (2× scale) | upstream | [buddy.cpp buddySetPeek](../src/buddy.cpp) |

### 1.4 Settings + persistence

| 项 | 上游来源 | 文件 |
|---|---|---|
| Settings struct + NVS Preferences storage | upstream | [stats.h Settings](../src/stats.h) (clawstick added `lang` field per F1.4) |
| Stats counter (approvals/denials/level/tokens/nap) | upstream | [stats.h Stats](../src/stats.h) |
| Owner name + pet name persistence | upstream | [stats.h ownerName/petName](../src/stats.h) |

---

## 2. 硬件 port 适配 [PORT] — Plus 1 → M5StickS3

Upstream targets M5StickC Plus (ESP32 + AXP192 + MPU6886 + ST7789v2 + UART). M5StickS3 differs in nearly every component → these layers re-written.

| 适配 | 改动 | 文件 |
|---|---|---|
| Library | M5StickCPlus.h → **M5Unified.h** | [main.cpp #include](../src/main.cpp), all .cpp |
| Sprite type | TFT_eSprite → **M5Canvas** (LovyanGFX) | [main.cpp](../src/main.cpp), [character.cpp](../src/character.cpp), [buddy.cpp](../src/buddy.cpp) |
| Surface base type | `TFT_eSPI*` → **`LovyanGFX*`** in render fn signatures | [buddy.cpp buddyRenderTo](../src/buddy.cpp), [character.cpp characterRenderTo](../src/character.cpp) |
| PIO board | `m5stick-c` → **`m5stack-stamps3`** + Octal PSRAM `qio_opi` flags (G4) | [platformio.ini](../platformio.ini) |
| Partition table | 4 MB → **8 MB no-OTA + big LittleFS** | [partitions/no_ota_8mb.csv](../partitions/no_ota_8mb.csv) |
| Audio API | `M5.Beep.tone` → **`M5.Speaker.tone`** + `setVolume(128)` (G26) | [main.cpp setup + beep()](../src/main.cpp) |
| RTC API | upstream `RTC_TimeTypeDef Hours/Minutes/Seconds` → **`m5::rtc_datetime_t {date, time}`** | [main.cpp clockRefreshRtc](../src/main.cpp), [data.h _applyJson](../src/data.h) |
| Buttons | upstream G37/G39 → **G11/G12** (M5Unified auto-map per references.md §D) | M5Unified handles |
| Power button | upstream `M5.Axp.GetBtnPress() == 0x02` (edge) → **`M5.BtnPWR.wasReleasedAfterHold()`** + `setHoldThresh(1000)` (G20) | [main.cpp setup + loop](../src/main.cpp) |
| LED GPIO10 | upstream direct GPIO write → **`ledIsSafe()` runtime gate** (G19, S3 GPIO10=EX_SDA not LED) | [main.cpp ledIsSafe()](../src/main.cpp) |
| IMU shake algorithm | algorithm verbatim from upstream (per G17 audit), but BMI270 vs MPU6886 axis convention untested per G21 | [main.cpp checkShake](../src/main.cpp) |
| Face-down nap | disabled on S3 (BMI270 axis unverified per G21) | [main.cpp isFaceDown()](../src/main.cpp) |
| USB-CDC | `_usbLine.feed(Serial, out)` **deleted** — USB-CDC available() hangs (G25); BLE-only protocol per references.md §A.1 | [data.h dataPoll](../src/data.h) |
| BLE name | **`Claude-XXXX`** (G12 — Hardware Buddy filter requires `Claude-` prefix) | [main.cpp startBt](../src/main.cpp) |

---

## 3. 已知 Bug fixes (G15-G27) — clawstick discoveries

详见 [research/gotchas.md](../../research/gotchas.md):

| ID | 问题 | 文件 | 状态 |
|---|---|---|---|
| **G15** ★ | Wire protocol permission JSON shape 错猜 | main.cpp sendPermission | ✅ verbatim from upstream |
| G16 | derive() ATTENTION fallback (intentional deviation 因无 drawApproval overlay) | main.cpp derive | ✅ documented |
| G17 | IMU shake EWMA 算法对齐上游 | main.cpp checkShake | ✅ verbatim |
| G18 | Audit methodology meta — 任何 wire/state 必须 verbatim 不许猜 | docs principle | ✅ doc |
| G19 | LED_PIN=10 = EX_SDA (S3) not LED → runtime gate | main.cpp ledIsSafe | ✅ |
| G20 | `pressedFor` continuous → `wasReleasedAfterHold` edge | main.cpp setup + loop | ✅ |
| G21 | Splash sprite 残留 (originally fixed, then **REVERTED per G27** as over-engineering) | — | ❌ reverted |
| G22 | LittleFS auto-format on first-boot mount fail | character.cpp characterInit | ✅ |
| **G25** ★★★ | `_usbLine.feed(Serial, out)` USB-CDC hang dataPoll → loop 死 (4 次猜错后 5-stage bisect 发现) | data.h dataPoll | ✅ deleted call |
| G26 | M5Unified Speaker default volume 64 → set 128 for audibility | main.cpp setup | ✅ |
| G27 | G21 over-engineering postmortem | doc | ✅ doc |

---

## 4. clawstick-original 体验设计 [CLAWSTICK] (post-D6 plan v2)

Per [plan v2 §3 M1-M8](../../../../../.claude/plans/per-project-chat-tender-badger.md):

### 4.1 F1.1 (commit 4aefb0b → 46ccb6d)

| ID | 体验设计 | 实现位置 | 上游有? |
|---|---|---|---|
| T0 (real fix) | RTC sanity check at boot + write 2026-01-01 placeholder if BM8563 garbage | [main.cpp setup](../src/main.cpp), [data.h dataRtcInvalidate/ForceValid](../src/data.h) | ❌ no |
| M1.1 | Approval color shift (绿/黄/红+闪) | [main.cpp drawApproval](../src/main.cpp) | ❌ upstream is monochrome HOT after 10s |
| M1.1 | Tick chime when waited ≥20s (1/sec) | [main.cpp loop after inPrompt](../src/main.cpp) | ❌ no |
| M1.1 | ALWAYS trigger 1.5s P_HEART on approve (was: only <5s) | [main.cpp BtnA approve path](../src/main.cpp) | ⚠️ upstream conditional |
| M2.1 | Completion tip (sessionsRunning N>0→0 single chime) | [main.cpp loop](../src/main.cpp) | ❌ no |
| M2 (dropped) | BUSY heartbeat — removed F1.1 per user "annoying like HDD click" | — | — |
| M4.1 | Welcome back chime + drawWelcome overlay (>10 min idle then any button) | [main.cpp wake() + drawWelcome](../src/main.cpp) | ❌ no |
| M8.1 ★ | Fireworks Easter egg (BtnA+B 同按 ≥2s) + design credit panel | [main.cpp loop M8.1 block + render block](../src/main.cpp) | ❌ no (★ user-requested) |

### 4.2 F1.2-F1.6 (commits 3c6f8d1 → c1cb65d)

| 范围 | 实现 | 上游有? |
|---|---|---|
| **M7 i18n EN/CN toggle** (F1.4) | `T(en, cn)` macro + per-call `cjkPrint`/`cjkDrawString` + `utf8SafeBytes` + Settings.lang NVS field | ❌ upstream English-only |
| **CJK font efontCN_10** (F1.4) | `setFont(&fonts::efontCN_10)` per call, GB18030 subset | ❌ no font loaded |
| **i18n menu/settings/reset items** (F1.5) | parallel EN/CN arrays + lang-aware item() helpers | ❌ no |
| **drawSettings/Menu/Reset Chinese layout** (F1.6) | scope efontCN to overlay + widen panel mw 118→132 | ❌ no |
| **About/Help/Credits i18n** (F1.6) | full Chinese translation of about + buttons + credits pages | ❌ no |
| **CREDITS update** (F1.6) | hardware M5StickS3 + ESP32-S3 + AXP2101 (was Plus 1+ESP32+AXP192) + add eashe credit | ❌ no |
| **drawHUD LH 8→10** (F1.3) | match efontCN ASCII width (when used) | ❌ no |
| **applyDisplayMode for all dismiss** (F1.3) | settings/menu/reset close clears full sprite (was characterInvalidate only y=0~82) | ⚠️ upstream same bug, doesn't surface in upstream's deploy mode |

### 4.3 Diagnostic infrastructure [CLAWSTICK]

| 项 | 用途 | 文件 |
|---|---|---|
| 1Hz Serial `[diag]` line | base/active state, buddyMode, loaded, screenOff, napping, connected, onUsb, promptId, heap | [main.cpp loop end](../src/main.cpp) |
| 1Hz `[hb]` heartbeat (history, removed per dd7af35) | proven loop alive during G25 hang | — |
| LCD-direct fillRect bisect rects (history, removed per F1.1) | found G25 hang point in 5 stages | — |

---

## 5. Reverse PR candidates → upstream

If anthropics/claude-desktop-buddy ever wants S3 support OR finds these helpful:

- **G15 wire protocol shape** — already verbatim from upstream, no PR needed (we matched)
- **G25 data.h: skip Serial.feed on USB-CDC builds** — if upstream supports S3 someday, this is needed; can guard with `#ifdef ARDUINO_USB_CDC_ON_BOOT`
- **G19 ledIsSafe() gate** — generic protection: helpful for any board where GPIO10 is critical
- **G26 setVolume(128) after Speaker.begin** — applies to any M5Unified-based port
- **G21 reverted** — upstream's latent bug doesn't surface in their deploy mode (Plus 1 + always-on-USB), so no PR necessary
- **i18n + CJK font** — out of upstream scope (project is monolingual)

---

## 6. File-level provenance summary

| File | Origin |
|---|---|
| [main.cpp](../src/main.cpp) | upstream skeleton + ~30% clawstick additions (port + bug fixes + F1.1-F1.6) |
| [character.cpp](../src/character.cpp) | upstream + Fix A (G24 GIF probe) + Fix A2 (gif.open self-heal) |
| [character.h](../src/character.h) | upstream verbatim |
| [buddy.cpp](../src/buddy.cpp) | upstream + sed swap (M5StickCPlus→M5Unified, TFT_eSPI→LovyanGFX, TFT_eSprite→M5Canvas) |
| [buddy.h](../src/buddy.h) | upstream verbatim |
| [buddy_common.h](../src/buddy_common.h) | upstream verbatim |
| [buddies/*.cpp](../src/buddies/) | upstream verbatim (18 species, all hardcoded ASCII art) |
| [data.h](../src/data.h) | upstream + G25 USB-CDC skip + dataRtcInvalidate/ForceValid (T0) |
| [stats.h](../src/stats.h) | upstream + Settings.lang field (F1.4) + persistence |
| [ble_bridge.{h,cpp}](../src/ble_bridge.cpp) | BlueDroid: verbatim from upstream BlueDroid version (after G14 evaluation) |
| [xfer.h](../src/xfer.h) | upstream verbatim |

---

## 7. 协作建议 (future maintainers)

1. **修 bug 前先看 §3 G15-G27** — 大概率已被 audit 过
2. **改 wire protocol 字段** → STOP, verbatim from upstream (G15 教训)
3. **改 audio / visual 设计** → 看 §4 是否 clawstick-original (改即可) 或 §1 upstream (考虑 reverse PR or fork rationale)
4. **加新硬件 port adaptation** → §2 (照该模式: 单独 file 隔离 + gotcha 记录)
5. **F2/F3 plan extensions** → 见 [plan v2 §5](../../../../../.claude/plans/per-project-chat-tender-badger.md)

## 8. 修订历史

| 版本 | 日期 | 改动 |
|---|---|---|
| v1.0 | 2026-05-06 | 创建 — F1.6 (commit c1cb65d) 后 ground truth snapshot, per user request "记录哪些是你设计的，哪些是引用的" |
