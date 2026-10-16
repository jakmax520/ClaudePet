#!/usr/bin/env python3
"""按键边沿状态机的回归测试 —— 守护「一次物理按下只被消费一次」。

背景（2026-10-16 修的一个偶发 bug）：
    信息屏/宠物屏翻页时，按一次 OK 有时会跳两页 —— 宠物屏只有 2 页，
    取模后正好回到原页，看起来像「按了没反应」。

根因在 src/hal/hal.cpp 的 buttonsUpdate()：pressed/released 是「本轮 update
内有效」的边沿标志，复位却写在去抖门槛判断的提前返回**之后**：

    if (s_candidateCount < 2) return;      # 未达门槛直接返回
    for (i...) { b.pressed = false; ...    # 复位在这里，够不着

于是未达门槛的轮次会保留上一轮的 pressed，而主循环每轮都查 wasPressed()，
同一个按下沿被重复消费。命中窗口是「按下沿置位后的下一轮 ADC 读数跳变了一下」
（s_candidate 被重置 → count 回到 1 → 提前返回）—— 按键刚接触时接触电阻不稳，
这种跳变很常见，所以表现为偶发。

修法：把两个边沿标志的复位移到提前返回之前，无条件每轮执行。

本脚本把 buttonsUpdate() 的状态机原样搬到主机侧。这个 bug 在真机上「偶发」、
靠人工按键复现成本极高，而状态机是纯逻辑 —— 喂指定波形就能稳定复现，
这也是当初定位它的手段。

局限：脚本里的状态机是 hal.cpp 的**镜像**，不是从源码解析出来的。它验证的是
「这个状态机的设计是否正确」，不是「hal.cpp 当前是不是这个状态机」。改动
hal.cpp 的按键逻辑时，记得同步下面的 model_buttons_update()。

用法：
    python tools/verification/btn_debounce_check.py
退出码 0 = 通过；1 = 有波形出现重复消费。
"""

import sys

NONE, UP, DOWN, OK = 0xFF, 0, 1, 2
NAMES = {NONE: "松开", UP: "UP", DOWN: "DOWN", OK: "OK"}


class Btn:
    def __init__(self):
        self.down = self.pressed = self.released = False


def model_buttons_update(state, raw, reset_early):
    """hal.cpp buttonsUpdate() 的状态机。

    reset_early=False 复刻修复前的行为（复位在提前返回之后），
    仅用于演示 bug；True 是当前固件的行为。
    返回本轮是否被主循环消费（即任一件的 pressed 为真）。
    """
    s_btn, _ = state["btn"], None
    if raw == state["candidate"]:
        if state["count"] < 2:
            state["count"] += 1
    else:
        state["candidate"] = raw
        state["count"] = 1

    if reset_early:
        for b in s_btn:
            b.pressed = False
            b.released = False

    if state["count"] >= 2:
        now = state["candidate"]
        for i, b in enumerate(s_btn):
            if not reset_early:
                b.pressed = False
                b.released = False
            down = (now == i)
            if down and not b.down:
                b.down = True
                b.pressed = True
            elif not down and b.down:
                b.down = False
                b.released = True
        state["stable"] = now

    return any(b.pressed for b in s_btn)


def run(wave, reset_early):
    state = {"btn": [Btn() for _ in range(3)], "stable": NONE,
             "candidate": NONE, "count": 0}
    return [model_buttons_update(state, raw, reset_early) for raw in wave]


# (名称, 波形, 期望的消费次数)
# 波形以「一次物理按下」为单位：从松开态出发，中途可能跳变，最后回到松开态。
WAVES = [
    ("OK 按下，中途一轮读数跳变（当初的触发场景）",
     [NONE, NONE, OK, OK, NONE, OK, OK, NONE, NONE], 1),
    ("OK 理想波形，全程不抖",
     [NONE, NONE, NONE, OK, OK, OK, OK, NONE, NONE, NONE], 1),
    ("OK 全程抖动（每轮档位都在变）",
     [NONE, OK, NONE, OK, NONE, OK, OK, OK, NONE, OK, NONE, NONE], 1),
    ("DOWN 切屏键，同样的跳变波形",
     [NONE, NONE, DOWN, DOWN, NONE, DOWN, DOWN, NONE, NONE], 1),
    ("OK 按下后长时间按住不放（应只有一次按下沿）",
     [NONE, NONE, OK] + [OK] * 20 + [NONE, NONE], 1),
    ("连续两次独立轻触（应消费两次）",
     [NONE, NONE, OK, OK, NONE, NONE, NONE, OK, OK, NONE, NONE], 2),
    ("轻触过快、未达 2 次采样门槛（应被整段丢弃）",
     [NONE, NONE, OK, NONE, NONE], 0),
]


def main():
    print("按键边沿状态机回归测试")
    print("（复刻 src/hal/hal.cpp 的 buttonsUpdate()；波形为逐轮 ADC 档位）\n")

    failed = []

    print("── 修复前的行为（仅演示 bug，不参与判定）──")
    for name, wave, _ in WAVES:
        hits = sum(run(wave, reset_early=False))
        mark = "  <<< 重复消费" if hits > 1 else ""
        print(f"  {name}\n    消费 {hits} 次{mark}")

    print("\n── 修复后的行为（判定项）──")
    for name, wave, want in WAVES:
        consumed = run(wave, reset_early=True)
        got = sum(consumed)
        ok = (got == want)
        if not ok:
            failed.append(f"{name}: 期望 {want} 次，实际 {got} 次")
        marks = "".join("#" if c else "." for c in consumed)
        print(f"  [{'通过' if ok else '失败'}] {name}")
        print(f"          波形 {[NAMES[w] for w in wave]}")
        print(f"          消费 {marks}  合计 {got} 次（期望 {want}）")

    print()
    if failed:
        print(f"{len(failed)} 项未通过：")
        for f in failed:
            print("  " + f)
        return 1
    print(f"全部 {len(WAVES)} 项通过：一次物理按下只被消费一次。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
