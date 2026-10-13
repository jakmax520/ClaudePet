"""用 WinRT 原生 API 完成 PASSKEY 配对。

为什么需要这个脚本：Arduino/PlatformIO 生态常用的 bleak，其 Windows 后端
（backends/winrt/client.py）把配对仪式硬编码为 DevicePairingKinds.CONFIRM_ONLY
（Just Works），回调只做无脑 args.accept()，**无法处理需要输入 PIN 的配对**。
而本固件要求 LE Secure Connections + MITM + 绑定，且设备侧是 DisplayOnly
（屏幕显示六位码、主机输入），必然走到 PIN 输入这一步。

本脚本直接调用 WinRT 的 DeviceInformationCustomPairing，注册 PROVIDE_PIN
回调，并用桌面对话框向用户索取设备屏幕上显示的六位数字。

用法：
    python pair_with_pin.py [设备地址]
    # 省略地址时扫描并选取第一个名字以 Claude 开头的设备

依赖：
    pip install winsdk        # 或 winrt-* 系列包
    （bleak 会一并带入 winrt 的运行时绑定）
"""

import asyncio
import sys
import threading

sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)

from winrt.windows.devices.bluetooth import BluetoothLEDevice
from winrt.windows.devices.enumeration import (
    DeviceInformation,
    DevicePairingKinds,
    DevicePairingProtectionLevel,
)

ADDR_INT = None      # 由 main() 填入
pin_requested = threading.Event()
pin_ready = threading.Event()
pin_box: dict[str, str | None] = {"value": None}
stop_dialog = threading.Event()


def pin_dialog_loop() -> None:
    """独立线程：被配对回调唤醒后，弹对话框向用户索取 PIN。"""
    while not stop_dialog.is_set():
        if not pin_requested.wait(timeout=0.5):
            continue
        pin_requested.clear()
        pin_ready.clear()
        value = None
        try:
            import tkinter as tk
            from tkinter import simpledialog

            root = tk.Tk()
            root.withdraw()
            root.attributes("-topmost", True)
            value = simpledialog.askstring(
                "BLE 配对", "请输入设备屏幕上显示的六位数字：", parent=root)
            root.destroy()
        except Exception as exc:
            print(f"!!! PIN 对话框不可用: {type(exc).__name__}: {exc}", flush=True)
        pin_box["value"] = value
        print(f"    用户输入 PIN = {value!r}", flush=True)
        pin_ready.set()


def on_pairing_requested(sender, args) -> None:
    kind = args.pairing_kind
    print(f"[pairing_requested] pairing_kind = {kind}", flush=True)

    needs_pin = kind in (DevicePairingKinds.PROVIDE_PIN,
                         DevicePairingKinds.DISPLAY_PIN)
    if not needs_pin:
        print("    无需 PIN，自动确认（Just Works）", flush=True)
        args.accept()
        return

    print("    >>> 请求用户输入设备屏幕上的六位数字 ...", flush=True)
    pin_requested.set()
    if not pin_ready.wait(timeout=90):
        print("    !!! 等待用户输入超时", flush=True)
        return

    pin = (pin_box["value"] or "").strip()
    if not pin:
        print("    !!! 用户取消输入", flush=True)
        return

    # 注意 winrt 的 Python 绑定把 WinRT 的重载 Accept() 拆成了带后缀的名字：
    #   accept          -> Accept()              无参
    #   accept_with_pin -> Accept(String pin)    本脚本要用的是这个
    # 若误写 args.accept(pin) 会抛 TypeError: Invalid parameter count，
    # PIN 根本提交不出去，配对必然失败。
    print(f"    提交 PIN = {pin}", flush=True)
    args.accept_with_pin(pin)


def find_device_address() -> int | None:
    """扫描首个名字以 Claude 开头的设备，返回其蓝牙地址（整数）。"""
    try:
        from bleak import BleakScanner
    except ImportError:
        print("需要 bleak 来扫描设备：pip install bleak", flush=True)
        return None

    async def scan():
        found = await BleakScanner.discover(timeout=8.0, return_adv=True)
        for addr, (dev, adv) in found.items():
            name = (dev.name or adv.local_name or "")
            if name.startswith("Claude"):
                print(f"找到设备 {name} ({addr})", flush=True)
                return int(addr.replace(":", ""), 16)
        return None

    return asyncio.run(scan())


async def main() -> int:
    global ADDR_INT

    if len(sys.argv) > 1:
        ADDR_INT = int(sys.argv[1].replace(":", ""), 16)
    else:
        ADDR_INT = find_device_address()
    if ADDR_INT is None:
        print("未找到设备。请先唤醒设备，或把蓝牙地址作为参数传入。", flush=True)
        return 2

    threading.Thread(target=pin_dialog_loop, daemon=True, name="pin-dialog").start()

    device = await BluetoothLEDevice.from_bluetooth_address_async(ADDR_INT)
    if device is None:
        print("找不到设备（请先唤醒）", flush=True)
        return 2

    info = await DeviceInformation.create_from_id_async(device.device_information.id)
    print(f"    is_paired = {info.pairing.is_paired}", flush=True)
    print(f"    can_pair  = {info.pairing.can_pair}", flush=True)

    if info.pairing.is_paired:
        print(">>> 已有旧绑定，先解除 ...", flush=True)
        result = await info.pairing.unpair_async()
        print(f"    解除结果 = {result.status}", flush=True)
        await asyncio.sleep(1.0)
        info = await DeviceInformation.create_from_id_async(
            device.device_information.id)

    custom = info.pairing.custom
    token = custom.add_pairing_requested(on_pairing_requested)
    try:
        print(">>> 开始配对 —— 设备屏幕将显示六位数字，请在桌面弹窗中输入", flush=True)
        result = await custom.pair_with_protection_level_async(
            DevicePairingKinds.PROVIDE_PIN,
            DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION,
        )
        # 0 = PAIRED（DevicePairingResultStatus）
        print(f">>> 配对结果 = {result.status}  （0 = PAIRED）", flush=True)
        print(f">>> 使用的保护级别 = {result.protection_level_used}", flush=True)
    except Exception as exc:
        print(f">>> !!! 配对异常: {type(exc).__name__}: {exc}", flush=True)
    finally:
        custom.remove_pairing_requested(token)
        stop_dialog.set()

    return 0


sys.exit(asyncio.run(main()))
