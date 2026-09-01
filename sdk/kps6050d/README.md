# KPS6050D SDK

纯 C 的 KPS6050D Modbus RTU SDK，支持 Windows、Linux 和 macOS 串口。

在 ava_tool 的「SDK 调用器 → C/C++」中点击 **KPS6050D**，会自动加载随程序附带的动态库和头文件。
数据表与 DYN200 使用同一套界面：

- `kps6050d_port_param`：点击搜索，使用下拉框选择串口，设置波特率和设备 ID，再点击连接；支持多个 USB 串口及设备描述。默认 2400 baud、ID 0、8N1，须与电源设置一致。搜索仅枚举串口，连接仅读取状态，不改变输出开关或设定值。
- `kps6050d_param`：读取当前设定；编辑 `voltage.val`（V）、`current.val`（A）、`output.enabled`（输出）、`ocp.enabled`（过流保护）、`remote_lock.enabled`（远程锁定）。开关值为 0/1，电压范围 0–60 V，电流范围 0–50 A。
- `kps6050d_rt`：自动读取电压、电流、计算功率（W）、设定值、输出/保护/恒流/报警状态和采样计数，可用于监视曲线。失败时 `fresh=0`，电压、电流和功率为 NaN，错误码与错误文本给出原因。

参数表使用 `write_<组名>` 门控。ava_tool 编辑分组字段时自动置位对应门控；直接调用 C API 时需手动置位。
每次写入先验证全部修改，再读取设备当前设置，仅替换门控指定的项，最后在一帧中写入；其他项保留设备当前值。
门控在调用结束时清零；没有门控时不会通信。`device` 显示最近读取或成功写入的设备状态。
原有 `kps6050d_open/read/set_*` 接口保持兼容。

在仓库根目录运行 `python tests/run_kps6050d_table_test.py` 可验证模拟串口协议与参数门控，不访问硬件。
串口枚举复用仓库内 `sdk/dyn200/src/dyn200_ports.c`，单独移植源码时需一并带上该文件和头文件；Windows 链接 `advapi32.lib`。

```sh
make
make example
```

Windows 使用 MSVC，产物位于 `lib/win/` 和 `build/win/`；Linux 使用 GCC，
macOS 使用 Clang，产物分别位于对应的平台子目录。

示例参数：

```sh
# Windows
build/win/example_48v_1a_loop.exe COM7 2400 0
# Linux
./build/linux/example_48v_1a_loop /dev/ttyUSB0 2400 0
# macOS
./build/mac/example_48v_1a_loop /dev/cu.usbserial-XXXX 2400 0
```

目录结构：`inc/` 为公共头文件，`src/` 为纯 C 实现，`example/` 为示例，
`build/<platform>/` 为中间文件和示例程序，`lib/<platform>/` 为动态库。
