# Safety 诊断

从"工具 → 功能安全"或变量管理器的"功能安全"按钮打开.先在"配置"中选择自己的 JSON 目录,再从列表选择文件,也可使用"打开 JSON"直接选择.程序不内置、不复制诊断 JSON;会话只记住所选目录和文件路径."系统变量"展开已加载 AXF/ELF 的整组状态,"单值或离线解析"支持手动输入.诊断只读,不会清故障或使能设备.

分组及解释规则由用户选择的 JSON 决定.配置中的 `fields` 按顺序优先显示常用状态和原因; `expand: true` 随后追加其它 DWARF 标量. `expand: false` 只显示指定字段, 用于参数保存或硬件快照, 避免展开参数副本和外设指针.未配置的计数,浮点测量和其它字段仍显示真实值;没有专用规则时不把非零当成错误.断线,暂停,字段缺失和无效数据会单独显示.

诊断 JSON 与固件的 `CUSTOM.json`,型号参数 JSON 不同,不参与参数下载.例如可直接选择固件仓库里的 `FocDriver/config/safety.json`,文件名不作限制.更换固件后应同时加载对应 AXF 和诊断配置.文件移动或不存在时提示重新选择,不自动回退到其它配置.旧会话自动保存的内置默认路径不再加载,明确选择过的文件路径继续保留.

数值,位图,枚举和条件解析共用 `ckit/include/safety.h`,`ckit/src/safety.c` 的纯 C 核心,不依赖 JSON,ImGui 或硬件;GUI 与离线 CLI 负责加载同一 JSON 并传入解释定义.

## 快速定位

"原因总览"跨对象筛选路径或名称含 `reason` 的字段, "全部字段"清除筛选. 默认电机控制组先列当前故障/警告, 再列自检, 校准和辨识状态, 以及保留的失败原因. 查看标题旁的完整变量路径即可定位源码.

- 电参数状态/等待原因: `g_mc.lo.safety.identify.status/e_wait_reason`.
- 首次拒绝: `g_mc.lo.safety.identify.reject.e_reject_reason`, 同对象保存该窗口数据.
- 最近稳定中断: `g_mc.lo.safety.identify.diag.stability_reset.reset_reason`.
- 惯量/磁链/电阻测温: `g_mc.lo.safety.identify.inertia/flux/temp`.
- GW 等器件: 保留各自 `lo.safety`, 例如 `g_gw.lo.safety.cali_reason`.

原因枚举使用含 `reason` 的名称, 返回码继续用 `err/status/result`, 不混成同一种错误码. 首次拒绝, 最近中断和自检失败现场都是保留记录, 不能直接当作当前故障. 当前原因与相应快照在固件中只有一份, 界面负责统一查询. 控制结果仍从 `g_mc.lo.obs.identify.out` 和 `g_mc.lo.obs.temp.out` 查看.

## 自定义

在自己的配置目录维护 JSON,使用工具中的配置加载入口打开.根变量可以是任意子系统,不要求名称包含 `safety`:

```json
{
  "id": "custom.device",
  "title": "设备诊断",
  "root": "g_device.diag",
  "expand": true,
  "fields": [
    {"path": "last_result", "title": "最近操作结果", "decoder": "merr.result"},
    {"path": "temperature", "title": "温度", "unit": "°C"}
  ]
}
```

加入 `groups` 即可.`fields.path` 相对于根路径,空字符串表示根变量.`decoder` 引用 `decoders` 中唯一的 ID;`when` 可按完整变量路径的值或掩码选择分组,例如 NEO 与 V25 协议.

解释器的 `kind` 为 `bits`,`enum`,`number`.值和掩码用字符串保存,64 位整数不会经过浮点数;复合掩码要求所有位同时置位.`type` 支持自动类型,布尔,8/16/32/64 位整数与 f32/f64.`cause/action/clear` 说明含义,排查方法和消除条件;`severity` 区分说明,正常,警告和错误.

解释器使用模块内的安全名称,例如 `gw.safety`,`gw.safety.cali.reason`.`aliases` 可保存旧 ID,供已有会话或调用继续引用同一规则;新配置使用正式 `id`,无需为旧名称复制一套规则.

故障位,等待状态,有效标志,历史快照和累计次数有不同含义.MERR 返回码,目标 C 库 errno,传感器 ALMC 与 LL 外设错误不能混用;混合来源字段保留原值并提示需查看的辅助字段.未知值不会自动解释为正常.

## 维护解析引擎

在 ava_tool 仓库运行:

```powershell
python scripts/sync_safety.py D:/GitSpace/neofsa/CM7/FocDriver
python scripts/sync_safety.py D:/GitSpace/neofsa/CM7/FocDriver --check
```

同步范围仅为上述两个 C 核心文件,不会读取、复制或修改用户 JSON,也不会覆盖其它 ckit.`--check` 只验证引擎文件是否一致.固件状态结构和枚举变化后,应在自己的配置目录维护匹配的说明.

构建和安装包不携带诊断 JSON.FocDriver 的 `tools/safety/safety_decode.py --help` 提供使用同一 JSON 的离线快照解析入口;原有错误码工具仍可使用.

Python 离线工具和同步校验需要本机 gcc,clang 或 cl 来临时构建同版 C 核心,也可设置 `SAFETY_LIBRARY` 指向已编译动态库;临时构建退出后自动清理.ava_tool 程序已包含该核心,运行时不需要编译器.

## 状态与错误总览

选择"状态与错误"集中显示 JSON 已定义的枚举状态和错误位图, 不依赖字段名包含 reason. 非零状态不等于故障. "全部字段"恢复数值和现场数据. MC 详细错误码在 `g_mc.lo.safety.error`, 辨识阶段与等待条件合并在 `identify.status`; 历史拒绝和中断现场仍单独保留. 结构变化后需加载匹配的新 AXF/ELF.

## 自动采样与读取状态

展开的对象只采样符合文本和类型筛选的字段. 折叠或筛掉的字段暂停采样, 报告保留旧值并标记暂停; 已提交请求最多延续当前轮次和约 1 秒. "只看非零"仍采样零值, 以便发现新故障.

连续地址合并读取, 每块最多 256 字节, 不跨越地址空隙和 4 KB 边界. 读取失败后逐项重试一次, 持续失败显示"读取失败". "等待刷新"表示尚未取得新样本或旧样本已过期; 超时根据刷新间隔和最近一轮读取耗时计算. 断线或切换设备后立即使旧样本失效, 新连接必须重新采样. 这些是读取状态, 不代表固件故障码.
