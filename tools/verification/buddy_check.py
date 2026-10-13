"""Hardware Buddy 链路体检：扫描 → 连接 → 协议往返 →（可选）审批流程。

自包含，只依赖 bleak，不引用其他仓库。协议行格式与 UUID 见 REFERENCE.md。

前置：设备已与 Windows 配对（绑定存在）。若尚未配对，先用 pair_with_pin.py。
在未配对状态下连接，加密特征会返回 Insufficient Authentication。

用法：
    python buddy_check.py              # 只做协议往返
    python buddy_check.py --approval   # 额外跑审批流程（需要人在设备上按键）
"""

import argparse
import asyncio
import json
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)

SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
CHR_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # 主机 -> 设备，write
CHR_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # 设备 -> 主机，notify

T0 = time.time()
rx_lines: list[str] = []


def log(msg: str) -> None:
    print(f"[+{time.time() - T0:5.1f}s] {msg}", flush=True)


def line(payload: dict) -> bytes:
    return (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode()


def heartbeat(total, running, waiting, msg, entries=(), tokens=0, tokens_today=0,
              prompt=None) -> bytes:
    payload = {
        "total": int(total), "running": int(running), "waiting": int(waiting),
        "msg": msg, "entries": list(entries),
        "tokens": int(tokens), "tokens_today": int(tokens_today),
    }
    if prompt:
        payload["prompt"] = {"id": prompt["id"], "tool": prompt.get("tool", ""),
                             "hint": prompt.get("hint", "")}
    return line(payload)


class LineDecoder:
    """按 \\n 切分通知分片；单行超长时丢弃并计数，避免内存无界增长。"""

    def __init__(self, limit: int = 4096) -> None:
        self._buf = bytearray()
        self._limit = limit
        self._drop = False
        self.overflow = 0

    def feed(self, data: bytes) -> list[str]:
        out: list[str] = []
        for b in data:
            if b == 0x0A:
                if self._drop:
                    self._drop = False
                elif self._buf:
                    if self._buf[-1:] == b"\r":
                        del self._buf[-1:]
                    out.append(self._buf.decode("utf-8", "replace"))
                self._buf.clear()
            elif self._drop:
                continue
            elif len(self._buf) >= self._limit:
                self._buf.clear()
                self._drop = True
                self.overflow += 1
            else:
                self._buf.append(b)
        return out


async def find_device():
    from bleak import BleakScanner
    found = await BleakScanner.discover(timeout=8.0, return_adv=True)
    for addr, (dev, adv) in found.items():
        name = (dev.name or adv.local_name or "")
        if name.startswith("Claude"):
            log(f"找到设备 {name} ({addr})")
            return addr
    return None


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--approval", action="store_true", help="额外跑审批流程")
    ap.add_argument("--address", default=None)
    args = ap.parse_args()

    from bleak import BleakClient

    addr = args.address or await find_device()
    if not addr:
        log("未找到 Claude-* 设备。请先唤醒设备。")
        return 2

    decoder = LineDecoder()

    def on_notify(_s, data: bytearray) -> None:
        for l in decoder.feed(bytes(data)):
            rx_lines.append(l)
            log(f"[RX] {l}")

    client = BleakClient(addr, timeout=60.0)
    log("连接（依赖已建立的绑定，不重新配对）")
    await client.connect()
    try:
        await client.start_notify(CHR_TX, on_notify)
    except Exception as exc:
        log(f"!!! 订阅失败: {type(exc).__name__}: {exc}")
        log("    说明绑定缺失或未加密 —— 先跑 pair_with_pin.py")
        await client.disconnect()
        return 3
    log("*** 已连接，通知已订阅（链路已加密）***")

    async def send(payload: bytes, label: str) -> None:
        for i in range(0, len(payload), 180):
            await client.write_gatt_char(CHR_RX, payload[i:i + 180], response=False)
        log(f"[TX] {label}")

    # ---- 协议往返 ----
    await send(line({"time": [int(time.time()), -time.timezone]}), "time")
    await asyncio.sleep(0.5)
    await send(line({"cmd": "owner", "name": "buddy_check"}), "owner")
    await asyncio.sleep(0.5)
    await send(line({"cmd": "status"}), "status")
    await asyncio.sleep(2.0)

    for l in rx_lines:
        if '"ack":"status"' in l:
            try:
                d = json.loads(l)["data"]
                log("设备状态：")
                log(f"    name={d.get('name')!r} owner={d.get('owner')!r} sec={d.get('sec')}")
                bat, sysinfo = d.get("bat", {}), d.get("sys", {})
                log(f"    电量 {bat.get('pct')}% / {bat.get('mV')}mV / usb={bat.get('usb')}")
                log(f"    堆余 {sysinfo.get('heap')} B   LittleFS {sysinfo.get('fsFree')}/{sysinfo.get('fsTotal')} B")
            except Exception as exc:
                log(f"    解析失败: {exc}")

    # ---- 状态切换 ----
    log("心跳 1：0 会话（期望屏幕 sleep/idle）")
    await send(heartbeat(0, 0, 0, "buddy_check idle"), "heartbeat(idle)")
    await asyncio.sleep(8)
    log("心跳 2：1 会话运行中（期望屏幕 busy）")
    await send(heartbeat(1, 1, 0, "buddy_check busy"), "heartbeat(busy)")
    await asyncio.sleep(8)

    # ---- 审批 ----
    if args.approval:
        for pid, expect in (("approve-1", "按 OK 批准"), ("deny-1", "按 DOWN 拒绝")):
            before = sum(1 for l in rx_lines if '"permission"' in l)
            log(f">>> 审批请求 {pid} —— 请在设备上{expect}")
            await send(heartbeat(1, 0, 1, f"approval {pid}",
                                 prompt={"id": pid, "tool": "Bash", "hint": "/tmp/x"}),
                       f"approval({pid})")
            for _ in range(30):
                await asyncio.sleep(1)
                if sum(1 for l in rx_lines if '"permission"' in l) > before:
                    break
            perms = [l for l in rx_lines if '"permission"' in l]
            if len(perms) > before:
                log(f"    收到决定: {perms[-1]}")
            else:
                log("    30 秒内未收到决定（设备可能熄屏，第一次按下被吞掉只用于唤醒）")
            await asyncio.sleep(3)

    await client.disconnect()
    log("已断开")
    return 0


sys.exit(asyncio.run(main()))
