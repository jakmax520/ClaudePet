# ClaudePet

把 Anthropic 的 [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
移植到 **FoloToy AI Passport**（ESP32-C3）的固件。

原版面向 M5StickC Plus（ESP32 双核 / AXP192 / ST7789v2 135×240 / MPU6886）。
本项目保留其全部应用逻辑与宠物资源，只替换底层硬件抽象。

## 移植原则

**渲染层与宠物资源与原版逐字节一致。** 具体来说：

| 文件 | 状态 |
| --- | --- |
| `src/buddies/`（18 种宠物 × 7 状态，约 4000 行） | **逐字节未改** |
| `src/buddy.cpp` / `buddy.h` / `buddy_common.h` | 仅改 3 个布局常量的值 |
| `src/character.cpp` / `character.h` | 仅改宠物区高度常量 |
| `src/data.h` / `src/xfer.h` | 未改动 |
| `src/ble_bridge.cpp` | 改用 NimBLE 栈（见下） |
| `src/ble_bridge.h` | 未改动（对外接口不变） |
| `src/main.cpp` | 布局与菜单排版改动（见下） |
| `src/stats.h` | 1 处类型修正 |

原版渲染接口本就是 `buddyRenderTo(TFT_eSPI*, ...)`，而非面向 M5，所以移植面很小。
18 种宠物能保持原封不动，是因为它们引用的是 `BUDDY_*` 具名常量（约 250 处）
而非字面坐标 —— 改常量的值即可整体重排。

## 硬件

| 项目 | 规格 |
| --- | --- |
| MCU | ESP32-C3 (QFN32) rev v1.1，单核 RISC-V @160MHz |
| Flash | 8MB，**无 PSRAM** |
| 屏幕 | ST7789P3，240 × 320 竖屏，4 线 SPI |
| 按键 | 三个键共用一个 ADC 分压脚（GPIO0） |
| 电量计 | CW2017（I2C，地址 0x63） |
| 音频 | ES8311 codec（本版未接入） |
| 无线 | Wi-Fi 2.4GHz + BLE 5，不支持蓝牙经典 |
| 控制台 | USB-Serial-JTAG（GPIO18/19） |

引脚与电气参数取自 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)
的 `components/bsp/include/bsp_pins.h`。**仅取硬件事实，未沿用其 ESP-IDF 代码结构。**

## 硬件抽象层的做法

原版依赖 `M5.Lcd` / `M5.Axp` / `M5.BtnA`、`BtnB` / `M5.Imu` / `M5.Beep` / `M5.Rtc`。
本项目**不重写上层调用**，而是提供同名同签名的门面（`src/hal/M5StickCPlus.h`），
经 `-Isrc/hal` 让上层原有的 `#include <M5StickCPlus.h>` 解析到它。因此：

- 上层四个包含该头文件的源文件，**包含语句一个字都没改**
- `main.cpp` 里的 M5 调用（45 处）**一行都没重写**

这也是宠物代码能保持原封不动的原因。

## 相对原版的改动

| 位置 | 改动 | 原因 |
| --- | --- | --- |
| `main.cpp` | 画布 `W/H` 由 135×240 改为 240×320；`LED_PIN` 置 -1 并加保护 | 满屏显示；原版 LED 在 GPIO10，本板该脚是 I2C SDA，驱动成数字口会破坏 I2C 总线 |
| `main.cpp` | 精灵图改在 BLE 之前分配 | 实测先起 BLE 再申请 150KB 会因连续块不足而失败，而彼时空闲堆仍有 160KB |
| `main.cpp` | 菜单/面板排版常量化并放大（见下） | 原尺寸按 135px 画布算，在 240px 上偏小 |
| `main.cpp` | 时钟/配对码/进度条的绝对 y 下移 40 | 画布由 240 加高到 320 |
| `buddy.cpp` | `BUDDY_X_CENTER` 67→120、`BUDDY_CANVAS_W` 135→240、`BUDDY_Y_BASE` 30→70 | 宠物区随画布重排（一处改值，18 种宠物全部生效） |
| `character.cpp` | `PEEK_TOP` 70→110；宠物区 140→180 | 同上 |
| `ble_bridge.cpp` | 改用 NimBLE-Arduino | 见"为什么换 BLE 栈" |
| `stats.h` | `min()` 第二个实参加 `(uint32_t)` | xtensa 上 `uint32_t` 是 `unsigned int`，riscv32 上是 `unsigned long`，模板推导会失败 |
| `platformio.ini` | 换板、换驱动、去 M5 依赖、加 NimBLE | — |
| `partitions.csv` | 新增 | 8MB Flash，app 3MB + LittleFS 5MB |
| `src/hal/` | 新增 | 硬件抽象层 |
| `src/hal/tft_c3_spi_fix.h` | 新增 | 修正 TFT_eSPI 在 C3 上的 SPI 基址缺陷，详见该文件 |

## 因硬件缺失而降级的功能

这些在原版存在，本板没有对应硬件，均已在代码注释中标注：

| 功能 | 原版依赖 | 本版行为 |
| --- | --- | --- |
| 摇一摇（dizzy） | MPU6886 | 失效（HAL 恒返回「平放朝上」） |
| 翻面打盹（nap） | MPU6886 | 失效 |
| 横屏时钟自动切换 | MPU6886 | 失效；`auto` 档恒竖屏，须在设置里手动锁 `land` |
| 提示音 | 蜂鸣器 | 静默（Settings 里 Sound 开关仍可切换） |
| 审批 LED 闪烁 | GPIO10 红灯 | 停用（该脚在本板是 I2C SDA） |
| 硬关机 | AXP192 | 记录日志不执行（无可控电源轨；且 UP 键所在 GPIO0 同时是 ADC 分压脚，休眠唤醒不可靠） |

## 键位映射

上游面向 M5StickC Plus **两键**设计，且两个键语义不对称：
**BtnA = 移动/下一页**（菜单里移动选择、页面间切换、长按开菜单），
**BtnB = 确认/执行**（菜单里执行选中项、翻页、审批拒绝）。

若照搬为 `OK→BtnA`、`DOWN→BtnB`，会出现「OK 只是移动选择、DOWN 才是确认」
的别扭手感。故按三键直觉重映射：

| 本板按键 | 对应原版 | 作用 |
| --- | --- | --- |
| `DOWN` | BtnA | 移动选择 / 切换屏幕 / 长按开菜单 / 审批**拒绝** |
| `OK` | BtnB | 确认执行 / 翻页 / 审批**批准** |
| `UP` | 电源键短按 | 切换熄屏 |

`main.cpp` 里 `BtnA`、`BtnB` 在审批页原本分别是「批准」与「拒绝」，对调后
这两段分支已互换，否则会变成「OK 拒绝、DOWN 批准」。

按键走 GPIO0 的 ADC 电阻分压，实测电压与厂商参数吻合
（`UP`=3mV / `DOWN`=307mV / `OK`=604mV / 松开=2918mV，见 `bsp_pins.h` 的
`BSP_BTN_MV_TABLE`）。去抖取连续 2 次采样 —— 主循环要推 153.6KB 精灵图、
一帧约 30ms，原先的 3 次去抖跨约 100ms，比一次轻触还长，会整段丢掉。

`HalButton` 的 `pressed` / `released` 是「**本轮 `update()` 内有效**」的边沿
标志，复位必须每轮无条件执行。它一旦落在去抖门槛的提前返回之后，未达门槛的
轮次就会保留上一轮的 `pressed`，而主循环每轮都查 `wasPressed()` —— 同一个按下沿
被重复消费，表现为「按一次翻两页」（2 页的页面取模后回到原页，看起来像没反应）。
主机侧回归测试见 `tools/verification/btn_debounce_check.py`。

## 构建与烧录

> **只想把设备刷起来？** 见 **[INSTALL.md](INSTALL.md)** —— 含预编译镜像的
> 一条命令刷机流程、配对步骤与常见问题排查。下面是源码构建的简版。

需要 [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/)。

```bash
# 编译
pio run

# 烧录固件
pio run -t upload --upload-port COM4

# 烧录角色包（GIF 宠物，可选）
# 先把角色包目录放到 data/characters/<name>/，再：
pio run -t uploadfs --upload-port COM4
```

或用仓库自带的脚本（会把 `characters/<name>` 暂存到 `data/` 再烧录）：

```bash
python tools/flash_character.py characters/bufo
```

## 为什么换 BLE 栈

本板无 PSRAM，满屏 16 位精灵图占 153.6KB。用 Arduino 内核自带的 Bluedroid 时，
启动可用堆约 228.7KB，被精灵图占去后 Bluedroid 再吃约 72KB，**剩余仅 2.9KB** ——
不足以容纳协议行（单行上限 4KB）与 JSON 解析，连接一建立就会分配失败。实测数据：

| 配置 | 运行时空闲堆 |
| --- | --- |
| Bluedroid + 16 位满屏 | 2.9 KB（不可用） |
| **NimBLE-Arduino + 16 位满屏** | **约 32 KB** |
| Bluedroid + 阶段 A 的 135×240 | 91.8 KB |

换成 NimBLE 后 `ble_bridge.cpp` 的对外接口（`ble_bridge.h`）未变，其余代码零改动。
迁移中遇到的三个坑已记在该文件注释里：

1. NimBLE-Arduino 无 `onPassKeyNotify` 回调，拿不到协议栈生成的配对码。
   解法是自生成六位码并经 `setSecurityPasskey()` 交给协议栈 ——
   库在 `NimBLEServer::handleGapEvent()` 的 `BLE_GAP_EVENT_PASSKEY_ACTION`
   分支里读回该值并调用 `ble_sm_inject_io()`，因此屏幕显示值与协议栈实际
   使用值天然一致。**注意该值不能恰好是 `123456`**：库把它当作「未设置」的
   哨兵，会转去问回调，故生成时需排除。
2. NimBLE 2.x 无 `setAccessPermissions`，加密要求改由创建特征时的
   `NIMBLE_PROPERTY` 标志表达。
3. `NimBLEAdvertising::setName()` 必须在 `enableScanResponse(true)` 之后调用：
   该方法先看扫描响应是否已启用，已启用才把名字放进去，否则塞进广播包 ——
   而广播包已被 128 位服务 UUID 占去大半，装不下 `Claude-XXXX`，会静默失败。
   实测丢了名字时广播里 `local_name` 为空，而 Claude Desktop 的设备列表按名字
   前缀 `Claude` 过滤，就选不中设备。

## 菜单与面板排版

TFT_eSPI 的位图字体只能整数倍缩放，没有中间档：

| 字号 | 单字符 | 设置页面板宽下限 |
| --- | --- | --- |
| size 1 | 6×8px | 约 114px（占屏宽 48%） |
| size 2 | 12×16px | 约 220px（占屏宽 92%） |

两档都实测过，最终取 size 1，靠加宽面板（118→150）与拉大行距（14→16）获得舒展观感。
常量集中在 `main.cpp` 的 `MENU_*` 一组里，四个菜单函数共用，避免改一个漏一个。

**info / pet 两页是另一套策略**：这两组页面（信息屏六页、宠物屏两页）正文短、留白多，
用 size 1 时内容只占内容区一半，观感很空，故统一取 size 2，常量是 `main.cpp` 的
`TSIZE_PAGE`。代价是行宽上限由每行 40 字符降到 19 字符、行高 8→16，放不下的句子
做了缩句或拆行（少数修饰性细节撤下界面、只留在 MANUAL.md）。改文案时按
19 字符/行核算，`tools/verification/layout_check.py` 会把这一步算给你看。

## 时钟页

上游的时钟页有个前提条件：

```cpp
bool clocking = ... && dataRtcValid() && _onUsb;   // 插在充电座上才显示
```

`_onUsb` 来自 AXP192 的 VBUS 检知。**本板没有这个电路**（`HalPower::GetVBusVoltage()`
恒返回 0），所以该条件永远为假，时钟页在这个移植里会是一段**跑不到的死代码**。
本项目去掉了该要求，时钟页因此第一次变得可用。

代价是：本板无法判断是否在 USB 供电，所以屏幕仍会按 30 秒超时自动熄屏
（上游在 USB 上是不熄屏的，好让时钟常显）。没有去掉这条，是因为本板由约 500mAh
电池供电，常亮会几小时耗尽。

**横屏时钟已恢复**（`main.cpp` 的 `ENABLE_LANDSCAPE_CLOCK = 1`），坐标按本板
320×240 重排过，集中在 `drawClock` 前的 `LSC_*` 一组常量里，改版式只动这一处。

它曾被禁用：启用后实测触发一条 `ESP_RST_PANIC`，当时归咎于横屏路径本身。后来
查明与该路径无关 —— 根因是按键诊断打印的越界索引（见「已知取舍与限制」），横屏
只是改变了主循环节奏、更容易踩中那条路径。根因修复后横屏已无已知障碍。

**本板只能手动锁横屏**：设置 → `clock rot` 三档 `auto/port/land`。auto 档靠重力
判断，而本板没有 IMU（`HalImu` 恒返回「平放朝上」），所以 **auto 在本板恒为竖屏**。
这是缺该硬件的必然结果，不是缺陷。

## 内存状况

本板无 PSRAM，满屏 16 位精灵图占 153.6KB，内存一直是本移植最紧的约束。
`src/hal/hal.cpp` 的 `heapGuard()` 每轮主循环调用一次
`heap_caps_check_integrity_all()`，每 5 秒打印一次余量：

```
[heap] ok free=31804 largest=13300
```

**`largest` 比 `free` 更值得看**：它是最长的一段连续空闲，任何超过它的单次
分配都会失败，哪怕账面上的空闲量还很多。实测空闲 31.8KB 时最大连续块只有
13.3KB（约 42%）—— 碎片化程度不低，这也是为什么满屏精灵图**必须在 BLE 之前
分配**（见 `main.cpp` 的 setup：先起 BLE 再申请 150KB 会因连续块不足而失败，
而彼时空闲堆仍有 160KB）。

顺带记录一个失败的尝试：曾把循环任务栈从 8KB 提到 16KB，想验证某个崩溃是否
由栈溢出引起。**结论是无效** —— 崩溃依旧，而多占的 8KB 堆把最大连续块从
13.3KB 压到 9.2KB，反而加剧紧张，故已撤销。空闲时栈水位
（`uxTaskGetStackHighWaterMark`）为 5984 字节，8KB 栈是够的。

## 已知取舍与限制

1. **「按 DOWN 必现重启」的崩溃 —— 已定位并修复**（`45aabd3`，自引入该缺陷的
   `165e348` 起存在）。

   症状是 `ESP_RST_PANIC`，且 `MTVAL` 落在字符串内容上（形如 `0x31322e25` 即
   `"%.21"`）。根因是本项目自己加的一段按键诊断打印：

   ```cpp
   static const char* const kDbgNames[4] = {"UP", "DOWN", "OK", "松开"};
   Serial.printf("[btn] ADC=%umV -> %s\n", (unsigned)mV, kDbgNames[now]);
   ```

   `now` 的取值域是 `{0=UP, 1=DOWN, 2=OK, HAL_BTN_NONE(=0xFF)}` —— 而 `0xFF` 是
   「松开」的哨兵值。拿它去索引 4 元素数组会越界读 255 项，把相邻内存里的字符串
   字面量字节当成 `char*` 交给 `%s` 解引用。已钳位为
   `kDbgNames[now < 4 ? now : 3]`。

   **四门排除掉的思路，以及对的原因**，值得作为方法论记下：

   | 怀疑方向 | 为什么站不住 |
   | --- | --- |
   | 栈溢出 | 栈 8KB→16KB 后崩溃依旧，反而把最大连续空闲块从 13.3KB 压到 9.2KB；空闲时栈水位 5984 字节，本来就够 |
   | 堆结构性损坏 | `heap_caps_check_integrity_all()` 只走空闲链表、校验块头，**对越界「读」完全无感** |
   | 主机侧驱动复现 | 按键走 `M5.update()` 的路径，必须有真实按键事件；从主机发心跳改写状态机不经过这条路径，因此长时间被误判为「不复现」 |
   | 串口标记定位 | USB-CDC 是异步缓冲的，崩溃时未发出的字节会丢 —— 不加 `Serial.flush()` 时看到的「最后标记」是缓冲假象 |

   `MTVAL` 是字符串内容这一点本身就是强提示：说明有 `char*` 被越界取到了。

   修复后实测：连续按键 31 次（DOWN 18 次 / OK 13 次）**零重启**（串口全程只有
   抓取开始时那一条 `boot#`），堆稳定在 `free=31892 / largest=13300`。

   诊断脚手架（`DBG_STAGE` 标记、各绘制函数的 stage 调用）保留在代码中但由
   `character.cpp` 的 `DBG_STAGE_ENABLED` 开关统一关闭 —— 标记函数含阻塞的
   `Serial.flush()` 且挂在主循环热路径上，开着会拖垮帧率。

2. **角色包上限 1.8MB**（原版限制，未改）。本板 LittleFS 分区 5MB，余量充足。

## 验证状态

已在真实设备（MAC `98:C3:77:F4:13:86`，`COM4`）上验证：

| 项目 | 结果 |
| --- | --- |
| 编译 | 通过 |
| 板级初始化：屏幕、背光、CW2017、三键 | 通过 |
| 屏幕点亮、颜色正常、满屏无黑边 | 通过（目视） |
| 宠物水平居中、垂直位置合理 | 通过（目视） |
| 三键 ADC 分压与去抖 | 通过（实测电压与厂商参数吻合） |
| 三键交互：DOWN 移动/切屏/长按菜单、OK 确认/翻页、UP 熄屏 | 通过 |
| **连续按键 31 次（DOWN 18 / OK 13）零重启** | **通过**（修复越界索引后实测） |
| **横屏时钟**：320×240 直绘、时间右半居中、宠物左上、三行间距均匀 | **通过**（目视 + 零重启） |
| LittleFS 挂载 + GIF 角色包加载与逐帧解码播放 | 通过 |
| ≥5 个页面目视检查：Info、Pet 统计、菜单、settings、宠物页 | 通过 |
| BLE 广播 `Claude-*` 且广播包含 NUS 服务 UUID（NimBLE 换栈后复测） | 通过 |
| **六位配对码配对（LE Secure Connections + MITM + 绑定）** | **通过** |
| **加密链路上 NUS 双向收发** | **通过** |
| **审批：OK 批准 / DOWN 拒绝，同一 ID 重复按键只回传一次** | **通过** |
| **CW2017 电量读数** | **通过**（上报 95% / 4154mV） |
| 30 秒心跳超时清除审批 | 脚本执行完成，未目视确认屏幕 |
| 真实 Claude Desktop 会话驱动的状态切换 | **未验证** |

配对与协议收发是用本机自建工具验证的（`tools/` 之外，见下方说明），走的是与
Claude Desktop 相同的 wire protocol 与安全要求，但**未经 Claude Desktop 本体验证** ——
该软件已在本机安装（winget，v2.19675.1），登录受地区限制无法完成。

要补上这一项，需要一台能登录 Claude Desktop 的机器，开启开发者模式
（**Help → Troubleshooting → Enable Developer Mode**）后：
**Developer → Open Hardware Buddy… → Connect**，选择 `Claude-*`，
把设备屏幕上显示的六位配对码输入系统配对框。

### 验证过程的两个副产物

1. Windows 侧配对脚本踩到一个 API 用法坑：`winrt` 的 Python 绑定把 WinRT 重载
   方法拆成了带后缀的名字，`DevicePairingRequestedEventArgs.Accept(String pin)`
   对应 `accept_with_pin(pin)`，而非 `accept(pin)` —— 后者是无参重载，
   会抛 `TypeError: Invalid parameter count`，PIN 根本提交不出去。
2. 上游的按键去抖（连续 3 次采样）在本板会漏掉轻触，原因见「键位映射」一节。

## 许可证

MIT，Copyright 2026 Anthropic, PBC。全文见 [LICENSE](LICENSE)。
上游仓库：<https://github.com/anthropics/claude-desktop-buddy>
