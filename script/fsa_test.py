"""
FSA 测试脚本: 单摆正反转冲击测试 (单线程多设备同步加速版)
pip install -i https://test.pypi.org/simple/ fi_fsa_v3
"""

import fi_fsa_v3
import time, math, signal, sys
from enum import IntEnum

# ===== 全局退出标志 =====
stop_event = False

# ===== 全局 FSA 列表 =====
fsa_list: list[fi_fsa_v3.FSA] = []


def handle_ctrl_c(signal_num, frame) -> None:
    global stop_event
    print("\n[退出] 所有执行器速度置零")
    stop_event = True
    for fsa in fsa_list:
        try:
            fsa.SetVelocity(0)
        except Exception:
            pass
    sys.exit(0)


def rads_to_rpm(rad_s: float) -> float:
    return (rad_s / (2 * math.pi)) * 60


def rpm_to_rads(rpm: float) -> float:
    return (rpm / 60) * (2 * math.pi)


class TestMode(IntEnum):
    IMPACT_MODE = 0  # 冲击模式
    FR_MODE = 1  # 正反转模式

    def __str__(self):
        return self.name


class TestCfg:
    def __init__(
            self,
            ref_high_vel=0,
            high_duration_time=0,
            ref_low_vel=0,
            low_duration_time=0,
            direction_time=0,
            curr_time=0,
            accel_rpm_s=25,
    ):
        self.ref_high_vel = ref_high_vel
        self.high_duration_time = high_duration_time
        self.ref_low_vel = ref_low_vel
        self.low_duration_time = low_duration_time
        self.direction_time = direction_time
        self.curr_timestamp = curr_time
        self.accel_rpm_s = accel_rpm_s


# ==== 预设参数 ====
FR_CFG = TestCfg(
    ref_high_vel=3,
    high_duration_time=30,
    ref_low_vel=-3,
    low_duration_time=30,
    accel_rpm_s=3,
)

IMPACT_CFG = TestCfg(
    ref_high_vel=3,
    high_duration_time=0.5,
    ref_low_vel=-5,
    low_duration_time=0.5,
    direction_time=100,
    accel_rpm_s=30,
)

INIT_FLAG = False


def traj_vel(fsa_list, start_rpm, end_rpm, accel_rpm_s, tags, step_time=0.05):
    """平滑速度过渡"""

    if accel_rpm_s <= 0:
        accel_rpm_s = 1.0
    delta_rpm = end_rpm - start_rpm
    duration_s = abs(delta_rpm) / accel_rpm_s
    steps = max(int(duration_s / step_time), 1)

    for i in range(steps + 1):
        if stop_event:
            return
        rpm = start_rpm + delta_rpm * (i / steps)
        for fsa, tag in zip(fsa_list, tags):
            ret = fsa.SetVelocity(rpm_to_rads(rpm))
            if ret != fi_fsa_v3.ret_e.SUCCESS:
                print(f"[{tag}] [错误] 平滑设置速度失败, ret: {ret}")
        time.sleep(step_time)


def fr_mode(fsa_list, cfg: TestCfg, tags):
    """正反转模式"""

    if stop_event:
        return

    # 正转阶段
    print(f"[正转] 目标: {cfg.ref_high_vel:.2f} RPM")

    global INIT_FLAG
    if not INIT_FLAG:
        traj_vel(fsa_list, 0, cfg.ref_high_vel, cfg.accel_rpm_s, tags)
        INIT_FLAG = True
    else:
        traj_vel(fsa_list, cfg.ref_low_vel, cfg.ref_high_vel, cfg.accel_rpm_s, tags)

    t0 = time.time()
    while time.time() - t0 < cfg.high_duration_time:
        if stop_event:
            return
        time.sleep(0.1)

    # 反转阶段
    print(f"[反转] 目标: {cfg.ref_low_vel:.2f} RPM")
    traj_vel(fsa_list, cfg.ref_high_vel, cfg.ref_low_vel, cfg.accel_rpm_s, tags)
    t0 = time.time()
    while time.time() - t0 < cfg.low_duration_time:
        if stop_event:
            return
        time.sleep(0.1)


def impact_mode(fsa_list, cfg: TestCfg, tags):
    """冲击模式"""

    static_vars = impact_mode.__dict__
    last_dir = static_vars.get("last_dir", 1)
    last_rpm = static_vars.get("last_rpm", 0.0)
    swapped = static_vars.get("swapped", False)  # 当前是否已交换正反速度配置

    # 每次运行都翻转方向
    last_dir *= -1
    print(f"[冲击] 当前方向: {'正转' if last_dir > 0 else '反转'}")

    # 检查是否需要交换正反速度（方向漂移翻转）
    elapsed = time.time() - cfg.curr_timestamp
    if elapsed >= cfg.direction_time:
        cfg.curr_timestamp = time.time()
        swapped = not swapped  # 交换一次正反速度配置
        print(
            f"[方向偏移翻转] 已交换正反速度定义: {'高低速度对调' if swapped else '恢复原定义'}"
        )

    # 根据 swapped 状态选择速度定义
    if not swapped:
        pos_vel, neg_vel = cfg.ref_high_vel, cfg.ref_low_vel
    else:
        pos_vel, neg_vel = -cfg.ref_low_vel, -cfg.ref_high_vel

    # 根据当前翻转方向选择目标速度
    target_rpm = pos_vel if last_dir > 0 else neg_vel
    hold_time = cfg.high_duration_time if last_dir > 0 else cfg.low_duration_time

    # 平滑过渡：从上次速度 -> 当前目标速度
    traj_vel(fsa_list, last_rpm, target_rpm, cfg.accel_rpm_s, tags)

    # 保持该方向运行一段时间
    t0 = time.time()
    while time.time() - t0 < hold_time:
        if stop_event:
            return
        time.sleep(0.05)

    # 保存当前状态
    static_vars["last_rpm"] = target_rpm
    static_vars["last_dir"] = last_dir
    static_vars["swapped"] = swapped


def init(ip: str) -> fi_fsa_v3.FSA | None:
    """初始化设备"""

    fsa = fi_fsa_v3.FSA()
    ret = fsa.Init(ip, fi_fsa_v3.net_recv_mode_e.YIELD_WAIT, "")
    if ret != fi_fsa_v3.ret_e.SUCCESS:
        print(f"[错误] FSA 初始化失败 ({ip}), ret: {ret}")
        return None
    ret = fsa.EnableControl(fi_fsa_v3.ctrl_mode_e.VELOCITY_MODE)
    if ret != fi_fsa_v3.ret_e.SUCCESS:
        print(f"[错误] 速度模式设置失败 ({ip}), ret: {ret}")
        return None
    print(f"[正常] FSA 已连接: {ip}")
    return fsa


def loop(fsa_list, mode, cfg: TestCfg):
    """主控制循环"""

    tags = [f"设备{i + 1}" for i in range(len(fsa_list))]
    cfg.curr_timestamp = time.time()

    while not stop_event:
        if mode == TestMode.FR_MODE:
            fr_mode(fsa_list, cfg, tags)
        elif mode == TestMode.IMPACT_MODE:
            impact_mode(fsa_list, cfg, tags)

    # 安全退出
    for idx, fsa in enumerate(fsa_list):
        try:
            fsa.SetVelocity(0)
            print(f"[设备{idx + 1}] 已停止")
        except Exception:
            pass


if __name__ == "__main__":
    signal.signal(signal.SIGINT, handle_ctrl_c)

    # 输入 IP 列表
    print("请输入多个 IP 地址最后一段 (例如: 101 102 103)")
    try:
        octets = input("输入: ").strip().split()
        if not octets:
            print("[错误] 至少输入一个 IP 地址")
            sys.exit(1)
        ips = [f"192.168.137.{int(o)}" for o in octets if 0 <= int(o) <= 255]
    except ValueError:
        print("[错误] 输入必须是数字 0-255")
        sys.exit(1)

    # 初始化所有设备
    fsa_list = []
    for ip in ips:
        fsa = init(ip)
        if fsa:
            fsa_list.append(fsa)
    if not fsa_list:
        print("[错误] 没有任何设备初始化成功")
        sys.exit(1)

    # 模式选择
    print("请选择模式:\n0. 正反转冲击模式 (IMPACT_MODE)\n1. 普通正反转模式 (FR_MODE)")
    try:
        user_input = int(input("输入模式编号: ").strip())
        if user_input == TestMode.FR_MODE:
            mode = TestMode.FR_MODE
            cfg = FR_CFG
        elif user_input == TestMode.IMPACT_MODE:
            mode = TestMode.IMPACT_MODE
            cfg = IMPACT_CFG
        else:
            print(f"[错误] 无效模式: {user_input}")
            sys.exit(1)
    except ValueError:
        print("[错误] 输入必须是数字 0 或 1")
        sys.exit(1)

    print(f"\n[开始测试] 设备数量: {len(fsa_list)} 个, 模式: {mode}")
    loop(fsa_list, mode, cfg)
