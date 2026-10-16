#!/usr/bin/env python3
"""核验 info / pet 界面的版式是否会溢出屏幕或撑破行宽。

背景：TFT_eSPI 的位图字体只能整数倍缩放，size 2 是 12x16px/字符、行高 16，
240px 屏宽下每行上限 19 字符。改一句文案就可能越过这两个上限，而这类问题
**在主机侧是能精确算出来的**（不像按键时序，只能上机测）。这个脚本就是
把算术固化下来，改完文案跑一遍，不必等烧进设备才发现截断。

局限（重要）：
  - 页面内容在下面硬编码。**改了 main.cpp 里的文案，必须同步更新 PAGE_* 清单**，
    否则脚本会拿着旧文案给你一个通过的假象。
  - 只算字符宽度的算术，不管字形的实际像素（位图字体是等宽的，所以够用）。
  - 宠物 GIF 与文字的重叠（PET 标题落在动画区 y=0..110 内）算不出来，
    只能上机肉眼确认。

用法：
    python tools/verification/layout_check.py

退出码 0 = 全部通过；1 = 有超宽或溢出。
"""

import sys

# 画布与字体度量，与 main.cpp 的 W/H、TSIZE_PAGE 对应
W, H = 240, 320
TOP = 110          # 内容起始线，见 character.cpp 的 PEEK_TOP
GLYPH_W, LINE_H = 12, 16
USABLE = H - TOP

errors = []


def emit(tag, x, y, text, step=LINE_H):
    """记录一行文字，返回下一行的 y。越界记进 errors。"""
    width = x + len(text) * GLYPH_W
    if width > W:
        errors.append(f"[超宽] {tag}: x={x} + {len(text)}字x12 = {width} > {W}  「{text}」")
    if y + LINE_H > H:
        errors.append(f"[溢出] {tag}: y={y}+16 = {y + LINE_H} > {H}  「{text}」")
    return y + step


def info_page(name, rows):
    """信息屏一页：页眉两行（Info + 计数行、section 名）+ 正文。

    rows 元素：str = 正文行；None = 空行（+8px）；(text, step) = 自定义步进。
    """
    y = TOP + 2
    emit("hdr", 4, y, "Info")
    emit("hdr-n", W - 40, y, "1/6")
    y += 18
    emit("sec", 4, y, "BLUETOOTH")        # 取最长 section 名做上界
    y += 16
    for r in rows:
        if r is None:
            y += 8
        elif isinstance(r, tuple):
            y = emit(name, 4, y, r[0], r[1])
        else:
            y = emit(name, 4, y, r)
    if y > H:
        errors.append(f"[页溢出] {name}: 结束 y={y} > {H}（超 {y - H}px）")
    return y


def check_pet_stats():
    y = TOP + 16
    emit("stats", 6, y - 6, "mood")
    y += 20
    emit("stats", 6, y - 6, "fed")
    y += 20
    emit("stats", 6, y - 6, "energy")
    # 标签放大后图形条要让位，否则被压在标签底下
    for tag, right in [("心形", 78 + 3 * 16 + 2 + 2),
                       ("圆点", 78 + 9 * 9 + 2),
                       ("能量条", 86 + 4 * 13 + 9)]:
        if right > W:
            errors.append(f"[超宽] stats {tag}最右 = {right} > {W}")
    y += 24
    emit("stats", 12, y, "Lv 99")
    if 6 + 72 > W:
        errors.append("[超宽] stats Lv 徽章右边缘超屏宽")
    y += 24
    for i, s in enumerate(["approved 65535", "denied   65535", "napped   999h59m",
                           "tokens   999.9M", "today    999.9M"]):
        emit("stats", 6, y + i * 16, s)
    y += 64 + 16
    if y > H:
        errors.append(f"[页溢出] PET STATS: 结束 y={y} > {H}")
    return y


def check_pet_howto():
    y = TOP + 2 + 8
    for s in ["MOOD", "approve fast = up", "deny lots = down", "FED",
              "50K tokens = level", "ENERGY", "drains over time",
              "idle 30s = off", "any button = wake", "DOWN: next screen",
              "OK: next page", "hold DOWN: menu"]:
        y = emit("howto", 6, y, s)
    if y > H:
        errors.append(f"[页溢出] PET HOWTO: 结束 y={y} > {H}")
    return y


def check_pet_header():
    """PET 顶部标题：按宠物图形左边缘限宽，超长先丢 owner 前缀，仍超退回 size 1。

    标题画在宠物之上，越过 GIF 左边缘就压上去了。宠物屏走 peek 模式，GIF 缩半
    居中 —— bufo 的 96x100 变成 48x50 @ x=96..144，故限宽到 x<96。
    """
    gif_left = 96                        # 与 main.cpp 的兜底值一致
    limit = (gif_left - 8) // GLYPH_W
    out = [f"  宠物左边缘 x={gif_left}，标题上限 {limit} 字符"]
    for owner, pet in [("jakmax520", "Buddy"), ("", "Buddy"),
                       ("a", "Rosie"), ("verylongownername", "Buddy")]:
        title = f"{owner}'s {pet}" if owner else pet
        dropped = False
        if len(title) > limit and owner:
            title, dropped = pet, True
        big = len(title) <= limit
        if big:
            emit("pet-hdr", 4, 70, title)
        note = "size2" if big else "size1 回退"
        if dropped:
            note += "，丢了 owner 前缀"
        out.append(f"  owner={owner!r:20s} -> 「{title}」{len(title)}字 {note}")
    return out


# ── 页面清单（改 main.cpp 文案后同步这里）────────────────────────────────────

PAGES = [
    ("ABOUT", [
        "I watch your Claude", "desktop sessions.", None, "I sleep when idle,",
        "wake when you work.", None, "Press OK to approve", "a prompt from here.",
        "18 species to pick", "in settings."]),
    ("BUTTONS", [
        "OK    centre", "    next page", "    approve prompt", "DOWN  lower",
        "    next screen", "    deny prompt", "UP    upper", "    screen off",
        "hold DOWN", "    menu"]),
    ("CLAUDE", [
        "sessions  99", "running   99", "waiting   99", None, "LINK",
        "via       completed", "ble       encrypted", "last msg  999999s",
        "state     attention"]),
    # usb in 一行恒不显示（本板 vBus_mV 恒 0），不计入排版
    ("DEVICE", [("100%", 20),
                "battery  3.91V", "current  -999mA", "SYSTEM", "owner    jakmaxxxxx",
                "uptime   123h 45m", "heap     31KB", "bright   4/4",
                "bt       linked", "temp     42C"]),
    ("BT-link", [("discover", 20),
                 "Claude-A1B2", "A4:CF:12:3E:8B:01", "last msg  999999s"]),
    ("BT-pair", [("discover", 20),
                 "Claude-A1B2", "A4:CF:12:3E:8B:01", "TO PAIR", "Open Claude desktop",
                 "> Developer", "> Hardware Buddy", "connects via BLE"]),
    ("CREDITS", [
        "made by", "Felix Rieseberg", None, "source", "anthropics/claude-",
        "desktop-buddy", None, "hardware", "M5StickC Plus", "ESP32 + AXP192"]),
]


def main():
    # 正文从 x=4 起排，所以每行可用 (W-4)px，即 19 个字符 —— 不是 W/12=20
    print(f"画布 {W}x{H}，内容区 y={TOP}..{H}（可用 {USABLE}px），"
          f"size 2 = {GLYPH_W}x{LINE_H}px，从 x=4 起每行上限 "
          f"{(W - 4) // GLYPH_W} 字符\n")

    print("── 信息屏 ──")
    for name, rows in PAGES:
        end = info_page(name, rows)
        print(f"  {name:10s} 结束 y={end:3d}  余量 {H - end:3d}px")

    print("\n── 宠物屏 ──")
    print(f"  PET STATS  结束 y={check_pet_stats():3d}")

    # HOWTO 是三页里最紧的一页：12 行 x 16px 铺满到 312，只剩 8px 余量，
    # 加一行就会溢出。改动时优先看这里的余量。
    howto_end = check_pet_howto()
    print(f"  PET HOWTO  结束 y={howto_end:3d}  余量 {H - howto_end:3d}px"
          f"{'   ← 最紧，加行需重排' if H - howto_end < 16 else ''}")

    print("\n── PET 标题 ──")
    for line in check_pet_header():
        print(line)

    print()
    if errors:
        print(f"发现 {len(errors)} 个问题：")
        for e in errors:
            print("  " + e)
        return 1
    print("全部页面在画布内，无超宽 / 溢出。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
