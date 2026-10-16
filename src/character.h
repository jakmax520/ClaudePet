#pragma once
#include <stdint.h>

struct Palette {
  uint16_t body, bg, text, textDim, ink;
};

// Call after M5.begin() and spr.createSprite(). Mounts LittleFS, reads
// /characters/<name>/manifest.json, parses colors, caches GIF paths.
bool characterInit(const char* name);
bool characterLoaded();

// 0..6: sleep, idle, busy, attention, celebrate, dizzy, heart.
// Closes current GIF, opens the one for this state. No-op if same state.
void characterSetState(uint8_t state);

// Advances timing; if it's time for the next frame, decodes it into the
// sprite. Call every loop iteration. Does nothing if not loaded.
void characterTick();
void characterInvalidate();
void characterClose();   // close GIF + clear loaded flag; FS stays mounted   // full clear + reopen current — call when an overlay closes

// Peek mode renders the GIF at half scale, centered in the info-panel
// header strip; off renders full-size centered in the upper home area.
// Adaptive to actual canvas height — no padding required in source art.
void characterSetPeek(bool peek);
class TFT_eSPI;
void characterRenderTo(TFT_eSPI* tgt, int cx, int cy);

// 当前宠物图形的左边缘 x。宠物屏的标题画在宠物之上，需要按这条线限宽避让
// （peek 模式下 GIF 缩半居中，如 96x100 的角色包占 x=96..144）。
// 返回值随角色包尺寸变化，未加载 GIF 时不可用 —— 调用方需自行兜底。
int characterGifLeft();

const Palette& characterPalette();
