#!/usr/bin/env python3
"""生成界面中文字模 src/font_cn.h。

只嵌入**界面实际用到**的字，而不是整套字库 —— 这是本方案能用 7.5KB 装下
中文的关键。字模存 PROGMEM，零堆占用（本板满屏精灵图之后只剩约 32KB 堆，
装不下任何完整的 CJK 字体文件）。

字体：
    汉字 —— 文泉驿点阵宋体 12pt（16x16 点阵，GPLv2 + 字体嵌入例外）。
        直接取 BDF 的 BITMAP，不经过矢量字体的二值化。16px 下笔画是逐像素
        设计好的，比 Noto 二值化清晰；BDF 的 FONT_ASCENT=14 也正好是全角
        字身框底边，天然与 8x16 拉丁基线对齐。
    拉丁 —— Noto Sans SC（SIL OFL 1.1）13px 渲染（见 render_ascii）。

    BDF 字库不随仓库分发，从 Debian 源码包获取（见 BDF_URL），解压后放到
    tools/fonts/wenquanyi_12pt.bdf。构建固件不需要它 —— font_cn.h 已入库，
    只有改文案后才要重跑本工具。

用法：
    python tools/gen_cn_font.py            # 生成 src/font_cn.h
    python tools/gen_cn_font.py --preview  # 另存一张字模对照图供肉眼检查

改文案后重跑即可，汉字集合自动更新 —— 少打一个字会在生成时暴露（见 TEXTS
下方的完整性检查），而不是等到设备上显示成空白。
"""

import argparse
import sys
from pathlib import Path

# 仅用于拉丁字形；汉字走下面的点阵 BDF
FONT_PATH = "C:/Windows/Fonts/NotoSansSC-VF.ttf"
FONT_WEIGHT = "Regular"
THRESHOLD = 128
HAN_W = HAN_H = 16        # 汉字 16x16 -> 每行 2 字节
ASCII_W, ASCII_H = 8, 16  # ASCII 8x16 半宽 -> 每行 1 字节

BDF_PATH = Path(__file__).resolve().parent / "fonts" / "wenquanyi_12pt.bdf"
BDF_URL = ("https://deb.debian.org/debian/pool/main/x/xfonts-wqy/"
           "xfonts-wqy_1.0.0~rc1.orig.tar.gz")
BDF_ASCENT = 14           # wenquanyi_12pt.bdf 的 FONT_ASCENT
# 拉丁基线比汉字字身框底边高 1px。16px 字号的下伸部（g/p/q/y 的尾巴）深约
# 4.5px，而画布在基线下方只有 2px：贴着 14 放会裁掉 2.5px，表现为英文底部
# 缺一块。抬到 13 后只裁 1.5px，混排的 1px 高差肉眼不可见。
BASELINE_Y = BDF_ASCENT - 1

# ASCII 与汉字同为 16px 字号，视觉上等高。代价是 D/O/W/N 宽 10~14px，超出
# 8px 半角字格 —— 不能像早期那样直接裁（DOWN 会变成 DCVN），而是把超宽字形
# 水平压缩到 8px。压缩比最大的 W/M（~0.57）笔画仍可辨；4x 超采样后缩小，
# 避免小字号二值化断笔。
ASCII_FONT_SIZE = 16
OVERSAMPLE = 4
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

    # 关于页：一句一行，正文缩进 24px（每行可用 212px = 26 个 ASCII / 13 个汉字）
    ("ab_1",  "I watch your Claude work.", "我盯着你的 Claude 会话"),
    ("ab_2",  "Sleep idle, wake on work",  "闲着就睡，你一忙就醒"),
    ("ab_3",  "Press OK to approve.",      "按 OK 批准屏幕上的请求"),
    ("ab_4",  "18 species in settings.",   "18 种宠物，在设置里换"),

    # 按键页：一键一行键名 + 一行动作，动作不再拆成两行。
    # 动作行统一缩进，彼此左对齐（不是各对齐到自己键名的描述文字）：
    #   中文 3 个空格 = 24px（与「翻页」同起点）
    #   英文 6 个空格 = 48px（与 OK/DOWN/UP 的描述文字同起点）
    ("bt_ok",    "OK    centre",       "OK 中键"),
    ("bt_ok_1",  "      next / approve",     "   翻页 / 批准请求"),
    ("bt_dn",    "DOWN  lower",        "DOWN 下键"),
    ("bt_dn_1",  "      next screen / deny", "   切换屏幕 / 拒绝请求"),
    ("bt_up",    "UP    upper",        "UP 上键"),
    ("bt_up_1",  "      screen off",         "   熄屏"),
    ("bt_hold",  "hold DOWN",          "长按 DOWN"),
    ("bt_hold_1","      menu",               "   打开菜单"),

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
    ("bl_p1",    "Open Claude desktop", "打开 Claude Desktop"),
    ("bl_p2",    "> Developer",        "> 开发者"),
    ("bl_p3",    "> Hardware Buddy",   "> 硬件 Buddy"),
    ("bl_p4",    "connects via BLE",   "通过 BLE 连接"),

    # 致谢页
    ("cr_by",    "made by",            "作者"),
    ("cr_hw",    "hardware",           "硬件"),
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


def load_bdf(path):
    """读 BDF 点阵：{码点: (w, h, xoff, yoff, 十六进制行列表)}。

    BDF 的坐标以基线为原点、y 向上；BBX 的 yoff + h 是字形顶边的字体坐标。
    """
    glyphs, cp, bbx, rows = {}, None, None, None
    for raw in Path(path).read_text(encoding="latin-1").splitlines():
        line = raw.rstrip()
        if line.startswith("ENCODING "):
            cp = int(line.split()[1])
        elif line.startswith("BBX "):
            _, w, h, xo, yo = line.split()
            bbx = (int(w), int(h), int(xo), int(yo))
        elif line == "BITMAP":
            rows = []
        elif line == "ENDCHAR":
            if cp is not None and cp >= 0 and bbx is not None and rows is not None:
                glyphs[cp] = (*bbx, rows)
            cp = bbx = rows = None
        elif rows is not None:
            rows.append(line)
    return glyphs


def bdf_to_bits(cp, glyphs):
    """把一个 BDF 字形摆进 16x16 字格，输出 1bpp 行优先、MSB 在左。

    屏幕行 = FONT_ASCENT - 字体 y。汉字 BBX 16x16 yoff=-2，正好占满
    0..15；个别全角标点更矮，落在字身框内。越界视为字库/字格不匹配，
    宁可生成失败也不静默截断。
    """
    w, h, xo, yo, rows = glyphs[cp]
    top = BDF_ASCENT - (yo + h)
    out = bytearray(HAN_W * HAN_H // 8)
    for j, hexrow in enumerate(rows):
        val = int(hexrow, 16) if hexrow else 0
        nbits = len(hexrow) * 4
        for i in range(w):
            if not (val & (1 << (nbits - 1 - i))):
                continue
            x, y = xo + i, top + j
            if not (0 <= x < HAN_W and 0 <= y < HAN_H):
                raise ValueError(f"U+{cp:04X} 字形越出 {HAN_W}x{HAN_H}：(x={x}, y={y})")
            out[y * (HAN_W // 8) + x // 8] |= 0x80 >> (x % 8)
    return bytes(out)


def render_ascii(ch, font_cache):
    """渲染一个 ASCII 到 8x16 字格。

    基线取 BASELINE_Y。字形水平居中，超过 8px 宽的按比例压到 8px（W/M/m/@
    等），保证右侧笔画不被裁掉。

    基线下方的下伸部（g/p/q/y 的尾巴）**垂直压缩**进剩余行，而不是让它溢出
    画布被裁：16px 行高里，满格汉字只给拉丁下伸部留了 3px，而它实际深
    4.5px。压缩后尾巴略扁，但形状完整，不会出现「字母底部缺一块」。
    """
    from PIL import Image, ImageDraw, ImageFont
    key = ("ascii", ASCII_FONT_SIZE)
    if key not in font_cache:
        f = ImageFont.truetype(FONT_PATH, ASCII_FONT_SIZE * OVERSAMPLE)
        f.set_variation_by_name(FONT_WEIGHT)
        font_cache[key] = f
    f = font_cache[key]
    s = OVERSAMPLE
    base = BASELINE_Y * s
    big = Image.new("L", (ASCII_W * 4 * s, ASCII_H * 2 * s), 0)
    ImageDraw.Draw(big).text((ASCII_W * s, base), ch, font=f, fill=255, anchor="ls")
    bb = big.getbbox()
    out = Image.new("L", (ASCII_W * s, ASCII_H * s), 0)

    def place(piece, top):
        w, h = piece.size
        if w > ASCII_W * s:                      # 超宽字形水平压缩
            piece = piece.resize((ASCII_W * s, h), Image.LANCZOS)
            w, h = piece.size
        out.paste(piece, ((ASCII_W * s - w) // 2, top))

    if bb:
        split = min(bb[3], base)
        if split > bb[1]:                        # 基线上方：保持原比例
            place(big.crop((bb[0], bb[1], bb[2], split)), bb[1])
        # 基线下方：真正的下伸部（g/p/q/y，≥2px）压进剩余 3px。
        # 1px 以内的是字形底边的抗锯齿灰边，必须丢掉 —— 若当成下伸部贴上去，
        # 会被放大成每个字母脚下的小竖线（实测就是这种「多余残留」）。
        if bb[3] - base >= 2 * s:
            lower = big.crop((bb[0], base, bb[2], bb[3]))
            avail = (ASCII_H - BASELINE_Y) * s
            lower = lower.resize((lower.size[0], min(avail, lower.size[1])),
                                 Image.LANCZOS)   # 只压缩，绝不放大
            place(lower, base)
    return out.resize((ASCII_W, ASCII_H), Image.LANCZOS)


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


def ascii_descent_overflow(font_cache):
    """ASCII 下伸部（g/p/q/y 的尾巴）超出 16px 行高的最大像素数。

    满格汉字与拉丁下伸部在 16px 行内无法共存。16px 字号的下伸部约 4.5px，
    画布只剩基线下方 2px，因此允许裁到 3px；再多说明字号与行高失配。
    """
    f = font_cache[("ascii", ASCII_FONT_SIZE)]   # 超采样字体，深度要换算回设计字号
    depth = max(f.getbbox(chr(c), anchor="ls")[3]
                for c in range(0x20, 0x7F)) / OVERSAMPLE
    return max(0.0, depth - (ASCII_H - BASELINE_Y))


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

    # ── 汉字（BDF 原生点阵）──
    if not BDF_PATH.exists():
        print(f"缺少字库：{BDF_PATH}\n"
              f"  从 {BDF_URL} 解压 wenquanyi_12pt.bdf 后放到该路径",
              file=sys.stderr)
        return 1
    bdf = load_bdf(BDF_PATH)
    absent = [c for c in han if ord(c) not in bdf]
    if absent:
        print(f"字库缺字（文泉驿 12pt 无此字形）：{''.join(absent)}", file=sys.stderr)
        return 1
    han_data = bytearray()
    index = []
    for ch in han:
        index.append((ord(ch), len(han_data)))
        han_data += bdf_to_bits(ord(ch), bdf)

    # ── ASCII（0x20..0x7E 顺序，直接下标寻址）──
    ascii_range = [chr(c) for c in range(0x20, 0x7F)]
    ascii_data = bytearray()
    for ch in ascii_range:
        ascii_data += to_bits(render_ascii(ch, font_cache), ASCII_W, ASCII_H)

    index.sort()  # 按码点升序，渲染层二分查找

    # ── 完整性检查：文案里有、字模里没有的字符 ──
    covered = set(han) | set(ascii_range)
    missing = sorted({c for _, en, zh in TEXTS for s in (en, zh) for c in s
                      if c not in covered})
    if missing:
        print(f"漏字（字模未覆盖）：{''.join(missing)}", file=sys.stderr)
        return 1

    # ── 完整性检查：ASCII 下伸部不能被裁掉太多 ──
    over = ascii_descent_overflow(font_cache)
    if over > 3:
        print(f"ASCII 下伸部被裁 {over}px（上限 3px）："
              f"检查 ASCII_FONT_SIZE 与 BASELINE_Y", file=sys.stderr)
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
// 字体：汉字 — 文泉驿点阵宋体 12pt（16x16 点阵，GPLv2 + 字体嵌入例外）
//       ASCII — Noto Sans SC（SIL OFL 1.1）13px 渲染后二值化
// 布局：汉字 {HAN_W}x{HAN_H} 1bpp，每行 {HAN_W // 8} 字节 → {HAN_W * HAN_H // 8} 字节/字
//       ASCII {ASCII_W}x{ASCII_H} 1bpp，半宽  → {ASCII_W * ASCII_H // 8} 字节/字符
//       行优先、MSB 在最左；拉丁基线与汉字字身框底边同在 y={BASELINE_Y}。
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
        save_preview(han, bdf)
    return 0


def save_preview(han, bdf, cols=24):
    """把全部汉字字模拼成一张放大图，用于肉眼确认清晰度。"""
    from PIL import Image
    rows = (len(han) + cols - 1) // cols
    scale = 3
    img = Image.new("L", (cols * HAN_W * scale, rows * HAN_H * scale), 0)
    for i, ch in enumerate(han):
        data = bdf_to_bits(ord(ch), bdf)
        b = Image.new("L", (HAN_W, HAN_H), 0)
        px = b.load()
        for y in range(HAN_H):
            for x in range(HAN_W):
                if data[y * (HAN_W // 8) + x // 8] & (0x80 >> (x % 8)):
                    px[x, y] = 255
        img.paste(b.resize((HAN_W * scale, HAN_H * scale), Image.NEAREST),
                  ((i % cols) * HAN_W * scale, (i // cols) * HAN_H * scale))
    path = Path("C:/Users/11761/AppData/Local/Temp/font_cn_preview.png")
    img.save(path)
    print(f"  字模对照图：{path}")


if __name__ == "__main__":
    sys.exit(main())
