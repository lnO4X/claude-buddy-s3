#include <M5Unified.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

M5Canvas spr = M5Canvas(&M5.Lcd);

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = 135, H = 240;
const int CX = W / 2;
const int CY_BASE = 120;
// Upstream M5StickC Plus has its red LED on GPIO10. M5StickS3 maps GPIO10 to
// EX_SDA (Hat I2C SDA) per M5Unified board init + research/hardware-delta.md
// L40 — StickS3 LED is wired through the audio codec/I2C expander, NOT a
// standalone GPIO. Driving GPIO10 here would corrupt the Hat I2C bus.
// Per arbitration Opus review 2026-05-04: gate all LED ops at runtime via
// M5.getBoard(). LED feature dead on S3 until E-phase wires up codec LED.
// Per gotcha G19 (added below).
static const int LED_PIN = 10;
static inline bool ledIsSafe() {
    return M5.getBoard() != m5::board_t::board_M5StickS3;
}

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

// M7 v2 i18n + per-call CJK font helpers (F1.4 redesign).
// Default font = Font0 5×7 ASCII (preserves all upstream layouts incl species
// art BUDDY_CHAR_H=8). When string contains UTF-8 (any byte > 0x7F), helpers
// temporarily setFont(efontCN_12), draw, then restore Font0. Chinese renders;
// ASCII paths untouched.
static inline bool hasUtf8(const char* s) {
  for (const uint8_t* p = (const uint8_t*)s; *p; p++) if (*p > 0x7F) return true;
  return false;
}
static inline const char* T(const char* en, const char* cn) {
  return settings().lang ? cn : en;
}
// F1.8.2 fix: save+restore actual prior font (not assume Font0). Previous
// behavior broke drawInfo's scoped efontCN_12 — when _infoHeader called
// cjkPrint("信息"), inner cjkPrint restored Font0, then outer drawInfo
// continued ln() lines in Font0 → Chinese bytes had no glyph → only ASCII
// fragments visible. Bug reported by user 2026-05-06 "信息页中文 只剩几个字母".
static void cjkPrint(M5Canvas& s, const char* str) {
  bool u = hasUtf8(str);
  const lgfx::IFont* prior = s.getFont();
  if (u) s.setFont(&fonts::efontCN_12);
  s.print(str);
  if (u) s.setFont(prior);
}
static void cjkDrawString(M5Canvas& s, const char* str, int32_t x, int32_t y) {
  bool u = hasUtf8(str);
  const lgfx::IFont* prior = s.getFont();
  if (u) s.setFont(&fonts::efontCN_12);
  s.drawString(str, x, y);
  if (u) s.setFont(prior);
}

// UTF-8 safe truncation: scan bytes, never split a multi-byte sequence.
// Returns max bytes ≤ maxBytes ending on a complete UTF-8 char boundary.
// Fixes drawApproval %.21s mid-byte cut → garbage display for Chinese hint.
static int utf8SafeBytes(const char* s, int maxBytes) {
  int i = 0;
  while (i < maxBytes && s[i]) {
    uint8_t b = (uint8_t)s[i];
    int n = 1;
    if      ((b & 0xE0) == 0xC0) n = 2;
    else if ((b & 0xF0) == 0xE0) n = 3;
    else if ((b & 0xF8) == 0xF0) n = 4;
    if (i + n > maxBytes) break;
    i += n;
  }
  return i;
}

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
// F1.7 (UX redesign per user 2026-05-06): info pages may overflow display
// after CN translation (efontCN_12 chars taller). BtnA in DISP_INFO scrolls
// down within current page; if "scrolled past content", reset + advance to
// next mode. infoLastLineY tracked during render to detect end-of-content.
// Architectural note: this differs from upstream (BtnA = next mode直接, no
// scroll concept). UPSTREAM-DELTA.md §4 logs this clawstick-original UX.
int     infoScroll      = 0;
int     infoLastLineY   = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
uint32_t welcomeUntil  = 0;
const uint32_t LONG_IDLE_MS = 10UL * 60UL * 1000UL;  // M4.1: 10 min
uint32_t fireworksUntil = 0;                          // M8.1 Easter egg
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
// G21: upstream MPU6886 (Plus 1) and our BMI270 (StickS3) may have inverted
// Z-axis mounting on the PCB. If StickS3's BMI270 reports az ≈ -1g when the
// screen faces UP (instead of upstream's +1g convention), this function
// would return true CONTINUOUSLY → napping flag latches → loop skips sprite
// render → splash sticks forever. Symptom user reported 2026-05-04: "splash
// 几分钟没动".
//
// Until we verify BMI270 axis convention against schematic, gate the entire
// face-down nap feature off on StickS3. Loss: no auto-dim when device flipped
// face-down. Gain: sprite render path always runs → splash clears, normal
// UI visible. Re-enable in Phase F polish after axis verification.
static bool isFaceDown() {
  if (M5.getBoard() == m5::board_t::board_M5StickS3) return false;
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { M5.Display.setBrightness((20 + brightLevel * 20) * 255 / 100); }

static void wake() {
  uint32_t now = millis();
  // M4.1 (F1): Welcome back ritual — long idle (>10 min) then any user
  // interaction → 2s welcome overlay + chime. Skip on boot (lastInteractMs=0)
  // and on quick re-presses (idleMs ≤ LONG_IDLE_MS). Per plan: 1s melody;
  // simplified to single 600ms chime to avoid blocking delay between tones
  // (M5Unified tone() is async but multi-tone sequencing requires state
  // machine — F2 polish if user wants 3-note melody).
  if (lastInteractMs > 0 && (now - lastInteractMs) > LONG_IDLE_MS) {
    welcomeUntil = now + 2000;
    if (settings().sound) M5.Speaker.tone(2200, 600);
  }
  lastInteractMs = now;
  if (screenOff) {
    M5.Display.wakeup();
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Speaker.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

// F1.5: i18n menu items — parallel arrays, T() picks by lang
const char* menuItemsEN[] = { "settings", "turn off", "help", "about", "demo", "close" };
const char* menuItemsCN[] = { "设置", "关机", "帮助", "关于", "演示", "关闭" };
inline const char* menuItem(uint8_t i) { return settings().lang ? menuItemsCN[i] : menuItemsEN[i]; }
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
// F1.4: added "language" entry for M7 EN/CN toggle. SETTINGS_N bumped 10→11,
// applySetting case 10 reused for "back" (was 9), case 9 = language toggle.
// F1.5: i18n with parallel CN array.
const char* settingsItemsEN[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "clock rot", "ascii pet", "reset", "language", "back" };
const char* settingsItemsCN[] = { "亮度", "声音", "蓝牙", "WiFi", "LED", "字幕条", "时钟旋转", "ASCII 角色", "重置", "语言", "返回" };
inline const char* settingsItem(uint8_t i) { return settings().lang ? settingsItemsCN[i] : settingsItemsEN[i]; }
const uint8_t SETTINGS_N = 11;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItemsEN[] = { "delete char", "factory reset", "back" };
const char* resetItemsCN[] = { "删除角色", "恢复出厂", "返回" };
inline const char* resetItem(uint8_t i) { return settings().lang ? resetItemsCN[i] : resetItemsEN[i]; }
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7: nextPet(); return;
    case 8: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 9: s.lang = (s.lang + 1) % 2; break;   // F1.4: language toggle EN ↔ CN
    case 10: settingsOpen = false; applyDisplayMode(); return;  // back (was case 9)
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  // F1.6: widened panel mw 118→132 (almost full 135) + scoped font efontCN_12
  // when lang=CN. Mixed-font per-line caused baseline misalignment + label
  // overflow into indicator column. Unified font in CN mode = consistent
  // baseline + room for Chinese glyphs (10×10 vs Font0 5×7).
  int mw = 132, mh = 16 + SETTINGS_N * 16 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool useCJK = (s.lang == 1);
  if (useCJK) spr.setFont(&fonts::efontCN_12);
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 16);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItem(i));   // font already set above
    spr.setCursor(mx + mw - 38, my + 8 + i * 16);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? T(" on", " 开") : T("off", "关"));
    } else if (i == 6) {
      static const char* const RNen[] = { "auto", "port", "land" };
      static const char* const RNcn[] = { "自动", "竖屏", "横屏" };
      spr.print(useCJK ? RNcn[s.clockRot] : RNen[s.clockRot]);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    } else if (i == 9) {
      // Language indicator — efontCN already set in CJK mode; "中" needs it
      // even in EN mode for char to render. Use cjkPrint as fallback safety.
      if (useCJK) spr.print(s.lang ? "中" : "EN");
      else cjkPrint(spr, s.lang ? "中" : "EN");
    }
  }
  if (useCJK) drawMenuHints(p, mx, mw, my + mh - 12, T("Next", "下"), T("Change", "改"));
  else        drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
  if (useCJK) spr.setFont(&fonts::Font0);   // restore default
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 132, mh = 16 + RESET_N * 16 + MENU_HINT_H;   // F1.6 wider
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  bool useCJK = (settings().lang == 1);
  if (useCJK) spr.setFont(&fonts::efontCN_12);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 16);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? T("really?", "确定?") : resetItem(i));
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
  if (useCJK) spr.setFont(&fonts::Font0);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: M5.Power.powerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 132, mh = 16 + MENU_N * 16 + MENU_HINT_H;   // F1.6 wider
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  bool useCJK = (settings().lang == 1);
  if (useCJK) spr.setFont(&fonts::efontCN_12);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 16);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItem(i));
    if (i == 4) spr.print(dataDemo() ? T("  on", " 开") : T("  off", "关"));
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
  if (useCJK) spr.setFont(&fonts::Font0);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static m5::rtc_time_t _clkTm;          // M5Unified RTC types (was RTC_TimeTypeDef)
static m5::rtc_date_t _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = (M5.Power.getVBUSVoltage() / 1000.0f) > 4.0f;
  m5::rtc_datetime_t dt;
  M5.Rtc.getDateTime(&dt);

  // T0: sanity check — if BM8563 returns out-of-range fields (uninit RTC /
  // setDateTime silently failed on M5StickS3), invalidate so drawClock path
  // skips and doesn't print garbage like "42949 / :49 / jan 429" (per user
  // 2026-05-04 night observation post-D6). Dirty values can survive even
  // after Hardware Buddy sent {"time":[...]} JSON if BM8563 I2C write didn't
  // commit — defensive read-side validation.
  bool sane = dt.time.hours <= 23
           && dt.time.minutes <= 59
           && dt.time.seconds <= 59
           && dt.date.month >= 1 && dt.date.month <= 12
           && dt.date.date  >= 1 && dt.date.date  <= 31
           && dt.date.year  >= 2024 && dt.date.year <= 2099;
  if (!sane) { dataRtcInvalidate(); return; }

  _clkTm = dt.time;
  _clkDt = dt.date;
}

static void clockUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.weekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.hours, _clkTm.minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.seconds);
  uint8_t mi = (_clkDt.month >= 1 && _clkDt.month <= 12) ? _clkDt.month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.date);

  if (clockOrient == 0) {
    paintedOrient = 0;
    // Bottom half — buddy naturally lives at y=0..82, GIF peeks at top
    // via peek mode. Clearing from 90 leaves both untouched.
    spr.fillRect(0, 90, W, H - 90, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 140);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 175);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 200);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Landscape: 240×135 direct-to-LCD. Full fill only on entry; after that
  // text glyph bg cells repaint themselves and the pet box (small, ~90×50)
  // gets a fillRect each pet tick — small enough not to tear.
  M5.Lcd.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Lcd.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  // Seconds tick at 1Hz; redrawing 3 strings at 60fps is 180 SPI ops/sec
  // for nothing. Gate on the second changing (or full repaint).
  if (repaint || _clkTm.seconds != lastSec) {
    lastSec = _clkTm.seconds;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.date);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.seconds);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(p.text, p.bg);    M5.Lcd.drawString(hm, 170, 42);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(p.textDim, p.bg); M5.Lcd.drawString(ssl, 170, 72);
                                                                  M5.Lcd.drawString(wdl, 170, 102);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
  }

  // Pet on left at 5 fps. Clear includes the overlay-particle zone above
  // the body (y<30) — species draw Zzz/hearts there via BUDDY_Y_OVERLAY=6
  // which doesn't go through _yb, so the box has to cover it.
  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    if (buddyMode) {
      // ASCII glyphs don't self-clear; wipe the box each tick. Species
      // hardcode BUDDY_X_CENTER=67 / BUDDY_Y_OVERLAY=6 for particles so
      // keep portrait coords and just swap the surface — pet lands
      // upper-left of landscape, which is where we want it anyway.
      M5.Lcd.fillRect(0, 0, 115, 90, p.bg);
      buddyRenderTo(&M5.Lcd, activeState);
    } else {
      // Full-frame GIFs paint every pixel (transparent → pal.bg), so a
      // per-tick clear just adds a visible black flash between wipe and
      // last scanline. The entry fillScreen on paintedOrient change
      // already covers the surround.
      characterSetState(activeState);
      characterRenderTo(&M5.Lcd, 57, 45);
    }
  }
  M5.Lcd.setRotation(0);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); cjkPrint(spr, T("Info", "信息"));
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); cjkPrint(spr, section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  // F1.7 i18n: drawPasskey labels translated. Use cjkPrint for CN font swap.
  spr.setCursor(8, 56);  cjkPrint(spr, T("BLUETOOTH PAIRING", "蓝牙配对"));
  spr.setCursor(8, 184); cjkPrint(spr, T("enter on desktop:", "在电脑端输入:"));
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  // F1.6: scope efontCN to whole info page when lang=CN — unified baseline
  // for Chinese paragraph text (avoid mixed-font drift in long blocks).
  bool useCJK = (settings().lang == 1);
  if (useCJK) spr.setFont(&fonts::efontCN_12);
  int LH = useCJK ? 14 : 8;       // F1.7: efontCN_12 needs 14 row spacing
  int y = TOP + 2 - infoScroll * LH;   // F1.7: scroll offset (negative when scrolled)
  // F1.7 scroll-aware ln: only paint if line within visible band; track lastY
  auto ln = [&](const char* fmt, ...) {
    char b[64]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    if (y >= TOP - LH && y < H - 2) { spr.setCursor(4, y); spr.print(b); }
    y += LH;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, useCJK ? "关于" : "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    if (useCJK) {
      ln("我看着你的 Claude");
      ln("桌面会话。");
      y += 4;
      ln("没事时打盹，");
      ln("你开工就醒来，");
      ln("积压批准时焦躁。");
      y += 4;
      spr.setTextColor(p.text, p.bg);
      ln("看到提示按 A 批准。");
      y += 4;
      spr.setTextColor(p.textDim, p.bg);
      ln("18 种角色。设置→");
      ln("ASCII 角色 切换。");
    } else {
      ln("I watch your Claude");
      ln("desktop sessions.");
      y += 6;
      ln("I sleep when nothing's");
      ln("happening, wake when");
      ln("you start working,");
      ln("get impatient when");
      ln("approvals pile up.");
      y += 6;
      spr.setTextColor(p.text, p.bg);
      ln("Press A on a prompt");
      ln("to approve from here.");
      y += 6;
      spr.setTextColor(p.textDim, p.bg);
      ln("18 species. Settings");
      ln("> ascii pet to cycle.");
    }

  } else if (infoPage == 1) {
    _infoHeader(p, y, useCJK ? "按键" : "BUTTONS", infoPage);
    if (useCJK) {
      spr.setTextColor(p.text, p.bg);    ln("A   前面");
      spr.setTextColor(p.textDim, p.bg); ln("    下一屏");
      ln("    批准提示"); y += 4;
      spr.setTextColor(p.text, p.bg);    ln("B   右侧");
      spr.setTextColor(p.textDim, p.bg); ln("    下一页");
      ln("    拒绝提示"); y += 4;
      spr.setTextColor(p.text, p.bg);    ln("长按 A");
      spr.setTextColor(p.textDim, p.bg); ln("    菜单"); y += 4;
      spr.setTextColor(p.text, p.bg);    ln("电源  左侧");
      spr.setTextColor(p.textDim, p.bg); ln("    单按=重启");
      ln("    双按=关机");
      ln("    长按=下载模式");
    } else {
      spr.setTextColor(p.text, p.bg);    ln("A   front");
      spr.setTextColor(p.textDim, p.bg); ln("    next screen");
      ln("    approve prompt"); y += 4;
      spr.setTextColor(p.text, p.bg);    ln("B   right side");
      spr.setTextColor(p.textDim, p.bg); ln("    next page");
      ln("    deny prompt"); y += 4;
      spr.setTextColor(p.text, p.bg);    ln("hold A");
      spr.setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
      // F1.14: M5StickS3 PWR behavior 跟 Plus 1 不同 (per refs.md §C.1):
      // single press = restart (M5PM1 hardware reset, firmware 收不到)
      // double press = power off
      // long press = download mode (ROM bootloader)
      spr.setTextColor(p.text, p.bg);    ln("Power  left side");
      spr.setTextColor(p.textDim, p.bg); ln("    tap = restart");
      ln("    double-tap = off");
      ln("    long = download");
    }

  } else if (infoPage == 2) {
    _infoHeader(p, y, useCJK ? "Claude 状态" : "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln(useCJK ? "  会话总数  %u"  : "  sessions  %u", tama.sessionsTotal);
    ln(useCJK ? "  运行中    %u"  : "  running   %u", tama.sessionsRunning);
    ln(useCJK ? "  等待中    %u"  : "  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln(useCJK ? "连接" : "LINK");
    spr.setTextColor(p.textDim, p.bg);
    // F1.8: translate dataScenarioName + ble status values + state name
    const char* via = dataScenarioName();
    const char* viaT = (!strcmp(via,"bt"))?"蓝牙":(!strcmp(via,"usb"))?"USB":(!strcmp(via,"none"))?"无":via;
    ln(useCJK ? "  通道      %s" : "  via       %s", useCJK ? viaT : via);
    const char* bleS = !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN";
    const char* bleT = !bleConnected() ? "-" : bleSecure() ? "已加密" : "未加密";
    ln(useCJK ? "  蓝牙      %s" : "  ble       %s", useCJK ? bleT : bleS);
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln(useCJK ? "  最近消息  %lus" : "  last msg  %lus", (unsigned long)age);
    static const char* stateNamesCN[] = { "睡眠", "空闲", "忙碌", "注意", "庆祝", "晕眩", "爱心" };
    ln(useCJK ? "  状态      %s"  : "  state     %s", useCJK ? stateNamesCN[activeState] : stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, useCJK ? "设备" : "DEVICE", infoPage);

    // F1.9.1 (per user 2026-05-06 "百分比和电压一直在动"): throttle reads to
    // 1 Hz + EWMA smoothing to stop jumping display. Each Power getter is an
    // instantaneous ADC read affected by LCD/BLE/CPU load. EWMA filters
    // micro-fluctuations.
    //
    // Caveat (also user-reported): USB-plugged shows 27-30% not 100% —
    // M5StickS3 newer batches use M5PM1 (not AXP2101) per references.md §C.3.
    // M5Unified Power_Class.cpp:1900-1907 reads M5PM1 register 0x26/0x27
    // which is "5VOUT" (5V rail output) NOT battery voltage. The displayed
    // percentage is NOT real battery state on M5PM1 boards. Real fix needs
    // M5PM1 datasheet to find correct battery-voltage register; F2 polish.
    static int    vBatSmooth = 0, iBatSmooth = 0, vBusSmooth = 0;
    static uint32_t lastReadMs = 0;
    if (millis() - lastReadMs > 1000) {
      lastReadMs = millis();
      int vBatRaw = (int)M5.Power.getBatteryVoltage();
      int iBatRaw = (int)M5.Power.getBatteryCurrent();
      int vBusRaw = (int)M5.Power.getVBUSVoltage();
      if (vBatSmooth == 0) { vBatSmooth = vBatRaw; iBatSmooth = iBatRaw; vBusSmooth = vBusRaw; }
      else {
        // EWMA α=0.3 — heavy smoothing, slow to react but stable display
        vBatSmooth = (vBatSmooth * 7 + vBatRaw * 3) / 10;
        iBatSmooth = (iBatSmooth * 7 + iBatRaw * 3) / 10;
        vBusSmooth = (vBusSmooth * 7 + vBusRaw * 3) / 10;
      }
    }
    int vBat_mV = vBatSmooth;
    int iBat_mA = iBatSmooth;
    int vBus_mV = vBusSmooth;
    // F1.15: piecewise Li-Po SoC curve replaces linear (vBat-3200)/10 formula
    // (per user 2026-05-07 "用完插上 12% 不准, UiFlow 准"). Linear mapping was
    // off ~10-20% in low-V region where Li-Po discharge curve is steeper.
    // Industry-standard Li-Po (3.0V cutoff, 4.2V full) approximation:
    int pct;
    if      (vBat_mV >= 4180) pct = 100;
    else if (vBat_mV >= 4000) pct = 80 + (vBat_mV - 4000) * 20 / 180;   // 80-100
    else if (vBat_mV >= 3850) pct = 60 + (vBat_mV - 3850) * 20 / 150;   // 60-80
    else if (vBat_mV >= 3700) pct = 30 + (vBat_mV - 3700) * 30 / 150;   // 30-60
    else if (vBat_mV >= 3500) pct = 10 + (vBat_mV - 3500) * 20 / 200;   // 10-30
    else if (vBat_mV >= 3300) pct = 2  + (vBat_mV - 3300) *  8 / 200;   // 2-10
    else if (vBat_mV >= 3000) pct = 0  + (vBat_mV - 3000) *  2 / 300;   // 0-2
    else                      pct = 0;
    if (pct > 100) pct = 100;
    if (pct < 0)   pct = 0;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    // F1.9.3 (per user 2026-05-06 "解决问题而不是隐藏 电池是有效信息"):
    // restored real % display. Root fix moved to setup() — calling
    // M5.Power.setBatteryCharge(true) enables M5PM1 register 0x06 bit 0
    // which M5Unified doesn't auto-enable at init for board_M5StickS3.
    // Now USB plugged actually charges battery → vBat rises → real %.
    // F1.15: 显原始 vBat 在 % 旁边 (per user "电池是有效信息, 不要隐藏")
    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    // Raw vBat right of percentage, smaller font for "ground truth" reference
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(60, y);
    spr.printf("%d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    // Charge status one line below
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(60, y + 10);
    spr.print(full ? T("full","满") : (charging ? T("charging","充电中") : (usb ? T("usb","USB") : T("battery","电池"))));
    y += 22;

    spr.setTextColor(p.textDim, p.bg);
    ln(useCJK ? "  电池电压 %d.%02dV" : "  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln(useCJK ? "  电流     %+dmA"   : "  current  %+dmA", iBat_mA);
    if (usb) ln(useCJK ? "  USB 电压 %d.%02dV" : "  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;

    // F1.10 on-screen PMIC diagnostic — user observed weird battery readings;
    // grounded confirm M5PM1 vs AXP2101 + raw register state
    spr.setTextColor(p.text, p.bg);
    ln(useCJK ? "PMIC 诊断" : "PMIC RAW");
    spr.setTextColor(p.textDim, p.bg);
    {
      static bool m5pm1_ack = false, axp2101_ack = false;
      static uint8_t reg06 = 0;
      static uint16_t rawVBat = 0, rawVBus = 0, raw5V = 0;
      static uint32_t pmicLastRead = 0;
      if (millis() - pmicLastRead > 2000) {
        pmicLastRead = millis();
        m5pm1_ack   = M5.In_I2C.scanID(0x6E);
        axp2101_ack = M5.In_I2C.scanID(0x34);
        if (m5pm1_ack) {
          reg06 = M5.In_I2C.readRegister8(0x6E, 0x06, 100000);
          uint8_t b22[2]={0,0}, b24[2]={0,0}, b26[2]={0,0};
          M5.In_I2C.readRegister(0x6E, 0x22, b22, 2, 100000);
          M5.In_I2C.readRegister(0x6E, 0x24, b24, 2, 100000);
          M5.In_I2C.readRegister(0x6E, 0x26, b26, 2, 100000);
          rawVBat = (b22[1]<<8)|b22[0];
          rawVBus = (b24[1]<<8)|b24[0];
          raw5V   = (b26[1]<<8)|b26[0];
        }
      }
      ln(useCJK ? "  M5PM1 0x6E %s" : "  M5PM1 0x6E %s", m5pm1_ack ? "ACK" : "NO");
      ln(useCJK ? "  AXP2101 %s" : "  AXP2101 0x34 %s", axp2101_ack ? "ACK" : "NO");
      if (m5pm1_ack) {
        ln(useCJK ? "  reg06=%02X 充电=%d" : "  reg06=%02X chg=%d", reg06, reg06 & 0x01);
        ln(useCJK ? "  原始 BAT=%dmV"  : "  raw BAT %dmV", rawVBat);
        ln(useCJK ? "  原始 VBUS=%dmV" : "  raw VBUS %dmV", rawVBus);
        ln(useCJK ? "  原始 5V=%dmV"   : "  raw 5VOUT %dmV", raw5V);
      }
    }
    y += 6;

    spr.setTextColor(p.text, p.bg);
    ln(useCJK ? "系统" : "SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln(useCJK ? "  主人     %s" : "  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln(useCJK ? "  运行时长 %luh %02lum" : "  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln(useCJK ? "  堆内存   %uKB" : "  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln(useCJK ? "  亮度     %u/4" : "  bright   %u/4", brightLevel);
    const char* btS = settings().bt ? (dataBtActive() ? "linked" : "on") : "off";
    const char* btT = settings().bt ? (dataBtActive() ? "已连"   : "开") : "关";
    ln(useCJK ? "  蓝牙     %s" : "  bt       %s", useCJK ? btT : btS);
    ln(useCJK ? "  温度     %dC" : "  temp     %dC", (int)0  /* AXP192 temp not available on AXP2101 */);

  } else if (infoPage == 4) {
    _infoHeader(p, y, useCJK ? "蓝牙" : "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? T("linked","已连") : (settings().bt ? T("discover","搜索中") : T("off","关")));
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln(useCJK ? "  最近消息 %lus" : "  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln(useCJK ? "配对方法" : "TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      if (useCJK) {
        ln(" 打开 Claude 桌面");
        ln(" → 开发者");
        ln(" → Hardware Buddy");
        y += 4;
        ln(" BLE 自动连接");
      } else {
        ln(" Open Claude desktop");
        ln(" > Developer");
        ln(" > Hardware Buddy");
        y += 4;
        ln(" auto-connects via BLE");
      }
    }

  } else {
    _infoHeader(p, y, useCJK ? "致谢" : "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln(useCJK ? "原作" : "made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    ln("Anthropic");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln(useCJK ? "源" : "source");
    y += 2;
    spr.setTextColor(p.text, p.bg);
    ln("anthropics/");
    ln("claude-desktop-buddy");
    y += 8;
    // F1.6: hardware updated M5StickC Plus → M5StickS3 (this port's actual
    // platform). AXP192 (Plus 1) → AXP2101 (S3 PMIC, per references.md §C.1).
    spr.setTextColor(p.textDim, p.bg);
    ln(useCJK ? "硬件" : "hardware");
    y += 2;
    spr.setTextColor(p.text, p.bg);
    ln("M5StickS3");
    ln("ESP32-S3 + AXP2101");
    y += 8;
    // F1.6: clawstick port + experience design credit (★ user requested)
    spr.setTextColor(p.textDim, p.bg);
    ln(useCJK ? "适配 + 设计" : "port + design");
    y += 2;
    spr.setTextColor(p.text, p.bg);
    ln("eashe@qq.com");
  }
  // F1.7 scroll: track final y so BtnA handler knows if more content below
  infoLastLineY = y;
  if (useCJK) spr.setFont(&fonts::Font0);   // restore default
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(1);
  spr.setCursor(4, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  // M1.1 (F1): countdown urgency color shift — green calm → yellow noticed →
  // red urgent. ≥20s drives RED + flicker-blink for "last call" feel. Plan
  // refers to elapsed (countdown direction is from desktop's deadline; we
  // track elapsed because that's what we have locally).
  uint16_t timeColor;
  if (waited < 10) timeColor = (uint16_t)0x07E0;        // GREEN: calm window
  else if (waited < 20) timeColor = (uint16_t)0xFFE0;   // YELLOW: getting urgent
  else                  timeColor = (uint16_t)0xF800;   // RED: last call
  // Flash on >=20s — toggle visibility every ~250ms via millis bit
  if (waited >= 20 && (millis() / 250) & 1) timeColor = p.textDim;
  spr.setTextColor(timeColor, p.bg);
  { char buf[32]; snprintf(buf, sizeof(buf), T("approve? %lus", "批准? %lus"), (unsigned long)waited); cjkPrint(spr, buf); }

  // Size 2 only if it fits one line (~10 chars at 12px on 135px screen)
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  spr.setCursor(4, H - AREA + (toolLen <= 10 ? 14 : 18));
  cjkPrint(spr, tama.promptTool);
  spr.setTextSize(1);

  // Hint wraps at ~21 chars to two lines under the tool name. UTF-8 safe
  // truncation (M7 follow-up): Chinese promptHint with %.21s would cut mid
  // multibyte → garbage. utf8SafeBytes ensures cut on char boundary.
  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  int n1 = utf8SafeBytes(tama.promptHint, 21);
  spr.setCursor(4, H - AREA + 34);
  { char b1[32]; int c = (n1 < 31) ? n1 : 31; memcpy(b1, tama.promptHint, c); b1[c]=0; cjkPrint(spr, b1); }
  if (hlen > n1) {
    int n2 = utf8SafeBytes(tama.promptHint + n1, 21);
    spr.setCursor(4, H - AREA + 42);
    { char b2[32]; int c = (n2 < 31) ? n2 : 31; memcpy(b2, tama.promptHint + n1, c); b2[c]=0; cjkPrint(spr, b2); }
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    cjkPrint(spr, T("sent...", "已发送..."));
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12);
    cjkPrint(spr, T("A: approve", "A: 批准"));
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 48, H - 12);
    cjkPrint(spr, T("B: deny", "B: 拒绝"));
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  // F1.8: scope efontCN if CN — full panel uses unified font
  bool useCJK = (settings().lang == 1);
  if (useCJK) spr.setFont(&fonts::efontCN_12);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print(useCJK ? "心情" : "mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 2); spr.print(useCJK ? "饱腹" : "fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 2); spr.print(useCJK ? "能量" : "energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1); spr.printf(useCJK ? "等级 %u" : "Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf(useCJK ? "批准 %u" : "approved %u", stats().approvals);
  spr.setCursor(6, y + 10);
  spr.printf(useCJK ? "拒绝 %u" : "denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y + 20);
  spr.printf(useCJK ? "休眠 %luh%02lum" : "napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt(useCJK ? "总令牌 " : "tokens   ", stats().tokens, y + 30);
  tokFmt(useCJK ? "今日   " : "today    ", tama.tokensToday, y + 40);
  if (useCJK) spr.setFont(&fonts::Font0);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  bool useCJK = (settings().lang == 1);
  if (useCJK) spr.setFont(&fonts::efontCN_12);
  int LH = useCJK ? 12 : 9;
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += LH;
  };
  auto gap = [&]() { y += 4; };

  y += 12;  // room for the PET header drawn by drawPet()

  ln(p.body,    T("MOOD", "心情"));
  ln(p.textDim, T(" approve fast = up", " 快批准 = 上升"));
  ln(p.textDim, T(" deny lots = down",  " 多拒绝 = 下降")); gap();

  ln(p.body,    T("FED", "饱腹"));
  ln(p.textDim, T(" 50K tokens =",      " 5万令牌 ="));
  ln(p.textDim, T(" level up + confetti"," 升级 + 彩纸")); gap();

  ln(p.body,    T("ENERGY", "能量"));
  ln(p.textDim, T(" face-down to nap",  " 面朝下打盹"));
  ln(p.textDim, T(" refills to full",   " 充满恢复")); gap();

  ln(p.textDim, T("idle 30s = off",     "30秒空闲 = 熄屏"));
  ln(p.textDim, T("any button = wake",  "任意键 = 唤醒")); gap();

  ln(p.textDim, T("A: screens  B: page", "A:换屏  B:翻页"));
  ln(p.textDim, T("hold A: menu",        "长按A: 菜单"));
  if (useCJK) spr.setFont(&fonts::Font0);
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  if (ownerName()[0]) {
    { char ln[64]; snprintf(ln, sizeof(ln), "%s's %s", ownerName(), petName()); cjkPrint(spr, ln); }
  } else {
    cjkPrint(spr, petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

// M4.1 (F1): Welcome back overlay — drawn ABOVE drawHUD when active.
// Triggered by wake() after >10 min idle. 2 sec window, then auto-fades.
static void drawWelcome() {
  const Palette& p = characterPalette();
  spr.fillRect(0, H/2 - 32, W, 64, p.bg);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  if (ownerName()[0]) {
    char line[40];
    snprintf(line, sizeof(line), T("Welcome, %s", "欢迎回来 %s"), ownerName());
    cjkDrawString(spr, line, W/2, H/2 - 8);
  } else {
    cjkDrawString(spr, T("Welcome back", "欢迎回来"), W/2, H/2 - 8);
  }
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  if (dataRtcValid()) {
    char hm[8];
    snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.hours, _clkTm.minutes);
    spr.drawString(hm, W/2, H/2 + 16);
  }
  spr.setTextDatum(TL_DATUM);
}

void drawHUD() {
  // Priority order: prompt > welcome > normal HUD. Prompt ALWAYS wins because
  // approval has a 24s deadline and any 2s welcome window suppression would
  // cost user critical timing visibility (per Opus review F1.1 medium #1).
  if (tama.promptId[0]) { drawApproval(); return; }
  if (welcomeUntil && (int32_t)(millis() - welcomeUntil) < 0) { drawWelcome(); return; }
  const Palette& p = characterPalette();
  // M7 follow-up: LH bumped 8→10 to match efontCN_12 font height (was hardcoded
  // for Font0 5×7 default, caused bottom transcript line overlap reported
  // 2026-05-04 night). AREA accordingly enlarged to keep clear region in sync.
  const int SHOW = 3, LH = 10, WIDTH = 21;
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, H - LH - 2);
    // F1.8: localize fallback "No Claude connected" msg to Chinese
    if (!tama.connected && settings().lang == 1) {
      cjkPrint(spr, "未连接 Claude");
    } else {
      cjkPrint(spr, tama.msg);
    }
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static char disp[32][24];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(4, H - AREA + 2 + i * LH);
    cjkPrint(spr, disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 18, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(0);
  // T0 real-fix (post-G27): probe BM8563 RTC at boot. If returns garbage
  // (uninit / coin cell drained / I2C transient), write a 2026-01-01 placeholder
  // and force-validate. clocking path then shows a sane clock immediately;
  // real time arrives via xfer "time" cmd from Hardware Buddy and overwrites.
  // Symmetric runtime sanity in clockRefreshRtc still defensive-invalidates
  // if RTC reads garbage post-init.
  {
    m5::rtc_datetime_t dt;
    M5.Rtc.getDateTime(&dt);
    bool sane = dt.time.hours <= 23 && dt.time.minutes <= 59 && dt.time.seconds <= 59
             && dt.date.month >= 1 && dt.date.month <= 12
             && dt.date.date  >= 1 && dt.date.date  <= 31
             && dt.date.year  >= 2024 && dt.date.year <= 2099;
    if (!sane) {
      m5::rtc_datetime_t ph;
      ph.date.year = 2026; ph.date.month = 1; ph.date.date = 1; ph.date.weekDay = 4;  // Thu
      ph.time.hours = 0; ph.time.minutes = 0; ph.time.seconds = 0;
      M5.Rtc.setDateTime(&ph);
      Serial.println("[rtc] BM8563 returned garbage at boot — wrote 2026-01-01 placeholder");
    }
    dataRtcForceValid();   // enable clocking path even pre-time-sync
  }

  // F1.12: detect USB power source FIRST (used by G31 + G32 + setVolume below)
  bool onUsbBoot = (M5.Power.getVBUSVoltage() / 1000.0f) > 4.0f;

  // G29 charge enable
  M5.Power.setBatteryCharge(true);

  // G31 (F1.11): disable 5V Hat output (BOOST_EN bit 3) — prevents power-flow
  // conflict with USB charging.
  M5.Power.setExtOutput(false);

  // G32 (F1.12) REVERTED 2026-05-06 — disable DCDC_EN killed device boot.
  // 5V DCDC powers critical M5StickS3 systems (LCD backlight / PSRAM 5V
  // supply / AW8737 amp init). Without DCDC, system stops at boot.
  // UiFlow 2.0 fast-charge mechanism is NOT just turning DCDC off; needs
  // proper M5PM1 datasheet investigation. Acceptable F1.x state: 12.9 mV/min
  // charge rate (~40 min full from ~30%). F2 polish candidate: research
  // M5PM1 charge current control register OR borrow UiFlow charge strategy.
  // Note: if device ever gets "stuck" with DCDC=0 (e.g. previous bad flash),
  // long-press PWR ≥10s for hard PMIC shutdown + re-power resets reg06 to
  // hardware default which has bit 1 = 1.

  // G30 (F1.10) PMIC diagnostic — user 2026-05-06 报 "拔 USB 后几十秒到 0%
  // 然后 bootloop", 需 grounded 确认 M5PM1 vs AXP2101 哪个是真硬件 + 充电
  // 寄存器实际状态。I2C 扫 0x6E (M5PM1) 和 0x34 (AXP2101) 看哪个 ack +
  // dump M5PM1 关键 reg 0x06 (charge_en) + 0x22/0x23 (vBat) + 0x24/0x25 (vBus)
  // + 0x26/0x27 (5VOUT)。结果 print Serial 你 monitor 看。
  {
    bool m5pm1_ack = M5.In_I2C.scanID(0x6E);
    bool axp2101_ack = M5.In_I2C.scanID(0x34);
    Serial.printf("[pmic] board=%d M5PM1@0x6E ack=%d AXP2101@0x34 ack=%d\n",
                  (int)M5.getBoard(), m5pm1_ack, axp2101_ack);
    if (m5pm1_ack) {
      uint8_t reg06 = M5.In_I2C.readRegister8(0x6E, 0x06, 100000);
      uint8_t buf22[2] = {0,0}, buf24[2] = {0,0}, buf26[2] = {0,0};
      M5.In_I2C.readRegister(0x6E, 0x22, buf22, 2, 100000);
      M5.In_I2C.readRegister(0x6E, 0x24, buf24, 2, 100000);
      M5.In_I2C.readRegister(0x6E, 0x26, buf26, 2, 100000);
      uint16_t vBat = (buf22[1] << 8) | buf22[0];
      uint16_t vBus = (buf24[1] << 8) | buf24[0];
      uint16_t v5v  = (buf26[1] << 8) | buf26[0];
      Serial.printf("[pmic] M5PM1 reg06=0x%02X (chgEn=%d) vBat=%dmV vBus=%dmV 5VOUT=%dmV\n",
                    reg06, reg06 & 0x01, vBat, vBus, v5v);
    }
  }

  M5.Speaker.begin();
  // G26 + G28 (F1.9 battery bootloop fix): set Speaker volume based on power
  // source. AXP2101 OCP triggers brownout reset when AW8737 amp peaks coincide
  // with BLE radio TX + LCD backlight inrush on battery. References.md §C.1
  // documents "<75% on battery" — empirically 50% (128) still triggered
  // reboot loop per user 2026-05-06 ("拔掉线 → 几秒重启 → bam → 无限循环").
  // Strategy: detect VBUS at boot; on USB use 128 (audible), on battery use
  // 64 (~25%, near M5Unified default but explicitly safe). Trade-off: chimes
  // quieter on battery but device DOESN'T crash — net win.
  M5.Speaker.setVolume(onUsbBoot ? 128 : 64);
  delay(50);   // small spacing after Speaker init before BLE TX inrush
  startBt();
  if (ledIsSafe()) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);   // off (active-low on Plus 1)
  }
  M5.BtnPWR.setHoldThresh(1000);   // PWR long-press = 1s (matches upstream AXP intent; G20)
  // G28 (F1.9): reduce LCD backlight on battery boot to lower current draw
  // and prevent OCP reset cascade. brightLevel=4 (max) draws ~80mA backlight
  // alone; brightLevel=1 (~40%) is plenty for ambient use, saves ~20mA.
  // User can cycle up via settings → brightness.
  if (!onUsbBoot) brightLevel = 1;
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  spr.createSprite(W, H);
  // M7 v2 (F1.4): per-call CJK font switching, NOT global. Global setFont
  // efontCN_12 broke ASCII species art layout (BUDDY_CHAR_H=8 for Font0)
  // + made Chinese 10×10 too small to read clearly. Reverted to Font0 default;
  // cjk* helpers (top of file) detect UTF-8 in user-data strings and switch
  // font per-call. Layouts preserved for ASCII; Chinese rendered when needed.

  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   cjkDrawString(spr, line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   cjkDrawString(spr, petName(), W/2, H/2 + 12);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString(T("Hello!", "你好!"), W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString(T("a buddy appears", "伙伴出现啦"), W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
    // Upstream verbatim — no fillSprite after delay. Loop iter 1 buddyTick
    // covers y=0~82 + drawHUD covers bottom; splash residue overwritten in
    // first frame. Previous G21 fillSprite was over-engineering: black sprite
    // PLUS broken render path = guaranteed black; reverting puts us back on
    // upstream's known-working contract.
  }

  // G24 root-fix: force ASCII mode at boot. Per references.md §A.1 "18 ASCII
  // pet species" = upstream PRIMARY identity, GIF is optional augmentation.
  // Even if /characters/ has uploaded pack, we start in ASCII so user sees
  // SOMETHING immediately. nextPet (main.cpp:78) lets user cycle to GIF if
  // they want; xfer.h post-upload still flips to GIF mode (line 229) which
  // can self-heal back via Fix A2 if open fails. ASCII default = robust.
  buddyMode = true;
  buddyInvalidate();

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5.update();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on attention, otherwise off
  if (activeState == P_ATTENTION && settings().led) {
    if (ledIsSafe()) digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
  } else {
    if (ledIsSafe()) digitalWrite(LED_PIN, HIGH);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // M1.1 (F1): tick chime when prompt elapsed ≥20s — every second a short
  // crisp tick emphasizes urgency (paired with drawApproval RED+flash). Plan
  // called this "5s 内每秒" but we don't know desktop's deadline locally so
  // tick on elapsed-second-boundary instead. Static lastTickedS dedupes per
  // second so user gets exactly 1 tick/sec.
  {
    static uint32_t lastTickedS = 0;
    if (inPrompt) {
      uint32_t elapsedS = (millis() - promptArrivedMs) / 1000;
      if (elapsedS >= 20 && elapsedS != lastTickedS) {
        lastTickedS = elapsedS;
        beep(1800, 30);
      }
    } else {
      lastTickedS = 0;
    }
  }

  // M8.1 (F1): fireworks Easter egg ★ user requested. BtnA + BtnB held ≥2s
  // simultaneously → 3s full-screen fireworks burst (drawn DIRECT to LCD,
  // bypasses sprite path so survives pushSprite). Random color circles + sweep
  // audio chimes. Set swallowBtn flags to prevent pre-launch button releases
  // from triggering approve/deny accidentally.
  {
    static uint32_t bothBtnSinceMs = 0;
    if (M5.BtnA.isPressed() && M5.BtnB.isPressed() && !inPrompt) {
      if (bothBtnSinceMs == 0) bothBtnSinceMs = now;
      if (now - bothBtnSinceMs >= 2000 && (int32_t)(now - fireworksUntil) >= 0) {
        fireworksUntil = now + 3000;
        swallowBtnA = swallowBtnB = true;
        if (settings().sound) M5.Speaker.tone(800, 80);  // initial whoosh
      }
    } else {
      bothBtnSinceMs = 0;
    }
  }

  // M2.1 v2 (F1.1, post-user-critique): heartbeat REMOVED — periodic 200Hz
  // pulse during BUSY was annoying-cluster-noise, not "余光陪伴". Only the
  // single-shot completion tip retained: when sessionsRunning N>0→0, short
  // "water drop" chime tells user task done without looking. Low cost,
  // single-shot per transition.
  static uint8_t m2_lastSessionsRunning = 0;
  if (settings().sound && !inPrompt && !screenOff && !napping) {
    if (m2_lastSessionsRunning > 0 && tama.sessionsRunning == 0) {
      M5.Speaker.tone(2200, 50);   // completion "tip"
    }
  }
  m2_lastSessionsRunning = tama.sessionsRunning;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  // Was upstream `M5.Axp.GetBtnPress() == 0x02` (edge-triggered, fires once).
  // Per arbitration Opus review 2026-05-04: pressedFor(1000) is CONTINUOUS
  // (returns true every loop iteration while held >1s) → screen state thrash.
  // Use wasReleasedAfterHold() instead — fires once on release after hold.
  // Default hold threshold is 500ms; we set 1000ms in setup() via setHoldThresh().
  if (M5.BtnPWR.wasReleasedAfterHold()) {
    if (screenOff) {
      wake();
    } else {
      M5.Display.sleep();
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    // F1.3: all overlay dismissals must call applyDisplayMode() (which does
    // spr.fillSprite(0) + characterInvalidate) NOT just characterInvalidate
    // alone — settings/menu/reset panels paint y=82~212 middle region which
    // characterInvalidate's buddyTick re-render only covers y=0~82, leaving
    // panel pixels stuck (user reported 2026-05-04 night settings residue).
    if (resetOpen) { resetOpen = false; applyDisplayMode(); }
    else if (settingsOpen) { settingsOpen = false; applyDisplayMode(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) applyDisplayMode();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        // M1.1 v2 (F1.1, post-user-critique): single beep restored — 2-note
        // ding-DING was marginal value vs 130ms blocking + complexity. ALWAYS
        // trigger 1.5s HEART retained (visual reward, no audio cost).
        beep(2400, 60);
        triggerOneShot(P_HEART, 1500);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else if (displayMode == DISP_INFO && infoLastLineY > H - 2) {
        // F1.7 (UX redesign per user 2026-05-06): in INFO mode, BtnA scrolls
        // page down if more content below visible. Only when last line is
        // already in visible area do we advance to next displayMode.
        // Architectural note: differs from upstream "BtnA = next mode 直接".
        // iOS-pattern: scroll-then-page-turn (similar to long article reading).
        beep(1800, 30);
        infoScroll++;
      } else {
        beep(1800, 30);
        infoScroll = 0;   // reset on mode change
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      // M1.1 v2 (F1.1): single beep restored — 2-note descending was
      // marginal value vs 170ms blocking. Single low tone is clear "no" feel.
      beep(600, 60);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
      infoScroll = 0;   // F1.7: reset scroll on page change
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (napping || screenOff || landscapeClock) {
    // skip sprite render — face-down, powered off, or landscape clock
    // (which draws direct-to-LCD below)
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print(T("installing", "安装中"));
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      cjkPrint(spr, T("no character loaded", "未加载角色"));
    }
  }
  if (landscapeClock) {
    drawClock();
  } else if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    spr.pushSprite(0, 0);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5.Display.setBrightness((8) * 255 / 100);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    M5.Display.sleep();
    screenOff = true;
  }

  // M8.1 v2 (F1.1): fireworks Easter egg + design credit overlay.
  // BtnA+B 同按 ≥2s → 3s 全屏 fireworks (随机彩色圆 + chime sweep) + 居中
  // 信用面板 "★ 彩蛋 ★ + eashe@qq.com"。Direct M5.Display
  // 旁路 sprite, 3s 后 sprite pushSprite 自然覆盖恢复正常显示。
  if (fireworksUntil && (int32_t)(now - fireworksUntil) < 0) {
    static const uint16_t fwColors[] = {
      0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF, 0xFFFF, 0xFC00, 0x83E0, 0xFD20
    };
    // 3-5 colored circles per frame
    int n = 3 + (esp_random() % 3);
    for (int i = 0; i < n; i++) {
      int x = esp_random() % W;
      int y = esp_random() % H;
      int r = 3 + (esp_random() % 8);
      uint16_t c = fwColors[esp_random() % (sizeof(fwColors)/sizeof(fwColors[0]))];
      M5.Display.fillCircle(x, y, r, c);
    }
    // F1.8: Easter egg panel updated per user — removed "design by", replaced
    // with playful "★ 彩蛋 ★" tagline + contact lines. CN char "彩蛋" needs
    // efontCN_12 font set on M5.Display (direct LCD path, distinct from spr).
    const int boxW = 124, boxH = 56;
    int bx = (W - boxW) / 2, by = (H - boxH) / 2;
    M5.Display.fillRect(bx, by, boxW, boxH, 0x0000);
    M5.Display.drawRect(bx, by, boxW, boxH, 0xFFFF);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xFFFF, 0x0000);
    M5.Display.setFont(&fonts::efontCN_12);
    M5.Display.drawString("★ 彩蛋 ★",      bx + boxW/2, by + 14);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.drawString("eashe@qq.com",  bx + boxW/2, by + 34);
    M5.Display.setTextDatum(TL_DATUM);
    // Audio chime sweep
    static uint32_t lastBurstMs = 0;
    if (now - lastBurstMs >= 250) {
      lastBurstMs = now;
      if (settings().sound) {
        uint16_t freq = 1200 + (esp_random() % 1800);
        M5.Speaker.tone(freq, 80);
      }
    }
  }

  // G23 diagnostic: 1 Hz Serial state dump — committed (not stripped) so
  // future regressions diagnosable from `pio device monitor` without re-
  // instrumenting. Cost: Serial.printf @ 1 Hz over USB-CDC = trivial.
  static uint32_t lastDiagMs = 0;
  if (now - lastDiagMs >= 1000) {
    lastDiagMs = now;
    Serial.printf("[diag] base=%d active=%d buddyMode=%d loaded=%d "
                  "screenOff=%d napping=%d connected=%d onUsb=%d "
                  "promptId=%s heap=%uK\n",
                  baseState, activeState, buddyMode,
                  characterLoaded(), screenOff, napping,
                  tama.connected, _onUsb,
                  tama.promptId[0] ? tama.promptId : "(none)",
                  ESP.getFreeHeap() / 1024);
  }

  delay(screenOff ? 100 : 16);
}
