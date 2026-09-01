# ava_eth SDK 集成

`include/ava` 是 ava_eth C ABI 的公开头文件快照。Windows 构建将 SDK DLL 和 `libwinpthread-1.dll` 嵌入 EXE，电机控制默认使用内置运行库，无需选择或另外分发这两个 DLL。首次连接时自动释放到 `%LOCALAPPDATA%/ava_tool/runtime/ava_eth/<内容版本>/`，校验内容后加载。可在“高级运行库设置”中关闭“使用内置运行库”并指定自定义 DLL。

在 ava_eth 中构建 SDK 后，在 ava_tool 根目录执行：

```powershell
.\scripts\sync_ava_eth_sdk.ps1
make -j4
```

默认使用相邻的 `../ava_eth`；可用 `-SdkProject PATH` 指定位置。脚本同步头文件、DLL 与 `libwinpthread-1.dll`，不安装驱动。运行库放在 `sdk/ava_eth/bin/win`，构建时写入 EXE，修改 DLL 后也需要重新构建。

公开接口及 ABI 以 ava_eth 仓库的 `sdk/c/include/ava/sdk.h` 为准。更新 SDK 时请同步头文件并重编译上位机。

`ava_eth_set_pid(handle, axis, mask, &values, &sequence)` 在 Npcap 连接内异步更新 PID，使用 `ava_eth_get_pid_status` 查询 PENDING / SENT / FAILED。支持单执行器、axis=0，mask 的低 8 位指定参数，必须非零。值在排队前完整校验；更新期间继续处理 PDO 和调用方的 keepalive，保持使能、模式与运动指令。参数逐项生效；状态中的 values 是编码后的发送值，不是设备回读，不保存 Flash。异常停止时保留仍映射的参数槽，避免零输出误写增益。具体协议见 `docs/motor_control.md`。此方案不修改 CM4。

旧 `ava_eth_write_pid_pdo(options, axis, mask, values)` 保留为离线配置接口，必须先关闭周期连接；上位机 PID 页面已改用上述在线接口。

现有固件的 PID 标量 SDO 是空回调，PID 参数也没有 TxPDO 权限。旧 `ava_eth_configure_pid` 入口现明确返回 `AVA_ETH_UNSUPPORTED`，不再把假成功或零数据当作有效回读。上位机对旧 DLL 缺少 PDO 写入入口的情况明确提示更新运行库。

NEO 反馈的 `AvaEthFeedback.torque` 是 `0x2008` 的负载转矩（Nm）；`electromagnetic_torque` 是 `0x200A` 的电磁转矩（Nm），占用原 `reserved3` 槽并保留旧别名，ABI 1 大小及偏移不变。电磁转矩需同时检查普通反馈有效位与 `AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID`；旧内核驱动／旧运行库缺少此标志时不可读取保留槽。负载转矩一直存在，此前 ava_tool 将其误标为电磁转矩，现已纠正。

`ava_eth_switch_mode(handle, &target, &interpolation)` 在 Npcap 后端原子更新模式和初始目标，保持使能状态，并通过 `target` 返回接受的目标。位置模式从最新反馈位置开始，其他目标分量清零。该入口与普通 `set_targets` 分开；普通写目标仍禁止在线换模式。旧运行库和内核后端使用上位机的失能／设置／恢复原使能状态兼容流程。

`ava_eth_set_special_cmd(handle, actuator, value)` 写入 FocDriver 的 PDO `0x2080`，值域 0..7。先检查 `AVA_ETH_CAP_SPECIAL_CMD`；内置 Npcap 支持，模拟器返回不支持，内核模式需更新对应驱动。`AvaEthFeedback.special_cmd` 使用原保留字段的位置，结构大小不变，表示最后收到 PDO 回包确认的指令值，不代表固件动作完成。使能和校准仍受看门狗保护，失能会清除特殊指令。

本地模拟器不需要内核设备或驱动；内核设备模拟器需要控制设备驱动；当前 SDK 的真实 EtherCAT 电机后端需要 ava_eth 控制设备和网络驱动。可选 Npcap 用户态后端（`AVA_ETH_NPCAP=4`），通过 `ava_eth_enumerate_npcap` 枚举网卡，无需 ava_eth 内核设备，但需安装 Npcap。内置 DLL 不会替代硬件通信所需驱动。
