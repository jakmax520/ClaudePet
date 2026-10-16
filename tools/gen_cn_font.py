#!/usr/bin/env python3
"""生成界面中文字模 src/font_cn.h。

只嵌入**界面实际用到**的字，而不是整套字库 —— 这是本方案能用 7.5KB 装下
中文的关键。字模存 PROGMEM，零堆占用（本板满屏精灵图之后只剩约 32KB 堆，
装不下任何完整的 CJK 字体文件）。

字体：Noto Sans SC（SIL OFL 1.1，可自由再分发）。
    注意它是**可变字体**，Pillow 的默认 instance 是 Thin —— 16x16 二值化后
    横画会断，必须显式 set_variation_by_name("Regular")。

用法：
    python tools/gen_cn_font.py            # 生成 src/font_cn.h
    python tools/gen_cn_font.py --preview  # 另存一张字模对照图供肉眼检查

改文案后重跑即可，汉字集合自动更新 —— 少打一个字会在生成时暴露（见 TEXTS
下方的完整性检查），而不是等到设备上显示成空白。
"""

import argparse
import sys
from pathlib import Path

FONT_PATH = "C:/Windows/Fonts/NotoSansSC-VF.ttf"
FONT_WEIGHT = "Regular"
THRESHOLD = 128
HAN_W = HAN_H = 16        # 汉字 16x16 -> 每行 2 字节
ASCII_W, ASCII_H = 8, 16  # ASCII 8x16 半宽 -> 每行 1 字节
OUT = Path(__file__).resolve().parent.parent / "src" / "font_cn.h"

# ── 文案表 ───────────────────────────────────────────────────────────────────
# 每条是 (英文, 中文)。这里既是翻译的唯一来源，也是字模的字符集来源 ——
# 界面代码只引用 key，实际字符串由这里出。
TEXTS = [
    # 浮层菜单
    ("menu_settings",  "settings",     "设置"),
    ("menu_off",       "turn off",     "关机"),
    ("menu_help",      "help",         "帮助"),
    ("menu_about",     "about",        "关于"),
    ("menu_demo",      "demo",         "演示"),
    ("menu_close",     "close",        "关闭"),
    # 面板底部的操作提示（键名类的 "DOWN"/"OK" 是印在壳上的，不译）
    ("hint_next",      "Next",         "下一个"),
    ("hint_change",    "Change",       "切换"),

    # 设置面板
    ("set_bright",     "brightness",   "亮度"),
    ("set_sound",      "sound",        "声音"),
    ("set_bt",         "bluetooth",    "蓝牙"),
    ("set_wifi",       "wifi",         "WiFi"),
    ("set_led",        "led",          "指示灯"),
    ("set_hud",        "transcript",   "转录"),
    ("set_rot",        "clock rot",    "屏幕方向"),
    ("set_pet",        "ascii pet",    "宠物样式"),
    ("set_lang",       "language",     "语言"),
    ("set_reset",      "reset",        "重置"),
    ("set_back",       "back",         "返回"),
    # 取值
    ("val_on",         "on",           "开"),
    ("val_off",        "off",          "关"),
    ("val_auto",       "auto",         "自动"),
    ("val_port",       "port",         "竖屏"),
    ("val_land",       "land",         "横屏"),
    ("val_off_s",      "off",          "关闭"),
    # 语言项自身的取值：显示当前语言名，因此随界面语言变
    ("val_lang_en",    "EN",           "EN"),
    ("val_lang_zh",    "Chinese",      "中文"),

    # 重置子菜单
    ("rst_char",       "delete char",  "删除角色"),
    ("rst_factory",    "factory reset", "恢复出厂"),
    ("rst_confirm",    "really?",      "确定？"),

    # 屏幕标识与分区名
    ("hdr_info",       "Info",         "信息"),
    ("sec_about",      "ABOUT",        "关于"),
    ("sec_buttons",    "BUTTONS",      "按键"),
    ("sec_claude",     "CLAUDE",       "Claude"),
    ("sec_device",     "DEVICE",       "设备"),
    ("sec_bt",         "BLUETOOTH",    "蓝牙"),
    ("sec_credits",    "CREDITS",      "致谢"),

    # 关于页
    ("ab_1",  "I watch your Claude",   "我盯着你的 Claude"),
    ("ab_2",  "desktop sessions.",     "桌面会话"),
    ("ab_3",  "I sleep when idle,",    "闲着就睡"),
    ("ab_4",  "wake when you work.",   "你一忙就醒"),
    ("ab_5",  "Press OK to approve",   "按 OK 批准"),
    ("ab_6",  "a prompt from here.",   "屏幕上的请求"),
    ("ab_7",  "18 species to pick",    "18 种宠物"),
    ("ab_8",  "in settings.",          "在设置里换"),

    # 按键页
    ("bt_ok",    "OK    centre",       "OK 中键"),
    ("bt_ok_1",  "    next page",      "  翻页"),
    ("bt_ok_2",  "    approve prompt", "  批准请求"),
    ("bt_dn",    "DOWN  lower",        "DOWN 下键"),
    ("bt_dn_1",  "    next screen",    "  切换屏幕"),
    ("bt_dn_2",  "    deny prompt",    "  拒绝请求"),
    ("bt_up",    "UP    upper",        "UP 上键"),
    ("bt_up_1",  "    screen off",     "  熄屏"),
    ("bt_hold",  "hold DOWN",          "长按 DOWN"),
    ("bt_hold_1","    menu",           "  打开菜单"),

    # Claude 页
    ("cl_sess",  "sessions  %u",       "会话    %u"),
    ("cl_run",   "running   %u",       "进行中  %u"),
    ("cl_wait",  "waiting   %u",       "等待中  %u"),
    ("cl_link",  "LINK",               "连接"),
    ("cl_via",   "via       %s",       "通道    %s"),
    ("cl_ble",   "ble       %s",       "链路    %s"),
    ("cl_last",  "last msg  %lus",     "最后消息 %lus"),
    ("cl_state", "state     %s",       "状态    %s"),
    # 通道取值
    ("sc_bt",    "bt",                 "蓝牙"),
    ("sc_usb",   "usb",                "USB"),
    ("sc_none",  "none",               "无"),
    ("sc_enc",   "encrypted",          "已加密"),
    ("sc_open",  "OPEN",               "明文"),

    # 设备页
    ("dv_full",  "full",               "已充满"),
    ("dv_chg",   "charging",           "充电中"),
    ("dv_usb",   "usb",                "USB"),
    ("dv_bat",   "battery",            "电池"),
    ("dv_vbat",  "battery  %d.%02dV",  "电压    %d.%02dV"),
    ("dv_cur",   "current  %+dmA",     "电流    %+dmA"),
    # 本板无 VBUS 检知，这一行恒不显示；保留是为了与上游版式一致
    ("dv_vbus",  "usb in   %d.%02dV",  "USB 输入 %d.%02dV"),
    ("dv_sys",   "SYSTEM",             "系统"),
    ("dv_owner", "owner    %.10s",     "主人    %.10s"),
    ("dv_up",    "uptime   %luh %02lum", "运行 %luh %02lum"),
    ("dv_heap",  "heap     %uKB",      "内存    %uKB"),
    ("dv_brt",   "bright   %u/4",      "亮度    %u/4"),
    ("dv_bt",    "bt       %s",        "蓝牙    %s"),
    ("dv_temp",  "temp     %dC",       "温度    %dC"),
    # bt 取值
    ("bt_linked","linked",             "已连接"),
    ("bt_on",    "on",                 "开"),
    ("bt_off",   "off",                "关"),

    # 蓝牙页
    ("bl_disc",  "discover",           "可发现"),
    ("bl_last",  "last msg  %lus",     "最后消息 %lus"),
    ("bl_pair",  "TO PAIR",            "待配对"),
    ("bl_p1",    "Open Claude desktop", "打开 Claude 桌面端"),
    ("bl_p2",    "> Developer",        "> 开发者"),
    ("bl_p3",    "> Hardware Buddy",   "> 硬件 Buddy"),
    ("bl_p4",    "connects via BLE",   "通过 BLE 连接"),

    # 致谢页
    ("cr_by",    "made by",            "作者"),
    ("cr_src",   "source",             "源码"),
    ("cr_hw",    "hardware",           "硬件"),
    # 上游写的是 M5StickC Plus / ESP32 + AXP192，那是原版硬件，本板不是
    ("cr_board", "FoloToy AI Passport", "FoloToy AI Passport"),
    ("cr_chip",  "ESP32-C3",           "ESP32-C3"),

    # 宠物屏
    # 标题：中文用 "的" 不用 " 's "，两种写法都要能塞进限宽（见 drawPet 的避让）
    ("pt_title", "%s's %s",            "%s的%s"),
    ("pt_mood",  "mood",               "心情"),
    ("pt_fed",   "fed",                "饱食"),
    ("pt_en",    "energy",             "精力"),
    ("pt_appr",  "approved %u",        "批准    %u"),
    ("pt_deny",  "denied   %u",        "拒绝    %u"),
    ("pt_nap",   "napped   %luh%02lum", "打盹 %luh%02lum"),
    ("pt_tok",   "tokens   ",          "累计    "),
    ("pt_today", "today    ",          "今日    "),
    ("pt_lv",    "Lv %u",              "Lv %u"),
    ("pt_how_m", "approve fast = up",  "批准越快越高"),
    ("pt_how_m2","deny lots = down",   "拒绝越多越低"),
    ("pt_how_f", "50K tokens = level", "5万令牌升级"),
    ("pt_how_e", "drains over time",   "随时间下降"),
    ("pt_how_i", "idle 30s = off",     "空闲30秒熄屏"),
    ("pt_how_i2","any button = wake",  "任意键唤醒"),
    ("pt_how_d", "DOWN: next screen",  "DOWN 切换屏幕"),
    ("pt_how_o", "OK: next page",      "OK 翻页"),
    ("pt_how_h", "hold DOWN: menu",    "长按 DOWN 菜单"),

    # 欢迎画面
    ("hi_owner", "%s's",               "%s 的"),
    ("hi_hello", "Hello!",             "你好！"),
    ("hi_buddy", "a buddy appears",    "一只小伙伴出现了"),

    # 审批页
    ("ap_wait",  "approve? %lus",      "待批准 %lus"),
    ("ap_ok",    "OK: approve",        "OK 批准"),
    ("ap_no",    "DOWN: deny",         "DOWN 拒绝"),
    ("ap_sent",  "sent...",            "已发送..."),

    # 配对页
    ("pk_title", "BLUETOOTH PAIRING",  "蓝牙配对"),
    ("pk_hint",  "enter on desktop:",  "在电脑上输入："),

    # 宠物状态名（信息页的状态字段用）
    ("st_sleep", "sleep",     "睡觉"),
    ("st_idle",  "idle",      "空闲"),
    ("st_busy",  "busy",      "忙碌"),
    ("st_att",   "attention", "待处理"),
    ("st_cel",   "celebrate", "庆祝"),
    ("st_dizzy", "dizzy",     "晕眩"),
    ("st_heart", "heart",     "开心"),

    # 角色包安装进度页
    ("xf_install", "installing",        "安装中"),
    # 其它
    ("no_char",  "no character loaded", "未安装角色包"),
]

# 必须存在、但不属于任何界面的字符：数字与单位会与格式化串一起出现，
# 单独列出以保证字模覆盖。
EXTRA_ASCII = "0123456789:%.-+/<>[]()%'\" ,!?=KMBhmsdVC" "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"


def collect_chars():
    han, ascii_ = set(), set()
    for _, en, zh in TEXTS:
        for s in (en, zh):
            for ch in s:
                if ord(ch) < 128:
                    ascii_.add(ch)
                else:
                    han.add(ch)
    ascii_.update(EXTRA_ASCII)
    # ASCII 字模覆盖全部可打印字符，主机下发的英文内容才显示得出来
    ascii_.update(chr(c) for c in range(0x20, 0x7F))
    return sorted(han), sorted(ascii_)


def render(ch, w, h, font_cache):
    from PIL import Image, ImageDraw, ImageFont
    key = (w, h)
    if key not in font_cache:
        f = ImageFont.truetype(FONT_PATH, h)
        f.set_variation_by_name(FONT_WEIGHT)
        font_cache[key] = f
    f = font_cache[key]
    img = Image.new("L", (w, h), 0)
    # 汉字按字身框渲染；ASCII 用 left-top 对齐，半宽字形自带右侧留白
    ImageDraw.Draw(img).text((0, 0), ch, font=f, fill=255)
    return img


def to_bits(img, w, h):
    """1bpp 行优先、MSB 在最左，与 TFT_eSPI 的取位顺序一致。"""
    px = img.load()
    row_bytes = (w + 7) // 8
    out = bytearray()
    for y in range(h):
        for bx in range(row_bytes):
            b = 0
            for bit in range(8):
                x = bx * 8 + bit
                if x < w and px[x, y] > THRESHOLD:
                    b |= 0x80 >> bit
            out.append(b)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true", help="另存字模对照图")
    args = ap.parse_args()

    try:
        import PIL  # noqa: F401
    except ImportError:
        print("需要 Pillow：uv run --with pillow python tools/gen_cn_font.py", file=sys.stderr)
        return 1

    han, ascii_ = collect_chars()
    font_cache = {}

    # ── 汉字 ──
    han_data = bytearray()
    index = []
    for ch in han:
        index.append((ord(ch), len(han_data)))
        han_data += to_bits(render(ch, HAN_W, HAN_H, font_cache), HAN_W, HAN_H)

    # ── ASCII（0x20..0x7E 顺序，直接下标寻址）──
    ascii_range = [chr(c) for c in range(0x20, 0x7F)]
    ascii_data = bytearray()
    for ch in ascii_range:
        ascii_data += to_bits(render(ch, ASCII_W, ASCII_H, font_cache), ASCII_W, ASCII_H)

    index.sort()  # 按码点升序，渲染层二分查找

    # ── 完整性检查：文案里有、字模里没有的字符 ──
    covered = set(han) | set(ascii_range)
    missing = sorted({c for _, en, zh in TEXTS for s in (en, zh) for c in s
                      if c not in covered})
    if missing:
        print(f"漏字（字模未覆盖）：{''.join(missing)}", file=sys.stderr)
        return 1

    # ── 导出 ──
    def emit_bytes(name, data, per_line=16):
        lines = [f"static const uint8_t {name}[] PROGMEM = {{"]
        for i in range(0, len(data), per_line):
            chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + per_line])
            lines.append(f"  {chunk},")
        lines.append("};")
        return "\n".join(lines)

    idx_lines = ["// (码点, 位图字节偏移)，按码点升序，渲染层二分查找",
                 "static const CnGlyph CN_INDEX[] PROGMEM = {"]
    for cp, off in index:
        ch = chr(cp)
        idx_lines.append(f"  {{0x{cp:04X}, {off}}},  // {ch}")
    idx_lines.append("};")

    # ── 文案表 ──
    # 与字模同源导出：两者都源自上面那份 TEXTS，所以不会出现「改了文案但字模
    # 没跟上」或「id 与字符串错位」。界面代码只引用枚举名。
    enum_lines, table_lines = [], []
    for key, en, zh in TEXTS:
        enum_lines.append(f"  T_{key.upper()},")
        esc = lambda s: s.replace("\\", "\\\\").replace('"', '\\"')
        table_lines.append(f'  {{"{esc(en)}", "{esc(zh)}"}},')
    enum_lines.append("  T_COUNT")

    texts_block = f"""// ── 文案表 ──────────────────────────────────────────────────────────────────
// 每条两个版本，下标即语言（见 cn_text.h 的 CN_EN / CN_ZH）。
// 由生成工具的 TEXTS 表导出，改文案请改那边后重跑。
enum CnStrId {{
{chr(10).join(enum_lines)}
}};

static const char* const CN_TEXTS[][2] = {{
{chr(10).join(table_lines)}
}};"""

    header = f"""#pragma once
#include <stdint.h>
#include <pgmspace.h>   // PROGMEM / pgm_read_byte —— 本头文件自带，不依赖包含顺序

// ⚠ 本文件由 tools/gen_cn_font.py 生成，请勿手改 —— 改文案请改该工具的 TEXTS
// 表后重跑，汉字集合会自动更新，漏字在生成时就会报错。
//
// 字体：Noto Sans SC（SIL OFL 1.1，可自由再分发），Regular instance。
// 布局：汉字 {HAN_W}x{HAN_H} 1bpp，每行 {HAN_W // 8} 字节 → {HAN_W * HAN_H // 8} 字节/字
//       ASCII {ASCII_W}x{ASCII_H} 1bpp，半宽  → {ASCII_W * ASCII_H // 8} 字节/字符
//       行优先、MSB 在最左。
//
// 规模：{len(index)} 个汉字（{len(han_data)} B）+ 95 个 ASCII（{len(ascii_data)} B）
//       合计约 {(len(han_data) + len(ascii_data)) / 1024:.1f}KB，全部存 flash，零堆占用。

#define CN_HAN_W     {HAN_W}
#define CN_HAN_H     {HAN_H}
#define CN_HAN_BYTES {HAN_W * HAN_H // 8}
#define CN_ASCII_W   {ASCII_W}
#define CN_ASCII_H   {ASCII_H}
#define CN_ASCII_BYTES {ASCII_W * ASCII_H // 8}
#define CN_ASCII_FIRST 0x{0x20:02X}
#define CN_ASCII_LAST  0x{0x7E:02X}

struct CnGlyph {{ uint32_t cp; uint16_t off; }};

static const uint16_t CN_INDEX_N = {len(index)};
{chr(10).join(idx_lines)}

{emit_bytes("CN_HAN_BITMAP", han_data)}

{emit_bytes("CN_ASCII_BITMAP", ascii_data)}

{texts_block}
"""
    OUT.write_text(header, encoding="utf-8")
    print(f"已生成 {OUT}")
    print(f"  汉字 {len(index)} 个，{len(han_data)} B")
    print(f"  ASCII {len(ascii_range)} 个，{len(ascii_data)} B")
    print(f"  合计 {(len(han_data) + len(ascii_data)) / 1024:.1f}KB")

    if args.preview:
        save_preview(han, font_cache)
    return 0


def save_preview(han, font_cache, cols=24):
    """把全部汉字字模拼成一张放大图，用于肉眼确认清晰度。"""
    from PIL import Image
    rows = (len(han) + cols - 1) // cols
    scale = 3
    img = Image.new("L", (cols * HAN_W * scale, rows * HAN_H * scale), 0)
    for i, ch in enumerate(han):
        bits = render(ch, HAN_W, HAN_H, font_cache)
        b = bits.point(lambda v: 255 if v > THRESHOLD else 0)
        img.paste(b.resize((HAN_W * scale, HAN_H * scale), Image.NEAREST),
                  ((i % cols) * HAN_W * scale, (i // cols) * HAN_H * scale))
    path = Path("C:/Users/11761/AppData/Local/Temp/font_cn_preview.png")
    img.save(path)
    print(f"  字模对照图：{path}")


if __name__ == "__main__":
    sys.exit(main())
