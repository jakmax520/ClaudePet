#!/usr/bin/env python3
"""核验界面版式是否会溢出屏幕或撑破行宽。

度量来自中文字模（src/font_cn.h，由 tools/gen_cn_font.py 生成）：
    汉字 16x16，ASCII 8x16，行高 16，正文从 x=4 起 → 每行可用 236px。

这两类问题**在主机侧能精确算出来**（字模等宽），不像按键时序那样非上机不可。
改文案或改布局后跑一遍，不必等烧进设备才发现截断。

文案从 gen_cn_font.py 的 TEXTS 直接读 —— 单一来源，不会出现"脚本过了但
固件里是另一句话"。中英两种语言都核（面板要按两者中**较宽**的那个排版）。

局限：
  - 各页的**行组合**仍是硬编码（下面 PAGES）。改了 main.cpp 的页面结构，
    要同步这里，否则行数核算会失真。
  - 只算字符宽度的算术，不管字形像素（字模等宽，所以够用）。
  - 宠物动画与文字的重叠算不出来（PET 标题落在 y=0..110 的动画区内），
    只能上机肉眼确认。

用法：
    python tools/verification/layout_check.py
退出码 0 = 全部通过；1 = 有问题。
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from gen_cn_font import TEXTS          # noqa: E402

W, H = 240, 320
TOP = 110                              # 内容起始线，见 character.cpp 的 PEEK_TOP
LINE_H = 17                            # 信息页正文：16px 字模 + 1px 行距
PET_LINE_H = 16                        # 宠物屏 HOWTO：12 行已铺满内容区
TEXT_X = 4                             # 正文左边缘
PANEL_PAD = 8                          # 菜单面板左右内边距
PANEL_W = 170                          # main.cpp 的 MENU_W
PANEL_VAL_W = 44                       # main.cpp 的 MENU_VAL_W
USABLE = W - TEXT_X                    # 一行可用的像素宽

# 文案表：{key: (en, zh)}
DOC = {k: (en, zh) for k, en, zh in TEXTS}
LANGS = (0, 1)
LANG_NAME = ("英文", "中文")

errors, notes = [], []


def px(s):
    """字符串的像素宽：汉字 16px，ASCII 8px。"""
    return sum(8 if ord(c) < 0x80 else 16 for c in s)


def check_text(key, raw=None):
    """核一条文案在两种语言下的宽度。"""
    for lang in LANGS:
        s = raw if raw is not None else DOC[key][lang]
        if px(s) > USABLE:
            errors.append(
                f"[超宽] {key} ({LANG_NAME[lang]}): {px(s) if raw is None else px(s)}px "
                f"> {USABLE}px  「{s}」")


def emit(name, y, s, step=LINE_H, indent=0):
    # indent 是子项缩进（px）；正文从 TEXT_X 起，扣掉缩进后才是这一行的可用宽度
    if px(s) + indent > USABLE:
        errors.append(f"[超宽] {name}: 「{s}」缩进 {indent}+{px(s)}px > {USABLE}px")
    if y + LINE_H > H:
        errors.append(f"[溢出] {name}: y={y}+{LINE_H} = {y + LINE_H} > {H}  「{s}」")
    return y + step


def t(key, lang):
    return DOC[key][lang]


# ── 信息屏六页：每页的 (类型, 参数) 序列 ────────────────────────────────────────
# "ln" = 一行正文；"gap" = 8px 空行；"num" = 数字占位（宽度按实际最长算）
INFO_PAGES = {
    # 第三项（fmt 为第四项）是该行的子项缩进，与 main.cpp 的 SUB_X 一致
    "ABOUT": [("ln", "ab_1", 24), ("ln", "ab_2", 24), ("gap",),
              ("ln", "ab_3", 24), ("ln", "ab_4", 24)],
    "BUTTONS": [("ln", k) for k in
                ("bt_ok", "bt_ok_1", "bt_dn", "bt_dn_1",
                 "bt_up", "bt_up_1", "bt_hold", "bt_hold_1")],
    "CLAUDE": [("fmt", "cl_sess", "99", 24), ("fmt", "cl_run", "99", 24),
               ("fmt", "cl_wait", "99", 24),
               ("gap",), ("ln", "cl_link"), ("fmt", "cl_via", "completed", 24),
               ("fmt", "cl_ble", "encrypted", 24), ("fmt", "cl_last", "999999", 24),
               ("fmt", "cl_state", "attention", 24)],
    "DEVICE": [("big100", 24), ("fmt", "dv_vbat", "3.91", 24),
               ("fmt", "dv_cur", "-999", 24), ("ln", "dv_sys"),
               ("fmt", "dv_owner", "jakmaxxxxx", 24), ("fmt", "dv_up", "123h 45m", 24),
               ("fmt", "dv_heap", "31", 24), ("fmt", "dv_brt", "4", 24),
               ("fmt", "dv_bt", "linked", 24), ("fmt", "dv_temp", "42", 24)],
    "BT-link": [("big", "bt_linked", 24), ("raw", "Claude-A1B2", 24),
                ("raw", "A4:CF:12:3E:8B:01", 24), ("fmt", "bl_last", "999999", 24)],
    "BT-pair": [("big", "bl_disc", 24), ("raw", "Claude-A1B2", 24),
                ("raw", "A4:CF:12:3E:8B:01", 24), ("ln", "bl_pair", 24),
                ("ln", "bl_p1", 48), ("ln", "bl_p2", 48),
                ("ln", "bl_p3", 48), ("ln", "bl_p4", 48)],
    "CREDITS": [("ln", "cr_by"), ("raw", "Jason Liu", 24), ("gap",),
                ("ln", "cr_hw"), ("ln", "cr_chip", 24)],
}

# (key, 缩进)：标题行顶格，说明行缩进一级
PET_HOWTO = [("pt_mood", 0), ("pt_how_m", 24), ("pt_how_m2", 24),
             ("pt_fed", 0), ("pt_how_f", 24),
             ("pt_en", 0), ("pt_how_e", 24), ("pt_how_i", 24), ("pt_how_i2", 24),
             ("pt_how_d", 24), ("pt_how_o", 24), ("pt_how_h", 24)]


def check_info_page(name, seq, lang):
    y = TOP + 2
    y += 18                                    # 页眉第一行（Info + n/N）
    y += LINE_H                                # section 名
    for item in seq:
        kind = item[0]
        if kind == "gap":
            y += 8
        elif kind == "ln":
            y = emit(name, y, t(item[1], lang),
                     indent=(item[2] if len(item) > 2 else 0))
        elif kind == "fmt":
            # 用最长的实际取值代入，检查真实宽度
            s = t(item[1], lang).replace("%u", item[2]).replace("%lu", item[2])
            s = s.replace("%d", item[2]).replace("%s", item[2])
            s = s.replace("%.10s", item[2]).replace("%.02d", "00").replace("%+d", item[2])
            s = s.replace("%lu", item[2]).replace("%02lu", "45")
            y = emit(name, y, s, indent=(item[3] if len(item) > 3 else 0))
        elif kind == "big":
            y = emit(name, y, t(item[1], lang), step=20,
                     indent=(item[2] if len(item) > 2 else 0))
        elif kind == "big100":
            y = emit(name, y, "100%", step=20,
                     indent=(item[1] if len(item) > 1 else 0))
        elif kind == "raw":
            y = emit(name, y, item[1], indent=(item[2] if len(item) > 2 else 0))
    if y > H:
        errors.append(f"[页溢出] {name} ({LANG_NAME[lang]}): 结束 y={y} > {H}（超 {y - H}px）")
    return y


def main():
    print(f"画布 {W}x{H}，内容区 y={TOP}..{H}（可用 {H - TOP}px），"
          f"汉字 16px / ASCII 8px / 信息页行高 {LINE_H}px、宠物屏 {PET_LINE_H}px，"
          f"正文从 x={TEXT_X} 起（每行 {USABLE}px）\n")

    # ── 逐条文案的宽度 ──
    print("── 全部文案宽度 ──")
    widest = []
    for key, (en, zh) in DOC.items():
        for lang in LANGS:
            s = (en, zh)[lang]
            widest.append((px(s), key, lang, s))
    widest.sort(reverse=True)
    for wpx, key, lang, s in widest[:5]:
        flag = "  <<< 超宽" if wpx > USABLE else ""
        print(f"  {wpx:3d}px  {key:14s} ({LANG_NAME[lang]})  「{s}」{flag}")
    for key in DOC:
        check_text(key)

    # ── 面板 ──
    # 只有**带取值列的面板**（设置页）需要条目列 + 取值列都放得下：
    #   PANEL_PAD + 条目宽 + 间隙 + PANEL_VAL_W <= PANEL_W
    # 菜单页（demo 的状态跟在同一行）和重置页都没有独立的取值列，
    # 只需条目本身不超面板宽。
    print("\n── 菜单面板 ──")
    settings_keys = ["set_bright", "set_sound", "set_bt", "set_wifi", "set_led",
                     "set_hud", "set_rot", "set_pet", "set_lang", "set_reset", "set_back"]
    plain_keys = ["menu_settings", "menu_off", "menu_help", "menu_about",
                  "menu_demo", "menu_close", "rst_char", "rst_factory", "rst_confirm"]
    for lang in LANGS:
        worst = max(settings_keys, key=lambda k: px("> " + DOC[k][lang]))
        need = PANEL_PAD + px("> " + DOC[worst][lang]) + PANEL_VAL_W
        ok = need <= PANEL_W
        print(f"  {LANG_NAME[lang]}: 设置页最宽「> {DOC[worst][lang]}」"
              f" 条目+取值需 {need}px / 面板 {PANEL_W}px  {'✓' if ok else '✗ 与取值列重叠'}")
        if not ok:
            errors.append(f"[面板超宽] {LANG_NAME[lang]}: 「{DOC[worst][lang]}」"
                          f"{need}px > {PANEL_W}px")

        worst2 = max(plain_keys, key=lambda k: px("> " + DOC[k][lang]))
        need2 = PANEL_PAD + px("> " + DOC[worst2][lang])
        ok2 = need2 <= PANEL_W
        print(f"  {LANG_NAME[lang]}: 无取值列面板最宽「> {DOC[worst2][lang]}」"
              f" 需 {need2}px / 面板 {PANEL_W}px  {'✓' if ok2 else '✗ 超面板宽'}")
        if not ok2:
            errors.append(f"[面板超宽] {LANG_NAME[lang]}: 「{DOC[worst2][lang]}」"
                          f"{need2}px > {PANEL_W}px")

    # ── 信息屏 ──
    for lang in LANGS:
        print(f"\n── 信息屏（{LANG_NAME[lang]}）──")
        for name, seq in INFO_PAGES.items():
            end = check_info_page(name, seq, lang)
            margin = H - end
            print(f"  {name:10s} 结束 y={end:3d}  余量 {margin:3d}px"
                  f"{'   <<< 溢出' if margin < 0 else ''}")

    # ── 宠物屏 HOWTO：12 行从 y=120 起铺到 312 ──
    for lang in LANGS:
        y = TOP + 2 + 8
        for key, indent in PET_HOWTO:
            y = emit("PET HOWTO", y, t(key, lang), step=PET_LINE_H, indent=indent)
        margin = H - y
        if y > H:
            errors.append(f"[页溢出] PET HOWTO ({LANG_NAME[lang]}): 结束 y={y} > {H}")
        print(f"\n── 宠物屏 HOWTO（{LANG_NAME[lang]}）结束 y={y}  余量 {margin}px"
              f"{'   ← 最紧' if 0 <= margin < 16 else ''}")

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
