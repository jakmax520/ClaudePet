# 安装与刷机

把 ClaudePet 刷进 **FoloToy AI Passport**（ESP32-C3）。

产品介绍、移植说明与已知限制见 [README.md](README.md)；这里只讲怎么装。

---

## 你需要什么

| 项目 | 说明 |
| --- | --- |
| 设备 | FoloToy AI Passport（ESP32-C3，8MB Flash） |
| 数据线 | **USB-C，必须支持数据传输** —— 只能充电的线认不出串口，这是最常见的坑 |
| 电脑 | Windows / macOS / Linux |
| 软件 | Python 3.8+，以及 `pip install esptool` |

设备通过 **USB-Serial-JTAG** 通信，不自带文件系统驱动，Windows 10/11、macOS、
主流 Linux 都免驱。设备在系统里显示为一个串口（Windows 上是 `COMx`）。

先确认能认到设备：

```bash
python -m esptool --chip esp32c3 --port COM4 chip_id
```

`COM4` 换成你自己的端口号。Windows 上可在「设备管理器 → 端口」里看，
或在 PlatformIO 环境下跑 `pio device list`。

> **关于 esptool 的版本差异**：本文一律用**下划线**形式（`chip_id` /
> `write_flash`），因为它在 esptool v4 和 v5 上都能跑。v5 会多打印一行
> `Deprecated: Command 'write_flash' is deprecated. Use 'write-flash' instead.`
> —— **忽略即可**，命令会正常执行。想消除这行警告就把下划线改成连字符；
> 但 v4 不认识连字符形式，所以文档统一用下划线。

---

## 方式一：刷预编译镜像（推荐）

适合只想把设备用起来的情况。**一条命令，含固件与内置角色包**。

从 [Releases](https://github.com/jakmax520/ClaudePet/releases) 下载
`ClaudePet-v1.0.0-full-8MB.bin`，然后：

```bash
python -m esptool --chip esp32c3 --port COM4 write_flash 0x0 ClaudePet-v1.0.0-full-8MB.bin
```

> **`0x0` 不能改。** 这是一个 8MB 的整片镜像，从地址 0 开始烧，
> 顺序覆盖 bootloader → 分区表 → 应用 → 文件系统。
>
> **这会清空设备上的所有数据**，包括蓝牙配对信息与角色包。

烧完设备自动重启，屏幕应显示宠物。约需 30 秒。

---

## 方式二：从源码构建

需要 [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/)。

```bash
git clone https://github.com/jakmax520/ClaudePet.git
cd ClaudePet

pio run                          # 编译
pio run -t upload --upload-port COM4   # 烧固件
```

首次烧录后设备还没有角色包，会用 ASCII 画的宠物兜底。装上 GIF 角色包：

```bash
pio run -t uploadfs --upload-port COM4
```

`uploadfs` 把仓库 `data/` 目录的内容烧进文件系统分区。仓库里预置了 `bufo`
一个角色包，也可用自带脚本从 `characters/` 暂存并烧录：

```bash
python tools/flash_character.py characters/bufo
```

> `data/` 与 `characters/` 的区别：前者是实际烧进设备的内容，后者是角色包的
> 原始素材库。角色包格式与制作方式见 README。

---

## 配对

设备开机即广播，名字形如 **`Claude-XXXX`**（XXXX 是蓝牙地址后四位）。

### 用 Claude Desktop

1. 打开 Claude Desktop，**Help → Troubleshooting → Enable Developer Mode**
2. 再点 **Developer → Open Hardware Buddy… → Connect**
3. 在设备列表里选 `Claude-XXXX`
4. **设备屏幕会显示六位数字**，把它输入系统弹出的配对框

> 若设备屏幕已熄灭，先按任意键唤醒 —— 熄屏时配对码不绘制，会看不到。

### 用仓库自带脚本（不依赖 Claude Desktop）

用于在没有 Claude Desktop 的环境下验证链路。

```bash
pip install bleak
python tools/verification/pair_with_pin.py

# 已知道蓝牙地址时可直接传入，跳过扫描：
python tools/verification/pair_with_pin.py 98:C3:77:F4:13:86
```

脚本会弹出一个桌面对话框，把设备屏幕上的六位数输进去。配对成功后可用
`buddy_check.py` 做一次协议往返体检：

```bash
python tools/verification/buddy_check.py
```

> 为什么不用通用的蓝牙配对面板：本固件要求 **LE Secure Connections + MITM +
> 绑定**，设备侧是 DisplayOnly（屏幕显示、主机输入），必须走 PIN 输入流程。
> `bleak` 的 Windows 后端把配对硬编码成 Just Works，处理不了，所以这个脚本
> 直接调用 WinRT 的 `DeviceInformationCustomPairing`。

---

## 操作

三个键共用一个 ADC 分压脚，键位按直觉重映射过：

| 按键 | 短按 | 长按 |
| --- | --- | --- |
| `DOWN` | 移动选择 / 切换页面 / 审批**拒绝** | 打开浮层菜单 |
| `OK` | 确认执行 / 翻页 / 审批**批准** | 打开浮层菜单 |
| `UP` | 熄屏 / 亮屏 | — |

设置菜单里的 **`clock rot`** 控制时钟页方向（`auto` / `port` / `land`）。
**本板没有重力传感器，`auto` 恒等于竖屏**，要横屏必须手动选 `land` —— 这不是
故障，详见 README。

---

## 常见问题

**认不到串口 / 没有 `COMx`**
换一根**支持数据传输**的 USB-C 线。这是绝大多数情况的原因。若设备之前以
异常状态运行，拔插一次重置。

**烧录报 `Could not open COMx`**
端口被别的程序占着 —— 关掉串口监视器、Arduino IDE、其他终端里的
`pio device monitor`。

**烧录成功但屏幕全黑**
先按一次键唤醒（出厂默认 30 秒自动熄屏）。若仍全黑，可能是面板批次的反色
设置不同：源码构建时把 `platformio.ini` 里的 `-DTFT_INVERSION_ON=1` 换成
`-DTFT_INVERSION_OFF=1` 重新烧录。画面呈负片（黑白颠倒）就是这一项。

**屏幕显示 `no character loaded`**
文件系统里没有角色包，或烧录镜像时没包含文件系统分区。按「方式二」跑一次
`pio run -t uploadfs`，或用完整镜像重刷。

**配对时窗口里找不到设备**
设备处于广播状态才会出现。若刚断开过连接，等几秒让它重新广播；同时确认
设备屏幕是亮的。

**配对码输错 / 配对失败**
重跑一次配对脚本即可，它会先解除旧绑定再重新配。

**时钟页出不来**
时钟页只在「没有会话在跑」且**时间已同步过**时显示，而时间是由主机通过
蓝牙下发的。也就是说：**没连过 Claude Desktop，就看不到时钟页**。

---

## 卸载 / 恢复出厂

设备没有独立的恢复分区。想彻底重置，重新刷一次完整镜像（见「方式一」）即可，
NVS 与文件系统都会被覆盖。
