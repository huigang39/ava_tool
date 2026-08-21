# KPS6050D SDK

纯 C 的 KPS6050D Modbus RTU SDK，支持 Windows、Linux 和 macOS 串口。

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
