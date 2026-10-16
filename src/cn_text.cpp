#include "cn_text.h"
#include "font_cn.h"
#include <TFT_eSPI.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint8_t s_lang = CN_EN;

uint8_t cnLang() { return s_lang; }
void    cnSetLang(uint8_t lang) { s_lang = (lang == CN_ZH) ? CN_ZH : CN_EN; }

const char* cnStr(uint16_t id) {
  if (id >= T_COUNT) return "";
  return CN_TEXTS[id][s_lang];
}

// UTF-8 解码一个码点，并推进指针。非法/截断序列按单字节处理。
static uint32_t utf8Next(const char** p) {
  const uint8_t* s = (const uint8_t*)*p;
  uint32_t cp = *s++;
  if (cp >= 0xF0) {
    cp = ((cp & 0x07) << 18) | ((s[0] & 0x3F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    s += 3;
  } else if (cp >= 0xE0) {
    cp = ((cp & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
    s += 2;
  } else if (cp >= 0xC0) {
    cp = ((cp & 0x1F) << 6) | (s[0] & 0x3F);
    s += 1;
  }
  *p = (const char*)s;
  return cp;
}

// 查字模。ASCII 直接下标（0x20..0x7E 连续存放），其余二分查找。
// 找不到返回 nullptr —— 调用方按空白占位，不能跳过，否则后面的字会往前挤。
static const uint8_t* findGlyph(uint32_t cp, int* w, int* h, int* rowBytes) {
  if (cp >= CN_ASCII_FIRST && cp <= CN_ASCII_LAST) {
    *w = CN_ASCII_W; *h = CN_ASCII_H; *rowBytes = CN_ASCII_W / 8;
    return CN_ASCII_BITMAP + (cp - CN_ASCII_FIRST) * CN_ASCII_BYTES;
  }
  int lo = 0, hi = (int)CN_INDEX_N - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint32_t c = pgm_read_dword(&CN_INDEX[mid].cp);
    if (c == cp) {
      uint16_t off = pgm_read_word(&CN_INDEX[mid].off);
      *w = CN_HAN_W; *h = CN_HAN_H; *rowBytes = CN_HAN_W / 8;
      return CN_HAN_BITMAP + off;
    }
    if (c < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

// 直接逐像素 drawPixel。
//
// 曾用 pushImage 把整个字一次贴上去（少 255 次调用），但那条路会把 sprite 的
// 内部状态搞坏：cnDraw 本身正常返回，随后主循环的 pushSprite 会**卡死**（不是
// 崩溃、不重启，只是屏幕永远停在 setup 里清屏后的空背景上，表现为开机黑屏）。
//
// 原因在 TFT_eSPI 2.5.43 的重载签名不一致：
//   头文件  void pushImage(..., uint16_t *data);
//           void pushImage(..., uint16_t *data, uint16_t transparent);
//   Sprite.cpp  void TFT_eSprite::pushImage(..., uint16_t *data, uint8_t sbpp);
// 第 6 个参数类型对不上（uint8_t vs uint16_t），传 uint16_t* 时的重载解析落到
// 语义未定义的路径上。drawPixel 是项目里早已大量使用的稳妥 API（乱码块、
// 进度条、呼吸灯都用它），换过来即可。
//
// 代价是每个字 256 次调用。实测信息页一帧约 200 字仍够用；若将来帧率吃紧，
// 正确做法是各自维护一个 16x16 的 uint16 缓冲后走 pushImage 的 **const** 重载
// （那条路径没有签名歧义），而不是回到这里。
void cnDraw(TFT_eSPI* t, int x, int y, const char* utf8, uint16_t fg, uint16_t bg) {
  if (!utf8) return;
  while (*utf8) {
    uint32_t cp = utf8Next(&utf8);
    if (cp == '\n') break;              // 不做多行折行，换行由调用方控制
    if (cp == '\r') continue;

    int w, h, rb;
    const uint8_t* bits = findGlyph(cp, &w, &h, &rb);
    if (!bits) {
      // 字模里没有的字符（如主机下发 emoji）：按汉字宽度填一块背景。
      // 跳过宽度不填会让后面的字挤过来，还会留下上一帧的残影。
      t->fillRect(x, y, CN_HAN_W, CN_HAN_H, bg);
      x += CN_HAN_W;
      continue;
    }
    for (int row = 0; row < h; row++) {
      const uint8_t* src = bits + row * rb;
      for (int bx = 0; bx < rb; bx++) {
        uint8_t b = pgm_read_byte(src + bx);
        for (int bit = 0; bit < 8; bit++) {
          int col = bx * 8 + bit;
          if (col >= w) break;
          t->drawPixel(x + col, y + row, (b & (0x80 >> bit)) ? fg : bg);
        }
      }
    }
    x += w;
  }
}

int cnWidth(const char* utf8) {
  if (!utf8) return 0;
  int w = 0;
  while (*utf8) {
    uint32_t cp = utf8Next(&utf8);
    if (cp == '\n') break;
    w += (cp < 0x80) ? CN_ASCII_W : CN_HAN_W;
  }
  return w;
}

void cnDrawCenter(TFT_eSPI* t, int cx, int y, const char* utf8, uint16_t fg, uint16_t bg) {
  cnDraw(t, cx - cnWidth(utf8) / 2, y, utf8, fg, bg);
}

// 格式化缓冲：最长的一条文案是 19 个汉字 = 57 字节（UTF-8），加数字余量取 96。
static char s_fmt[96];

void cnPrint(TFT_eSPI* t, int x, int y, uint16_t fg, uint16_t bg,
             const char* fmt, ...) {
  va_list a; va_start(a, fmt);
  vsnprintf(s_fmt, sizeof(s_fmt), fmt, a);
  va_end(a);
  cnDraw(t, x, y, s_fmt, fg, bg);
}

void cnPrintCenter(TFT_eSPI* t, int cx, int y, uint16_t fg, uint16_t bg,
                   const char* fmt, ...) {
  va_list a; va_start(a, fmt);
  vsnprintf(s_fmt, sizeof(s_fmt), fmt, a);
  va_end(a);
  cnDrawCenter(t, cx, y, s_fmt, fg, bg);
}
