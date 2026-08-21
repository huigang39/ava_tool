# DSOX2024A SDK

这是一个 Keysight DSO-X 2024A C SDK。裸机通过后面板 USB Device 方口使用
USBTMC/VISA；安装 DSOXLAN 模块后也可以通过 LAN TCP 5025 使用。

```sh
make
make example
# Windows
build/win/example_capture.exe
# Linux / macOS
./build/linux/example_capture
```

USB 模式运行前需安装 Keysight IO Libraries Suite。SDK 会自动搜索 USBTMC 设备，
并通过 `*IDN?` 选择 DSOX2024A。也可以将 Connection Expert 显示的 VISA
Resource String 作为命令行参数传入。编译 SDK 不需要 VISA 头文件或导入库。

SDK 提供四通道配置、时基、边沿触发、运行/停止/单次采集、频率/Vpp/RMS 测量和校准后的 BYTE 波形读取。其他 SCPI 功能可用 `dsox2024a_write` / `dsox2024a_query` 调用。

`dsox2024a_read_waveform` 采用两阶段调用：先以空缓冲区读取点数，再分配 `double` 数组并获取数据；`example_capture.c` 给出了完整用法。

目录结构：`inc/` 为公共头文件，`src/` 为纯 C 实现，`example/` 为示例，
`build/<platform>/` 为中间文件和示例程序，`lib/<platform>/` 为动态库。
