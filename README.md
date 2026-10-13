# ClaudePet

把 Anthropic 的 [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
移植到 **FoloToy AI Passport**（ESP32-C3）的固件。

原版面向 M5StickC Plus（ESP32 双核 / AXP192 / ST7789v2 135×240 / MPU6886）。
本项目保留其全部应用逻辑与宠物资源，只替换底层硬件抽象。

## 移植原则

**渲染层与宠物资源与原版逐字节一致。** 具体来说：

| 文件 | 状态 |
| --- | --- |
| `src/buddies/`（18 种宠物 × 7 状态，约 4000 行） | **逐字节未改** |
| `src/buddy.cpp` / `buddy.h` / `buddy_common.h` | 仅改 3 个布局常量的值 |
| `src/character.cpp` / `character.h` | 仅改宠物区高度常量 |
| `src/data.h` / `src/xfer.h` | 未改动 |
| `src/ble_bridge.cpp` | 改用 NimBLE 栈（见下） |
| `src/ble_bridge.h` | 未改动（对外接口不变） |
| `src/main.cpp` | 布局与菜单排版改动（见下） |
| `src/stats.h` | 1 处类型修正 |

原版渲染接口本就是 `buddyRenderTo(TFT_eSPI*, ...)`，而非面向 M5，所以移植面很小。
18 种宠物能保持原封不动，是因为它们引用的是 `BUDDY_*` 具名常量（约 250 处）
而非字面坐标 —— 改常量的值即可整体重排。

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
| `main.cpp` | 画布 `W/H` 由 135×240 改为 240×320；`LED_PIN` 置 -1 并加保护 | 满屏显示；原版 LED 在 GPIO10，本板该脚是 I2C SDA，驱动成数字口会破坏 I2C 总线 |
| `main.cpp` | 精灵图改在 BLE 之前分配 | 实测先起 BLE 再申请 150KB 会因连续块不足而失败，而彼时空闲堆仍有 160KB |
| `main.cpp` | 菜单/面板排版常量化并放大（见下） | 原尺寸按 135px 画布算，在 240px 上偏小 |
| `main.cpp` | 时钟/配对码/进度条的绝对 y 下移 40 | 画布由 240 加高到 320 |
| `buddy.cpp` | `BUDDY_X_CENTER` 67→120、`BUDDY_CANVAS_W` 135→240、`BUDDY_Y_BASE` 30→70 | 宠物区随画布重排（一处改值，18 种宠物全部生效） |
| `character.cpp` | `PEEK_TOP` 70→110；宠物区 140→180 | 同上 |
| `ble_bridge.cpp` | 改用 NimBLE-Arduino | 见"为什么换 BLE 栈" |
| `stats.h` | `min()` 第二个实参加 `(uint32_t)` | xtensa 上 `uint32_t` 是 `unsigned int`，riscv32 上是 `unsigned long`，模板推导会失败 |
| `platformio.ini` | 换板、换驱动、去 M5 依赖、加 NimBLE | — |
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

## 为什么换 BLE 栈

本板无 PSRAM，满屏 16 位精灵图占 153.6KB。用 Arduino 内核自带的 Bluedroid 时，
启动可用堆约 228.7KB，被精灵图占去后 Bluedroid 再吃约 72KB，**剩余仅 2.9KB** ——
不足以容纳协议行（单行上限 4KB）与 JSON 解析，连接一建立就会分配失败。实测数据：

| 配置 | 运行时空闲堆 |
| --- | --- |
| Bluedroid + 16 位满屏 | 2.9 KB（不可用） |
| **NimBLE-Arduino + 16 位满屏** | **约 32 KB** |
| Bluedroid + 阶段 A 的 135×240 | 91.8 KB |

换成 NimBLE 后 `ble_bridge.cpp` 的对外接口（`ble_bridge.h`）未变，其余代码零改动。
迁移中遇到的两个坑已记在该文件头部：NimBLE 无 `onPassKeyNotify`（改为自生成配对码
经 `setSecurityPasskey()` 设定）、无 `setAccessPermissions`（改为创建特征时的
`NIMBLE_PROPERTY` 标志），以及 `NimBLEAdvertising::setName()` 必须在
`enableScanResponse(true)` 之后调用，否则名字会被塞进空间不足的广播包而丢失 ——
丢了名字 Claude Desktop 的设备列表就过滤不到本机。

## 菜单与面板排版

TFT_eSPI 的位图字体只能整数倍缩放，没有中间档：

| 字号 | 单字符 | 设置页面板宽下限 |
| --- | --- | --- |
| size 1 | 6×8px | 约 114px（占屏宽 48%） |
| size 2 | 12×16px | 约 220px（占屏宽 92%） |

两档都实测过，最终取 size 1，靠加宽面板（118→150）与拉大行距（14→16）获得舒展观感。
常量集中在 `main.cpp` 的 `MENU_*` 一组里，四个菜单函数共用，避免改一个漏一个。

## 已知取舍与限制

1. **时钟页横屏时内容锚在左上**，不居中。竖屏时钟已按 320 高重排，横屏路径未动。
2. **角色包上限 1.8MB**（原版限制，未改）。本板 LittleFS 分区 5MB，余量充足。

## 验证状态

已在真实设备（MAC `98:C3:77:F4:13:86`，`COM4`）上验证：

| 项目 | 结果 |
| --- | --- |
| 编译 | 通过 |
| 板级初始化：屏幕、背光、CW2017、三键 | 通过 |
| 屏幕点亮、颜色正常、满屏无黑边 | 通过（目视） |
| 宠物水平居中、垂直位置合理 | 通过（目视） |
| 三键四项交互：长按 OK 菜单 / 短按 OK 切页 / DOWN 翻页 / UP 熄屏 | 通过 |
| LittleFS 挂载 + GIF 角色包加载与逐帧解码播放 | 通过 |
| ≥5 个页面目视检查：Info、Pet 统计、菜单、settings、宠物页 | 通过 |
| BLE 广播 `Claude-*` 且广播包含 NUS 服务 UUID（NimBLE 换栈后复测） | 通过 |
| 与 Claude Desktop 配对、状态切换、审批流程 | **未验证** |

**未验证项的原因**：Claude Desktop 已在本机安装（winget，v2.19675.1），
但其登录受地区限制无法完成，因此配对与审批链路未能实测。这不是代码问题 ——
设备侧的广播、NUS 服务、屏幕与按键均已实测通过，未验证的只是与桌面端的交互。

要补上这项，需要一台能登录 Claude Desktop 的机器，开启开发者模式
（**Help → Troubleshooting → Enable Developer Mode**）后：
**Developer → Open Hardware Buddy… → Connect**，选择 `Claude-*`，
把设备屏幕上显示的六位配对码输入系统配对框。

## 许可证

MIT，Copyright 2026 Anthropic, PBC。全文见 [LICENSE](LICENSE)。
上游仓库：<https://github.com/anthropics/claude-desktop-buddy>
