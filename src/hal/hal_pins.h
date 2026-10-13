// FoloToy AI Passport 硬件参数。
//
// 参数来源：FoloToy/ai-passport 的 components/bsp/include/bsp_pins.h。
// 仅取其中的硬件事实（引脚、电气参数），未沿用其 ESP-IDF 代码结构。
#pragma once

// ============================================================================
// 显示：ST7789P3 240x320，4 线 SPI
// 具体引脚经 platformio.ini 的 TFT_eSPI 构建标志配置，此处只放 HAL 自身要用的。
// ============================================================================
#define HAL_LCD_W 240
#define HAL_LCD_H 320

// 背光：LEDC PWM 调光
#define HAL_BL_PIN 21
#define HAL_BL_FREQ_HZ 5000
#define HAL_BL_RES_BITS 10
#define HAL_BL_MAX_DUTY ((1 << HAL_BL_RES_BITS) - 1)

// ============================================================================
// 按键：三键共用一个 ADC 引脚，靠分压电阻区分
//
//   上(UP)   : 0Ω    → 约   0 mV
//   下(DOWN) : 1k    → 约 300 mV
//   确定(OK) : 2.2k  → 约 595 mV
//   松开     : 上拉到约 3300 mV
//
// ★ 不可改用内部上拉：约 45kΩ 且精度差，会把三档全挤到 0~154mV 并随温漂重叠。
// ============================================================================
#define HAL_BTN_PIN 0
#define HAL_BTN_NONE 0xFF

// 每键电压窗口上界（mV），边界取相邻档中点。
// OK 键上界留宽到 1900，是为了和松开态的 3300mV 拉开距离。
#define HAL_BTN_UP_MAX_MV 150
#define HAL_BTN_DOWN_MAX_MV 447
#define HAL_BTN_OK_MAX_MV 1900

// ============================================================================
// I2C：ES8311(音频 codec) 与 CW2017(电量计) 共用一条总线
// 本期只用到 CW2017。
// ============================================================================
#define HAL_I2C_SDA 10
#define HAL_I2C_SCL 7
#define HAL_I2C_FREQ_HZ 100000
#define HAL_CW2017_ADDR 0x63   // 7 位地址

// CW2017 寄存器
#define HAL_CW_REG_VERSION 0x00   // 版本号，上电应答即代表芯片在位
#define HAL_CW_REG_VCELL_H 0x02   // 14bit 电压，V = raw * 312.5e-6
#define HAL_CW_REG_SOC_H 0x04     // 高字节 = 整数百分比
#define HAL_CW_REG_CONFIG 0x08    // 0x00 = 正常
