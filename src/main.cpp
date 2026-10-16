#include <M5StickCPlus.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

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

// 注：曾试过把循环任务栈从 8KB 提到 16KB，用于验证崩溃是否由栈溢出引起。
// 结论是【无效】——崩溃依旧，而且多占的 8KB 堆把最大连续空闲块从
// 13.3KB 压到 9.2KB（实测），反而加剧内存紧张，故已撤销。
// 空闲时 uxTaskGetStackHighWaterMark 为 5984 字节，在 8KB 栈下是够的。
// 阶段 B：逻辑画布改为与本板屏幕同尺寸（原版面向 M5StickC Plus 的 135x240）。
const int W = 240, H = 320;
const int CX = W / 2;
const int CY_BASE = 120;
// 原版 M5StickC Plus 在 GPIO10 上有颗红色 LED，审批时闪烁提示（active-low）。
// 本板 GPIO10 是 I2C SDA（CW2017 与 ES8311 共用），驱动成数字口会破坏 I2C 总线，
// 且本板无用户可控 LED，故停用该提示。Settings 里的 LED 开关仍可切换，只是无效果。
const int LED_PIN = -1;

// 画布已与屏幕同尺寸，不再需要居中偏移。
// （阶段 A 时此处为 (240-W)/2 / (320-H)/2，用于把 135x240 画布居中并留黑边。）
const int CANVAS_X = 0;
const int CANVAS_Y = 0;

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

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
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
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
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { M5.Axp.ScreenBreath(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Axp.SetLDO2(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Beep.tone(freq, dur);
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

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "clock rot", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 10;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
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
    case 9: settingsOpen = false; characterInvalidate(); return;
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

// ── 菜单/面板排版 ─────────────────────────────────────────────────────────
// 原版面向 135x240：size 1 字体（6x8px）、面板宽 118、行距 14。
//
// 关于字号：TFT_eSPI 的位图字体只能整数倍缩放，没有中间档。
//   size 1 → 面板宽下限约 114px（占屏宽 48%），偏小
//   size 2 → 面板宽下限约 220px（占屏宽 92%），几乎铺满整屏
// 两档都试过了，取 size 1；靠加宽面板 + 拉大行距/内边距获得舒展观感。
// 宽度下限由最长条目决定：设置页 "> brightness"（12 字符）+ 取值 "19/19"（5 字符）。
const int MENU_TSIZE   = 1;                       // 字号（原 1）
const int MENU_GLYPH_W = 6 * MENU_TSIZE;          // 单字符宽 = 6
const int MENU_ROW_H   = 16;                      // 行距（原 14）
const int MENU_PAD     = 8;                       // 左右内边距（原 6）
const int MENU_TOP_PAD = 12;                      // 顶部内边距（原 8）
const int MENU_W       = 150;                     // 面板宽（原 118）
const int MENU_VAL_W   = 44;                      // 取值列宽（原 36）
const int MENU_HINT_H  = 22;                      // 底部提示行高（原 14）

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "DOWN", const char* rightLbl = "OK") {
  spr.drawFastHLine(mx + MENU_PAD, hy - 6, mw - 2 * MENU_PAD, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  int x = mx + MENU_PAD + 2;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * MENU_GLYPH_W + 6;
  spr.fillTriangle(x, hy + 2, x + 8, hy + 2, x + 4, hy + 9, p.textDim);
  x = mx + mw / 2 + 6;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * MENU_GLYPH_W + 6;
  spr.fillTriangle(x, hy, x, hy + 7, x + 7, hy + 3, p.textDim);
}

static void drawSettings() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(60); }  // drawSettings
  const Palette& p = characterPalette();
  int mw = MENU_W, mh = MENU_TOP_PAD + SETTINGS_N * MENU_ROW_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 6, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 6, p.textDim);
  spr.setTextSize(MENU_TSIZE);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + MENU_PAD, my + MENU_TOP_PAD + i * MENU_ROW_H);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - MENU_VAL_W, my + MENU_TOP_PAD + i * MENU_ROW_H);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 14, "Next", "Change");
}

static void drawReset() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(61); }  // drawReset
  const Palette& p = characterPalette();
  int mw = MENU_W, mh = MENU_TOP_PAD + RESET_N * MENU_ROW_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 6, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 6, HOT);
  spr.setTextSize(MENU_TSIZE);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + MENU_PAD, my + MENU_TOP_PAD + i * MENU_ROW_H);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 14);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: M5.Axp.PowerOff(); break;
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
  { extern void dbgStageSet(uint32_t); dbgStageSet(62); }  // drawMenu
  const Palette& p = characterPalette();
  int mw = MENU_W, mh = MENU_TOP_PAD + MENU_N * MENU_ROW_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 6, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 6, p.textDim);
  spr.setTextSize(MENU_TSIZE);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + MENU_PAD, my + MENU_TOP_PAD + i * MENU_ROW_H);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 14);
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
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
// 上游用它判断「是否插在充电座上」，是时钟页显示条件的一部分。本板没有 VBUS
// 检知电路故恒为 false，且 clocking 的条件里已不再引用它 —— 保留字段是为了
// 把与上游的差异限制在必要范围内。
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = M5.Axp.GetVBusVoltage() > 4.0f;
  M5.Rtc.GetTime(&_clkTm);
  M5.Rtc.GetDate(&_clkDt);
}

// 横屏时钟开关。
//
// 这段曾被禁用：开启后实测触发 Load access fault（MTVAL 为字符串内容，
// 栈回溯落在 LittleFS 打开文件 + printf 这条链上），当时归咎于横屏路径
// 本身。后来查明与横屏无关 —— 根因是按键诊断打印的越界索引（见 hal.cpp
// 的 buttonsUpdate），横屏只是改变了主循环节奏、更容易踩中那条路径。
// 根因修复后横屏已无已知障碍，故恢复。
//
// 【本板怎么进横屏】设置 → Rot 有三档 auto/port/land。本板没有 IMU
// （HalImu 恒返回「平放朝上」），而 auto 档靠重力判断，所以 **auto 在本板
// 恒为竖屏**，必须手动选 land。这不是缺陷，是缺该硬件的必然结果。
//
// 坐标已按本板重排：上游按 240×135 画布写，本板面板旋转后是 320×240，
// 见 drawClock 前的 LSC_* 一组常量。
#define ENABLE_LANDSCAPE_CLOCK 1

static void clockUpdateOrient() {
#if !ENABLE_LANDSCAPE_CLOCK
  clockOrient = 0;
  orientFrames = 0;
#else
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
#endif
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }

// 横屏布局常量。上游这套坐标按 240×135 画布写（M5StickC Plus 面板旋转后的
// 尺寸），本板 240×320 面板旋转后是 320×240，故整体重排：宠物仍占左上，
// 时钟文字落到右侧剩余区域的中心。
// 竖向沿用上游观感 —— 文字块中心取屏高的约 52%（上游是 72/135）。
static const int LSC_W       = 320;   // 横屏逻辑宽（rotation 1 与 3 都是 320×240）
static const int LSC_H       = 240;
static const int LSC_PET_BOX = 120;   // 宠物清除方框边长，也即左侧留给宠物的宽度
static const int LSC_PET_CX  = LSC_PET_BOX / 2;            // 宠物中心（半尺寸渲染）
static const int LSC_PET_CY  = LSC_PET_BOX / 2;
static const int LSC_TXT_X   = (LSC_PET_BOX + LSC_W) / 2;  // 右侧区域中心 = 220
static const int LSC_TXT_Y0  = 95;    // 三行文字的首行中心
static const int LSC_TXT_DY  = 30;    // 行距

static void drawClock() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(63); }  // drawClock
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.Date);

  if (clockOrient == 0) {
    paintedOrient = 0;
    // Bottom half — buddy naturally lives at y=0..82, GIF peeks at top
    // via peek mode. Clearing from 90 leaves both untouched.
    // 竖屏时钟：原版这些 y 是按 240 高排的，320 高需要整体下移 40 保持居中。
    spr.fillRect(0, 130, W, H - 130, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 180);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 215);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 240);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Landscape: 320×240 direct-to-LCD，只在本函数内临时切 rotation，末尾恢复 0，
  // 精灵图的坐标系不受影响。
  // 全屏填充仅在进入横屏时做一次；此后文字自带背景格、宠物框每帧只做小面积
  // fillRect，撕裂范围足够小。
  M5.Lcd.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Lcd.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  // Seconds tick at 1Hz; redrawing 3 strings at 60fps is 180 SPI ops/sec
  // for nothing. Gate on the second changing (or full repaint).
  if (repaint || _clkTm.Seconds != lastSec) {
    lastSec = _clkTm.Seconds;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.Seconds);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(p.text, p.bg);
    M5.Lcd.drawString(hm,  LSC_TXT_X, LSC_TXT_Y0);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(p.textDim, p.bg);
    M5.Lcd.drawString(ssl, LSC_TXT_X, LSC_TXT_Y0 + LSC_TXT_DY);
    M5.Lcd.drawString(wdl, LSC_TXT_X, LSC_TXT_Y0 + LSC_TXT_DY * 2);
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
      // ASCII 路径不接受坐标参数（buddyRenderTo 内部用 BUDDY_X_CENTER=120 /
      // BUDDY_Y_BASE=70，那是 240 宽画布的值），在 320 宽的横屏里宠物会横跨
      // 到时钟文字区。清除框因此放宽到整个左半屏以免残影 —— 轻微重叠是这个
      // 降级态的已知代价，装了 GIF 角色包就不会走这条路径。
      M5.Lcd.fillRect(0, 0, LSC_W / 2, LSC_H, p.bg);
      buddyRenderTo(&M5.Lcd, activeState);
    } else {
      // 整帧 GIF 每个像素都会画（透明像素填 pal.bg），所以不必每帧清屏 ——
      // 那会在清除与最后一条扫描线之间留下可见黑闪。进入横屏时的 fillScreen
      // 已经覆盖了周边。
      characterSetState(activeState);
      characterRenderTo(&M5.Lcd, LSC_PET_CX, LSC_PET_CY);
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




// ── info / pet 界面的字号 ─────────────────────────────────────────────────
// 位图字体只能整数倍缩放，没有 1.5 档。size 1 是 6x8px（每行 40 字符），
// size 2 是 12x16px（每行 19 字符，行高 16）。这两个页面内容区只有
// 210px 高（TOP=110 到 H=320），全用 size 2 时每页上限约 10 行。
//
// 26-10-16 起 info 与 pet 两个界面**整体**用 size 2 —— 原先只放大标题的做法
// 在真机上看着头重脚轻、内容只占半屏。代价是每页文案必须压到 19 字符宽，
// 放不下的句子做了缩句（语义保留），详见各页注释。
//
// 改字号必须同步改 y 步进，否则下方内容会重叠：size 2 的行步进是 16。
const int TSIZE_PAGE = 2;

// Persistent screen-level title row ("Info  n/N") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
// 两行都放大：只放大 section 名会让页眉和正文大小不一，反而更乱。
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 40, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 18;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 16;
}

void drawPasskey() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(64); }  // drawPasskey
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 96);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 224); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 150);
  spr.print(b);
}

void drawInfo() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(65); }  // drawInfo
  const Palette& p = characterPalette();
  // 内容起始线。原版为 70（= 宠物缩略条高度），画布由 240 加高到 320 后
  // 随宠物区一起下移 40，与 character.cpp 的 PEEK_TOP 保持一致。
  const int TOP = 110;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(TSIZE_PAGE);
  int y = TOP + 2;
  // 全页 size 2 后每行上限 19 字符（19 × 12px = 228px，从 x=4 排到 232）。
  // 下面各页的文案都按这个宽度重排过。
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 16;
  };
  auto gap = [&]() { y += 8; };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    // 原句 "I sleep when nothing's happening, wake when you start working..."
    // 与 "18 species. Settings > ascii pet to cycle." 放大后逐行超宽，
    // 此处缩句，语义未丢。
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    gap();
    ln("I sleep when idle,");
    ln("wake when you work.");
    gap();
    spr.setTextColor(p.text, p.bg);
    ln("Press OK to approve");
    ln("a prompt from here.");
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species to pick");
    ln("in settings.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    // 本板三键。原版的两键 A/B 与 Power 说明已不适用（本板无独立电源键，
    // 电源键语义由 UP 承担，见 src/hal/M5StickCPlus.h 的键位映射说明）。
    // 键名顶格、说明缩进，靠缩进和颜色分组，不再留行间距。
    spr.setTextColor(p.text, p.bg);    ln("OK    centre");
    spr.setTextColor(p.textDim, p.bg); ln("    next page");
    ln("    approve prompt");
    spr.setTextColor(p.text, p.bg);    ln("DOWN  lower");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    deny prompt");
    spr.setTextColor(p.text, p.bg);    ln("UP    upper");
    spr.setTextColor(p.textDim, p.bg); ln("    screen off");
    spr.setTextColor(p.text, p.bg);    ln("hold DOWN");
    spr.setTextColor(p.textDim, p.bg); ln("    menu");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    // 原标签各带 2 空格缩进；放大后 "  ble       encrypted" 是 21 字符会超宽，
    // 去掉缩进正好 19。缩进原本只是视觉层次，放大后标签本身够醒目，不需要了。
    spr.setTextColor(p.textDim, p.bg);
    ln("sessions  %u", tama.sessionsTotal);
    ln("running   %u", tama.sessionsRunning);
    ln("waiting   %u", tama.sessionsWaiting);
    gap();
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("via       %s", dataScenarioName());
    ln("ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("last msg  %lus", (unsigned long)age);
    ln("state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = (int)(M5.Axp.GetBatVoltage() * 1000);
    int iBat_mA = (int)M5.Axp.GetBatCurrent();
    int vBus_mV = (int)(M5.Axp.GetVBusVoltage() * 1000);
    int pct = (vBat_mV - 3200) / 10;   // (v-3.2)/(4.2-3.2)*100 = (v-3.2)*100 = (mv-3200)/10
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    // 电量百分比仍靠亮色突出（全页同字号后无法再靠字号拉开层次）。
    // "100%" 4 字符 × 12 = 48px，从 x=4 排到 52，状态词从 72 起不会撞。
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(72, y);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("current  %+dmA", iBat_mA);
    // usb 行恒不显示：本板无 VBUS 检知，vBus_mV 恒为 0。
    if (usb) ln("usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    // owner 名最长 31 字符，放大后 9 + 31 远超 19 字符的行宽，截到 10。
    if (ownerName()[0]) ln("owner    %.10s", ownerName());
    uint32_t up = millis() / 1000;
    ln("uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("bright   %u/4", brightLevel);
    ln("bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    ln("temp     %dC", (int)M5.Axp.GetTempInAXP192());

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    y += 20;

    spr.setTextColor(p.text, p.bg);
    ln("%s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("%02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln("Open Claude desktop");
      ln("> Developer");
      ln("> Hardware Buddy");
      // 原句 "auto-connects via BLE" 放大后 21 字符超宽，缩句。
      ln("connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    gap();
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    spr.setTextColor(p.text, p.bg);
    // "github.com/anthropics" 22 字符、仓库名本身 "claude-desktop-buddy"
    // 20 字符，放大后分别是 264px / 240px，都超出 240px 屏宽，
    // 按 owner/repo 的连字符切开，信息不变。
    ln("anthropics/claude-");
    ln("desktop-buddy");
    gap();
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    ln("M5StickC Plus");
    ln("ESP32 + AXP192");
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
// 转写行缓冲的每行字符容量。原版按 135px 宽换行 21 字符、缓冲取 24；
// 阶段 B 画布加宽到 240px，换行 38 字符，缓冲相应取 40。
// 用具名常量避免行宽在多处各写一个数字（先前 wrapInto 的参数里就写死了 24）。
static const int MSG_COLS = 40;

static uint8_t wrapInto(const char* in, char out[][MSG_COLS], uint8_t maxRows, uint8_t width) {
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
  { extern void dbgStageSet(uint32_t); dbgStageSet(66); }  // drawApproval
  const Palette& p = characterPalette();
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Size 2 only if it fits one line (~10 chars at 12px on 135px screen)
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  spr.setCursor(4, H - AREA + (toolLen <= 10 ? 14 : 18));
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // Hint wraps at ~21 chars to two lines under the tool name
  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(4, H - AREA + 34);
  spr.printf("%.21s", tama.promptHint);
  if (hlen > 21) {
    spr.setCursor(4, H - AREA + 42);
    spr.printf("%.21s", tama.promptHint + 21);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("OK: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 66, H - 12);
    spr.print("DOWN: deny");
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
  { extern void dbgStageSet(uint32_t); dbgStageSet(67); }  // drawPetStats
  // 内容起始线。原版为 70（= 宠物缩略条高度），画布由 240 加高到 320 后
  // 随宠物区一起下移 40，与 character.cpp 的 PEEK_TOP 保持一致。
  const int TOP = 110;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(TSIZE_PAGE);
  int y = TOP + 16;

  // 三个指标标签与右侧图形条同一行。标签放大到 12px/字符后 "energy" 独占
  // 72px（x=6 排到 78），图形条必须整体让到 78 之后，否则会被压在标签底下。
  // 标签用 y-6 让 16px 字高居中于图形中线（约 y+2）。
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 6); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(78 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 6); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 78 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 6); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 86 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  // 徽章随文字一起放大：文字 "Lv NN" 需 60px + 内边距，42x14 → 72x20。
  // 步进同步由 20 加到 24，否则下方统计行会贴到徽章底边。
  y += 24;
  spr.fillRoundRect(6, y - 2, 72, 20, 4, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(12, y); spr.printf("Lv %u", stats().level);

  // 统计行改 size 2 后行距由 10 加到 16（字号本身已占 16px）。
  y += 24;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(6, y + 16);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y + 32);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 48);
  tokFmt("today    ", tama.tokensToday, y + 64);
}

static void drawPetHowTo(const Palette& p) {
  { extern void dbgStageSet(uint32_t); dbgStageSet(68); }  // drawPetHowTo
  // 内容起始线。原版为 70（= 宠物缩略条高度），画布由 240 加高到 320 后
  // 随宠物区一起下移 40，与 character.cpp 的 PEEK_TOP 保持一致。
  const int TOP = 110;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(TSIZE_PAGE);
  int y = TOP + 2;
  // 全页 size 2：行步进 16。缩进全部去掉 —— 加缩进后最长的几行会到 19 字符
  // 上限，去掉后最长的 "50K tokens = level" 只占 18 字符（x=6 排到 222）。
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 16;
  };

  y += 8;  // room for the PET header drawn by drawPet()

  // 12 行 × 16px = 192px，从 y=120 排到 312，刚好铺满 320 的内容区，
  // 因此不再留行间空行 —— 靠 p.body / p.textDim 的明暗差分组。
  ln(p.body,   "MOOD");
  ln(p.textDim, "approve fast = up");
  ln(p.textDim, "deny lots = down");
  ln(p.body,   "FED");
  ln(p.textDim, "50K tokens = level");
  // 上游这里写的是 "face-down to nap / refills to full"，但本板没有加速度计
  // （HalImu::getAccelData 恒返回平放朝上，isFaceDown() 要求 az < -0.7 恒不成立），
  // 打盹永不触发，精力只降不升 —— 照抄会让人去找一个永远不会发生的动作。
  ln(p.body,   "ENERGY");
  ln(p.textDim, "drains over time");
  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any button = wake");
  ln(p.textDim, "DOWN: next screen");
  ln(p.textDim, "OK: next page");
  ln(p.textDim, "hold DOWN: menu");
}

void drawPet() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(69); }  // drawPet
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right.
  // 标题放大后占 y..y+16，落在宠物动画区（0..110）内；多占的 8px 应落在
  // GIF 居中的底部留白里。刷机后若见压住宠物，把 y 下调或改回小字。
  char title[48];
  if (ownerName()[0]) snprintf(title, sizeof(title), "%s's %s", ownerName(), petName());
  else                snprintf(title, sizeof(title), "%s", petName());
  // 标题画在宠物之上，越过宠物图形的左边缘就会压上去，故按那条线限宽。
  // 宠物屏走 peek 模式（applyDisplayMode 里 displayMode != DISP_NORMAL 即 peek），
  // GIF 缩半居中 —— bufo 的 96x100 变成 48x50，占 x=96..144。
  // 放不下时先丢掉 "owner's" 前缀只留宠物名（宠物名才是主角），仍放不下才退回小字。
  int gifLeft = characterGifLeft();
  if (gifLeft <= 0 || gifLeft > W) gifLeft = 96;   // 未加载 GIF 时按实测值兜底
  const int TITLE_MAX_CHARS = (gifLeft - 8) / 12;
  if ((int)strlen(title) > TITLE_MAX_CHARS && ownerName()[0])
    snprintf(title, sizeof(title), "%s", petName());
  bool bigTitle = (int)strlen(title) <= TITLE_MAX_CHARS;
  spr.setTextSize(bigTitle ? TSIZE_PAGE : 1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y);
  spr.print(title);
  spr.setTextSize(TSIZE_PAGE);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 40, y + (bigTitle ? 4 : 2));   // 与标题垂直居中
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(70); }  // drawHUD
  if (tama.promptId[0]) { drawApproval(); return; }
  extern void dbgStageSet(uint32_t); dbgStageSet(71);
  const Palette& p = characterPalette();
  // WIDTH 是换行字符数（6px/字符），留 2 列余量给行缓冲。
  const int SHOW = 3, LH = 8, WIDTH = MSG_COLS - 2;
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  extern void dbgStageSet(uint32_t); dbgStageSet(72);
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, H - LH - 2);
    extern void dbgStageSet(uint32_t); dbgStageSet(73);
    spr.print(tama.msg);
    extern void dbgStageSet(uint32_t); dbgStageSet(77);
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static char disp[32][MSG_COLS];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  extern void dbgStageSet(uint32_t); dbgStageSet(74);
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
    extern void dbgStageSet(uint32_t); dbgStageSet(75);
    spr.print(disp[row]);
  }
  extern void dbgStageSet(uint32_t); dbgStageSet(76);
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 18, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(0);

  // 满屏精灵图（240x320x2 = 150KB）是本板最吃内存的一项，必须在任何会
  // 切碎堆的子系统之前分配：实测先起 BLE 再申请 150KB 会因连续块不足而失败，
  // 而彼时空闲堆仍有 160KB。放在这里申请到的是一整块未切分的堆。
  {
    // 16 位满屏精灵图要 153.6KB，本板无 PSRAM，分配后仅剩约 2.9KB 空闲堆 ——
    // 不足以容纳协议行（单行上限 4KB）与 JSON 解析。改用 8 位色深减半到 76.8KB。
    // 代价是颜色经 RGB332 量化（每通道 3/3/2 位），对平涂卡通风格影响有限。
    void* spriteBuf = spr.createSprite(W, H);
    Serial.printf("[ui] sprite %dx%d depth16 = %u B, %s, heap=%u\n",
                  W, H, (unsigned)(W * H * 2),
                  spriteBuf ? "OK" : "ALLOC FAILED",
                  (unsigned)ESP.getFreeHeap());
  }

  M5.Imu.Init();
  M5.Beep.begin();
  startBt();
  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);   // off
  }
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
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
    // 这块只停 1.8 秒，字号要够大才读得到，主标题取 size 3（18x24px）。
    // 但 owner / pet 名都是可配置的，居中绘制一旦超宽就两侧同时溢出，
    // 所以按实际字宽逐级降档，取放得下的最大字号。
    auto shout = [&](const char* s, int cy, uint16_t col, uint8_t big) {
      uint8_t sz = big;
      while (sz > 1 && (int)strlen(s) * 6 * sz > W - 8) sz--;
      spr.setTextSize(sz);
      spr.setTextColor(col, p.bg);
      spr.drawString(s, W/2, cy);
    };
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      shout(line, H/2 - 24, p.text, 3);
      shout(petName(), H/2 + 24, p.body, 3);
    } else {
      // First boot, no owner pushed yet — say hi.
      shout("Hello!", H/2 - 24, p.body, 3);
      shout("a buddy appears", H/2 + 24, p.textDim, 2);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    { extern void dbgStageSet(uint32_t); dbgStageSet(78); }   // pushSprite 前
    spr.pushSprite(CANVAS_X, CANVAS_Y);
    { extern void dbgStageSet(uint32_t); dbgStageSet(79); }   // pushSprite 后
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5.update();
  { extern void dbgStageSet(uint32_t); dbgStageSet(81); }   // M5.update 之后(含 heapGuard)
  M5.Beep.update();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  { extern void dbgStageSet(uint32_t); dbgStageSet(82); }   // dataPoll 之后
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on attention, otherwise off
  if (LED_PIN >= 0) {
    if (activeState == P_ATTENTION && settings().led) {
      digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
    } else {
      digitalWrite(LED_PIN, HIGH);
    }
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

  { extern void dbgStageSet(uint32_t); dbgStageSet(83); }   // prompt 块之后
  bool inPrompt = tama.promptId[0] && !responseSent;

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
  if (M5.Axp.GetBtnPress() == 0x02) {
    if (screenOff) {
      wake();
    } else {
      M5.Axp.SetLDO2(false);
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        // 键位对调后 BtnA 在本板是 DOWN 键，审批页承担「拒绝」。
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        statsOnDenial();
        beep(600, 60);
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
      } else {
        beep(1800, 30);
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
      // 键位对调后 BtnB 在本板是 OK 键，审批页承担「批准」。
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (millis() - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      beep(2400, 60);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
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
  // 上游在这里还有一个 && _onUsb（插在充电座上才显示时钟）。本板没有 VBUS
  // 检知电路（HalPower::GetVBusVoltage() 恒返回 0），照搬会让时钟页成为永不
  // 可达的死代码，故去掉。这是移植的有意改动，不是临时 hack。
               && dataRtcValid();
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    extern uint32_t dbgStagePeek();
    extern void dbgStageSet(uint32_t);
    dbgStageSet(40);
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    dbgStageSet(41);
    characterInvalidate();
    dbgStageSet(42);
    if (buddyMode) buddyInvalidate();
    dbgStageSet(43);
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
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
      spr.setCursor(8, 130);
      spr.print("installing");
      spr.setCursor(8, 142);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 156, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 157, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 140);
      spr.print("no character loaded");
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
    { extern void dbgStageSet(uint32_t); dbgStageSet(78); }   // pushSprite 前
    spr.pushSprite(CANVAS_X, CANVAS_Y);
    { extern void dbgStageSet(uint32_t); dbgStageSet(79); }   // pushSprite 后
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
    M5.Axp.ScreenBreath(8);
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
    M5.Axp.SetLDO2(false);
    screenOff = true;
  }

  { extern void dbgStageSet(uint32_t); dbgStageSet(80); }   // 循环体末尾
  delay(screenOff ? 100 : 16);
}
