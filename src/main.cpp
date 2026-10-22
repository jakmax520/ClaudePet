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
#include "cn_text.h"

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
// 宠物状态名。改存文案 ID 以便随界面语言切换，下标与 PersonaState 一致。
// （character.cpp 里另有一份 STATE_NAMES 是给 manifest.json 用的，那是文件
// 格式的一部分，必须保持英文，两者不要合并。）
static const uint16_t STATE_IDS[] = {
  T_ST_SLEEP, T_ST_IDLE, T_ST_BUSY, T_ST_ATT, T_ST_CEL, T_ST_DIZZY, T_ST_HEART
};
static_assert(sizeof(STATE_IDS) / sizeof(STATE_IDS[0]) == (size_t)P_HEART + 1,
              "STATE_IDS 要与 PersonaState 一一对应");

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

// 菜单条目一律存文案 ID，实际字符串由 cnStr() 按当前语言取 —— 中英切换只是
// 改一个下标，不需要两套面板代码。
const uint16_t menuItems[] = {
  T_MENU_SETTINGS, T_MENU_OFF, T_MENU_HELP, T_MENU_ABOUT, T_MENU_DEMO, T_MENU_CLOSE
};
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const uint16_t settingsItems[] = {
  T_SET_BRIGHT, T_SET_SOUND, T_SET_BT, T_SET_WIFI, T_SET_LED,
  T_SET_HUD, T_SET_ROT, T_SET_PET, T_SET_LANG, T_SET_RESET, T_SET_BACK
};
const uint8_t SETTINGS_N = 11;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const uint16_t resetItems[] = { T_RST_CHAR, T_RST_FACTORY, T_SET_BACK };
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
    case 8:
      // 语言：立即生效。文字是每帧重画的，下一帧就是新语言，无需额外失效。
      s.lang = (s.lang + 1) % 2;
      cnSetLang(s.lang);
      break;
    case 9:  resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 10: settingsOpen = false; characterInvalidate(); return;
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
// 中文化后条目一律走 cnDraw（cn_text）：ASCII 8px/字符、汉字 16px/字、行高 16。
// 尺寸按**中英两种里较宽的那个**排，切换语言不需要两套常量 —— 英文条目普遍
// 更长（"> brightness" 12 字符 = 96px vs "屏幕方向" 4 字 = 64px），按英文排
// 即可同时容纳中文；反过来则不行，所以面板宽度不能按中文压缩。
// 列宽下限由最长条目 + 取值列决定：8 + 96 + 间隙 + 44 ≤ MENU_W。
const int MENU_GLYPH_W = 8;                       // ASCII 单字符宽 = CN_ASCII_W
const int MENU_ROW_H   = 20;                      // 行距（字高 16 + 4 间隔）
const int MENU_PAD     = 8;                       // 左右内边距
const int MENU_TOP_PAD = 12;                      // 顶部内边距
const int MENU_W       = 170;                     // 面板宽（原 150，为 8px 字符放宽）
const int MENU_VAL_W   = 44;                      // 取值列宽
const int MENU_HINT_H  = 22;                      // 底部提示行高

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
// 标签由调用方给 —— 设置面板传文案表里的「下一个 / 切换」，键名类的传
// "DOWN"/"OK" 原样（键是印在壳上的，不随界面语言变）。
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "DOWN", const char* rightLbl = "OK") {
  spr.drawFastHLine(mx + MENU_PAD, hy - 6, mw - 2 * MENU_PAD, p.textDim);
  int x = mx + MENU_PAD + 2;
  cnDraw(&spr, x, hy, downLbl, p.textDim, PANEL);
  x += cnWidth(downLbl) + 6;
  spr.fillTriangle(x, hy + 2, x + 8, hy + 2, x + 4, hy + 9, p.textDim);
  x = mx + mw / 2 + 6;
  cnDraw(&spr, x, hy, rightLbl, p.textDim, PANEL);
  x += cnWidth(rightLbl) + 6;
  spr.fillTriangle(x, hy, x, hy + 7, x + 7, hy + 3, p.textDim);
}

static void drawSettings() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(60); }  // drawSettings
  const Palette& p = characterPalette();
  int mw = MENU_W, mh = MENU_TOP_PAD + SETTINGS_N * MENU_ROW_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 6, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 6, p.textDim);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    const int rowY = my + MENU_TOP_PAD + i * MENU_ROW_H;
    // 选中标记与条目拼成一条再画：分两次 cnDraw 会在中间露出背景格
    char label[40];
    snprintf(label, sizeof(label), "%s%s", sel ? "> " : "  ", cnStr(settingsItems[i]));
    cnDraw(&spr, mx + MENU_PAD, rowY, label, sel ? p.text : p.textDim, PANEL);

    char val[24] = "";
    uint16_t valCol = p.textDim;
    if (i == 0) {
      snprintf(val, sizeof(val), "%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      valCol = vals[i-1] ? GREEN : p.textDim;
      snprintf(val, sizeof(val), "%s", cnStr(vals[i-1] ? T_VAL_ON : T_VAL_OFF));
    } else if (i == 6) {
      static const uint16_t RN[] = { T_VAL_AUTO, T_VAL_PORT, T_VAL_LAND };
      snprintf(val, sizeof(val), "%s", cnStr(RN[s.clockRot]));
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      snprintf(val, sizeof(val), "%u/%u", pos, total);
    } else if (i == 8) {
      valCol = p.text;
      snprintf(val, sizeof(val), "%s", cnStr(s.lang ? T_VAL_LANG_ZH : T_VAL_LANG_EN));
    }
    cnDraw(&spr, mx + mw - MENU_VAL_W, rowY, val, valCol, PANEL);
  }
  drawMenuHints(p, mx, mw, my + mh - 14, cnStr(T_HINT_NEXT), cnStr(T_HINT_CHANGE));
}

static void drawReset() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(61); }  // drawReset
  const Palette& p = characterPalette();
  int mw = MENU_W, mh = MENU_TOP_PAD + RESET_N * MENU_ROW_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 6, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 6, HOT);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    // 二次确认时标签翻成「确定？」，与英文 really? 同一套机制
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    char label[40];
    snprintf(label, sizeof(label), "%s%s", sel ? "> " : "  ",
             armed ? cnStr(T_RST_CONFIRM) : cnStr(resetItems[i]));
    cnDraw(&spr, mx + MENU_PAD, my + MENU_TOP_PAD + i * MENU_ROW_H, label,
           armed ? HOT : (sel ? p.text : p.textDim), PANEL);
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
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    char label[48];
    if (i == 4) {   // demo 项后面跟开关状态
      snprintf(label, sizeof(label), "%s%s  %s", sel ? "> " : "  ",
               cnStr(menuItems[i]), cnStr(dataDemo() ? T_VAL_ON : T_VAL_OFF));
    } else {
      snprintf(label, sizeof(label), "%s%s", sel ? "> " : "  ", cnStr(menuItems[i]));
    }
    cnDraw(&spr, mx + MENU_PAD, my + MENU_TOP_PAD + i * MENU_ROW_H, label,
           sel ? p.text : p.textDim, PANEL);
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
// "bt" / "usb" / "none" 是本项目自己产生的字面量（data.h 的 dataScenarioName），
// 翻成当前语言；demo 模式返回的场景名（asleep / busy / ...）来自主机侧的数据表，
// 设备不知道语义，原样显示。
static const char* scenarioLabel(const char* s) {
  if (!strcmp(s, "bt"))   return cnStr(T_SC_BT);
  if (!strcmp(s, "usb"))  return cnStr(T_SC_USB);
  if (!strcmp(s, "none")) return cnStr(T_SC_NONE);
  return s;
}

static void _infoHeader(const Palette& p, int& y, uint16_t sectionId, uint8_t page) {
  cnDraw(&spr, 4, y, cnStr(T_HDR_INFO), p.text, p.bg);
  char n[16];
  snprintf(n, sizeof(n), "%u/%u", page + 1, INFO_PAGES);
  cnDraw(&spr, W - 40, y, n, p.textDim, p.bg);
  y += 18;
  cnDraw(&spr, 4, y, cnStr(sectionId), p.body, p.bg);
  y += 16;
}

void drawPasskey() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(64); }  // drawPasskey
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  int y = 96;
  cnDraw(&spr, 8, y, cnStr(T_PK_TITLE), p.textDim, p.bg);
  y += 44;
  // 六位配对码保留 TFT_eSPI 的内置字体 —— cnDraw 不支持缩放，8px 宽的数字
  // 在这里太小；数字是纯 ASCII，内置字体会适。
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, y);
  spr.print(b);
  spr.setTextSize(1);
  cnDraw(&spr, 8, 224, cnStr(T_PK_HINT), p.textDim, p.bg);
}

void drawInfo() {
  { extern void dbgStageSet(uint32_t); dbgStageSet(65); }  // drawInfo
  const Palette& p = characterPalette();
  // 内容起始线。原版为 70（= 宠物缩略条高度），画布由 240 加高到 320 后
  // 随宠物区一起下移 40，与 character.cpp 的 PEEK_TOP 保持一致。
  const int TOP = 110;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  int y = TOP + 2;
  uint16_t col = p.textDim;
  // 一行正文：x=4、行高 16（= 字模高度）。颜色改用 col 变量切换 ——
  // 绘制已全部走 cnDraw，不再有 spr.setTextColor/setCursor/print 那套状态。
  // 注意：无参数的文案里不能出现裸 '%'，它会被 vsnprintf 当成格式符。
  // 字模本身 16 行满格，行距为 0 时上下两行会贴在一起。+1px 留缝；
  // 设备页有 9 行正文，这是 320 高度内能加的上限（再多会溢出）。
  const int LINE_H = 17;
  auto ln = [&](const char* fmt, ...) {
    char b[64]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    cnDraw(&spr, 4, y, b, col, p.bg); y += LINE_H;
  };
  // 分组标题（LINK / 系统 / 蓝牙状态）下的字段：缩进 24px 拉开层级，
  // 与按键页动作行一致；配对步骤再缩一级到 48px。
  const int SUB_X = 4 + 24;
  const int SUB2_X = 4 + 48;
  auto sub = [&](const char* fmt, ...) {
    char b[64]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    cnDraw(&spr, SUB_X, y, b, col, p.bg); y += LINE_H;
  };
  auto sub2 = [&](const char* fmt, ...) {
    char b[64]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    cnDraw(&spr, SUB2_X, y, b, col, p.bg); y += LINE_H;
  };
  auto gap = [&]() { y += 8; };

  if (infoPage == 0) {
    _infoHeader(p, y, T_SEC_ABOUT, infoPage);
    col = p.textDim;
    sub(cnStr(T_AB_1));         // 一句一行，正文缩进到标题下
    sub(cnStr(T_AB_2));
    gap();
    col = p.text;
    sub(cnStr(T_AB_3));
    col = p.textDim;
    sub(cnStr(T_AB_4));

  } else if (infoPage == 1) {
    _infoHeader(p, y, T_SEC_BUTTONS, infoPage);
    // 本板三键。原版的两键 A/B 与 Power 说明已不适用（本板无独立电源键，
    // 电源键语义由 UP 承担，见 src/hal/M5StickCPlus.h 的键位映射说明）。
    // 键名顶格、说明缩进，靠缩进和颜色分组。
    col = p.text;    ln(cnStr(T_BT_OK));
    col = p.textDim; ln(cnStr(T_BT_OK_1));
    col = p.text;    ln(cnStr(T_BT_DN));
    col = p.textDim; ln(cnStr(T_BT_DN_1));
    col = p.text;    ln(cnStr(T_BT_UP));
    col = p.textDim; ln(cnStr(T_BT_UP_1));
    col = p.text;    ln(cnStr(T_BT_HOLD));
    col = p.textDim; ln(cnStr(T_BT_HOLD_1));

  } else if (infoPage == 2) {
    _infoHeader(p, y, T_SEC_CLAUDE, infoPage);
    col = p.textDim;
    sub(cnStr(T_CL_SESS), tama.sessionsTotal);
    sub(cnStr(T_CL_RUN), tama.sessionsRunning);
    sub(cnStr(T_CL_WAIT), tama.sessionsWaiting);
    gap();
    col = p.text;
    ln(cnStr(T_CL_LINK));
    col = p.textDim;
    sub(cnStr(T_CL_VIA), scenarioLabel(dataScenarioName()));
    sub(cnStr(T_CL_BLE), !bleConnected() ? "-" : cnStr(bleSecure() ? T_SC_ENC : T_SC_OPEN));
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    sub(cnStr(T_CL_LAST), (unsigned long)age);
    sub(cnStr(T_CL_STATE), cnStr(STATE_IDS[activeState]));

  } else if (infoPage == 3) {
    _infoHeader(p, y, T_SEC_DEVICE, infoPage);

    int vBat_mV = (int)(M5.Axp.GetBatVoltage() * 1000);
    int iBat_mA = (int)M5.Axp.GetBatCurrent();
    int vBus_mV = (int)(M5.Axp.GetVBusVoltage() * 1000);
    int pct = (vBat_mV - 3200) / 10;   // (v-3.2)/(4.2-3.2)*100 = (v-3.2)*100 = (mv-3200)/10
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    // 电量百分比靠亮色突出（同一套字模无法再靠字号拉开层次）。
    // "100%" 4 字符 × 8px = 32px，从 x=4 排到 36，状态词从 72 起不会撞。
    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    cnDraw(&spr, SUB_X, y, pbuf, p.text, p.bg);
    // 状态词相对百分比保持 68px 间距（原 4 → 72），避免与 4 位百分比相撞
    cnDraw(&spr, SUB_X + 68, y, cnStr(full ? T_DV_FULL : charging ? T_DV_CHG : usb ? T_DV_USB : T_DV_BAT),
           full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    y += 20;

    col = p.textDim;
    sub(cnStr(T_DV_VBAT), vBat_mV/1000, (vBat_mV%1000)/10);
    sub(cnStr(T_DV_CUR), iBat_mA);
    // usb 行恒不显示：本板无 VBUS 检知，vBus_mV 恒为 0。
    if (usb) sub(cnStr(T_DV_VBUS), vBus_mV/1000, (vBus_mV%1000)/10);

    col = p.text;
    ln(cnStr(T_DV_SYS));
    col = p.textDim;
    // owner 名来自主机、长度不可控，截到 10 个字符保证不撑破行宽。
    if (ownerName()[0]) sub(cnStr(T_DV_OWNER), ownerName());
    uint32_t up = millis() / 1000;
    sub(cnStr(T_DV_UP), up / 3600, (up / 60) % 60);
    sub(cnStr(T_DV_HEAP), ESP.getFreeHeap() / 1024);
    sub(cnStr(T_DV_BRT), brightLevel);
    sub(cnStr(T_DV_BT), cnStr(settings().bt ? (dataBtActive() ? T_BT_LINKED : T_VAL_ON) : T_VAL_OFF));
    sub(cnStr(T_DV_TEMP), (int)M5.Axp.GetTempInAXP192());

  } else if (infoPage == 4) {
    _infoHeader(p, y, T_SEC_BT, infoPage);
    bool linked = settings().bt && dataBtActive();

    cnDraw(&spr, 4, y, cnStr(linked ? T_BT_LINKED : (settings().bt ? T_BL_DISC : T_VAL_OFF)),
           linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    y += 20;

    col = p.text;
    sub("%s", btName);
    col = p.textDim;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    sub("%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      sub(cnStr(T_BL_LAST), (unsigned long)age);
    } else if (settings().bt) {
      col = p.text;
      sub(cnStr(T_BL_PAIR));
      col = p.textDim;
      sub2(cnStr(T_BL_P1));
      sub2(cnStr(T_BL_P2));
      sub2(cnStr(T_BL_P3));
      sub2(cnStr(T_BL_P4));
    }

  } else {
    _infoHeader(p, y, T_SEC_CREDITS, infoPage);
    col = p.textDim;
    ln(cnStr(T_CR_BY));
    col = p.text;
    sub("Jason Liu");           // 人名不译
    gap();
    col = p.textDim;
    ln(cnStr(T_CR_HW));
    sub(cnStr(T_CR_CHIP));
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
  // 改走 cnDraw 后每行占 16px（原来是内置 6x8 字体），78px 装不下
  // 「状态行 + 工具名 + 两行提示 + 按钮行」共 5 行，故加高到 96。
  // 审批页覆盖在主屏内容区之上，上面本来就没有别的内容，加高无副作用。
  const int AREA = 96;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  cnPrint(&spr, 4, H - AREA + 4, (waited >= 10 ? HOT : p.textDim), p.bg,
          cnStr(T_AP_WAIT), (unsigned long)waited);

  // 工具名与参数提示都来自主机（claude-desktop 下发），设备不知道语义、
  // 翻不了，只能原样显示；长度也不可控，按屏宽截断 —— 8px/字符时
  // 一行能放 (240-8)/8 = 29 个字符。
  char buf[64];
  snprintf(buf, sizeof(buf), "%.29s", tama.promptTool);
  cnDraw(&spr, 4, H - AREA + 22, buf, p.text, p.bg);

  snprintf(buf, sizeof(buf), "%.29s", tama.promptHint);
  cnDraw(&spr, 4, H - AREA + 40, buf, p.textDim, p.bg);
  if (strlen(tama.promptHint) > 29) {
    snprintf(buf, sizeof(buf), "%.29s", tama.promptHint + 29);
    cnDraw(&spr, 4, H - AREA + 56, buf, p.textDim, p.bg);
  }

  if (responseSent) {
    cnDraw(&spr, 4, H - 20, cnStr(T_AP_SENT), p.textDim, p.bg);
  } else {
    cnDraw(&spr, 4, H - 20, cnStr(T_AP_OK), GREEN, p.bg);
    const char* deny = cnStr(T_AP_NO);
    cnDraw(&spr, W - cnWidth(deny) - 4, H - 20, deny, HOT, p.bg);
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
  int y = TOP + 16;

  // 三个指标标签与右侧图形条同一行。图形条起点按**两种语言里较宽的标签**定：
  // 中文标签 2 字 = 32px，英文 "energy" = 48px，取后者 —— 6 + 48 = 54，
  // 留 6px 间隙后从 60 起。标签用 y-6 让 16px 字高居中于图形中线（约 y+2）。
  const int BAR_X = 60;
  cnDraw(&spr, 6, y - 6, cnStr(T_PT_MOOD), p.textDim, p.bg);
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(BAR_X + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  cnDraw(&spr, 6, y - 6, cnStr(T_PT_FED), p.textDim, p.bg);
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = BAR_X + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  cnDraw(&spr, 6, y - 6, cnStr(T_PT_EN), p.textDim, p.bg);
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = BAR_X + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  // 徽章宽度按文字算：最多 "Lv 12" = 5 字符 × 8px = 40px，两侧各留 10px。
  y += 24;
  spr.fillRoundRect(6, y - 2, 60, 20, 4, p.body);
  cnPrint(&spr, 16, y, p.bg, p.body, cnStr(T_PT_LV), stats().level);

  y += 24;
  cnPrint(&spr, 6, y, p.textDim, p.bg, cnStr(T_PT_APPR), stats().approvals);
  cnPrint(&spr, 6, y + 16, p.textDim, p.bg, cnStr(T_PT_DENY), stats().denials);
  uint32_t nap = stats().napSeconds;
  cnPrint(&spr, 6, y + 32, p.textDim, p.bg, cnStr(T_PT_NAP), nap/3600, (nap/60)%60);
  // 对齐靠文案里自带的尾随空格（"tokens   " / "累计    "）—— 两种语言的标签
  // 宽度不同，在这里补空格反而要跟着语言改。
  auto tokFmt = [&](uint16_t id, uint32_t v, int yPx) {
    if (v >= 1000000)   cnPrint(&spr, 6, yPx, p.textDim, p.bg, "%s%lu.%luM", cnStr(id), v/1000000, (v/100000)%10);
    else if (v >= 1000) cnPrint(&spr, 6, yPx, p.textDim, p.bg, "%s%lu.%luK", cnStr(id), v/1000, (v/100)%10);
    else                cnPrint(&spr, 6, yPx, p.textDim, p.bg, "%s%lu", cnStr(id), v);
  };
  tokFmt(T_PT_TOK, stats().tokens, y + 48);
  tokFmt(T_PT_TODAY, tama.tokensToday, y + 64);
}

static void drawPetHowTo(const Palette& p) {
  { extern void dbgStageSet(uint32_t); dbgStageSet(68); }  // drawPetHowTo
  // 内容起始线。原版为 70（= 宠物缩略条高度），画布由 240 加高到 320 后
  // 随宠物区一起下移 40，与 character.cpp 的 PEEK_TOP 保持一致。
  const int TOP = 110;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  int y = TOP + 2;
  // 行高 16（= 字模高度）。标题（心情/饱食/精力）用 p.body 亮色顶格，
  // 其下的说明用 p.textDim 灰色并缩进 24px，形成两级层次。
  auto ln = [&](uint16_t c, uint16_t id) {
    cnDraw(&spr, 6, y, cnStr(id), c, p.bg); y += 16;
  };
  auto sub = [&](uint16_t id) {
    cnDraw(&spr, 6 + 24, y, cnStr(id), p.textDim, p.bg); y += 16;
  };

  y += 8;  // room for the PET header drawn by drawPet()

  // 12 行 × 16px = 192px，从 y=120 排到 312，刚好铺满 320 的内容区，
  // 因此不再留行间空行 —— 靠 p.body / p.textDim 的明暗差分组。
  // 若加条目需重排：内容区只剩 8px 余量（tools/verification/layout_check.py 会报）。
  ln(p.body,   T_PT_MOOD);
  sub(T_PT_HOW_M);
  sub(T_PT_HOW_M2);
  ln(p.body,   T_PT_FED);
  sub(T_PT_HOW_F);
  ln(p.body,   T_PT_EN);
  // 上游这里写的是 "face-down to nap / refills to full"，但本板没有加速度计
  // （HalImu::getAccelData 恒返回平放朝上，isFaceDown() 要求 az < -0.7 恒不成立），
  // 打盹永不触发，精力只降不升 —— 照抄会让人去找一个永远不会发生的动作。
  sub(T_PT_HOW_E);
  sub(T_PT_HOW_I);
  sub(T_PT_HOW_I2);
  sub(T_PT_HOW_D);
  sub(T_PT_HOW_O);
  sub(T_PT_HOW_H);
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
  if (ownerName()[0]) snprintf(title, sizeof(title), cnStr(T_PT_TITLE), ownerName(), petName());
  else                snprintf(title, sizeof(title), "%s", petName());
  // 标题画在宠物之上，越过宠物图形的左边缘就会压上去，故按那条线限宽。
  // 宠物屏走 peek 模式（applyDisplayMode 里 displayMode != DISP_NORMAL 即 peek），
  // GIF 缩半居中 —— bufo 的 96x100 变成 48x50，占 x=96..144。
  // 名字长度不可控（owner 由主机下发），超宽时只留宠物名 —— 它才是主角。
  // 注意限的是**像素宽**而不是字符数：中文一个字 16px，英文一个 8px。
  // cnDraw 不支持缩放、中文字模也没有小号档，所以这里只做取舍不做缩字。
  int gifLeft = characterGifLeft();
  if (gifLeft <= 0 || gifLeft > W) gifLeft = 96;   // 未加载 GIF 时按实测值兜底
  if (ownerName()[0] && cnWidth(title) > gifLeft - 8)
    snprintf(title, sizeof(title), "%s", petName());
  cnDraw(&spr, 4, y, title, p.text, p.bg);
  char n[16];
  snprintf(n, sizeof(n), "%u/%u", petPage + 1, PET_PAGES);
  cnDraw(&spr, W - 40, y + 4, n, p.textDim, p.bg);   // 与 16px 高的标题垂直居中
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
  cnSetLang(settings().lang);   // 必须在任何绘制之前生效
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
    // 中文化后不再用 TFT_eSPI 的 drawString —— 它画不了汉字，而这段文案
    // 两种语言都要出。代价是失去 size 3 的大字效果（cnDraw 不支持缩放，
    // 中文字模也没有小号/大号档），这里是 16px 高居中。画面本身很空，
    // 两行 16px 文字仍看得清。
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), cnStr(T_HI_OWNER), ownerName());
      cnDrawCenter(&spr, W/2, H/2 - 30, line, p.text, p.bg);
      cnDrawCenter(&spr, W/2, H/2 + 10, petName(), p.body, p.bg);
    } else {
      // First boot, no owner pushed yet — say hi.
      cnDrawCenter(&spr, W/2, H/2 - 30, cnStr(T_HI_HELLO), p.body, p.bg);
      cnDrawCenter(&spr, W/2, H/2 + 10, cnStr(T_HI_BUDDY), p.textDim, p.bg);
    }
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
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      cnDraw(&spr, 8, 130, cnStr(T_XF_INSTALL), p.textDim, p.bg);
      char b[32];
      snprintf(b, sizeof(b), "%luK / %luK", done/1024, total/1024);
      cnDraw(&spr, 8, 146, b, p.textDim, p.bg);
      int barW = W - 16;
      spr.drawRect(8, 166, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 167, fill - 1, 6, p.body);
      }
    } else {
      cnDraw(&spr, 8, 140, cnStr(T_NO_CHAR), p.textDim, p.bg);
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
