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

`src/font_cn.h` 里的点阵字模由 `tools/gen_cn_font.py` 生成，不是手绘的。
汉字与 ASCII 来自两种字体，分别声明如下。

**汉字 —— 文泉驿点阵宋体 12pt（16x16 点阵）**

| 项 | 内容 |
| --- | --- |
| 字体 | WenQuanYi Bitmap Song 12pt（`wenquanyi_12pt.bdf`） |
| 版权 | © 2004–2010, WenQuanYi Project Board of Trustees and Qianqian Fang |
| 许可 | **GNU GPL v2，附字体嵌入例外（font embedding exception）** |
| 项目主页 | <https://wenq.org/> |
| 许可全文 | <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> |

该字体的嵌入例外允许把字体（及其子集）嵌入文档或软件中分发，而不会让被嵌入
的文档/软件整体受 GPL 约束。本项目嵌入的是界面实际用到的 195 个汉字的
16x16 点阵子集（约 6.1KB）。

**ASCII —— Noto Sans SC 13px 渲染**

| 项 | 内容 |
| --- | --- |
| 字体 | Noto Sans SC（可变字体，取 Regular instance） |
| 版权 | © 2014–2021 Adobe (<http://www.adobe.com/>), with Reserved Font Name 'Source' |
| 许可 | **SIL Open Font License, Version 1.1** |
| 许可全文 | <http://scripts.sil.org/OFL> |

几点说明：

- 本项目**未内嵌字体文件本身**，只嵌入点阵子集（汉字约 6.1KB + ASCII 1.5KB）。
  字模是字体的**衍生作品**，按各自许可分发。
- OFL 禁止衍生字体使用保留字体名，本字体的保留名是 **'Source'**
  （Noto Sans SC 与思源黑体同源，共此声明）。本项目未在任何字体名、标识或
  宏名中使用它。
- 汉字只用 BDF 中现有的字形，不做二次描摹；ASCII 由 Pillow 渲染，
  13px 是 8px 半角字格能完整容纳大写字母的上限（见 `render_ascii`）。
- 换汉字源只需改 `tools/gen_cn_font.py` 顶部的 `BDF_PATH` 后重跑。
  BDF 字库不随仓库分发，从 Debian 源码包解压到该路径即可（见脚本顶部
  `BDF_URL`）。**本仓库提交了生成结果 `src/font_cn.h`，因此构建固件不需要
  字库文件**，只有改文案后才需要重跑工具。

## 上游仓库

- Anthropic claude-desktop-buddy：<https://github.com/anthropics/claude-desktop-buddy>
- 线协议说明：<https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md>
