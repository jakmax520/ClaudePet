// M5StickC Plus 兼容垫片
//
// 目的：让基于 claude-desktop-buddy 的上层代码不做任何改动即可编译并运行在
// FoloToy AI Passport（ESP32-C3）上。
//
// 原版面向 M5StickC Plus，硬件依赖集中在 M5.Lcd / M5.Axp / M5.BtnA / M5.BtnB /
// M5.Imu / M5.Beep / M5.Rtc。本文件提供同名同签名的门面，底层换成
// hal.cpp 里的真实驱动。上层四个包含本头文件的源文件（main.cpp、data.h 经
// xfer.h、buddy.cpp、character.cpp）无需修改包含语句。
//
// 只实现上层实际用到的 API 子集，不做完整模拟。缺失硬件对应的行为见各成员注释。
#pragma once

#include <Arduino.h>
// FS.h 必须先于 TFT_eSPI.h：TFT_eSPI 在启用 SMOOTH_FONT 时会先 #define
// FS_NO_GLOBALS 再包含 FS.h，使 fs::File 不再进入全局命名空间。放在前面可以
// 让 FS.h 的头文件保护先以「全局可见」的状态锁定，避免这类顺序陷阱。
#include <FS.h>
#include <TFT_eSPI.h>

#include "hal_pins.h"

// M5StickC Plus 的 In_eSPI.h 提供了一批不带前缀的颜色名，而 TFT_eSPI 只有
// TFT_ 前缀版本。上层 main.cpp 用到了 GREEN / RED，在此补齐。
#ifndef GREEN
#define GREEN 0x07E0
#endif
#ifndef RED
#define RED 0xF800
#endif

// ---------------------------------------------------------------------------
// M5 的 RTC 结构体。
// 字段名与顺序必须与上层用法一致：data.h 用 3 项/4 项聚合初始化，
// main.cpp 访问 Hours/Minutes/Seconds 与 WeekDay/Month/Date/Year。
// ---------------------------------------------------------------------------
typedef struct {
  uint8_t Hours;
  uint8_t Minutes;
  uint8_t Seconds;
} RTC_TimeTypeDef;

typedef struct {
  uint8_t WeekDay;
  uint8_t Month;
  uint8_t Date;
  uint16_t Year;
} RTC_DateTypeDef;

// ---------------------------------------------------------------------------
// 按键角色映射
//
// 上游是面向 M5StickC Plus 两键设计，且两个键语义**不对称**：
//   BtnA = 移动/下一页（菜单里移动选择、页面间切换、长按开菜单）
//   BtnB = 确认/执行   （菜单里执行选中项、Info/Pet 翻页、审批拒绝）
// 另有电源键短按熄屏。
//
// 本板三个键，若照搬（OK->BtnA、DOWN->BtnB）会出现「OK 只是移动选择、
// DOWN 才是确认」的别扭手感。故按三键直觉映射：
//
//   DOWN -> BtnA   下移 / 下一页（长按不开菜单，见下）
//   OK   -> BtnB   确认 / 执行 / 审批**批准**
//   UP   -> 电源键等价物（短按切换熄屏）—— 见 HalAxp::GetBtnPress()
//
// 注意：BtnA 与 BtnB 在审批页原本分别是「批准」与「拒绝」，此处对调后需要在
// main.cpp 里把这两段分支互换，否则会变成「OK 拒绝、DOWN 批准」。
// ---------------------------------------------------------------------------
#define HAL_BTN_IDX_UP 0
#define HAL_BTN_IDX_DOWN 1
#define HAL_BTN_IDX_OK 2

class HalButton {
 public:
  void attach(uint8_t idx) { _idx = idx; }
  bool isPressed() const;             // 当前是否按住
  bool wasPressed() const;            // 本轮 update() 内是否出现按下沿
  bool wasReleased() const;           // 本轮 update() 内是否出现抬起沿
  bool pressedFor(uint32_t ms) const; // 是否已连续按住 >= ms

 private:
  uint8_t _idx = 0;
};

// ---------------------------------------------------------------------------
// 显示。原版 M5.Lcd 是 TFT_eSPI 的子类，且 buddyRenderTo()/characterRenderTo()
// 接受的正是 TFT_eSPI*，所以这里直接继承即可，凡上层用到的绘制 API 全部可用。
// ---------------------------------------------------------------------------
struct HalLcd : public TFT_eSPI {};

// ---------------------------------------------------------------------------
// 电源。原版是 AXP192 PMIC。本板没有 PMIC，只有 CW2017 电量计，因此：
//   ScreenBreath  -> LEDC 占空比调光（保留 0..100 的入参语义）
//   SetLDO2       -> 原版用于屏电开关，这里映射为背光开关
//   PowerOff      -> 本板无可控电源轨，当前为记录日志的空实现（见 hal.cpp）
//   GetVBusVoltage-> 本板无 VBUS 检知电路，恒返回 0
//   GetBatCurrent -> CW2017 不提供电流，恒返回 0
//   GetTempInAXP192-> 无 PMIC 温度传感器，恒返回 0
//   GetBtnPress   -> 本板无独立电源键，映射到 UP 键短按（见键位映射说明）
// ---------------------------------------------------------------------------
struct HalPower {
  void ScreenBreath(int percent);
  void SetLDO2(bool on);
  void PowerOff();
  float GetVBusVoltage();
  float GetBatVoltage();
  float GetBatCurrent();
  float GetTempInAXP192();
  uint8_t GetBtnPress();
};

// ---------------------------------------------------------------------------
// 时钟。原版 M5StickC Plus 上 M5.Rtc 由 BM8563 提供（掉电不保持）。
// 本板无 RTC 芯片，改用基于 millis() 的软件时钟，
// 由协议里的 {"time":[epoch, tz]} 消息在每次连接时校准。
// ---------------------------------------------------------------------------
struct HalRtc {
  void GetTime(RTC_TimeTypeDef* t);
  void GetDate(RTC_DateTypeDef* d);
  void SetTime(RTC_TimeTypeDef* t);
  void SetDate(RTC_DateTypeDef* d);
};

// ---------------------------------------------------------------------------
// 加速度计。本板无 IMU。
// Init() 为空实现；getAccelData() 恒返回 (0, 0, 1.0)，语义为「平放朝上」。
//
// 这个取值让依赖 IMU 的两个功能自然失效而不需要改上层代码：
//   - 翻面打盹 isFaceDown() 要求 az < -0.7，恒不成立
//   - 摇一摇 require 加速度突变，恒不触发
// 时钟页的自动横竖屏判定会因此固定在默认朝向（可手动在 Settings 里锁定）。
// ---------------------------------------------------------------------------
struct HalImu {
  void Init() {}
  void getAccelData(float* ax, float* ay, float* az);
};

// ---------------------------------------------------------------------------
// 蜂鸣器。本板无蜂鸣器（音频走 ES8311 codec，本期未接入）。
// 三个方法均为空实现，因此提示音静默失效；Settings 里的 Sound 开关仍可切换，
// 只是听不到声音。
// ---------------------------------------------------------------------------
struct HalBeep {
  void begin() {}
  void tone(uint16_t freq, uint16_t dur) { (void)freq; (void)dur; }
  void update() {}
};

// ---------------------------------------------------------------------------
// 门面本体
// ---------------------------------------------------------------------------
struct HalM5 {
  HalLcd Lcd;
  HalPower Axp;
  HalRtc Rtc;
  HalImu Imu;
  HalBeep Beep;
  HalButton BtnA;
  HalButton BtnB;

  void begin();
  void update();
};

extern HalM5 M5;
