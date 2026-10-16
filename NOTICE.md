# 来源与归属声明

本仓库是 [Anthropic claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
面向 **FoloToy AI Passport（ESP32-C3）** 的硬件移植版。
本文件说明各部分代码与素材的来源与著作权归属。

## 1. 来自上游的文件（著作权归 Anthropic, PBC）

下列路径下的文件来自上游仓库，仅作移植所需的少量修改，**著作权归 Anthropic, PBC，
适用其 MIT 许可（全文见第 3 节）**：

| 路径 | 说明 |
| --- | --- |
| `src/`（`src/hal/` 除外） | 应用逻辑、18 种宠物、BLE 桥、协议解析、GIF 渲染 |
| `tools/` | 角色包预处理与烧录脚本 |
| `characters/` | 示例角色包（美术资源另有声明，见第 4 节） |
| `docs/` | 文档与截图 |
| `REFERENCE.md` | Hardware Buddy 线协议说明 |
| `CONTRIBUTING.md` | 上游贡献说明（仅将 `M5Stick` 改为 `ESP32`） |

其中被修改的上游文件及改动内容：

- `src/main.cpp` —— 画布居中偏移；停用原版位于 GPIO10 的 LED（本板该脚为 I2C SDA）
- `src/stats.h` —— `min()` 第二实参补 `uint32_t` 转型（xtensa 与 riscv32 的整型宽度差异）

## 2. 本仓库新增的文件

下列部分由 jakmax520 编写，**以 MIT 许可提供**（条款见根目录 `LICENSE`）：

| 路径 | 说明 |
| --- | --- |
| `src/hal/` | ESP32-C3 / FoloToy AI Passport 硬件抽象层 |
| `platformio.ini` | 板级与驱动构建配置 |
| `partitions.csv` | 8MB Flash 分区表 |
| `README.md` | 本仓库说明 |
| `NOTICE.md` | 本文件 |

`src/hal/tft_c3_spi_fix.h` 记录并修正了 TFT_eSPI 2.5.43 在 ESP32-C3 上的一个缺陷
（`REG_SPI_BASE` 收到 `SPI2_HOST` 枚举值而非外设序号，导致 SPI 寄存器基址算成 0）。
该修正针对的是第三方库的行为，不涉及上游代码。

## 3. 上游 MIT 许可全文

以下为上游 `claude-desktop-buddy` 的许可原文，按其要求随附于此：

> Copyright 2026 Anthropic, PBC.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## 4. 第三方美术资源的单独声明

**`characters/bufo/` 下的 GIF 资源不属于 MIT 许可范围。**

它们是社区 "bufo" 表情包（<https://bufo.zone>）的素材，随仓库分发只为方便取用，
**全部权利归原作者所有**。上游 `LICENSE` 中对此有同样的声明。

详见 `characters/bufo/README.md`。若要用于商业用途，请自行确认授权。

## 5. 硬件参数的参考来源

`src/hal/hal_pins.h` 中的引脚定义与电气参数（屏幕引脚、按键 ADC 分压窗口、
CW2017 寄存器用法）参考了 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)
的 `components/bsp/include/bsp_pins.h`。

**仅取其中的硬件事实，未沿用其代码结构**——该项目是基于 ESP-IDF 的独立实现，
与本仓库（Arduino / PlatformIO）架构不同。

## 6. 中文字模的来源与许可

`src/font_cn.h` 里的点阵字模由 `tools/gen_cn_font.py` 从 **Noto Sans SC** 渲染而来，
不是手绘的。按 SIL Open Font License 的要求在此声明：

| 项 | 内容 |
| --- | --- |
| 字体 | Noto Sans SC（可变字体，取 Regular instance） |
| 版权 | © 2014–2021 Adobe (<http://www.adobe.com/>), with Reserved Font Name 'Source' |
| 许可 | **SIL Open Font License, Version 1.1** |
| 许可全文 | <http://scripts.sil.org/OFL> |

字体文件里自带的许可描述原文：

> This Font Software is licensed under the SIL Open Font License, Version 1.1.
> This Font Software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
> CONDITIONS OF ANY KIND, either express or implied.

几点说明：

- 本项目**未内嵌字体文件本身**，嵌入的是针对界面实际用到的 195 个字渲染出的
  16x16 二值点阵（约 6.2KB）。字模是该字体的**衍生作品**，仍按 OFL 分发。
- OFL 禁止衍生字体使用保留字体名，本字体的保留名是 **'Source'**
  （Noto Sans SC 与思源黑体同源，共此声明）。本项目未在任何字体名、标识或
  宏名中使用它。
- 换字体只需改 `tools/gen_cn_font.py` 顶部的 `FONT_PATH` 后重跑，
  不必改动任何界面代码 —— 若在意字体选择，可换成任意覆盖简体中文的字体。
- 生成工具依赖系统里存在该字体文件（Windows 11 自带）。**本仓库提交了生成结果
  `src/font_cn.h`，因此构建固件不需要该字体**，只有改文案后才需要重跑工具。

## 上游仓库

- Anthropic claude-desktop-buddy：<https://github.com/anthropics/claude-desktop-buddy>
- 线协议说明：<https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md>
