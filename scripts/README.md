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

`release.py` 默认只在本地打包；添加 `--publish` 后，构建成功会在 `origin` 对应的 GitHub 仓库创建 `v<版本号>` Release，自动生成发布说明并上传 `dist/ava_tool_setup_<版本号>.exe`。需要安装 GitHub CLI 并执行 `gh auth login`。脚本不自动提交或推送代码；发布要求工作区干净，且远端同名标签指向当前提交，支持轻量标签和附注标签。已有同名 Release 时命令报错，不覆盖已有附件；发布失败会保留本地安装包。

例如发布新版本（将 `1.2.0` 换成实际版本）：

```powershell
python scripts/release.py 1.2.0
git add src/version.hpp installer/ava_tool.iss
git add scripts/release.py scripts/README.md
git commit -m "Release v1.2.0"
git tag v1.2.0
git push origin HEAD
git push origin v1.2.0
python scripts/release.py --publish
```

同步脚本的 `--check` 检查同步差异，不改写目标文件。安全诊断 JSON 由用户在自己的目录维护，通过程序选择加载。运行 J-Link 诊断时应使用与当前固件匹配的 RAM 地址。

构建直接调用的图标、签名和格式工具保留在 `tools/`；测试入口和夹具保留在 `tests/`。构建产物进入 `build/`、运行产物进入 `bin/`、安装包进入 `dist/`。Python 缓存不纳入版本控制。
