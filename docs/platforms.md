# 多平台目录与构建

四个平台在同一仓库维护，共用 `src/core`。平台实现统一放在 `src/platform`；没有独立的根目录 Android 工程副本。

```text
src/
  core/                     共享 ELF/DWARF、采样、算法与业务代码
  gui/                      Windows/Linux/macOS 的 ImGui 界面
  platform/
    native_dlg.hpp          共用文件接口
    system_tray.*           共用托盘接口及条件实现
    win/native_dlg.cpp
    linux/native_dlg.cpp
    mac/native_dlg.mm
    android/
      AndroidManifest.xml
      CMakeLists.txt
      java/com/ava/tool/    Android 生命周期、触屏界面、USB Host/SWD
      native/              JNI 桥接及轻量类型兼容头
      res/                 Android 资源
ckit/lib/
  ckit/                    ckit 自身的构建产物
  jlink/                   JLink_x64.dll/.lib、libjlinkarm.so.8/.dylib
  fftw/                    libfftw3f-3.dll/.lib
scripts/build.py            统一构建入口
tests/android/              安卓协议与 JNI 测试
build/<平台>/               中间产物
bin/<平台>/                 可运行产物
```

`jlink`、`fftw` 依赖文件按组件平铺，不再添加 OS 子目录。Linux/macOS 的 FFTW 继续使用原来的系统/Homebrew 依赖；没有人为生成不存在的库。桌面 J-Link 库保持原内容及文件名，未被转换成安卓库。

## 统一入口

在仓库根目录运行（Python 3.9+）：

```sh
python scripts/build.py win
python scripts/build.py linux
python scripts/build.py mac
```

桌面构建沿用根 Makefile，必须在对应系统运行；无参数时自动选择本机平台。原来的 `make` 入口仍可用。

Android（可在 Windows/Linux/macOS 主机交叉构建）：

```sh
python scripts/build.py android --sdk <Android-SDK> --ndk <Android-NDK> --jdk <JDK>
```

也可设置 `ANDROID_SDK_ROOT`、`ANDROID_NDK_HOME`、`JAVA_HOME`。需要 SDK Platform 35、Build Tools 35.0.0、JDK 17、NDK r27c、CMake、Ninja；`--build-tools`、`--cmake`、`--ninja` 可覆盖对应目录名或路径。

APK 输出：`bin/android/ava_tool.apk`。最低 Android 8 / API 26，目前包含 arm64-v8a，适用于 Pixel 4。构建会自动生成并复用 `.local-signing/android-debug.keystore`，验证 APK 签名和 ZIP 对齐；私钥不纳入版本控制。这是开发签名包，更新安装应保留同一签名密钥。

## 平台界面

桌面端共用 ImGui 业务界面，固定采用 macOS 风格的 Island 布局：圆角面板、留白分隔与统一控件。统一轮廓图标、动作颜色及文字对比度规则见 [UI 设计约定](ui_design.md)。

“设置 → 外观”统一提供明暗模式和海蓝、石墨、鸢紫、薄荷、玫瑰、琥珀六种配色，不再提供平台风格切换。配色仅保存到本机 `.ava_tool/color_scheme.txt`，不会写入 `.ava` 会话。切换时重新从未缩放的样式应用 DPI，避免重复放大。键盘行为仍跟随操作系统，macOS 的主快捷键为 Command，Windows/Linux 为 Ctrl。

“设置 → 外观”支持跟随系统、按时间切换、固定浅色、固定深色。默认日间为 07:00–19:00，桌面可自定义整点边界，Android 可自定义小时和分钟。偏好保存到本机，不进入会话。Windows 读取应用主题；macOS 查询系统外观；Linux 读取 KDE/GNOME 明暗偏好。桌面查询在后台执行，运行中可切换主题，Windows 标题栏同步明暗。

桌面工作区提供首页、常用操作栏、底部状态栏以及 `Ctrl/Cmd+K` 命令搜索。工具栏在窄窗口收起文字，保留图标及悬停说明；设备配置和采集选项分别放在“设备”“采集”菜单。修改会话后新建/打开会先处理未保存内容；保存失败不会清除未保存标记。普通复制、文本编辑空格等输入不会再触发全局设备操作。

- 监视器支持纵向排列和适应宽度的网格，使用常驻输入框按名称、路径或地址筛选通道；单 Scope 聚焦仍可从各 Scope 自身操作。每幅图保留可读高度，多图通过滚动浏览。次要操作进入“选项”，不再为每个 Scope 常驻整排按钮。
- 变量表和曲线表根据字体设置列宽、支持横向滚动及完整文字提示；同一结构体连成一个色块，普通成员和折叠子结构体保持同色，展开子结构体时才将其标题和成员切换到下一层颜色，块内无横线或明暗间隔。也可“进入”分组并通过路径返回。列分隔线仅在表头或调整宽度时显示。表格过窄时释放固定名称列，避免遮住数值。
- 变量管理器和监视器按结构体绘制圆角 Island 卡片，展开子结构体时出现嵌套卡片，普通成员之间无缝隙；平面数据表的表头与内容组成独立卡片。保留已有列宽、排序、滚动和拖拽设置。底栏不常驻“画面实时更新”，只在冻结时显示状态。
- Dock 关闭使用点击位置附近的小确认框；点击外部或 Esc 取消，不遮暗整个工作区。
- 设备管理器围绕 SDK/设备模板组织设备库，提供搜索、设备概览、属性、操作和 SDK 设置；已打开 SDK 调用器的元数据可以用于创建设备模板，打开或执行 SDK 操作仍由用户明确触发。

这是平台风格适配，桌面控件仍由 ImGui 绘制；系统窗口装饰和文件对话框使用现有平台实现。尚未新增 macOS 顶部系统菜单集成，也不会完整复制 Linux 用户安装的任意第三方主题。

Android 0.1.7 使用原生触屏界面，固定提供“设备、变量、示波器、设置”四个入口。手机使用底部导航，600dp 及以上使用侧栏，各页保留独立滚动位置。设备页分“本机设备 / 远程设备”，提供连接、共享和按需查看的入门帮助；变量页只管理监控变量。变量与示波器页底部固定显示开始/停止采样，采样期间切到其他页仍可直接停止，停止按钮使用暖色。

Windows 0.1.38 与 Android 0.1.7 增加双向跨设备连接，同一实例可同时共享本机 J-Link 和连接远端。支持配对、变量读写、单写者控制权、设备端普通采样与复位；采样最高 50 Hz。Android 共享由前台服务持有 USB，离开界面可继续服务远端。能力、使用方法和验证边界见 [跨设备连接](remote_devices.md)。

示波器顶部直接选择通道，曲线下方显示当前值、最小值、最大值，统计范围为当前显示点（HSS 为降采样显示点，不代表原始全量统计）。读取、CSV 导出和清空记录在右上角更多菜单。设置页再分“采样、外观、日志”三个固定页签，不再反复折叠高级采样。设置 → 日志显示安装包实际版本。

变量名称完整换行，详情和复制放入更多操作。按钮根据连接/授权/忙碌状态启用，危险动作确认。控件保持最小 48dp 触控高度，窄屏或大字体时操作行纵向排列。系统栏和键盘通过 inset 留出空间。外观切换在当前 Activity 内更新，不为换色断开 USB；时间、时区与系统主题变化会更新外观，已经打开的对话框下次打开时使用新主题。其他需要 Activity 重建的配置变化仍遵循原有生命周期，停止采样并释放 USB；切换应用内页面不会断开连接。

验证：`python tests/run_platform_style_test.py` 覆盖 Linux 环境识别、明暗色板、语义按钮文字对比度、时间边界、DPI、图标和 Ctrl/Command 输入。Windows 和 Android 已在本机编译；Android APK 签名、对齐、外观时间规则及协议/USB 模拟/HSS 离线回归通过。macOS/Linux 尚未在对应系统构建，Android 0.1.6 在独立 Android 11 / x86_64 模拟器验证了冷启动、四页导航、添加变量、通道选择、设置页签、实时明暗切换、320dp 窄屏和 1.4 倍字体。修复了 Android 11 冷启动访问尚未创建的系统栏控制器、主题 Context 丢失对话框窗口 token 两个崩溃问题。UI 回归脚本为 `tests/run_android_ui_test.py`（只允许 emulator 序列号，需预装兼容 ABI 的测试包）。本次没有进行手机 OTG / J-Link 实机回归。0.1.5 平台 UI 安装包保留在 `bin/android/ava_tool-0.1.5-platform-ui.apk`，新版为 `bin/android/ava_tool-0.1.6-android.apk`。

## Android 功能范围

Android 编译的是仓库现有的 `src/core/elf_parser.cpp`、`dwarf_parser.cpp`，通过 JNI 导出符号；没有复制一份解析器。触屏 UI 和直接 USB Host 的 J-Link 后端属于平台代码。桌面的完整 ImGui 界面及全部业务功能尚未移植。

实现了：

- 安卓系统 USB 授权、J-Link USB vendor/bulk 接口访问、断连释放；无需电脑服务或 root。
- 支持 SWD 的 SEGGER 探针，ARM ADIv5 AHB-AP，AP 索引可选，默认 0；连接读取固件信息、VTref、DPIDR/APIDR，不自动复位或停核。
- RAM/寄存器 8/16/32 位对齐访问，字节通道处理、WAIT 重试、FAULT/奇偶校验/超时检查；64 位变量分为两次 32 位访问，不保证原子性。
- ELF/AXF 标量符号导入、结构体成员展开、单维数组前 256 项、手动地址变量、最多 32 个监控变量、单通道曲线、写后回读和 CSV。
- 保存变量列表和连接设置；应用切到后台自动停止采样。采样按实际读取完成时间记录，每轮完成后再等待设定间隔。

尚未包含：Flash 擦除/烧录、通用 SEGGER HSS 支持、RTT/SWO、JTAG、ADIv6/multidrop、桌面固件身份/参数布局校验、会话 `.ava` 兼容、FFT/Bode、电机控制及 SDK 插件。无 DWARF 的标量默认按同宽无符号整数解释；使用时应核对目标固件、地址和类型，不能把导入符号视为已完成固件匹配。位字段不导入。

Android 0.1.5 HSS 实验实现：基于用户 2026-10-26 的 Windows USB 抓包（`hss-session-02.pcap`，探针 Pro V4），识别到 CPU 配置请求以及 `C5 00 0E` 的能力查询、启动、读取、停止流程。当前仅接受 Pro V4、固件 `Sep 22 2022 15:00:37`、AP0、DPIDR `6BA02477`、APIDR `84770001`、CPUID `411FC271` 的已验证组合；CPU 配置字节是该组合的实测参数，不是通用协议规范。仅采集当前曲线对应的一个对齐的 32 位 H74x RAM 变量，拒绝寄存器区、未知探针/固件和其他访问端口，不自动修改固件、保护配置或复位 MCU。缓存在 Cortex-M7 D-cache 内尚未写回 RAM 的值不能靠 HSS 看到，采集对象仍需确保对调试端可见。

HSS 目标频率可设置 1–50 kHz，按整数微秒向上取整采样周期；目标值不是实际频率保证。探针帧为 32 位微秒时间戳及 32 位变量原始值。大响应的长度头和数据体共享 USB 包或跨包的情况由传输层保留，已用 1/3/512/4096/8196 字节分段回归验证。数据保留最近 20 万点，明确显示覆盖数量；时间戳统计实际频率、长间隔（大于请求周期的 1.5 倍），长间隔不等同于确认丢帧。界面约 10 Hz 刷新，曲线最多显示 1200 个降采样点；单独的 HSS CSV 导出保留缓冲区内全部原始点，时间列为相对首个采样点的探针微秒时间。停止、切后台、断开或出错会结束 HSS 并关闭 USB，需要重新连接。普通轮询入口和普通 CSV 独立保留。

验证状态（2026-10-26）：生产 Android SWD/HSS 代码经 Windows SDK 的 `WriteEmu/ReadEmu` 字节传输桥访问用户的 Pro V4 + STM32H745II，没有调用 SDK Connect/HSS/内存/复位 API。SWD 12 MHz、HSS 请求 25 kHz 时，15 秒实测 357573 点 / 23834.18 Hz；关闭后重新打开，再采集 12 秒得到 285950 点 / 23822.91 Hz。采样变量会变化，最近 20 万点环形缓存及覆盖也经过实测。日志位于 `build/android-tests/hss-hardware-locked.log` 和 `hss-hardware-reconnect.log`。默认 HSS 目标设为 25000 Hz，使用时将 SWD 手动设为 12000 kHz；显示实测值，不保证均匀 40 us 间隔（记录到大于 60 us 的长间隔）。

早期测试曾出现重新连接 DPIDR 同步失败，以及持续采集长度错位。Windows SDK 传输桥现在用 `JLINKARM_Lock/Unlock` 将命令、长度头和完整响应包在同一临界区，避免 SDK 后台通信插入拆开的 WriteEmu/ReadEmu 调用；随后上述持续测试及重新连接通过。Android 直接 USB Host 不含这个 SDK 后台线程，已有单一 I/O 工作线程串行访问。没有证据表明这解释或修复了此前的手机整机重启。0.1.5 仍是特定探针/固件的实验版，手机界面、OTG 吞吐和 ROM 稳定性尚未实测。`bin/android/ava_tool.apk` 现为 0.1.7-android，包含上述实验 HSS、触屏分页及跨设备连接；此前的 HSS 实验产物单独保留为 `bin/android/ava_tool-0.1.5-hss-experimental.apk`。

离线回归：`python tests/run_android_test.py --jdk <JDK>` 包含 HSS 请求/应答边界、非法长度、负状态、停止幂等、时间戳回绕/倒退及 25 万点持续输入后的环形缓存/CSV快照测试。`tests/android/HssProbeTest.java` 和 `probe_usb_bridge.py` 是手动硬件实验工具，不由测试套件自动执行，也不打包进 APK。实验需要独占探针，使用显式 DLL 路径和序列号；其 Windows SDK 关闭行为不代表 Android USB Host，不能替代手机验证。

## 验证与真机验收

Android 0.1.4：用户已反馈 0.1.3 可以连接并读取 STM32H745，但点击连接后曾出现手机完整重启，早期版本也有此现象。尚未确定是 OTG 供电、移植 ROM 的 USB/内核故障还是其他原因，不能将本版视为已解决整机重启。应暂停带 J-Link 的复现测试，先在拔下探针的状态打开“复制设备诊断（无需插 J-Link）”。它只读取本机系统/版本信息、允许当前应用读取的重启原因属性和本地连接记录，不访问 USB、不调用 root、不上传。系统拒绝读取时会明确显示，不将空值判定为没有故障。0.1.3 及以前没有持久记录，无法追溯恢复其连接日志。

0.1.4 的连接阶段及异常记录在 I/O 工作线程保存到有大小限制的本地文件，重启后可读取；持续采样不逐次写盘。USB 接口使用非强制占用，接口已被系统占用时停止；占用失败/异常及释放异常都会关闭连接。通信错误及 Activity 进入后台后释放 USB，回到应用后需要手动连接（包括打开系统文件选择器的情况）。已移除插入 USB 时自动唤起本应用的声明。新增的模拟 USB 测试覆盖缓冲、非强制占用与异常释放，日志测试覆盖重新打开、大小上限及存储错误；这些测试不等于已验证移植 ROM 稳定性。

Android 0.1.3 修正 J-Link 输入采样位置及分段读取首位数据的处理：先发送完整的只读 DPIDR 事务，对 ACK bit 8/9 两种布局检查 ARM 标识与奇偶校验，要求两次一致才允许后续 DP/AP 配置；无法确认时至多尝试三次并停止。在 bit 8 布局下，将请求阶段最后一个采样位保留为读数据 bit 0，与下一次 USB 返回的数据拼接。不会在普通读写过程中更换布局或忽略 FAULT。测试纳入了 [J-Link 实测记录 §12.1](https://jon.dev/traces/j-link-over-usb#121-common-scan-body) 的原始请求/数据响应，可复现旧实现把正常 ACK 误读为 FAULT。该实测记录来自 EDU Mini V2，当前用户探针仍需真机确认。

Android 0.1.2 增加连接阶段日志与“复制日志”：连接失败时也会保留探针固件、VTref、已读取的 DPIDR、DP 电源状态，并报告失败的 DP/AP 寄存器和 SELECT。AP IDR 识别发生 FAULT 时记录状态、清除 DP 粘滞错误并仅重试一次；不会自动重试内存写入。FAULT、奇偶校验失败和 WAIT 超时会使连接失效，后续内存访问需要重新连接。最低系统版本保持 Android 8 / API 26，签名密钥不变。

STM32H745 的 Cortex-M7 使用 AP0，Cortex-M4 使用 AP3。连接阶段 FAULT 不等同于变量地址错误或已确认读保护；需要结合阶段日志检查。可先保留正确的 AP，将 SWD 降为 100 kHz 做一次对照。当前实现不会自动复位目标、解除保护或修改 STM32 选项字节。参考 [ST STM32H7 Debug 资料](https://www.st.com/content/ccc/resource/training/technical/product_training/group0/8a/bf/62/da/24/f0/49/7b/STM32H7-System-Debug_DBG/files/STM32H7-System-Debug_DBG.pdf/_jcr_content/translations/en.STM32H7-System-Debug_DBG.pdf)。

```sh
python tests/run_android_test.py --jdk <JDK>
python tests/run_android_test.py --jdk <JDK> --native
```

第一条用独立的模拟 SWD 目标验证协议帧、转向、ACK/WAIT/FAULT、AP posted read、内存字节通道、读写和数值范围。第二条额外需要主机 C++ 编译器、CMake 和 `arm-none-eabi-gcc`，编译带 DWARF 的 ARM ELF，实际调用 Java → JNI → 共享解析器，检查结构体、数组、类型及无效文件拒绝。模拟协议通过不代表真实 J-Link 兼容性已经确认。

2026-10-26：Windows 与 Android 构建、协议模拟测试、Java/JNI 共享解析器测试已通过；安卓 APK 已通过签名和对齐验证。JNI 主机测试使用完整 JDK 21，APK 使用 JDK 17 构建。当前电脑禁止访问移动设备，因此尚未在手机上安装、验证界面或完成 J-Link OTG 实测。Linux/macOS 源码路径已同步，未在对应系统构建。

请在允许使用移动设备的环境完成验收：

1. 安装 APK，将 J-Link 用 OTG 连接手机，目标板连接 SWDIO/SWCLK/GND/VTref 并供电。
2. 打开 AvA Tool，连接 J-Link并接受系统 USB 授权，核对 VTref、DPIDR 和 APIDR。
3. 先对已知只读地址读取并对照桌面版；Cortex-M 可手动添加 `0xE000ED00`、类型 `u32` 核对 CPUID。
4. 使用匹配固件的 ELF/AXF 选择一个已知 RAM 测试变量，检查数值、采样、导出和断开/重连。
5. 在允许写入的 RAM 测试变量上验证写入与回读，再按需验证 MCU 复位。记录探针型号、版本、芯片型号及 AP。

协议命令参考：[jaylink](https://github.com/jonas-schievink/jaylink)。APK 内含 cJSON 和静态 C++ 运行库的相关许可声明。
