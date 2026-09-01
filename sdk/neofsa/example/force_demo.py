"""固件 PD（-128）力控示例公共运行器。主机循环不保证硬实时，周期 PDO 由 ava_eth 发送。"""
import argparse
import csv
import math
import time
import sys
from adapter_names import adapter_label, resolve_names
from neofsa import NeoFsa, Param, Realtime, Adapter, PdParam


def interaction_torque(kind, position, velocity, reference, kp, kd, wall_distance=0.2, wall_softness=0.02):
    """人推动机构后的反馈力矩，输入为出轴 rad、rad/s，输出 Nm。"""
    displacement = position - reference
    if kind == "hand_guiding":
        # 无位置回正项：推动时有阻尼，停下后不主动回起点。
        return -kd * velocity
    if kind == "impedance":
        return -kp * displacement - kd * velocity
    if kind == "wall":
        penetration = abs(displacement) - wall_distance
        if penetration <= 0:
            return 0.0
        normal = 1.0 if displacement > 0 else -1.0
        # 接触阻尼渐入，撤手时不产生朝墙内的吸力。
        contact = min(1.0, penetration / wall_softness)
        return -normal * max(0.0, kp * penetration + kd * normal * velocity * contact)
    raise ValueError(f"未知的人机交互模式：{kind}")


def interaction_state(kind, position, velocity, reference, wall_distance=0.2):
    if kind == "wall":
        displacement = position - reference
        if abs(displacement) <= wall_distance:
            return "自由区"
        return "正向墙接触" if displacement > 0 else "反向墙接触"
    if kind == "hand_guiding":
        return "阻尼拖动" if abs(velocity) > 0.01 else "静止，无回正力"
    if kind == "impedance":
        return "弹簧回正" if abs(position - reference) > 0.01 else "平衡位置"
    return "转矩测试"


def command(kind, t, duration, ramp, amplitude, frequency, kp, kd, position, velocity, reference, limit,
            wall_distance=0.2, wall_softness=0.02):
    envelope = max(0.0, min(1.0, t / ramp, (duration - t) / ramp))
    if kind == "constant":
        torque = amplitude
    elif kind == "sine":
        torque = amplitude * math.sin(2 * math.pi * frequency * t)
    else:
        torque = interaction_torque(kind, position, velocity, reference, kp, kd, wall_distance, wall_softness)
    return max(-limit, min(limit, torque * envelope))


def pd_target(kind, t, duration, ramp, amplitude, frequency, kp, kd, position, velocity, reference, limit,
              wall_distance=0.2, wall_softness=0.02):
    """返回固件 PD 的位置/速度参考及采样时刻的力矩估计；不使用 torque 字段。"""
    if kind in ("constant", "sine"):
        if kp <= 0:
            raise ValueError("恒值/正弦 PD 转矩测试要求 Kp > 0")
        desired = amplitude if kind == "constant" else amplitude * math.sin(2 * math.pi * frequency * t)
        dp, dv = desired / kp, 0.0
    elif kind == "hand_guiding":
        if kp != 0:
            raise ValueError("手动拖动要求固件 Kp=0")
        dp, dv = 0.0, -velocity
    elif kind == "impedance":
        dp, dv = reference - position, -velocity
    elif kind == "wall":
        displacement = position - reference
        penetration = abs(displacement) - wall_distance
        if penetration <= 0:
            dp, dv = 0.0, 0.0
        else:
            normal = 1.0 if displacement > 0 else -1.0
            dp = -normal * penetration
            dv = -velocity * min(1.0, penetration / wall_softness)
            if (kp * dp + kd * dv) * normal > 0:
                dp, dv = 0.0, 0.0  # 撤回时不吸入墙内
    else:
        raise ValueError(f"未知 PD 交互模式：{kind}")
    estimate = kp * dp + kd * dv
    envelope = max(0.0, min(1.0, t / ramp, (duration - t) / ramp))
    scale = envelope
    if abs(estimate) * scale > limit:
        scale = limit / abs(estimate)
    # Kp/Kd 始终由固件执行，主机只调整参考误差；不用主机 CST 环。
    return position + dp * scale, velocity + dv * scale, estimate * scale


def configure_pd(neo, backend, kp, kd, dt):
    if backend == "sim":
        print("模拟器不支持 PD 增益写入；仅验证 -128 模式和目标流程，不模拟真实 PD 力学。")
        return kp, kd
    gains = PdParam()
    gains.kp, gains.kd = kp, kd
    neo.call("write_pd_param", gains)
    expected_sequence = gains.sequence
    deadline = time.perf_counter() + 3.0
    while True:
        neo.call("read_pd_param", gains)
        if gains.sequence != expected_sequence:
            raise RuntimeError("PD 参数请求被其他调用方替换")
        if gains.state == 2:
            print(f"PD 增益已发送：Kp={gains.sent_kp:g}，Kd={gains.sent_kd:g}（发送精度，非设备回读）")
            return gains.sent_kp, gains.sent_kd
        if gains.state == 3:
            raise RuntimeError(f"PD 增益发送失败 ({gains.result})：{gains.message.decode('utf-8', errors='replace')}")
        if time.perf_counter() >= deadline:
            raise RuntimeError("等待 PD 增益发送超时，不使能")
        time.sleep(dt)


def check_load_torque(measured, limit, initial=False):
    if abs(measured) > limit:
        stage = "使能前检查" if initial else "运行中检查"
        action = "不使能" if initial else "停止输出并请求失能"
        raise RuntimeError(
            f"{stage}：负载传感器转矩={measured:+.6g} Nm，绝对值={abs(measured):.6g} Nm，"
            f"超过 max_load_torque={limit:.6g} Nm，{action}。"
            "请检查外力、重力负载及传感器零偏/标定；此阈值独立于 max_torque，程序不会自动调大或自动清零。")


def main(kind):
    try:
        run(kind)
        return 0
    except (RuntimeError, OSError, ValueError) as exc:
        print(f"运行已停止：{exc}", file=sys.stderr)
        return 1


def run(kind):
    parser = argparse.ArgumentParser(description={
        "constant": "恒转矩：缓升、保持、缓降，负载转矩单位 Nm",
        "sine": "正弦转矩：用于低幅值响应测试",
        "impedance": "人机交互 / 虚拟弹簧：手推偏离起点产生回正力，松手后阻尼回弹",
        "hand_guiding": "人机交互 / 手动拖动：人推着走，速度越大阻尼越大，停下不回起点",
        "wall": "人机交互 / 柔性限位墙：自由区内可拖动，越过边界产生连续反力",
    }[kind])
    parser.add_argument("--backend", choices=["sim", "npcap"], default="sim")
    parser.add_argument("--adapter", default="", help="Npcap 网卡路径")
    parser.add_argument("--list-adapters", action="store_true")
    parser.add_argument("--dll", help="neofsa_c.dll 路径")
    parser.add_argument("--runtime", default="", help="ava_eth_sdk.dll 路径")
    parser.add_argument("--duration", type=float, default=30.0 if kind in ("hand_guiding", "impedance", "wall") else 5.0)
    parser.add_argument("--dt", type=float, default=0.01, help="主机更新间隔，秒")
    parser.add_argument("--ramp", type=float, default=1.0)
    parser.add_argument("--torque", type=float, default=0.1, help="恒值或正弦幅值，Nm")
    parser.add_argument("--frequency", type=float, default=0.5, help="正弦频率 Hz")
    parser.add_argument("--kp", type=float, default=0.5, help="Nm/rad")
    parser.add_argument("--kd", type=float, default=0.05, help="Nm*s/rad")
    parser.add_argument("--wall-distance", type=float, default=0.2, help="双向虚拟墙距起始位置的 rad")
    parser.add_argument("--wall-softness", type=float, default=0.02, help="接触阻尼渐入距离 rad")
    parser.add_argument("--max-torque", type=float, default=0.5)
    parser.add_argument("--max-load-torque", type=float, default=5.0, help="负载传感器测量值超过此 Nm 时失能")
    parser.add_argument("--max-velocity", type=float, default=2.0, help="超过此 rad/s 停止")
    parser.add_argument("--max-travel", type=float, default=0.5, help="相对起始位置最大 rad")
    parser.add_argument("--csv", help="反馈 CSV 路径")
    args = parser.parse_args()
    values = [args.duration, args.dt, args.ramp, args.torque, args.frequency, args.kp, args.kd,
              args.max_torque, args.max_velocity, args.max_travel, args.wall_distance, args.wall_softness, args.max_load_torque]
    if not all(math.isfinite(v) for v in values):
        parser.error("所有参数必须为有限数值")
    if min(args.duration, args.dt, args.ramp, args.frequency, args.max_torque, args.max_velocity, args.max_travel, args.max_load_torque) <= 0:
        parser.error("时间、频率和限幅必须大于零")
    if args.dt > 0.02 or args.dt < 0.001 or args.duration < 2 * args.ramp or min(args.kp, args.kd) < 0:
        parser.error("dt 须在 0.001..0.02 秒，duration >= 2*ramp，Kp/Kd 非负")
    if max(args.kp, args.kd) > 65504:
        parser.error("PD 增益不得超过 float16 最大值 65504")
    if kind in ("constant", "sine") and args.kp <= 0:
        parser.error("恒值/正弦 PD 转矩测试要求 Kp > 0")
    if abs(args.torque) > args.max_torque:
        parser.error("转矩幅值超过 max-torque")
    if kind == "wall" and not (0 < args.wall_distance < args.max_travel and
                               0 < args.wall_softness < args.max_travel - args.wall_distance):
        parser.error("虚拟墙距离须在 0..max-travel 内，渐入距离须小于墙外剩余行程")
    if kind == "sine" and args.frequency * args.dt > 0.1:
        parser.error("正弦每周期至少需要 10 个主机更新点")
    if args.backend == "npcap" and not args.adapter and not args.list_adapters:
        parser.error("实机必须通过 --adapter 指定网卡，可先 --list-adapters")
    neo = NeoFsa(args.dll)
    rt = Realtime()
    pd_mode = kind in ("hand_guiding", "impedance", "wall")
    rt.target.mode = -128 if pd_mode else 10
    mode_label = "PD(-128)" if pd_mode else "CST(10)"
    log = None
    connected = False
    try:
        if args.list_adapters:
            item = Adapter()
            item.backend = 4
            item.runtime_dll = args.runtime.encode("utf-8")
            neo.call("read_adapter_param", item)
            count = item.count
            names = resolve_names()
            for index in range(count):
                item.index = index
                neo.call("read_adapter_param", item)
                path = item.name.decode("utf-8")
                print(f"{index}: {adapter_label(path, names)}\n   {path}")
            return
        param = Param()
        param.runtime_dll = args.runtime.encode("utf-8")
        param.adapter = args.adapter.encode("utf-8")
        param.backend = 1 if args.backend == "sim" else 4
        param.axes, param.cycle_us, param.watchdog_ms = 1, 1000, 100
        param.master_action = 1
        neo.call("write_param", param)
        connected = True
        deadline = time.perf_counter() + 1.0
        while True:
            neo.call("read_rt", rt)
            if rt.feedback.flags & 3 == 3:
                break
            if time.perf_counter() >= deadline:
                raise RuntimeError("等待有效反馈超时")
            time.sleep(args.dt)
        if rt.feedback.flags & (8 | 16) or rt.feedback.error_code:
            raise RuntimeError("初始反馈存在故障或命令过期")
        if not all(math.isfinite(v) for v in (rt.feedback.position, rt.feedback.velocity, rt.feedback.torque)):
            raise RuntimeError("初始反馈存在无效数值")
        rt.action = 2
        neo.call("write_rt", rt)
        kp, kd = args.kp, args.kd
        if pd_mode:
            kp, kd = configure_pd(neo, args.backend, 0.0 if kind == "hand_guiding" else args.kp, args.kd, args.dt)
        if kind in ("constant", "sine") and kp <= 0:
            raise RuntimeError("发送精度下 Kp 已量化为零，无法执行转矩测试")
        # 参数发送期间人可能移动机构：重新采样后再初始化 PD 参考。
        neo.call("read_rt", rt)
        if rt.feedback.flags & 3 != 3 or rt.feedback.flags & (8 | 16) or rt.feedback.error_code:
            raise RuntimeError("PD 参数发送后反馈异常，不使能")
        if not all(math.isfinite(v) for v in (rt.feedback.position, rt.feedback.velocity, rt.feedback.torque)):
            raise RuntimeError("PD 参数发送后反馈存在无效数值")
        print(f"使能前负载传感器转矩：{rt.feedback.torque:+.6g} Nm；停机阈值：±{args.max_load_torque:g} Nm")
        check_load_torque(rt.feedback.torque, args.max_load_torque, initial=True)
        reference = rt.feedback.position
        rt.target.position = reference
        rt.target.velocity = rt.feedback.velocity
        rt.write_target = 1  # 先在失能状态初始化位置/速度，初始 PD 误差为零
        neo.call("write_rt", rt)
        rt.action = 1
        neo.call("write_rt", rt)
        deadline = time.perf_counter() + 1.0
        while True:
            rt.action = 4
            neo.call("write_rt", rt)
            neo.call("read_rt", rt)
            if rt.feedback.flags & (8 | 16):
                raise RuntimeError("使能期间出现故障或命令过期")
            if rt.feedback.flags & 7 == 7 and rt.feedback.mode == rt.target.mode:
                break
            if time.perf_counter() >= deadline:
                raise RuntimeError(f"等待 {mode_label} 使能反馈超时")
            time.sleep(args.dt)
        if args.csv:
            log = open(args.csv, "w", newline="", encoding="utf-8")
            writer = csv.writer(log)
            writer.writerow(["time_s", ("pd_estimate_Nm" if pd_mode else "command_Nm"), "load_Nm", "position_rad", "velocity_rad_s", "flags", "relative_rad", "interaction", "target_position_rad", "target_velocity_rad_s", "mode"])
        print(f"{kind} / {mode_label} / {args.backend}，起始位置 {reference:.6g} rad，Ctrl+C 停止")
        if kind in ("hand_guiding", "impedance", "wall"):
            print(f"人机交互：Kp={kp:g} Nm/rad，Kd={kd:g} Nm*s/rad，采样时刻估计限幅={args.max_torque:g} Nm，负载传感器停机阈值={args.max_load_torque:g} Nm")
            if kind == "hand_guiding":
                print("请轻推机构：慢推阻力小、快推阻力大；停下后不会主动回起点。")
            elif kind == "impedance":
                print("请轻推离开起始位置：偏离越大回正力越大，松手后向起始位置回弹。")
            else:
                print(f"请在起点两侧 ±{args.wall_distance:g} rad 内拖动：自由区无目标转矩，越界后感受墙的反力。")
            if args.backend == "sim":
                print("当前为接口模拟器，不包含人推动机构的力学响应；实际手感需接实机体验。")
        start = last = time.perf_counter()
        next_print = 0.0
        while True:
            now = time.perf_counter()
            t = now - start
            if t >= args.duration:
                break
            if now - last > 0.05:
                raise RuntimeError("主机循环延迟超过 50 ms，停止输出")
            last = now
            neo.call("read_rt", rt)
            f = rt.feedback
            if f.flags & 7 != 7 or f.flags & (8 | 16) or f.error_code or f.mode != rt.target.mode:
                raise RuntimeError(f"反馈异常：flags=0x{f.flags:x}, error=0x{f.error_code:x}, mode={f.mode}")
            if not all(math.isfinite(v) for v in (f.position, f.velocity, f.torque)):
                raise RuntimeError("反馈出现无效数值")
            if abs(f.position - reference) > args.max_travel or abs(f.velocity) > args.max_velocity:
                raise RuntimeError("超过位移或速度限值，停止输出")
            check_load_torque(f.torque, args.max_load_torque)
            if pd_mode:
                rt.target.position, rt.target.velocity, estimate = pd_target(
                    kind, t, args.duration, args.ramp, args.torque, args.frequency, kp, kd,
                    f.position, f.velocity, reference, args.max_torque, args.wall_distance, args.wall_softness)
                rt.target.torque = 0.0  # PD 使用 t_ff；公开 ABI 未暴露，不能用 t 冒充
            else:
                estimate = command(kind, t, args.duration, args.ramp, args.torque, args.frequency, kp, kd,
                                   f.position, f.velocity, reference, args.max_torque)
                rt.target.torque = estimate
            rt.write_target = 1
            neo.call("write_rt", rt)  # 新目标同时续约看门狗；不使用后台永久续约
            state = interaction_state(kind, f.position, f.velocity, reference, args.wall_distance)
            if log:
                writer.writerow([t, estimate, f.torque, f.position, f.velocity, f.flags,
                                 f.position - reference, state, rt.target.position, rt.target.velocity, f.mode])
            if t >= next_print:
                torque_label = "PD估计" if pd_mode else "目标"
                print(f"{t:6.3f}s  {torque_label}={estimate:+.4f} Nm  负载={f.torque:+.4f} Nm  Δq={f.position-reference:+.4f} rad  v={f.velocity:+.4f} rad/s  {state}")
                next_print = t + 0.25
            time.sleep(max(0.0, args.dt - (time.perf_counter() - now)))
    except KeyboardInterrupt:
        print("已请求停止")
    finally:
        try:
            if connected:
                rt.action = 2
                neo.call("write_rt", rt)
        finally:
            neo.close()
            if log:
                log.close()
