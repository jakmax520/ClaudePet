# 验证工具

把硬件 Buddy 链路验一遍的脚本。用于在没有 Claude Desktop 的环境下（或在其之前）
确认设备侧实现正确 —— 走的是同一套 wire protocol 与安全要求。

## 环境

```bash
python -m venv .venv
.venv/Scripts/python -m pip install bleak
# pair_with_pin.py 还需要 winrt 绑定，bleak 会一并带入
```

## 顺序

### 1. `pair_with_pin.py` —— 配对

```bash
python pair_with_pin.py            # 自动扫描 Claude-* 设备
python pair_with_pin.py 98:C3:77:F4:13:86
```

设备屏幕会显示六位数字，桌面上会弹出输入框，填进去即可。

**为什么不用 bleak 自带的 `pair=True`**：bleak 的 Windows 后端把配对仪式硬编码为
`DevicePairingKinds.CONFIRM_ONLY`（Just Works），回调只做无脑 `args.accept()`，
**在协议上无法提供 PIN**。而本固件要求 LE Secure Connections + MITM + 绑定，
设备侧是 DisplayOnly，必然需要主机输入 PIN，因此用 bleak 永远配不上。
本脚本直接调 WinRT 的 `DeviceInformationCustomPairing` 补上这一环。

脚本里有一处容易踩的坑：`winrt` 的 Python 绑定把 WinRT 的重载方法拆成了带后缀的
名字，`Accept(String pin)` 对应 `accept_with_pin(pin)`，而不是 `accept(pin)`
（后者是无参重载，会抛 `TypeError: Invalid parameter count`）。

### 2. `buddy_check.py` —— 链路体检

```bash
python buddy_check.py              # 扫描、连接、协议往返、状态切换
python buddy_check.py --approval   # 额外跑审批流程（需要人在设备上按键）
```

需要先完成配对：未配对的链路没有加密，订阅 TX 通知会失败
（NUS 特征要求加密访问）。

`--approval` 会依次发两个审批请求，提示你在设备上按 `OK`（批准）与 `DOWN`（拒绝），
并打印设备回传的 `{"cmd":"permission",...}`。

> 若设备已熄屏（30 秒无操作自动熄屏），**第一次按下会被吞掉、只用于唤醒**，
> 不会触发动作。跑审批测试前先按一下任意键把屏幕唤醒。

## 键位

| 按键 | 普通页面 | 菜单 | 审批页 |
| --- | --- | --- | --- |
| `DOWN` | 切换屏幕 | 移动选择 | **拒绝** |
| `OK` | 翻页 | 确认执行 | **批准** |
| `UP` | 熄屏/亮屏 | — | — |

长按 `DOWN` 开菜单。

## 设备侧诊断

固件启动时会打印板级初始化结果，连接与配对事件也会打印：

```
[hal] CW2017 在线 VERSION=0x0F
[hal] 显示就绪 240x320，背光 GPIO21
[hal] 按键就绪: GPIO0 ADC 三键分压 (UP/DOWN/OK)
[ble] advertising as 'Claude-1386'
[ble] connected, passkey 688084
[ble] auth ok (enc=1 authen=1 bonded=1)
```

`src/hal/hal.cpp` 里的 `HAL_BTN_DEBUG` 置 1 后，会每秒打印按键 ADC 电压并在
每次按下时打印键名，用于核对三键分压：

```
[btn] ADC=   3mV 档位=UP
[btn] ADC= 307mV 档位=DOWN
[btn] ADC= 604mV 档位=OK
[btn] ADC=2918mV 档位=松开
```
