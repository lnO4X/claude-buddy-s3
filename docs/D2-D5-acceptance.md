# D2-D5 Acceptance Protocol & Test Journey

> Phase D 协议层验收文档 + 实际测试过程的 retrospective。
> Future D 阶段 / 回归测试可直接 follow。
> 引用 per [P19](../../../../principles/principles.md)；脚本 PASS per [P5/P18](../../../../principles/principles.md)。

---

## 1. 验收方法论

PLAN §3 D2-D6 的 acceptance 全部依赖 BLE pair + GATT 交互。**实际**用过的工具按可信度：

| 工具 | 用途 | 评价 |
|---|---|---|
| **nRF Connect for Mobile** (Nordic Semi, free) | 主验证 — raw BLE scan + GATT 读写 + 配对 | ★★★★★ 最可靠；不经任何应用层 filter |
| Claude Desktop Hardware Buddy menu | 真实集成验证（D6 入口） | ★★★★ 最终 acceptance；但有 device-name filter（见 G12）|
| macOS LightBlue Explorer (App Store) | desktop alt to nRF Connect | 未测；候选 |
| macOS Settings → Bluetooth → Nearby Devices | （error-prone） | ★ 不可靠；过滤 buddy-class peripheral，"看不到" ≠ "未广播"（详 G8）|
| Python `bleak` | 自动化脚本 | ★ macOS 上 subprocess 不继承 Claude.app TCC 权限（详 G8） |

**正确顺序**：
1. firmware Serial 输出 `[ble] advertising as 'X'` = 必要不充分（只证 bleInit 调用通过，不证电波出去）
2. **nRF Connect mobile** 看到设备 + service UUIDs 正确 = firmware 全 OK
3. nRF Connect 配对成功（passkey 输入对）= 加密层 OK
4. nRF Connect 写 JSON 到 RX char + 收到屏反应 = D3 wire protocol 通
5. 按板上 button + nRF Connect 收到 TX notification = D4 协议出栈通
6. 摇板 + 屏变 DIZZY = D5 IMU 通
7. **最后**：Claude Desktop Hardware Buddy 实际配对 = D6 集成通（最终 acceptance）

---

## 2. D2-D5 acceptance — 实测 verified 2026-05-03 evening

### D2 BLE NUS advertise + connect + encrypted pairing — ✅ PASS

**验证步骤**：
1. nRF Connect mobile → Scan → 设备列表
   - **预期 / 实测**：见 `Claude-XXXX` (XXXX = BT MAC 末 2 字节，hex)，Services: `Nordic UART Service`，RSSI ~-40 dBm 强信号
   - **首版错误**：原 firmware advertise 名 `clawstick` —— Hardware Buddy filter 看不到（G12）；nRF Connect 能看到（不过滤）
2. 点 Connect → 应弹 LE Secure Connections passkey 配对对话框
   - **预期**：屏底栏出现红色大字 `PIN: XXXXXX` (6 位) → 在 nRF Connect 输入 → Pair OK
   - **实测**：屏 PIN 显示 OK，输入正确后配对成功，屏底栏变 "paired (encrypted)" 深绿
3. 屏 BLE 状态行从黄色 `BLE: advertising` → 绿色 `BLE: connected`

### D3 wire protocol JSON parse — ✅ PASS

**验证步骤**：
1. nRF Connect 找 Nordic UART Service → 展开
2. **UART TX Characteristic** (`6e400003-...`)：点行右 ↓ 按钮 → subscribe to notifications（icon 变蓝）
3. **UART RX Characteristic** (`6e400002-...`)：点行右 ↑ 按钮 → Write Value 框
4. 选 ByteArray 模式，输入 hex（57 bytes）：
   ```
   7B2270726F6D7074223A7B226964223A227465737431222C22746F6F6C223A2262617368222C2268696E74223A22726D2074657374227D7D0A
   ```
   = `{"prompt":{"id":"test1","tool":"bash","hint":"rm test"}}\n`（**末尾 `0A` = `\n` 必须有**，line-buffered parser）
5. Write Type = Command（无 ack 更快），点 Write
6. **预期 / 实测**：屏在 ~100ms 内
   - state 行从灰 `sleep` → 黄 `attention`
   - msg 行显示 `rm test`
   - 底栏出现黄字 `A=approve  B=reject`

### D4 buttons → JSON TX — ✅ PASS

**验证步骤**：
1. 紧接 D3 状态（屏 attention，prompt active）
2. 按板上前面大 BtnA
3. nRF Connect TX char Value 字段或 Log tab 应自动 update
4. **预期 / 实测**（updated 2026-05-03 late evening per G15 fix — 上游 verbatim shape）：收到 notification:
   ```
   {"cmd":"permission","id":"test1","decision":"once"}
   ```
   （末尾还有 `\n`）
5. 按 BtnB 同理 → `"decision":"deny"`

**警告：D2-D5 first ship (commit c5c442e) 用 `{"approval":{"id":...,"action":"approve|reject"}}` 是猜的，与上游 Claude Desktop 不兼容（G15 D6 失败根因，2026-05-03 late evening 修，commit 6534487）。如果你 verify 旧固件 binary 看到旧 shape，必须重 flash 新 firmware。**

### D5 IMU shake → DIZZY — ✅ PASS（incidentally verified）

**验证步骤（标准）**：
1. 物理震动设备（摇）
2. **预期**：屏 state 行变紫 `dizzy` 5 秒，然后回 base state

**实测惊喜（2026-05-03 evening）**：用户尝试短按 PWR 软重启时，按动作本身的物理震动触发 IMU 阈值（>2g），DIZZY 状态出现 5 秒。意外验证 D5 通了 + 提示 IMU 阈值偏低（PWR 按可能误触发；可考虑调到 2.5g 或加 button-press debounce）—— 见 G7。

---

## 3. 关键 gotchas 出现顺序（learning 时间线）

| # | Gotcha | 发现时机 | 解决 |
|---|---|---|---|
| G1 | PIO upload S3 native USB DTR/RTS 失败 | C5 第一次 flash | 加 `--before=usb_reset` |
| G2 | Firmware running 后 USB-CDC 占用，esptool 卡 | C5 重 flash | 手动 ROM bootloader |
| G3 | M5StickS3 ROM bootloader = BtnA + PWR + 松 | C5 第二次 flash | 屏黑 = 进 ROM，不是关机 |
| G7 | PWR 短按触发 IMU shake → DIZZY | D2-D5 monitor 测试 | accidental D5 verification |
| G8 | macOS Settings BLE 列表不可靠 | NimBLE D2 调试初期 | 改用 nRF Connect |
| G11 | NimBLE/BlueDroid 共存禁止 | BlueDroid 切换 | platformio.ini 删 NimBLE lib_dep |
| **G12** | **Hardware Buddy filter 要求 BLE name 前缀 `Claude-`** | D2 nRF Connect 看到但 Hardware Buddy 看不到 | firmware 改 advertise 名 `Claude-XXXX` 模式 |

---

## 4. nRF Connect 测试链替代脚本（自动化未来）

D 阶段后续 / 回归测试要复用，建议脚本化：

```python
# 候选未来：projects/clawstick/code/scripts/test-d2-d5.py
# 用 bleak 自动化 D2-D5 GATT 交互。
# 阻塞：macOS TCC 权限（G8） — Linux 上跑就没问题
# 流程：
#   1. scan for "Claude-*" 设备
#   2. connect + pair (passkey 仍需人输入；可用 IO_CAP_DISPLAY_YESNO 跳过)
#   3. subscribe TX notify
#   4. write 一系列 JSON 到 RX，断言屏（OCR? camera?）状态变化
#   5. 模拟按 BtnA (BLE control char? GPIO?) — 需要 firmware 加 test hook
#   6. 断言 TX notification 内容
#   7. 摇 IMU — 暂无办法远程模拟
```

不在 v0.1 范围。Phase F CI 时考虑。

---

## 5. 给未来 D 阶段 / D6 / 调试者的 checklist

新 firmware build 后：
- [ ] firmware 编译过 (`pio run -e clawstick-s3`)
- [ ] flash 成功 (`pio run -e clawstick-s3 -t upload` —— 失败先 G3 手动 ROM)
- [ ] Serial monitor 抓 boot 输出，确认 `[ble] BT MAC ... → name 'Claude-XXXX'` + `[ble] advertising as 'Claude-XXXX'` 都打印
- [ ] nRF Connect mobile scan，确认 device + NUS service + 信号强度合理
- [ ] nRF Connect Connect → passkey 配对成功
- [ ] D3 hex JSON write to RX → 屏 ATTENTION + 文字
- [ ] BtnA → TX notification with `{"cmd":"permission","id":"<id>","decision":"once"}` (G15 — verbatim upstream shape, NOT the early-ship `{"approval":...,"action":...}` guess)
- [ ] 摇板 → DIZZY
- [ ] **Claude Desktop Hardware Buddy menu** → 看到 `Claude-XXXX` → Pair → 配对 → 实测 dangerous prompt 触发 buddy

每步任一失败 → 停下 debug 不继续，避免雪球
