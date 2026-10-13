// FoloToy AI Passport (ESP32-C3) 硬件抽象层实现。
//
// 对应上层看到的 M5StickC Plus 门面（见 M5StickCPlus.h）。这里只实现上层
// 用到的能力，缺失硬件的行为在各自注释里说明。

#include "M5StickCPlus.h"

#include <Arduino.h>
#include <Wire.h>

#include "hal_pins.h"

// ===========================================================================
// I2C 与 CW2017 电量计
// ===========================================================================
static bool s_i2cReady = false;
static bool s_cwPresent = false;

static bool cwRead(uint8_t reg, uint8_t* buf, size_t n) {
  if (!s_i2cReady) return false;
  Wire.beginTransmission(HAL_CW2017_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)HAL_CW2017_ADDR, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)Wire.read();
  return true;
}

static bool cwWrite(uint8_t reg, uint8_t val) {
  if (!s_i2cReady) return false;
  Wire.beginTransmission(HAL_CW2017_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static void i2cInit() {
  Wire.begin(HAL_I2C_SDA, HAL_I2C_SCL, HAL_I2C_FREQ_HZ);
  s_i2cReady = true;

  // 版本寄存器能应答即代表芯片在位（同 bsp_battery.c 的判据）。
  uint8_t ver = 0;
  s_cwPresent = cwRead(HAL_CW_REG_VERSION, &ver, 1);
  if (s_cwPresent) {
    cwWrite(HAL_CW_REG_CONFIG, 0x00);  // 退出睡眠，进正常模式
    delay(100);                        // 等首次 SOC 计算完成
    Serial.printf("[hal] CW2017 在线 VERSION=0x%02X\n", ver);
  } else {
    Serial.println("[hal] CW2017 无应答，电量将显示为不可用");
  }
}

// ===========================================================================
// 背光：LEDC PWM
// ===========================================================================
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define HAL_LEDC_ATTACH(pin, freq, res) ledcAttach((pin), (freq), (res))
#define HAL_LEDC_WRITE(pin, duty) ledcWrite((pin), (duty))
#else
#define HAL_LEDC_CH 0
#define HAL_LEDC_ATTACH(pin, freq, res) \
  do { ledcSetup(HAL_LEDC_CH, (freq), (res)); ledcAttachPin((pin), HAL_LEDC_CH); } while (0)
#define HAL_LEDC_WRITE(pin, duty) ledcWrite(HAL_LEDC_CH, (duty))
#endif

static void backlightInit() {
  HAL_LEDC_ATTACH(HAL_BL_PIN, HAL_BL_FREQ_HZ, HAL_BL_RES_BITS);
  HAL_LEDC_WRITE(HAL_BL_PIN, HAL_BL_MAX_DUTY);
}

// ===========================================================================
// 按键：GPIO0 单脚 ADC 分压，三键
// ===========================================================================
struct BtnState {
  bool down;
  bool pressed;      // 本轮 update() 内的按下沿
  bool released;     // 本轮 update() 内的抬起沿
  uint32_t downSince;
};

static BtnState s_btn[3];
static uint8_t s_stable = HAL_BTN_NONE;  // 当前去抖后的稳定档位
static uint8_t s_candidate = HAL_BTN_NONE;
static uint8_t s_candidateCount = 0;
static bool s_powerPressPending = false;  // UP 短按抬起，待 GetBtnPress() 取走

// 电压 → 键位。窗口边界取相邻档中点，与 bsp_pins.h 的 BSP_BTN_MV_TABLE 一致。
static uint8_t classifyButton(uint32_t mv) {
  if (mv <= HAL_BTN_UP_MAX_MV) return HAL_BTN_IDX_UP;
  if (mv <= HAL_BTN_DOWN_MAX_MV) return HAL_BTN_IDX_DOWN;
  if (mv <= HAL_BTN_OK_MAX_MV) return HAL_BTN_IDX_OK;
  return HAL_BTN_NONE;  // 松开态约 3300mV
}

static void buttonsInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  analogSetAttenuation(ADC_11db);  // 需要读到约 3.3V，必须用 11dB 衰减
#else
  analogSetPinAttenuation(HAL_BTN_PIN, ADC_11db);
#endif
  pinMode(HAL_BTN_PIN, INPUT);  // 外部 10k 上拉，不能用内部上拉
}

static void buttonsUpdate() {
  const uint8_t raw = classifyButton(analogReadMilliVolts(HAL_BTN_PIN));

  // 去抖：连续 3 次同一档位才认可。
  if (raw == s_candidate) {
    if (s_candidateCount < 3) ++s_candidateCount;
  } else {
    s_candidate = raw;
    s_candidateCount = 1;
  }
  if (s_candidateCount < 3) return;

  const uint8_t now = s_candidate;
  const uint32_t ms = millis();

  for (uint8_t i = 0; i < 3; ++i) {
    const bool down = (now == i);
    BtnState& b = s_btn[i];
    b.pressed = false;
    b.released = false;
    if (down && !b.down) {
      b.down = true;
      b.pressed = true;
      b.downSince = ms;
    } else if (!down && b.down) {
      b.down = false;
      b.released = true;
      if (i == HAL_BTN_IDX_UP) s_powerPressPending = true;  // UP 短按 = 熄屏/亮屏
    }
  }
  s_stable = now;
}

bool HalButton::isPressed() const { return s_btn[_idx].down; }
bool HalButton::wasPressed() const { return s_btn[_idx].pressed; }
bool HalButton::wasReleased() const { return s_btn[_idx].released; }
bool HalButton::pressedFor(uint32_t ms) const {
  return s_btn[_idx].down && (millis() - s_btn[_idx].downSince) >= ms;
}

// ===========================================================================
// 电源（原 AXP192 门面）
// ===========================================================================
void HalPower::ScreenBreath(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  HAL_LEDC_WRITE(HAL_BL_PIN, (uint32_t)percent * HAL_BL_MAX_DUTY / 100);
}

void HalPower::SetLDO2(bool on) {
  // 原版 LDO2 控制 LCD 屏电。本板屏电硬接 3.3V 不可控，映射为背光开关，
  // 上层「熄屏/亮屏」的观感一致。
  HAL_LEDC_WRITE(HAL_BL_PIN, on ? HAL_BL_MAX_DUTY : 0);
}

void HalPower::PowerOff() {
  // 本板无可控电源轨。原版靠 AXP192 断电，这里无法等效实现。
  // 不做 esp_deep_sleep：UP 键所在 GPIO0 同时是 ADC 分压脚，
  // 唤醒配置若不可靠会让设备看起来「变砖」，故本期只记录日志。
  Serial.println("[hal] PowerOff 被调用，但本板无可控电源轨，未执行");
}

float HalPower::GetVBusVoltage() {
  return 0.0f;  // 本板无 VBUS 检知电路，无法判断是否在 USB 供电
}

float HalPower::GetBatVoltage() {
  if (!s_cwPresent) return 0.0f;
  uint8_t b[2] = {0, 0};
  if (!cwRead(HAL_CW_REG_VCELL_H, b, 2)) return 0.0f;
  const uint32_t raw = (((uint32_t)b[0] << 8) | b[1]) & 0x3FFF;  // 低 14 位
  return (float)((raw * 3125) / 10000) / 1000.0f;                // mV → V
}

float HalPower::GetBatCurrent() {
  return 0.0f;  // CW2017 不提供电流读数
}

float HalPower::GetTempInAXP192() {
  return 0.0f;  // 无 PMIC 温度传感器
}

uint8_t HalPower::GetBtnPress() {
  // 原版电源键短按返回 0x02。本板无独立电源键，改由 UP 键承担。
  if (s_powerPressPending) {
    s_powerPressPending = false;
    return 0x02;
  }
  return 0x00;
}

// ===========================================================================
// 软件时钟
// ===========================================================================
static bool s_clkValid = false;
static int s_year = 2026, s_mon = 1, s_day = 1;
static int s_hour = 0, s_min = 0, s_sec = 0, s_wday = 0;
static uint32_t s_clkBaseMs = 0;

static bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static int daysInMonth(int y, int m) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && isLeap(y)) return 29;
  return (m >= 1 && m <= 12) ? d[m - 1] : 30;
}

static void advanceOneSecond() {
  if (++s_sec < 60) return;
  s_sec = 0;
  if (++s_min < 60) return;
  s_min = 0;
  if (++s_hour < 24) return;
  s_hour = 0;
  s_wday = (s_wday + 1) % 7;
  if (++s_day <= daysInMonth(s_year, s_mon)) return;
  s_day = 1;
  if (++s_mon <= 12) return;
  s_mon = 1;
  ++s_year;
}

// 只在被读取时补上经过的秒数，避免占用定时器。
static void clockTick() {
  if (!s_clkValid) return;
  const uint32_t now = millis();
  uint32_t elapsed = (now - s_clkBaseMs) / 1000;
  if (elapsed == 0) return;
  s_clkBaseMs += elapsed * 1000;
  if (elapsed > 3600) elapsed = 3600;  // 异常情况下的一次性上限
  while (elapsed--) advanceOneSecond();
}

void HalRtc::SetTime(RTC_TimeTypeDef* t) {
  if (!t) return;
  s_hour = t->Hours;
  s_min = t->Minutes;
  s_sec = t->Seconds;
  s_clkBaseMs = millis();
  s_clkValid = true;
}

void HalRtc::SetDate(RTC_DateTypeDef* d) {
  if (!d) return;
  s_wday = d->WeekDay % 7;
  s_mon = d->Month;
  s_day = d->Date;
  s_year = d->Year;
  s_clkBaseMs = millis();
  s_clkValid = true;
}

void HalRtc::GetTime(RTC_TimeTypeDef* t) {
  if (!t) return;
  clockTick();
  t->Hours = (uint8_t)s_hour;
  t->Minutes = (uint8_t)s_min;
  t->Seconds = (uint8_t)s_sec;
}

void HalRtc::GetDate(RTC_DateTypeDef* d) {
  if (!d) return;
  clockTick();
  d->WeekDay = (uint8_t)s_wday;
  d->Month = (uint8_t)s_mon;
  d->Date = (uint8_t)s_day;
  d->Year = (uint16_t)s_year;
}

// ===========================================================================
// 加速度计占位
// ===========================================================================
void HalImu::getAccelData(float* ax, float* ay, float* az) {
  // 恒返回「平放朝上」，使依赖 IMU 的翻面打盹与摇一摇自然失效。
  if (ax) *ax = 0.0f;
  if (ay) *ay = 0.0f;
  if (az) *az = 1.0f;
}

// ===========================================================================
// 门面本体
// ===========================================================================
HalM5 M5;

void HalM5::begin() {
  Serial.begin(115200);

  Lcd.init();
  Lcd.setRotation(0);
  Lcd.fillScreen(TFT_BLACK);

  backlightInit();
  buttonsInit();
  i2cInit();

  // 键位映射见 M5StickCPlus.h 的说明：OK -> BtnA，DOWN -> BtnB，UP -> 电源键等价物。
  BtnA.attach(HAL_BTN_IDX_OK);
  BtnB.attach(HAL_BTN_IDX_DOWN);

  Serial.printf("[hal] 显示就绪 %dx%d，背光 GPIO%d\n", HAL_LCD_W, HAL_LCD_H, HAL_BL_PIN);
  Serial.println("[hal] 按键就绪: GPIO0 ADC 三键分压 (UP/DOWN/OK)");
}

void HalM5::update() {
  buttonsUpdate();
}
