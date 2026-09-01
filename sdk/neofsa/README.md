# neofsa：通过 ava_eth 控制 FSANeo

Windows x64 C ABI 插件，供 ava_tool SDK 面板和 Python 脚本使用。
主站改为 `ava_eth_sdk.dll`：周期 EtherCAT 收发、CiA402 状态机和命令看门狗均由 ava_eth 管理。
不再启动 `fsa_soem_master.exe`，也不访问 `Local\SOEM_PDO_FREERUN`。

```text
ava_tool SDK 面板 / Python demo
          ↓
     neofsa_c.dll
          ↓
    ava_eth_sdk.dll
          ↓
Npcap EtherCAT / 内核设备 / 本地模拟器
```

## 构建与运行库

在仓库根目录执行：

```powershell
make -C sdk/neofsa
```

生成 `sdk/neofsa/lib/win/neofsa_c.dll`。插件通过公开 C ABI 动态加载 ava_eth，
无需 SOEM 头文件、共享内存定义、half 转换或额外的链接库。

默认优先查找插件旁的 `ava_eth_sdk.dll`，其次向上查找仓库的 `sdk/ava_eth/bin/win`。
也可在 `neofsa_param.runtime_dll` 中指定 UTF-8 DLL 路径。
独立分发时将以下三个文件放在一起：

- `neofsa_c.dll`
- `ava_eth_sdk.dll`
- `libwinpthread-1.dll`

SDK 头文件和运行库应保持同步，可用仓库的 `scripts/sync_ava_eth_sdk.ps1` 更新。
Npcap 实机连接需要安装 Npcap，并具备访问所选网卡的权限；本地模拟器无需驱动。
当前 Npcap 后端支持单执行器网段。不要让电机控制页面、其他 demo 或其他主站同时占用同一网卡。

## 复制到其他电脑：便携包

在仓库根目录执行：

```powershell
python scripts/package_neofsa.py
```

生成 `dist/neofsa-force-demo-win-x64.zip` 和 `.zip.sha256`。
整个 ZIP 复制到 Windows 10/11 x64 电脑，完整解压后双击 `run.cmd`。
包内自带官方 Python 嵌入式运行时、全部 demo 脚本、neofsa/ava_eth DLL、
winpthread 和应用本地 C/C++ 运行库，无需目标电脑安装 Python 或 Visual Studio。
目标电脑的 Npcap 驱动须单独安装，便携包不包含驱动安装器。

菜单支持三个 PD 交互示例、基础转矩测试、模拟器/实机网卡选择与环境自检。
网卡按 GUID 精确匹配 Windows 名称，显示友好名称、硬件描述和连接状态；连接仍使用原始 Npcap 路径。
未匹配到名称时保留原始路径供选择，菜单和命令行 `adapters` / `--list-adapters` 均支持名称显示。
修改 `settings.json` 调整菜单启动参数，反馈自动写入 `logs`；退出运行按 Ctrl+C。
也可运行 `run.cmd self-test`，或 `run.cmd virtual_spring --backend sim --duration 5`。
命令行直传参数不会读取菜单配置文件。操作细节见包内 `README.txt`。

打包脚本首次从 python.org 下载固定版本的 x64 嵌入式运行时，核对官方 SHA256 后缓存到 `build`。
后续使用缓存可离线重新打包。每次发布会验证实际 ZIP 解压后的文件哈希，
并在中文/空格路径、干净 PATH 和隔离 Python 环境中运行三个 PD demo 及菜单日志测试。
这些检查不打开硬件，也不代表已在另一台物理电脑或真实机构上验证。

## SDK 面板

1. 加载本目录的 `neofsa_c.h` 和构建后的 `neofsa_c.dll`。
2. 用 `neofsa_adapter_param` 枚举网卡：`backend=4`，读取 `count`，修改从零开始的 `index` 后读取 `name`。
3. 在 `neofsa_param` 中设置 `backend=4`、`adapter=网卡路径`，或设置 `backend=1` 使用本地模拟器。
   `axes=0` 默认 1 轴，`cycle_us=0` 默认 1000 us，`watchdog_ms=0` 默认 100 ms。
4. 设置 `master_action=START` 并写入，连接不会使能。`master_running` 表示持有连接句柄，
   在线和使能状态应检查 `neofsa_rt.feedback.flags`。
5. 在 `neofsa_rt` 设置 `axis=0`、`target.mode=CST`（10），先写入零转矩目标，再执行 `action=ENABLE`。
   编辑 `target.*` 会置位 `write_target`，目标发送成功后清零。先失能再修改控制模式；
   插件使用普通 `set_targets`，不隐式进行在线切换。
6. 运行时连续提交目标，或周期执行 `action=KEEPALIVE`。读取反馈不续约；无修改的自动写入也不续约。
   100 ms 看门狗需要显著更短的调用周期；手动操作可先配置适当的看门狗时间。
7. `action=DISABLE` 优先执行，并丢弃同一请求中未发送的目标。
   `master_action=STOP` 关闭整个会话。

动作字段调用后清零，失败也不自动重放。目标写入失败时保留 `write_target`，可修正后重新发送。
若先发目标成功、随后使能失败，目标已被接受；应检查返回错误和反馈，不能视为整组原子操作。

单位统一为位置 rad、速度 rad/s、电流 A、转矩 Nm。
`feedback.torque` 为负载转矩；电磁转矩为 `electromagnetic_torque`，仅在 `flags & 128` 时有效。
`flags & 2` 表示反馈有效，`flags & 4` 表示已使能，`flags & 8` 表示故障，`flags & 16` 表示命令过期。
`neofsa_last_error()` 提供当前调用线程的中文错误说明及 ava_eth 原始详情。

## 力控 demo

需要 64 位 Python 3，无第三方 Python 依赖。所有 demo 默认连接本地模拟器，
人机交互 demo 使用固件 **PD（-128）**，主机更新默认 10 ms，EtherCAT 周期默认 1 ms。
恒值/正弦两个基础转矩测试仍使用 CST（10），用于独立测试转矩接口。

优先体验以下三个**人与机构通过力反馈交互**的示例。人用手推动实际机构，主机更新位置/速度参考，固件执行位置速度 PD 并输出转矩。

| 脚本 | 人的动作 | 机构的响应 |
| --- | --- | --- |
| `example/hand_guiding.py` | 慢推、快推、停下、反向推 | `tau=-Kd*v`；速度越快阻尼越大，停下后不主动回起点 |
| `example/virtual_spring.py` | 推离启动位置，再松手 | `tau=-Kp*(q-q0)-Kd*v`；偏离越远回正力越大，松手后阻尼回弹 |
| `example/virtual_wall.py` | 在自由区内拖动，再推向两侧边界 | 自由区目标转矩为零，越过边界后产生反力；继续推则反力增大，撤手不产生朝墙内的吸力 |

交互示例默认持续 30 秒，Ctrl+C 停止。启动位置 `q0` 取有效反馈；不是坐标零点。
墙位于 `q0 ± wall-distance`，默认距离 0.2 rad；接触阻尼在 `wall-softness`（默认 0.02 rad）范围内渐入，
采样时刻的估计反力连续。墙为软件柔性反力，仍可被人推过，不是机械硬限位。

先以较小转矩体验手动拖动，再体验弹簧和墙。以下命令连接实机；将网卡路径替换为枚举结果：

```powershell
python sdk/neofsa/example/hand_guiding.py --backend npcap --adapter '\Device\NPF_{实际GUID}' --kd 0.05 --max-torque 0.2
python sdk/neofsa/example/virtual_spring.py --backend npcap --adapter '\Device\NPF_{实际GUID}' --kp 0.5 --kd 0.05 --max-torque 0.2
python sdk/neofsa/example/virtual_wall.py --backend npcap --adapter '\Device\NPF_{实际GUID}' --wall-distance 0.2 --kp 2 --kd 0.05 --max-torque 0.2 --csv wall.csv
```

终端显示相对位移、速度、PD 估计转矩、负载传感器转矩及“自由区/正向墙接触/反向墙接触”等状态。
CSV 同时记录交互状态，可比较推入和撤回时的位移—力矩曲线。

手动拖动未做重力/摩擦补偿；零目标转矩不等于机械上完全无阻力。上述增益只是示例初值，
实际手感、可回驱性和稳定性取决于机构、固件转矩环及通信延迟；需要按机构调小限幅后验证。
本地模拟器不含手推机构的动力学，运行时会明确提示，不能据其输出判断实际交互手感。

原有 `constant_torque.py`（恒转矩）、`sine_torque.py`（正弦转矩）保留用于基础转矩测试；
`impedance_torque.py` 与 `virtual_spring.py` 使用同一固件 PD 弹簧阻尼控制。

### PD 模式的实现与增益发送

三个交互 demo 都设置 `target.mode=-128`。启动时先失能，通过
`neofsa_write_pd_param` → `ava_eth_set_pid(mask=3)` 发送固件 PD 的 Kp/Kd，
再通过 `neofsa_read_pd_param` 等待同一序列号的 SENT 状态；失败或超时不使能。
发送值是 float16 编码精度，SENT 代表传输完成，不是设备回读，也不保存 Flash。
程序使用返回的发送精度计算参考和显示估计值，退出不恢复原增益（当前不能回读原值）。

固件控制律为 `tau_pd = Kp*(p_ref-p) + Kd*(v_ref-v) + t_ff`，其后进入转矩环。
在启用负载转矩传感器的固件配置中，转矩环使用负载传感器反馈。
当前 ava_eth 的公开目标 ABI 没有 `t_ff`，因此这些 demo 不发送转矩前馈，
也不把普通 `target.torque`（PDO 的 `t`）冒充 PD 前馈。

- 手动拖动：固件 Kp=0、Kd=`--kd`，速度参考为零，形成阻尼，不锁定起始位置。
- 虚拟弹簧：固件 Kp/Kd 取命令行值，位置参考为启动时重新采样的位置、速度参考为零。
- 虚拟墙：固定固件 Kp/Kd，自由区参考跟随反馈，墙外位置参考限制在边界，
  通过速度参考实现接触阻尼渐入，撤回时消除朝墙内的估计吸力。

主机还根据启动/结束包络和估计转矩限幅缩小位置、速度参考误差；没有在主机另做 CST 控制环。
墙内参考跟随反馈存在主机采样及网络延迟，实际自由区可能有残余阻力。
`--max-torque` 只限制**当前采样时刻根据 PD 增益计算的估计值**，不是固件硬限幅：
采样间机构运动、目标速度量化及内部控制器限幅都会使实际输出与估计不同。
实际 PD 输出上限应在执行器配置中设定。

`--max-load-torque`（默认 5 Nm）单独检查**负载传感器**测量值，超过阈值立即请求失能。使能前超限则拒绝使能，报告带正负号的实测值、绝对值与阈值，
不自动提高阈值或传感器清零；应先检查真实负载、重力和传感器零偏/标定。
电磁转矩只是电流估计量，不用作人施力测量、交互判断或该停机条件。
CSV 的 `load_Nm` 保存负载传感器值，`pd_estimate_Nm` 保存 PD 估计值，另记录位置/速度参考和实际反馈模式。

本地模拟器不支持 PD 增益写入，demo 会明确说明并跳过增益下发，只验证 -128 模式与目标流程。
不会伪造参数发送成功，也不验证真实 PD 力学。当前增益下发要求 Npcap、单执行器 axis=0。

从仓库根目录运行：

```powershell
python sdk/neofsa/example/constant_torque.py --torque 0.1 --duration 5
python sdk/neofsa/example/sine_torque.py --torque 0.1 --frequency 0.5 --duration 10 --csv sine.csv
python sdk/neofsa/example/impedance_torque.py --kp 0.5 --kd 0.05 --duration 10
```

枚举并指定实机网卡：

```powershell
python sdk/neofsa/example/constant_torque.py --list-adapters
python sdk/neofsa/example/constant_torque.py --backend npcap --adapter '\Device\NPF_{实际GUID}' --torque 0.1
```

通用参数：`--max-torque`（默认 0.5 Nm，PD 为采样时刻的估计限幅）、
`--max-load-torque`（负载传感器停机阈值，默认 5 Nm）、`--max-velocity`（2 rad/s）、
`--max-travel`（距起始位置 0.5 rad）、`--ramp`、`--dt`、`--dll`、`--runtime`、`--csv`。
示例检查反馈有效、使能、模式、故障、主机循环延迟及速度/位移限值；正常结束、Ctrl+C 或异常时尝试失能并关闭连接。
这些主机限值不是驱动器硬件保护；实机运行前需根据机构调整，并配置执行器自身限值。

这里的“力控”以出轴转矩 Nm 为控制量。若机构需要直线力 N，需按机构的有效力臂或传动关系换算，
例如恒定垂直力臂下 `tau = F*r`；demo 没有接入外部力传感器，也没有额外的外部力 PI 环。
交互示例会在失能时发送固件 PD 增益，仅修改 Kp/Kd 两项。Python/Windows 不保证硬实时。
本地模拟器用于验证 API、调度和看门狗，不代表真实机构的力学响应，不能据此调参。

## 生命周期与迁移

这是 **C ABI v2**，结构布局与旧 SOEM 插件不兼容，DLL 和头文件必须一起更新。
旧会话应重新加载头文件并重建数据表，旧 ctypes/C 调用方必须更新结构定义。

| 旧接口 | 新接口 |
| --- | --- |
| `master_exe`、`start_timeout_ms` | `runtime_dll`，连接超时由 ava_eth 管理 |
| `slave_index`（从 1 开始） | `neofsa_rt.axis`（从 0 开始） |
| `command.control_word` | `action=ENABLE/DISABLE/RESET_FAULT/KEEPALIVE` |
| `command.mode_of_op`、`set_pvct.*` | `target.mode/position/velocity/current/torque` |
| `status/get_pvct/telemetry` | `feedback`，有效性由 flags 判断 |
| SOEM 心跳、PDO 大小 | ava_eth 在线状态、反馈时间戳和序列号 |

旧的原始 PDO 特殊指令及独立前馈字段未保留；当前公开目标接口仅包含位置、速度、电流和转矩。
插件增加 `neofsa_pd_param` 表格用于失能时发送 PD 增益、查询异步状态。
需要其他 PID 项或特殊指令时，可使用 ava_eth 原生 API 或 ava_tool 电机控制页面。

一个插件 DLL 在进程中维护一个共享会话，调用通过互斥锁串行化。
自定义宿主卸载 DLL 前必须调用 `neofsa_shutdown()`；重复调用安全。
ava_tool 加载器通过可选导出 `void ava_tool_sdk_shutdown(void)` 在卸载前自动清理，
不在 `DllMain` 的加载器锁下等待周期线程。

## 验证

```powershell
python tests/run_neofsa_sdk_test.py
```

测试真实插件配合 ava_eth 本地模拟器，覆盖头文件解析布局、目标写入、反馈、非法目标拒绝、失能优先、
看门狗过期、关闭重连及 SDK 面板加载器的卸载钩子；运行全部 demo，并验证中文路径下独立分发的运行库。
另用合成人手位移/速度轨迹验证拖动阻尼、弹簧回正、双向墙接触、撤回无吸力、边界连续和限幅。
测试还覆盖 PD 参数表布局、模拟器不支持增益写入的错误返回、-128 实际反馈模式、
运行中拒绝修改增益，以及主机参考到原生 PD 控制律的等价关系。
这些测试不访问 EtherCAT 硬件，不能替代实机验证。
