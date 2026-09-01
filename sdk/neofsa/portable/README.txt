FSANeo 力反馈交互便携包 — Windows 10/11 x64

解压整个 ZIP 后，双击 run.cmd。不要直接在压缩包内运行，也不要只复制单个脚本。
包内自带 Python、neofsa、ava_eth 和 C/C++ 运行时，不依赖目标电脑的 Python、PATH 或源码仓库。

使用方法
1. 双击 run.cmd，选择手动拖动、虚拟弹簧或柔性墙。
2. 先选本地模拟器验证运行环境。模拟器不含真实 PD 力学，不代表实际手感。
3. 实机电脑单独安装 Npcap：https://npcap.com/#download
   使用有权限访问的专用有线网卡，不要同时运行其他 EtherCAT 主站。
4. 选 Npcap 实机及网卡（显示 Windows 名称、硬件描述、连接状态），核对参数后输入 start，程序会发送 PD 增益并使能。
5. 手推机构体验力反馈；Ctrl+C 停止，或等待 duration 到期后自动失能。

修改 settings.json 可调整菜单启动时的增益、持续时间和阈值；参数在启动时读取。
手动拖动 Kp 固定为零。增益通过 PDO 发给固件，不保存 Flash，退出不恢复原增益。
目标转矩估计限幅 max_torque 不是固件实际输出限幅，固件限值须另行配置。
max_load_torque 默认 5 Nm，可在 settings.json 修改；这是示例值，应按机构及传感器量程设置。
max_load_torque 对负载传感器测量转矩超限进行失能检查，不使用电流估算的电磁转矩。
默认阈值仅是示例，实际使用须按机构调整。柔性墙可被推过，并非机械硬限位。

菜单运行的 CSV 在 logs 目录。包含负载传感器转矩、PD 估计值、位置/速度参考和反馈模式。
详细接口、控制原理和迁移说明见 SDK_README.md，C 头文件为 neofsa_c.h。

命令行（在包目录打开终端）
  .\run.cmd self-test
  .\run.cmd adapters
  .\run.cmd virtual_spring --backend sim --duration 5
  .\run.cmd hand_guiding --backend npcap --adapter "\Device\NPF_{实际GUID}" --kd 0.05 --max-torque 0.2
  .\run.cmd virtual_wall --help

命令行参数直接传给 demo，不自动读取 settings.json；设置 CSV 时相对路径基于包目录。
手动直接运行脚本时可用：runtime\python\python.exe -B example\virtual_spring.py --backend sim

故障排查
- 无法加载 DLL：完整解压，检查 lib/win 与 runtime/python；优先运行 self-test。
- 无可用网卡：检查 Npcap 是否已安装、网卡权限和连线。必要时使用管理员终端运行 run.cmd。
- PD 参数发送失败：需匹配固件和当前 ava_eth；当前仅支持 Npcap、单执行器 axis=0。
- “初始负载传感器转矩超过限值”：使能前的传感器实测值超过 max_load_torque，程序未使能。
  新版会显示带正负号的实测 Nm 和阈值。检查机构外载、重力及传感器零偏/标定；不要直接按估计转矩去设置该阈值。
  max_torque 是 PD 估计限幅，max_load_torque 是实际负载测量阈值，二者独立。程序不自动提高阈值或清零传感器。
- 反馈或过载异常：程序会尝试失能并关闭会话，先检查机构、传感器、限值与网线再重新启动。
- 不支持 Windows 7 或原生 ARM64。此包是 x64，不包含 Npcap 驱动或驱动安装器。

自检只使用本地模拟器，不打开硬件。便携包的运行验证不能替代实际机构测试。
