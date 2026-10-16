#pragma once
#include <stdint.h>
// 带上字模与文案表：cnStr() 的 id 参数类型（CnStrId）就定义在那边，
// 使用方只需 include 本头文件。
#include "font_cn.h"

// 中英混排文本渲染层。
//
// 为什么不复用 TFT_eSPI 的内置字体：它的 GLCD 位图字体只有 ASCII，画不了汉字；
// 而官方的矢量中文途径（Smooth Font）会把整个 .vlw 读进堆，本板满屏精灵图之后
// 只剩约 32KB，装不下。改成自制点阵 —— 只嵌入界面实际用到的 192 个字，约 7.5KB，
// 存 flash 零堆占用，字模与文案表都由 tools/gen_cn_font.py 生成（见 font_cn.h）。
//
// 字形尺寸：汉字 16x16、ASCII 8x16，两者**等高**，所以可以同行混排。
// 所有绘制都填背景色 —— 精灵图上只画前景会让上一帧的文字留下残影。
//
// 有一类文字**永远翻不了**：主机经 BLE 下发的动态内容（会话描述、工具名、
// 参数提示、owner 名）设备不知道语义，只能按原样显示。这是协议决定的，
// 最终界面必然是「本地文案中文 + 主机内容英文」的混排。

class TFT_eSPI;

// 语言下标，与 font_cn.h 里 CN_TEXTS 的第二维一致
enum { CN_EN = 0, CN_ZH = 1 };

// 取文案。id 用 font_cn.h 的 CnStrId 枚举（如 T_MENU_SETTINGS）。
const char* cnStr(uint16_t id);

// 当前语言。由设置项驱动，运行时可切。
uint8_t cnLang();
void    cnSetLang(uint8_t lang);

// 文本像素宽度。汉字按 16、ASCII 按 8 累加，不查字模表。
int cnWidth(const char* utf8);

// 从左上角 (x, y) 起绘制一行 UTF-8。超出画布的部分由 TFT_eSPI 自行裁剪。
void cnDraw(TFT_eSPI* t, int x, int y, const char* utf8, uint16_t fg, uint16_t bg);

// 以 cx 为水平中心绘制
void cnDrawCenter(TFT_eSPI* t, int cx, int y, const char* utf8, uint16_t fg, uint16_t bg);

// 带格式化的版本 —— 文案里的 %u / %s / %luh 等照常工作
void cnPrint(TFT_eSPI* t, int x, int y, uint16_t fg, uint16_t bg,
             const char* fmt, ...) __attribute__((format(printf, 6, 7)));
void cnPrintCenter(TFT_eSPI* t, int cx, int y, uint16_t fg, uint16_t bg,
                   const char* fmt, ...) __attribute__((format(printf, 6, 7)));
