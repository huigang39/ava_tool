# 项目维护脚本

项目维护入口统一放在 `scripts/`。下列命令从仓库根目录运行；依赖外部固件或 SDK 的脚本需要显式指定其路径。

| 脚本 | 用途 | 示例 |
| --- | --- | --- |
| `build.py` | Windows、Linux、macOS、Android 构建入口 | `python scripts/build.py win` |
| `release.py` | 本地构建并生成安装包，可选同步版本号 | `python scripts/release.py` |
| `setup_ngspice.py` | 下载、校验并安装固定版本的 Windows ngspice 运行库 | `python scripts/setup_ngspice.py` |
| `sync_ckit.py` | 同步固件项目的 ckit，保留桌面构建设置 | `python scripts/sync_ckit.py <FocDriver/ckit> --check` |
| `sync_safety.py` | 只同步安全诊断共享 C 引擎，不复制 JSON | `python scripts/sync_safety.py <FocDriver> --check` |
| `sync_motor_faults.py` | 根据固件源码生成诊断表 | `python scripts/sync_motor_faults.py <FocDriver> --check` |
| `sync_ava_eth_sdk.ps1` | 同步 ava_eth SDK 头文件与 Windows 运行库 | `./scripts/sync_ava_eth_sdk.ps1 -SdkProject <ava_eth>` |
| `package_neofsa.py` | 打包 neofsa 便携示例及 Python 运行库，并验证解压运行 | `python scripts/package_neofsa.py --help` |
| `capture_jlink.ps1` | 手动复现 J-Link 连接问题并保存诊断日志 | `./scripts/capture_jlink.ps1 -TimingAddress <地址> -HeartbeatAddress <地址>` |

`release.py` 不执行 Git 提交、推送或 GitHub 发布。`--check` 检查同步差异，不改写目标文件。安全诊断 JSON 由用户在自己的目录维护，通过程序选择加载。运行 J-Link 诊断时应使用与当前固件匹配的 RAM 地址。

构建直接调用的图标、签名和格式工具保留在 `tools/`；测试入口和夹具保留在 `tests/`。构建产物进入 `build/`、运行产物进入 `bin/`、安装包进入 `dist/`。Python 缓存不纳入版本控制。
