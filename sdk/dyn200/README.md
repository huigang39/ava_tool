# DYN-200 C SDK（含 ava_tool 数据表）

纯 C 的 DYN-200 动态扭矩传感器 SDK，支持 Windows、Linux 和 macOS 的 RS-485
串口，涵盖 Modbus RTU 测量、参数读写、清零、恢复出厂，以及 HEX 主动上传
模式 0/3 的帧解析。

SDK 同时提供两套接口：

| 接口 | 用途 |
| --- | --- |
| `dyn200_port_param` / `dyn200_param` / `dyn200_rt` | ava_tool 的 SDK 面板按命名约定自动生成的数据表：选串口、配参数、看实时数据 |
| 扁平 C API（`dyn200_open` / `dyn200_read` / …） | 自己写程序、Python 调用，或把标量接口直接绑到 ava_tool 的 LOCAL 变量上 |

两套接口共用同一个连接，所以面板里连上的串口，程序里也照样能用
`dyn200_update()` / `dyn200_get_torque_nm()` 取到最近一帧。

```sh
make
make test

# Windows（默认 8N1，地址 1）
build/win/example_read.exe COM7 9600 1
# 通信模式 0（HEX 6 字节主动上传）
build/win/example_read.exe COM7 38400 active0
# `0` 是 active0 的简写
build/win/example_read.exe COM7 38400 0
# 通信模式 3（HEX 8 字节主动上传）
build/win/example_read.exe COM7 38400 active3
# 用数据表接口读（等价于 ava_tool 面板里的操作）
build/win/example_tables.exe COM7 9600 1 [滤波值]
# Linux
./build/linux/example_read /dev/ttyUSB0 9600 1
```

最后一个参数是传感器机码。Modbus 读取允许 `1..120`，出厂设置通常为 `1`；
`0` 是广播地址，不能用于读取返回值。

传感器需要设置为通信模式 1（Modbus RTU）。接线为黄色 RS485-A、蓝色
RS485-B；传感器仍需独立 24 V 供电。SDK 使用说明书中的原始地址，不添加
PLC 常见的 40000/44000 偏移。

`dyn200_open()` 使用常见的 8N1；若传感器菜单配置为两个停止位，请改用
`dyn200_open_ex(..., 2)`。

通信模式 0/3 不使用 Modbus 机码。调用 `dyn200_open_active()` 后，分别使用
`dyn200_read_active6()` 或 `dyn200_read_active8()`；读取函数会在连续字节流中
自动寻找 CRC 正确的帧边界。主动上传帧不含功率，SDK 按扭矩和转速计算功率。

## 接入 ava_tool

新版 SDK 调用器顶部提供 **DYN200** 快捷按钮，点击即可同时加载配套动态库和头文件。
同一排还提供 KPS6050D、DSOX2024A 和 FSA v3。安装包会携带这些配套文件；
使用便携版时请保留 `ava_tool.exe` 旁边的 `sdk` 文件夹。也可按以下步骤手动加载。

1. SDK 面板 → **Browse DLL** → `sdk/dyn200/lib/win/dyn200.dll`
2. **Browse Header** → `sdk/dyn200/inc/dyn200.h`
3. **串口表 `dyn200_port_param`**：`count` 是电脑上的串口数量，`name`/`desc`
   是 `index` 那一个（Windows 会给出 `USB-SERIAL CH340 (COM7)` 这样的描述）。
   这张表在面板里是**按钮式操作表**：没有"下发"，也没有"自动读取"，
   只有一排按钮——**搜索 / 连接 / 断开**。因为它的职责就是搜索和连接：
   按下"搜索"先枚举串口并把选中的口（`port`，没填就用 `index` 指向的那个）
   自动填好；按下"连接"就按下面的参数把口打开。新版 ava_tool 在搜索后提供
   **串口选择**下拉框，列出全部端口号及设备描述；选择时同步更新 `index` 和
   `port`，然后点击"连接"。重新搜索按端口名保留选择，设备拔出时提示不在搜索
   结果中，不会自动切换到其他适配器。也可以直接
   在 `port` 里手填 `COM7`（手填优先），再按一次"连接"
4. 在同一张表里填 `baud_rate`、`protocol`、`device_address`、`stop_bits`、
   `timeout_ms`；这三个按钮对应的就是 `action = CONNECT` / `DISCONNECT` /
   `RESCAN`（一次性，做完自动回到 NONE）
5. 选好口之后也可以直接去读 rt 表：SDK 会用同样的参数自动把串口打开
6. **实时表 `dyn200_rt`**：勾上 **自动读取**，`measurement.*` 就是实时扭矩/
   转速/功率；每个字段左边的勾选框决定要不要持续更新到变量监视器，
   按住 `=` 手柄拖到变量监视器或曲线窗口即可持续记录
7. 读回来的值有变化时，那一行的底色会闪烁一下，一眼就能看出哪些数据更新了
8. **参数表 `dyn200_param`**：`device.*` 是传感器里当前存着的设置（连上后
   自动读回），要改哪一项就改 `filter.val` / `zero_on_boot.enable` /
   `direction.reversed`，编辑后自动下发（也可以点 **下发**）
9. 清零：把 `dyn200_rt.command.zero` 改成 1；收工：串口表 `action = DISCONNECT`
   再读一次
10. 通信报错（超时、`Modbus exception`、`unexpected device address: expected 1,
    received 252` 这类）时看面板里的 **SDK 调用日志**（带时间戳，失败是红字，
    后面还跟着 SDK 的 `last_error` 文本），再对照下面的错误码说明排查

### 勾选与"持续"的规矩

- **`SDK returned error -4` 是什么意思**：`-4` = `DYN200_ERR_NOT_OPEN`，就是
  "还不知道该连哪个串口"。在串口表里按一次"搜索"（`port` 会自动填上选中的
  口）或填好 `port`/参数再按"连接"，rt 表就会自己连上。错误文本同样写在实时表的
  `last_error` 字段里，其他常见码：`-1` 参数错、`-2` 这个口打不开、
  `-6` 超时（波特率/接线/机码不对）、`-9` 协议错、`-10` Modbus 异常应答。
- 把参数表、实时表或其中字段的 `=` 手柄拖入变量管理器/监视器，即自动持续读取；
  在变量管理器或监视器中修改绑定值时自动下发。同步频率用表头的频率滑块调整
  （默认 20 Hz），不再需要自动读取/自动写入开关或字段更新勾选框。
  **串口表既没有下发也没有自动读取**：它只有搜索/连接/断开三个按钮。
- 表格旁边不再显示每次调用的绿色返回值，改成面板顶部的**SDK 调用日志**
  （可滚动、可清空、带时间戳，失败红字）：周期读取成功不刷屏（只在从失败恢复
  时记一条），失败、手动读取、下发和串口表的搜索/连接/断开都会记下来，
  失败时还会附上 SDK 的 `last_error` 文本。
- **`unexpected device address: expected 1, received 252` 是什么意思**：SDK 按
  Modbus 发了请求，回来的第一个字节不是机码 1，说明这不是一条 Modbus 应答——
  最常见的原因是传感器菜单里的**通信模式不是 1（Modbus）**，而是模式 0/3 的
  主动上传：那样线上是连续的数据帧，第一个字节是扭矩高位，正好会是 252(0xFC)
  这种值；其次是波特率/停止位不匹配或 A/B 接反。先在传感器菜单里确认通信模式：
  如果就是 0/3，把串口表的 `protocol` 改成 6 或 8（波特率也要一致）再按"连接"，
  之后 rt 表直接能读。
- 导入头文件**不会**自动把这些表加进变量管理器：需要在变量管理器/曲线里用某个
  表或某个字段时，把它的 `=` 手柄拖过去即可（只拖字段也行，不会连带建整表）。
- 拖入的字段持续同步，移除最后一个绑定变量后停止自动同步。字段类型显示为
  `u32`、`i32`、`f32`、`f64` 等短名称，数值变化时仍会闪烁提示。
- 编辑任意字段都会立刻下发一次。分组字段（`filter.*`、`zero_on_boot.*`、
  `command.*` 等）会先自动置位同名的 `write_<组名>` 门控，`write_*` 只下发
  被门控的那一组，下发完立即清零——所以新建一张表、或者打开"周期写入"，
  都不会反复写传感器。
- `action` 和 `command.*` 是一次性动作，执行后自动回到 0，不会被自动写入重放。
- **串口表 `dyn200_port_param` 的参数会随 ava_tool 会话保存**（端口、波特率、
  机码下次自动带出来）；实时表每次从 0 开始，面板不会让实时表重放历史命令。
- 连接是**自动的**：串口表读一次就把选中的口记下来并连上；之后 rt 表（以及
  参数表的下发）在串口掉了的时候会自己用这套参数重连，不用每次手动连。
- 已经连着的时候改 `baud_rate`/`protocol`/`device_address`/`stop_bits`/
  `timeout_ms` 再读一次，SDK 会按新参数重连；改 `port`/`index` 则是换口。
- **波特率与传感器模式决定硬件上限**：9600、8N1 下 Modbus 一次读取仅
  8 字节请求 + 17 字节应答就需要约 26 ms，另有帧间静默、设备处理和驱动延迟。
  主动上传 6/8 字节在 115200、8N1 下分别需约 0.52/0.69 ms；这只是线上时间，
  不代表传感器实际生成新数据的速率。改变连接参数时必须与传感器菜单一致。
  面板顶部的 `[rate]` 日志会写"实测 N Hz / 每次多少 ms"，据此判断是卡在波特率
  （ms per call ≈ 理论线上时间）还是卡在丢帧（failed 占比高）。
- `action = DISCONNECT` 是"先不连"，参数保留，再点 CONNECT 或换一个口名就会
  重新连上；调用扁平 API 的 `dyn200_close()` 则是彻底关闭，之后不会再自动重连。
- 自动重连失败后会等 2 秒再试，所以在自动读取时把没接设备的表打开也不会
  每 50 ms 去敲一次串口。

### 数据表字段

`dyn200_port_param`（串口表：面板里就是 搜索/连接/断开 三个按钮；参数会随会话保存）

| 字段 | 说明 |
| --- | --- |
| `index` | 输入：浏览/选择第几个串口，0 起 |
| `count`、`name`、`desc` | 只读：串口数量，以及 `index` 那一个的名字和描述 |
| `port` | 输入：要连接的口（例如 `COM7`）；空的话用 `index` 指向的那一个，Read now 会自动填上 |
| `protocol` | 1 = Modbus；6 = 主动上传 6 字节；8 = 主动上传 8 字节。0 → 1 |
| `baud_rate` | 波特率，0 → 9600 |
| `device_address` | Modbus 机码 1..120，0 → 1；主动上传模式忽略 |
| `stop_bits` | 1 或 2，0 → 1（8N1） |
| `timeout_ms` | 读写超时，0 → 200 |
| `action` | 一次性动作：`CONNECT` / `DISCONNECT` / `RESCAN` |
| `connected`、`active_port`、`active_protocol`、`active_baud_rate`、`active_device_address` | 只读：当前实际连接状态 |
| `last_error_code`、`last_error` | 只读：最近一次操作的结果 |

程序里想显式执行一次动作，用 `dyn200_port_apply(&ports)`（面板不用它，因此这张表
在面板里不会出现"下发"按钮）。

`dyn200_param`（参数表：传感器自身设置）

| 字段 | 说明 |
| --- | --- |
| `scaling.torque_decimals` / `speed_decimals` / `power_decimals` | 与传感器菜单一致；0 → 默认 `(1, 0, 0)`。只在本地换算，不写进传感器 |
| `filter.val` | 滤波 1..100；只有 Modbus 模式能写 |
| `zero_on_boot.enable` | 0/1：上电是否自动清零 |
| `direction.reversed` | 0/1：扭矩是否反向 |
| `write_filter` / `write_zero_on_boot` / `write_direction` | 编辑对应组时自动置 1 的门控，下发后清零 |
| `device.filter` / `device.zero_on_boot` / `device.reversed` | 只读：传感器里当前存着的值，连上后约 2 秒回读一次 |
| `link.port` / `link.protocol` / `link.baud_rate` | 只读：当前实际在用的串口/通讯模式/波特率（连接信息放这里，不占实时表） |
| `connected`、`last_error_code`、`last_error` | 只读反馈 |

`dyn200_rt`（实时表：只放需要高频刷新的东西）

| 字段 | 说明 |
| --- | --- |
| `write_command` | 编辑 `command.*` 时自动置 1 |
| `command.zero` | 1 = 扭矩清零（写地址 0x0000） |
| `command.factory_reset` | 1 = 恢复出厂设置（写地址 0x0002） |
| `sample_count` | 累计成功采样次数，数值在涨就说明读数正常 |
| `last_error_code`、`last_error` | 只读：最近一次操作的结果 |
| `measurement.torque_raw` / `speed_raw` / `power_raw` | 原始 32 位定点数，只读 |
| `measurement.torque_nm` / `speed_rpm` / `power_kw` | 按小数位换算后的工程量，只读 |

## 用扁平 API 喂 ava_tool 变量

也可以不用数据表：加载 `dyn200.dll` 和 `dyn200.h`，把 LOCAL 浮点变量的 DLL
读取函数绑定到 `dyn200_read_torque_nm`、`dyn200_read_speed_rpm` 或
`dyn200_read_power_kw`。或者先周期调用一次 `dyn200_update`，再把多个变量分别
绑到无串口读取开销的 `dyn200_get_torque_nm`、`dyn200_get_speed_rpm`、
`dyn200_get_power_kw`。

变量类型为 F32 时使用 `dyn200_read_torque_f32`、`dyn200_read_speed_f32`、
`dyn200_read_power_f32`；变量类型为 F64 时使用对应的 `*_nm`、`*_rpm`、
`*_kw` 接口。原始 I32 数据也提供 `dyn200_read_*_raw` 接口。

想在自己的代码里枚举串口，用 `dyn200_port_count()` 和 `dyn200_port_name()`
（和数据表用的是同一份列表）。

示例程序会静态链接 SDK，复制 `example_read.exe` 即可运行，不依赖
`dyn200.dll`。构建仍同时生成动态库和 `dyn200_static.lib`，方便不同项目使用。

测量值是 32 位有符号定点数。SDK 初始小数位为 `(1,0,0)`，可通过
`dyn200_set_decimals()` 或参数表 `scaling.*` 修改；
请以传感器菜单中的扭矩、转速和功率小数位为准。`Dyn200Measurement` 同时保留
原始值，便于处理固件差异。数据表里的 `scaling.*` 写 0 就等于用这个默认值，
因此默认 `torque_nm = torque_raw / 10`，例如 `-83 → -8.3 N·m`。
在 ava_tool 的 `dyn200_param → scaling → torque_decimals` 中设 `1` 为除以 10，
设 `2` 为除以 100，支持 `1..6` 位；编辑后立即应用于后续采样，参数随会话保存。
已有会话若保存了 `2`，需要手动改为 `1`。

修改设备地址、波特率、停止位或通信模式会令当前连接参数立即失效，因此 SDK
只通过通用 `dyn200_write_i32()` 暴露这些寄存器；写入后应关闭并用新参数重连。
同理，`transmit_zero` / `transmit_full` / `range` / `coefficient` 这些量程
标定寄存器也只走 `dyn200_read_i32()` / `dyn200_write_i32()`，不放进数据表。

## 构建与依赖

```sh
cd sdk/dyn200
make          # lib/win/dyn200.dll、lib/win/dyn200_static.lib、三个示例
make test     # 协议、数据表、主动流传输自检（都不需要接传感器）
```

Windows 走 MSVC，产物为 x64。串口枚举用 SetupAPI（按需 `LoadLibrary`，不会
变成链接依赖）加设备注册表键里的 `PortName`，因此读设备名用到 advapi32：
用 `cl` 驱动链接时 MSVC 会自动带上，直接调 `link.exe` 时要在命令行里加
`advapi32.lib`（本目录的 Makefile 已经加了）。Unix 端直接扫描 `/dev`，
没有额外依赖。

`test/test_tables.c` 里有一组 `_Static_assert`，把数据表的字段偏移钉死。
ava_tool 的头文件解析器按“自然对齐、枚举占 4 字节、没有 `#pragma pack`”
计算偏移，改动结构体字段时这些断言会先报错，提醒重新用面板解析头文件。

## 采集速率与延迟验证

`dyn200_rt.wait_fresh = 0` 适合实时显示：首次等待有效数据，之后每次取当前
队列中最新的完整帧，没有新帧时立即复用上次值。`fresh = 0` 的调用不能计入
新采样率；`sample_count` 只累计实际交付的新测量值。流停超过 1.5 秒会报超时，
随后收到有效数据会自动恢复。显示慢于上传时会跳过旧帧。

`wait_fresh = 1`、`dyn200_update()` 和 `dyn200_read_active6/8()` 按顺序逐帧
消费，队列为空时才等待。Windows 使用官方 COMMTIMEOUTS 首字节等待模式，
批量到达的字节一起读取，缓存仅在补充新字节时整理，避免每交付一帧就搬动
整个缓冲区。Unix 使用 `poll()` 和单调时钟控制毫秒超时。

有硬件时运行（按实际端口和传感器配置替换）：

```sh
build/win/example_bench.exe COM7 115200 6 1 10
```

输出的 `calls` 是调用频率，`new` 是实际交付的新测量值频率，`held` 是复用
旧值的次数。`rt_latest` 测最新值读取，`rt_fresh` 与 `update` 测顺序采集。
应测数秒以上并观察超时和 CRC 错误，不能把缓存突发或高频空读当成传感器速率。

SDK 不修改 USB 驱动注册表，也不使用未公开的 FTDI IOCTL。FTDI VCP 的硬件
延迟计时器应通过驱动端口属性调整；其影响与 `ReadFile` 的超时配置不同。
Windows 每次打开串口都会完整初始化二进制、无流控 DCB，避免继承其他程序的
零字节过滤、DSR 接收门控等设置。主动上传模式 6/8 还会启用 `EV_RXFLAG`，
将 `EvtChar` 设为 `0`：FTDI 收到零字节时可提前交付 USB 缓冲，避免连续流
被攒成接近 4 KB 的大块。事件字符仍保留在数据里，解析继续按帧长和 CRC 进行，
不会把零字节当作帧边界。每次自动重连都会重新设置，Modbus 模式关闭该事件。
这使用标准 Win32 API，参见 [FTDI AN232-06](https://www.ftdichip.cn/Support/Documents/AppNotes/AN232-06.pdf)
及 [FTDI 事件字符说明](https://ftdichip.com/Support/Knowledgebase/an232b_04eventchar.htm)。
如果数据长期不包含零字节，或转接器不支持事件字符提前交付，仍取决于驱动正常
缓冲策略；此优化不保证所有数据值和转接器都达到相同频率。

RTU 帧间隔按实际字符长度计算，高于 19200 bps 使用至少 1.75 ms 的间隔。
参考 [Microsoft COMMTIMEOUTS](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-commtimeouts)、
[FTDI 延迟计时器说明](https://www.ftdichip.com/Support/Knowledgebase/an232b_04adjlatency.htm)、
[Modbus 串行规范](https://www.modbus.org/docs/Modbus_over_serial_line_V1_02.pdf)。

无硬件回归 `test/test_stream.c` 覆盖两种帧长的 4000 帧批量顺序消费（每种只需
2 次模拟底层读取）、超过 16 KB 的噪声恢复、逐字节分包、最新帧选择、尾部半帧、
停止后恢复和错误时 `fresh` 清零。该测试证明软件队列行为，不是硬件吞吐测试。

Windows 的 `test_serial_win32` 检查污染过的旧 DCB 能否被完整归一化；流测试
还覆盖事件配置失败、重新配置和读超时更新不会关闭事件交付。

排查“先打开串口助手才变快”时，可关闭所有占用串口的软件，拔插转接器后运行：

```sh
python example/diagnose_serial.py COM11 --baud 115200 --frame-bytes 6 --seconds 5
python example/diagnose_serial.py COM11 --baud 115200 --frame-bytes 6 --seconds 5 --normalize --rx-flag
```

此诊断只读取主动流，不发送传感器命令；退出时恢复原 DCB、超时和事件掩码。
对比 `batches_per_s`、`batch_max`、`gap_max_ms`，不能只看有效帧总数，因为
每秒收到很多帧也可能是每隔数百毫秒才收到一大包。
