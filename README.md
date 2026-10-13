# ClaudePet

把 Anthropic 的 [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
移植到 **FoloToy AI Passport**（ESP32-C3）的固件。

原版面向 M5StickC Plus（ESP32 双核 / AXP192 / ST7789v2 135×240 / MPU6886）。
本项目保留其全部应用逻辑与宠物资源，只替换底层硬件抽象。

## 移植原则

**渲染层与宠物资源与原版逐字节一致。** 具体来说：

| 文件 | 状态 |
| --- | --- |
| `src/buddies/`（18 种宠物 × 7 状态，约 4000 行） | 未改动 |
| `src/buddy.cpp` / `buddy.h` / `buddy_common.h` | 未改动 |
| `src/character.cpp` / `character.h` | 未改动 |
| `src/ble_bridge.cpp` / `ble_bridge.h` | 未改动 |
| `src/data.h` / `src/xfer.h` | 未改动 |
| `src/main.cpp` | 少量改动（见下） |
| `src/stats.h` | 1 处类型修正 |

原版渲染接口本就是 `buddyRenderTo(TFT_eSPI*, ...)`，而非面向 M5，所以移植面很小。

## 硬件

| 项目 | 规格 |
| --- | --- |
| MCU | ESP32-C3 (QFN32) rev v1.1，单核 RISC-V @160MHz |
| Flash | 8MB，**无 PSRAM** |
| 屏幕 | ST7789P3，240 × 320 竖屏，4 线 SPI |
| 按键 | 三个键共用一个 ADC 分压脚（GPIO0） |
| 电量计 | CW2017（I2C，地址 0x63） |
| 音频 | ES8311 codec（本版未接入） |
| 无线 | Wi-Fi 2.4GHz + BLE 5，不支持蓝牙经典 |
| 控制台 | USB-Serial-JTAG（GPIO18/19） |

引脚与电气参数取自 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)
的 `components/bsp/include/bsp_pins.h`。**仅取硬件事实，未沿用其 ESP-IDF 代码结构。**

## 硬件抽象层的做法

原版依赖 `M5.Lcd` / `M5.Axp` / `M5.BtnA`、`BtnB` / `M5.Imu` / `M5.Beep` / `M5.Rtc`。
本项目**不重写上层调用**，而是提供同名同签名的门面（`src/hal/M5StickCPlus.h`），
经 `-Isrc/hal` 让上层原有的 `#include <M5StickCPlus.h>` 解析到它。因此：

- 上层四个包含该头文件的源文件，**包含语句一个字都没改**
- `main.cpp` 里的 M5 调用（45 处）**一行都没重写**

这也是宠物代码能保持原封不动的原因。

## 相对原版的改动

| 位置 | 改动 | 原因 |
| --- | --- | --- |
| `main.cpp` | 新增 `CANVAS_X/CANVAS_Y`，两处 `pushSprite` 加偏移 | 135×240 画布居中放到 240×320 屏上 |
| `main.cpp` | `LED_PIN` 置 -1 并加保护 | 原版 LED 在 GPIO10，本板该脚是 I2C SDA，驱动成数字口会破坏 I2C 总线 |
| `stats.h` | `min()` 第二个实参加 `(uint32_t)` | xtensa 上 `uint32_t` 是 `unsigned int`，riscv32 上是 `unsigned long`，模板推导会失败 |
| `platformio.ini` | 换板、换驱动、去 M5 依赖 | — |
| `partitions.csv` | 新增 | 8MB Flash，app 3MB + LittleFS 5MB |
| `src/hal/` | 新增 | 硬件抽象层 |
| `src/hal/tft_c3_spi_fix.h` | 新增 | 修正 TFT_eSPI 在 C3 上的 SPI 基址缺陷，详见该文件 |

## 因硬件缺失而降级的功能

这些在原版存在，本板没有对应硬件，均已在代码注释中标注：

| 功能 | 原版依赖 | 本版行为 |
| --- | --- | --- |
| 摇一摇（dizzy） | MPU6886 | 失效（HAL 恒返回「平放朝上」） |
| 翻面打盹（nap） | MPU6886 | 失效 |
| 提示音 | 蜂鸣器 | 静默（Settings 里 Sound 开关仍可切换） |
| 审批 LED 闪烁 | GPIO10 红灯 | 停用（该脚在本板是 I2C SDA） |
| 硬关机 | AXP192 | 记录日志不执行（无可控电源轨；且 UP 键所在 GPIO0 同时是 ADC 分压脚，休眠唤醒不可靠） |

## 键位映射

原版两个键 + 一个电源键，本板三个键。映射时保持原版交互语义不变：

| 本板按键 | 对应原版 | 作用 |
| --- | --- | --- |
| `OK` | BtnA | 确认 / 审批通过 / 长按开菜单 |
| `DOWN` | BtnB | 滚动 / 审批拒绝 / 翻页 |
| `UP` | 电源键短按 | 切换熄屏 |

## 构建与烧录

需要 [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/)。

```bash
# 编译
pio run

# 烧录固件
pio run -t upload --upload-port COM4

# 烧录角色包（GIF 宠物，可选）
# 先把角色包目录放到 data/characters/<name>/，再：
pio run -t uploadfs --upload-port COM4
```

或用仓库自带的脚本（会把 `characters/<name>` 暂存到 `data/` 再烧录）：

```bash
python tools/flash_character.py characters/bufo
```

## 已知取舍与限制

1. **画面非满屏。** 逻辑画布仍是原版的 135×240，居中显示在 240×320 上，四周留黑边。
   满屏重排（240×320）是后续独立工作，需改动 `main.cpp` 的 204 处坐标引用
   与 `buddy_common.h` 的全部布局常量，且精灵图会从 64.8KB 涨到 150KB。
2. **时钟页横屏时内容锚在左上**，不居中。原因同上。
3. **角色包上限 1.8MB**（原版限制，未改）。本板 LittleFS 分区 5MB，余量充足。

## 验证状态

已在真实设备（MAC `98:C3:77:F4:13:86`，`COM4`）上验证：

| 项目 | 结果 |
| --- | --- |
| 编译（RAM 22.2% / Flash 38.4%） | 通过 |
| 板级初始化：屏幕、背光、CW2017、三键 | 通过 |
| 屏幕点亮且颜色正常 | 通过 |
| LittleFS 挂载 + GIF 角色包加载与解码播放 | 通过 |
| BLE 广播 `Claude-*` 且含 NUS 服务 UUID | 通过 |
| 与 Claude Desktop 配对、状态切换、审批流程 | **未验证** |

未验证项需要 Claude Desktop 开启开发者模式
（**Help → Troubleshooting → Enable Developer Mode**）后配合实测。

## 许可证

MIT，Copyright 2026 Anthropic, PBC。全文见 [LICENSE](LICENSE)。
上游仓库：<https://github.com/anthropics/claude-desktop-buddy>
