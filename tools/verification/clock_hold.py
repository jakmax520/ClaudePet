"""把设备按在时钟页上，便于目视检查时钟渲染（含横屏）。

为什么需要这个脚本：时钟页的显示条件（`main.cpp` 的 `clocking`）是「没有会话
在跑、RTC 已同步、未打开菜单」。其中 **RTC 同步（`dataRtcValid()`）只能由主机
通过蓝牙下发** —— 也就是说设备没连过主机，时钟页压根不显示，无从验证。

本脚本补上这一环：连接设备 → 发一次时间同步 → 之后每 5 秒喂一次 `(0,0,0)`
心跳（维持「无会话」），把设备稳定按在时钟页上，直到超时。

用法：
    python clock_hold.py                       # 保持 150 秒
    python clock_hold.py 600                   # 保持 600 秒
    python clock_hold.py --address 98:C3:77:F4:13:86

前置：设备已与主机配对（绑定存在）。未配对时订阅 TX 通知会失败（NUS 特征要求
加密访问），先用 pair_with_pin.py。

**想看横屏时钟**：脚本跑起来、设备显示竖屏时钟后，在设备上长按 `DOWN` →
`settings` → `clock rot` → 改成 `land`，然后退出菜单。本板没有重力传感器，
`auto` 档恒判定为竖屏，**必须手动锁** —— 这不是故障，详见 README。
"""

import argparse
import asyncio
import json
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)

CHR_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # 主机 -> 设备，write
CHR_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # 设备 -> 主机，notify

HEARTBEAT_PERIOD = 5.0   # 秒。设备侧没有心跳超时，但断了连接状态会变，稳妥起见持续喂。


def line(payload: dict) -> bytes:
    return (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode()


def log(msg: str) -> None:
    print(msg, flush=True)


async def find_device():
    from bleak import BleakScanner
    found = await BleakScanner.discover(timeout=8.0, return_adv=True)
    for addr, (dev, adv) in found.items():
        name = (dev.name or adv.local_name or "")
        if name.startswith("Claude"):
            log(f"[*] 找到设备 {name} ({addr})")
            return addr
    return None


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", nargs="?", type=int, default=150,
                    help="保持多少秒（默认 150）")
    ap.add_argument("--address", default=None, help="设备蓝牙地址，省略则自动扫描")
    args = ap.parse_args()

    from bleak import BleakClient

    addr = args.address or await find_device()
    if not addr:
        log("!! 未找到 Claude-* 设备。请确认设备已开机（屏幕亮着更容易被发现）。")
        return 2

    def on_notify(_s, data) -> None:
        log(f"[RX] {bytes(data).decode('utf-8', 'replace').strip()}")

    client = BleakClient(addr, timeout=60.0)
    log("[*] 连接（用已有绑定，不重新配对）")
    await client.connect()
    try:
        await client.start_notify(CHR_TX, on_notify)
    except Exception as exc:
        log(f"!!! 订阅失败: {type(exc).__name__}: {exc}")
        log("    说明绑定缺失或未加密 —— 先跑 pair_with_pin.py")
        await client.disconnect()
        return 3

    async def send(payload: bytes) -> None:
        # 与 buddy_check.py 一致：NUS 单次写入上限按 180 字节切片。
        for i in range(0, len(payload), 180):
            await client.write_gatt_char(CHR_RX, payload[i:i + 180], response=False)

    await send(line({"time": [int(time.time()), -time.timezone]}))
    log("[TX] time —— 时钟页从此可用")
    await asyncio.sleep(0.5)

    log(f"[*] 开始保持 {args.seconds} 秒。现在可以在设备上切到 clock rot = land 看横屏。")
    end = time.time() + args.seconds
    next_tick = 0
    while time.time() < end:
        await send(line({"total": 0, "running": 0, "waiting": 0,
                         "msg": "clock hold", "entries": [],
                         "tokens": 0, "tokens_today": 0}))
        now = time.time()
        if now >= next_tick:
            next_tick = now + 30.0
            log(f"[*] 还剩 {int(end - now)}s")
        await asyncio.sleep(HEARTBEAT_PERIOD)

    await client.disconnect()
    log("[*] 已断开")
    return 0


sys.exit(asyncio.run(main()))
