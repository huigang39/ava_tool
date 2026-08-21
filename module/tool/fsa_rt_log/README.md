# fsa_rt_log

`fsa_rt_log` 是一个无需安装的 FSA 实时数据采集工具。Windows 版本发布为单个 EXE，Linux 版本发布为全静态 ELF，运行时不需要额外携带动态库。

## 构建

```powershell
cd module/tool
make fsa_rt_log
```

程序生成在 `build/fsa_rt_log.exe`。

在 Linux/WSL 下构建并生成发布包：

```bash
cd module/tool
make package
```

程序生成在 `build/fsa_rt_log`，发布包生成在 `build/fsa_rt_log-<版本>-linux-<架构>.tar.gz`。

## 使用

必须至少提供一个设备 IP。未提供 IP 时程序会显示错误并退出：

```powershell
.\build\fsa_rt_log.exe 192.168.137.102
```

同时采集多个设备：

```powershell
.\build\fsa_rt_log.exe 192.168.137.102 192.168.137.110
```

默认网段为 `192.168.137`，因此可以只写 IP 最后一段，也可以使用 `~` 输入闭区间：

```powershell
.\build\fsa_rt_log.exe 101
.\build\fsa_rt_log.exe 10~16
.\build\fsa_rt_log.exe 92 101~105
```

上面的 `10~16` 会展开为 `192.168.137.10` 到 `192.168.137.16`，共 7 个设备。完整 IPv4、最后一段和范围可以混合使用，重复地址会自动去重；设备总数最多为 64。

也可以重复指定 `--ip`，其参数同样支持完整地址、最后一段和范围：

```powershell
.\build\fsa_rt_log.exe --ip 192.168.137.102 --ip 192.168.137.110 -o logs
```

`-o` 接收输出目录，而不是文件名。默认目录为 `fsa_rt_logs`。每个 IP 使用最后一段作为子目录，目录内的轮转文件直接使用创建时间命名：

```text
fsa_rt_logs/
├── 102/
│   ├── 20260817_153012_123456.csv
│   └── 20260817_154501_654321.csv
└── 110/
    └── 20260817_153012_234567.csv
```

日志直接使用项目公共 `log` 模块的 `LOG_RING_ROTATE` 模式。单个文件达到默认的 16 MiB 后会创建新的时间戳文件。每个 IP 默认最多保留 10 个文件，可通过 `--rotate-size` 和 `--max-files` 调整。

采样速度使用频率配置，默认是 `1000 Hz`；接收超时仍使用微秒：

```powershell
.\build\fsa_rt_log.exe 192.168.137.102 --frequency 1000 --timeout-us 500
```

错误码使用独立的 RMA 请求和独立调度频率读取，默认是 `100 Hz`，不会跟随实时数据采样频率变化。可通过 `--error-frequency` 单独调整：

```powershell
.\build\fsa_rt_log.exe 192.168.137.102 --frequency 1000 --error-frequency 100
```

工具每次读取 `ERROR1` 到 `ERROR8`。检测到任一非零错误码后，默认继续采集 3 秒，随后停止所有设备的采集并退出：

```powershell
.\build\fsa_rt_log.exe 192.168.137.102 --error-stop-delay-s 3
```

使用 `--ignore-error ERRORn:MASK` 按位忽略指定错误。该参数可以重复使用，同一个 ERROR 槽位的多个配置会按位合并：

```powershell
.\build\fsa_rt_log.exe 192.168.137.102 `
    --ignore-error ERROR1:0x10 `
    --ignore-error ERROR1:0x20 `
    --ignore-error ERROR3:0x04
```

ERROR2 的低两位是编码器类型，不是错误，因此默认屏蔽 `ERROR2:0x03`。例如 ERROR2 为 `0x06` 时，去除类型值 `0x02` 后仍有错误位 `0x04`，会正常触发停止倒计时。将 `--error-stop-delay-s` 设为 `0` 可在检测到错误后立即退出。

每个时间戳日志文件都会自动写入列名：

```text
timestamp_us,producer_id,ref_pos,fdb_pos,ref_vel,fdb_vel,ref_cur,fdb_cur,ref_tor,fdb_elec_tor,latency_us
```

实时数据分为 `pvct`、`control`、`pid` 和 `power-status` 四组，读取频率都与 `--frequency` 相同。PVCT 默认开启，可以显式开启或关闭：

```powershell
.\build\fsa_rt_log.exe 92 --pvct true
.\build\fsa_rt_log.exe 92 --pvct false --control true
```

其余三组默认关闭，可以按需启用：

```powershell
# 控制模式、控制字、反馈工作模式
.\build\fsa_rt_log.exe 92 --control true

# 位置/速度环和 PD 参数
.\build\fsa_rt_log.exe 92 --pid true

# MOS 温度、线圈温度和母线电压
.\build\fsa_rt_log.exe 92 --power-status true

# 同时启用三组
.\build\fsa_rt_log.exe 92 --control true --pid true --power-status true
```

启用后会在 `latency_us` 之前依次追加以下列：

```text
control_mode,control_word,work_mode
p_kp,v_kp,v_ki,pd_kp,pd_kd
temp_mos,temp_coil,vbus
```

使用 `--pvct false` 后，PVCT 读请求和对应 CSV 列都会被移除。四组数据不能同时关闭。四个数据组选项都通过空格连接布尔值，并且只接受小写的 `true` 或 `false`。

查看全部参数与版本：

```powershell
.\build\fsa_rt_log.exe --help
.\build\fsa_rt_log.exe --version
```

按 `Ctrl+C` 停止采集。日志中每行包含时间戳、指令值、反馈值和本次通信耗时；设备 IP 由其所在子目录表示。
CSV 中的所有浮点字段统一保留三位小数。
