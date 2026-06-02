# AvA Tool

AvA Tool 是一个面向嵌入式调试和电机控制实验的实时变量监控工具。它可以从 ELF/AXF 符号、手动地址、J-Link、UDP、SHM 或 CSV 数据源读取变量，并在 Monitor/Scope 中进行表格、时域曲线、FFT 频域分析、波形注入和 Bode 扫频观察。

## 主要功能

- 变量管理：拖入 ELF/AXF/BIN/CSV 文件，浏览符号并添加变量。
- 实时监控：按 Monitor 和 Scope 组织通道，支持表格视图和曲线视图。
- J-Link 采样：支持普通轮询和 HSS 高速采样模式，可设置目标采样率。
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
- J-Link 区域：选择设备、设置速度、连接/断开、复位 MCU。
- 顶栏 `PAUSE`：全局暂停显示刷新；采集线程仍会继续工作。

### Variables

- `Load File...`：加载 ELF/AXF/BIN。
- `Add Variable`：手动添加变量，可选择 JLINK、UDP、SHM 端口。
- `Refresh(ms)`：变量窗口后台轮询刷新周期。
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
