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

### 3. `clock_hold.py` —— 把设备按在时钟页上

```bash
python clock_hold.py                     # 保持 150 秒
python clock_hold.py 600                 # 保持 600 秒
python clock_hold.py --address 98:C3:77:F4:13:86
```

时钟页的显示条件是「无会话在跑 + **RTC 已同步** + 未打开菜单」，而 RTC 同步
**只能由主机通过蓝牙下发** —— 设备没连过主机，时钟页根本不显示，无从验证。
本脚本发一次 `time` 后持续喂 `(0,0,0)` 心跳，把设备稳定按在时钟页上，便于
目视检查渲染。

**顺带看横屏**：脚本跑起来、设备显示竖屏时钟后，在设备上长按 `DOWN` →
`settings` → `clock rot` → 改成 `land`，再退出菜单。本板没有重力传感器，
`auto` 档恒判定为竖屏，**必须手动锁** —— 不是故障，见 README 的说明。

### 4. `layout_check.py` —— 版式核算（不需要设备）

```bash
python tools/verification/layout_check.py    # 无需 venv、无需蓝牙
```

TFT_eSPI 的位图字体只能整数倍缩放，信息屏与宠物屏整体用 size 2（12×16px/字符，
行高 16，从 x=4 起每行上限 19 字符）。改一句文案就可能撑破行宽、或把内容顶出
屏幕下沿 —— 而这两类问题**在主机侧能精确算出来**，不必烧进去才发现。

脚本按各页的 y 累加与字符宽度核算，越界时指出具体哪一行、超出多少像素，
退出码非 0。

两个注意点：

- 页面内容在脚本里**硬编码**。改了 `main.cpp` 的文案，必须同步更新脚本里的
  `PAGES` 清单，否则会拿到一个"通过"的假象。
- 宠物动画与文字的重叠算不出来（PET 标题落在 y=0..110 的动画区内），只能上机看。

### 5. `btn_debounce_check.py` —— 按键边沿状态机（不需要设备）

```bash
python tools/verification/btn_debounce_check.py
```

把 `src/hal/hal.cpp` 里 `buttonsUpdate()` 的去抖状态机复刻到主机侧，喂各种 ADC
档位波形，断言「一次物理按下只被主循环消费一次」。

这是为 2026-10-16 修的一个偶发 bug 建的回归测试：翻页时按一次 OK 偶尔跳两页
（宠物屏只有 2 页，取模后回到原页，看起来像没反应）。根因是 `pressed`/`released`
这两个「本轮有效」的边沿标志，复位写在了去抖门槛判断的提前返回**之后**，于是未达
门槛的轮次保留了上一轮的 `pressed`，被主循环重复消费。

这类 bug 在真机上偶发、靠人工按键几乎复现不了，而状态机是纯逻辑 —— 喂一串波形
就能稳定复现（同一波形修复前消费 3 次、修复后 1 次）。脚本里同时保留了修复前的
逻辑用于演示，判定只看修复后的。

改动 `hal.cpp` 的按键逻辑时需要同步脚本体里的状态机镜像。

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
