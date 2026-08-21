# AvA Tool

AvA Tool 是一个面向嵌入式调试和电机控制实验的实时变量监控工具。它可以从 ELF/AXF 符号、手动地址、J-Link、UDP、SHM 或 CSV 数据源读取变量，并在 Monitor/Scope 中进行表格、时域曲线、FFT 频域分析、波形注入和 Bode 扫频观察。

## 主要功能

- 变量管理：拖入 ELF/AXF/BIN/CSV 文件，浏览符号并添加变量；LOCAL 标量可绑定动态库中的 C 读写函数。
- 设备管理：创建设备类型及其实例，把动态库中的 C 函数绑定为实例方法或发现方法，并将输入、输出或返回值映射到类型化属性。
- 实时监控：按 Monitor 和 Scope 组织通道，支持表格视图和曲线视图。
- J-Link 采样与 RTT：支持普通轮询、HSS 高速采样，以及复用当前调试连接的 SEGGER RTT 双向终端。
- FFT 分析：Scope 可切换到频域视图，FFT 在后台线程计算，避免阻塞界面 FPS。
- 波形发生器：对可写变量配置正弦、方波、三角波输出。
- Bode Plot：选择写入、输入、输出通道后执行扫频并绘制幅频/相频曲线。
- CSV 导入导出：可导入历史数据，也可导出当前 Monitor 的采样结果。
- 会话保存：保存 Monitor、Scope、变量、布局和最近打开记录。
- 自动更新：通过 GitHub Releases 检查并安装新版本。

## 快速开始

1. 启动 `ava_tool`。
2. 在顶部栏选择 J-Link 设备，设置 SWD 速度，然后点击 `CONNECT`。
3. 将 ELF/AXF 文件拖入 Variables 窗口，或点击 `Load File...` 加载。
4. 在符号列表中把变量拖到 Monitor 的 Scope 里。
5. 在 Scope 中查看实时曲线；点击 `Freq` 切换 FFT 频域视图，点击 `Time` 回到时域。
6. 点击 `Export CSV` 可导出当前 Monitor 数据。

## 界面说明

### 顶部栏

- `File`：新建、打开、保存会话，以及最近会话。
- `Tools`：计算器、Bode Plot、Assembly Viewer 等工具入口。
- `Help`：版本信息和更新检查。
- J-Link 区域：选择设备、设置速度、连接/断开、复位 MCU，并可打开 RTT 终端。
- 顶栏 `PAUSE`：全局暂停显示刷新；采集线程仍会继续工作。

### Variables

- `Load File...`：加载 ELF/AXF/BIN。
- `Add Variable`：手动添加变量，可选择 JLINK、LOCAL、SHM 或 AUDIO 数据源。旧会话中的 MANUAL 变量会自动迁移为只读 LOCAL 变量。
- 属性编辑器会保留当前标量、STRUCT 或 UNION 类型；DWARF 联合体会按 UNION 展示和展开，LOCAL 也可手动创建联合体。
- `Refresh(ms)`：变量窗口后台轮询刷新周期。
- `Function Browser`：与符号浏览器堆叠在变量表下方，可独立展开；支持同时加载多组动态库及声明导出函数的 C/C++ 头文件，每个函数和变量绑定都会记录所属动态库。也可以把 DLL/SO/dylib 和头文件直接拖入变量窗口；右键点击 LOCAL 标量后选择 `Bind Read/Write Functions...` 绑定访问器。C++ 类和成员方法可浏览，但变量访问器必须绑定顶层 C ABI 函数。
- 读取访问器支持 `T read(void)` 或 `status read(T *out)`；写入访问器支持 `status write(T value)` 或 `status write(T *value)`。`status` 可以是 `void` 或整数，负整数表示调用失败。函数在独立工作线程中按 `Refresh(ms)` 周期调用。
- 从符号表或变量表拖拽条目到 Monitor Scope，可创建监控通道。

### Monitor 和 Scope

- `Add Scope`：新增 Scope。
- `Clear Data`：清空采样数据并重启采样状态。
- `Export CSV`：导出所有 Scope/Channel 数据。
- `History(s)`：保留并显示的历史时长。
- `Max Pts`：单帧绘图点数预算。
- `MaxHz`：目标采样率。
- `HSS/POLL`：切换高速采样或普通轮询。
- `FULL/FOLLOW/MANUAL`：切换绘图视图模式。

Scope 工具栏：

- `Plot/Table`：切换曲线视图和表格视图。
- `Freq/Time`：切换 FFT 频域视图和时域视图。
- `Table`：显示或隐藏右侧 Stats/Peaks 面板。
- `Hide line/Show line`：隐藏或显示当前 Scope 内所有曲线。
- `Hide Scope/Show Scope`：折叠或展开当前 Scope。
- `Pause/Resume`：暂停或恢复当前 Scope 的采集任务。
- TABLE 视图中右键通道可通过 `Access mode / 读写模式` 切换只读或读写；该操作会应用到当前选择的通道并随会话保存。JLINK、SHM 和 LOCAL 支持读写，LOCAL 写入会调用变量绑定的写函数；AUDIO 保持只读。
- 变量管理器与 Monitor TABLE 的右键菜单共享删除、属性编辑、读写模式、DWT 跟踪和枚举编辑功能；LOCAL 通道也可从 Monitor 直接打开所属变量的函数绑定窗口。
- 各类变量的右键菜单保持固定布局；当前变量不支持的操作会置灰而不是隐藏。ENUM 的类型列会显示底层整数类型，例如 `ENUM (U8)`、`ENUM (I8)` 或 `ENUM (U32)`。
- 置灰菜单会注明适用条件（如“仅 LOCAL 标量”）；顶层及嵌套变量都可在属性窗口覆盖标量类型，嵌套聚合类型可改成标量并通过“重置”恢复 DWARF 原始类型。
- 每个 Scope 的 TABLE 列宽、表头升降序或手动拖拽顺序都会随会话保存并在下次加载时恢复。

### SEGGER RTT 终端

1. 先在顶部栏连接 J-Link 和目标 MCU，再点击 `RTT`。RTT 终端是可拖动、可缩放的普通工具窗口，不会像弹出菜单一样始终压在其他窗口前面；点击标题栏关闭按钮只隐藏窗口，不会停止已经运行的 RTT。
2. 默认使用 Up Channel 0 接收目标输出、Down Channel 0 向目标发送数据；这两个编号是 RTT 缓冲区通道，不是 RTT Viewer 的虚拟 Terminal 编号。
3. `Control Block` 留空时由 J-Link 自动搜索 `_SEGGER_RTT` 控制块；自动搜索失败时，可填写控制块的固定地址，例如 `0x20000000`，然后重新启动 RTT。
4. 点击 `START` 后，状态会先显示正在搜索控制块，找到后切换为运行状态。发送区支持把换行符附加到输入末尾；下行 FIFO 暂时写满时，未发送完的数据会保留并继续重试。
5. 目标固件需要集成并初始化 SEGGER RTT。若持续找不到控制块，请确认选择了准确的目标器件，并检查控制块是否位于 J-Link 可访问的 RAM 中。

RTT 与内存采样、HSS 共享同一条 J-Link 连接；高采样率下若终端输出延迟，可适当降低 `MaxHz`。关闭或断开 J-Link 时，RTT 读取线程会先停止，不会另行占用 COM 口。

## FFT 使用说明

在 Scope 中点击 `Freq` 进入频域视图：

- FFT 点数可选择 256 到 1048576。
- Peaks 输入框用于设置峰值列表数量。
- `Line/Bar` 可切换频谱绘制方式。
- 频谱计算由独立 FFT worker 线程执行，渲染线程只绘制最近一次计算结果。
- 大点数 FFT 会自动降低计算刷新率，并把用于绘图的频谱压缩到有限点数；峰值检测仍基于完整 FFT 结果。

## Bode Plot

1. 打开 `Tools -> Bode Plot`。
2. 选择 `Write` 通道作为扫频写入对象。
3. 选择 `Input` 和 `Output` 通道。
4. 设置起始频率、终止频率、步进、驻留时间和幅值。
5. 点击 `Start` 开始扫频，完成后查看幅频和相频曲线。

## 设备管理器

1. 打开 `Tools -> Device Manager`，先创建设备类型，再在该类型下手动添加一个或多个具体设备。
2. 为设备类型选择动态库（Windows DLL、Linux SO 或 macOS dylib）以及声明导出函数的 C/C++ 头文件，然后点击 `Load`。动态库路径、头文件、属性定义和方法绑定都属于设备类型；每个具体设备保存各自的属性实际值。恢复会话或导入类型时不会自动读取头文件或加载动态库，需要手动执行 `Reparse` 和 `Load`。
3. 在设备类型的 `Properties` 中定义 `bool`、定宽整数、`float` 或 `double` 属性。`Discovery Key` 是发现 JSON 使用的稳定字段名，不会随显示名称改变，并且在同一类型中必须唯一。实例方法可以把普通参数绑定到常量或属性值，把指针参数绑定到兼容属性地址，并把输出参数或返回值更新到当前设备实例。
4. 需要批量发现设备时，把类型方法设为发现方法，并配置调用方提供的 JSON 缓冲区 ABI：指定一个可写 `char *` 缓冲区参数、一个 32/64 位整数容量参数和缓冲区大小，例如 `int32_t discover(char *out_json, uint32_t capacity)`。发现函数把 NUL 结尾的 UTF-8 JSON 写入缓冲区；负返回值表示失败。JSON 支持直接返回设备数组，或返回如下对象：

   ```json
   {
     "devices": [
       {
         "key": "stable-device-key",
         "name": "Device 1",
         "properties": { "serial": "12345678", "channel": "1" }
       }
     ]
   }
   ```

   函数返回值必须等于 JSON 中的设备数量；管理器按 `key` 批量新增或更新设备实例，避免重复创建。`properties` 的字段名对应属性的 `Discovery Key`；I64/U64 值必须写成 JSON 字符串，避免 64 位精度损失。同样可以使用 `[{"key":"...","name":"...","properties":{...}}]` 作为顶层数组。现有厂商接口如果返回“数量 + 结构体数组”，需要增加一个薄 C wrapper，把结果转换到上述 JSON 缓冲区 ABI。
5. 保存 `.ava` 会话会保留所有设备类型、实例及其属性值；`.avadev` 导入和导出同样以一个设备类型及其全部实例为单位。

设备方法按 C ABI 调用。建议动态库导出 `extern "C"` 函数，并使用定宽标量或标量指针；当前不支持 C++ 成员函数、可变参数和按值传递的结构体。传给函数的属性地址、字符串和发现缓冲区仅在本次同步调用期间有效，动态库不能保存这些指针或在返回后异步访问。

## 构建

Windows 环境下可直接使用 Makefile：

```powershell
make
```

构建产物：

```text
bin/win/ava_tool.exe
```

如果需要打包安装包，使用 `installer/ava_tool.iss` 配合 Inno Setup。

## 发布

创建 GitHub Release 示例：

```powershell
gh release create v0.1.0 `
  bin/win/ava_tool.exe `
  --title "ava_tool v0.1.0" `
  --generate-notes
```

如需上传安装包，可把安装包路径追加到命令中。

## 常见问题

### FFT 开启后界面仍然卡顿

优先减少可见通道数量，或缩小可见时间窗口。大点数 FFT 已做后台计算、自动节流和绘图降采样，但 1048576 点 FFT 仍会消耗明显 CPU。

### J-Link 连接失败

检查目标板供电、SWD 线序、设备型号选择和 J-Link 驱动。必要时断开重连 J-Link，或降低 SWD 速度。

### HSS 模式启动失败

降低 `MaxHz` 或减少通道数量。HSS 可达到更高频率，但受 J-Link 型号、目标内存块数量和 USB 带宽限制。

### 修改变量值没有生效

确认变量来自可写端口，地址正确，并且输入后按 Enter 提交。
