# DYN-200 C SDK

纯 C 的 DYN-200 动态扭矩传感器 SDK，支持 Windows、Linux 和 macOS 的
RS-485 串口，涵盖 Modbus RTU 测量、参数读写、清零、恢复出厂，以及 HEX
主动上传模式 0/3 的帧解析。

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

示例会持续输出数据，按 `Ctrl+C` 结束。用于 ava_tool 变量监视器时，加载
`dyn200.dll` 和 `dyn200.h`，将 LOCAL 浮点变量的 DLL 读取函数绑定到
`dyn200_read_torque_nm`、`dyn200_read_speed_rpm` 或 `dyn200_read_power_kw`。
也可以先周期调用一次 `dyn200_update`，再将多个变量分别绑定到无串口读取开销的
`dyn200_get_torque_nm`、`dyn200_get_speed_rpm`、`dyn200_get_power_kw`。

变量类型为 F32 时使用 `dyn200_read_torque_f32`、`dyn200_read_speed_f32`、
`dyn200_read_power_f32`；变量类型为 F64 时使用对应的 `*_nm`、`*_rpm`、
`*_kw` 接口。原始 I32 数据也提供 `dyn200_read_*_raw` 接口。

示例程序会静态链接 SDK，复制 `example_read.exe` 即可运行，不依赖
`dyn200.dll`。构建仍同时生成动态库和 `dyn200_static.lib`，方便不同项目使用。

测量值是 32 位有符号定点数。`dyn200_set_decimals()` 的默认值为 `(2,0,0)`；
请以传感器菜单中的扭矩、转速和功率小数位为准。`Dyn200Measurement` 同时保留
原始值，便于处理固件差异。

修改设备地址、波特率、停止位或通信模式会令当前连接参数立即失效，因此 SDK
只通过通用 `dyn200_write_i32()` 暴露这些操作；写入后应关闭并用新参数重连。
