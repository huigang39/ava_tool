"""便携包入口；命令行与菜单都只使用包内 Python、脚本和 DLL。"""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from adapter_names import adapter_label, resolve_names

ROOT = Path(__file__).resolve().parent
DEMOS = {
    "hand_guiding": "PD 手动拖动：推得越快阻尼越大，停下不回起点",
    "virtual_spring": "PD 虚拟弹簧：推离起点产生回正力，松手回弹",
    "virtual_wall": "PD 柔性墙：自由区拖动，越过边界产生反力",
    "constant_torque": "CST 恒转矩基础测试",
    "sine_torque": "CST 正弦转矩基础测试",
}


def demo_args(name, options):
    return [sys.executable, "-B", str(ROOT / "example" / (name + ".py")),
            "--dll", str(ROOT / "lib/win/neofsa_c.dll"), *options]


def execute(name, options):
    return subprocess.call(demo_args(name, options), cwd=ROOT)


def choose(title, choices):
    print("\n" + title)
    for i, label in enumerate(choices, 1):
        print(f"  {i}. {label}")
    while True:
        value = input("输入编号，q 退出：").strip().lower()
        if value in ("q", "quit", "exit"):
            return None
        if value.isdecimal() and 1 <= int(value) <= len(choices):
            return int(value) - 1
        print("编号无效，请重新输入。")


def adapters():
    from neofsa import NeoFsa, Adapter
    neo = NeoFsa(ROOT / "lib/win/neofsa_c.dll")
    try:
        item = Adapter()
        item.backend = 4
        neo.call("read_adapter_param", item)
        count = item.count
        result = []
        for i in range(count):
            item.index = i
            neo.call("read_adapter_param", item)
            result.append(item.name.decode("utf-8"))
        return result
    finally:
        neo.close()


def self_test():
    # 此路径不枚举网卡、不打开硬件；只使用本地模拟器。
    import ctypes
    from neofsa import NeoFsa, Param, Realtime
    neo = NeoFsa(ROOT / "lib/win/neofsa_c.dll")
    try:
        p = Param()
        p.backend, p.master_action = 1, 1
        neo.call("write_param", p)
        rt = Realtime()
        rt.target.mode = -128
        rt.write_target = 1
        neo.call("write_rt", rt)
        rt.action = 1
        neo.call("write_rt", rt)
        time.sleep(.01)
        neo.call("read_rt", rt)
        if rt.feedback.mode != -128 or rt.feedback.flags & 7 != 7:
            raise RuntimeError("模拟器 PD 模式反馈不符合预期")
        rt.action = 2
        neo.call("write_rt", rt)
    finally:
        neo.close()
    for name in ("hand_guiding", "virtual_spring", "virtual_wall"):
        with tempfile.TemporaryDirectory(prefix="neofsa-selftest-") as tmp:
            csv_path = Path(tmp) / "feedback.csv"
            subprocess.run(demo_args(name, ["--backend", "sim", "--duration", "0.4", "--ramp", "0.1",
                                            "--csv", str(csv_path)]), cwd=tmp, check=True, timeout=15)
            with csv_path.open(encoding="utf-8", newline="") as stream:
                rows = list(csv.DictReader(stream))
            if not rows or any(int(row["mode"]) != -128 for row in rows):
                raise RuntimeError(f"{name} 未记录有效 PD 模式数据")
    print(f"便携包自检通过：Python {sys.version.split()[0]} / {ctypes.sizeof(ctypes.c_void_p)*8} 位，DLL、模拟器、三个 PD demo、CSV 正常。")
    return 0


def menu():
    print("FSANeo 力反馈交互 demo / ava_eth 主站 / Windows x64")
    print("退出运行按 Ctrl+C。实机交互模式会先发送固件 PD 增益，再使能。")
    names = list(DEMOS)
    selected = choose("选择示例", [DEMOS[n] for n in names] + ["便携包自检（仅模拟器）"])
    if selected is None:
        return 0
    if selected == len(names):
        return self_test()
    name = names[selected]
    backend = choose("选择连接方式", ["本地模拟器（无真实手感）", "Npcap 实机（需要已安装 Npcap）"])
    if backend is None:
        return 0
    settings = json.loads((ROOT / "settings.json").read_text(encoding="utf-8-sig"))
    settings = {**settings["common"], **settings.get(name, {})}
    options = []
    for key, value in settings.items():
        options.extend(["--" + key.replace("_", "-"), str(value)])
    if backend == 1:
        devices = adapters()
        if not devices:
            print("未找到可用 Npcap 网卡，请检查驱动和网卡。")
            return 1
        adapter_names = resolve_names()
        index = choose("选择专用 EtherCAT 网卡", [adapter_label(path, adapter_names) for path in devices])
        if index is None:
            return 0
        print("已选网卡：" + adapter_label(devices[index], adapter_names))
        print("连接路径：" + devices[index])
        options += ["--backend", "npcap", "--adapter", devices[index]]
    else:
        options += ["--backend", "sim"]
    logs = ROOT / "logs"
    logs.mkdir(exist_ok=True)
    csv_path = logs / (time.strftime("%Y%m%d-%H%M%S-") + name + ".csv")
    options += ["--csv", str(csv_path)]
    print("\n" + DEMOS[name])
    print("参数：" + json.dumps(settings, ensure_ascii=False))
    print("日志：" + str(csv_path))
    if backend == 1:
        print("该操作会写入 PD 增益并使能；请核对参数、机构和所选网卡。")
    if input("输入 start 开始，其他输入退出：").strip().lower() != "start":
        return 0
    return execute(name, options)


def main():
    if len(sys.argv) == 1:
        return menu()
    name, *options = sys.argv[1:]
    if name in ("--help", "-h"):
        print("run.cmd [self-test | adapters | demo名称 [参数]]")
        for key, label in DEMOS.items():
            print(f"  {key}: {label}")
        return 0
    if name == "self-test":
        return self_test()
    if name == "adapters":
        names = resolve_names()
        for i, path in enumerate(adapters()):
            print(f"{i}: {adapter_label(path, names)}\n   {path}")
        return 0
    if name not in DEMOS and name != "impedance_torque":
        raise ValueError("未知 demo，请使用 run.cmd --help")
    return execute(name, options)


if __name__ == "__main__":
    try:
        result = main()
    except KeyboardInterrupt:
        print("已请求停止。")
        result = 130
    except Exception as exc:
        print(f"运行失败：{exc}", file=sys.stderr)
        result = 1
    sys.exit(result)
