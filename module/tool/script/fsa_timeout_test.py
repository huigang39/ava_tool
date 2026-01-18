import datetime
import json
import signal
import socket
import sys
import threading
import time

BOARDCAST_IP = "192.168.137.255"
stop_event = threading.Event()

fsa_v2_list = []
fsa_v3_list = []

GR3_IP_LIST = [
    # 左臂
    "192.168.137.10",
    "192.168.137.11",
    "192.168.137.12",
    "192.168.137.13",
    "192.168.137.14",
    "192.168.137.15",
    "192.168.137.16",
    # 右臂
    "192.168.137.30",
    "192.168.137.31",
    "192.168.137.32",
    "192.168.137.33",
    "192.168.137.34",
    "192.168.137.35",
    "192.168.137.36",
    # 左腿
    "192.168.137.71",
    "192.168.137.72",
    "192.168.137.73",
    "192.168.137.74",
    "192.168.137.75",
    # 右腿
    "192.168.137.51",
    "192.168.137.52",
    "192.168.137.53",
    "192.168.137.54",
    "192.168.137.55",
    # 腰
    "192.168.137.90",
    "192.168.137.91",
    "192.168.137.92",
    # 头
    "192.168.137.93",
    "192.168.137.95",
]


def log_print(*args):
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    msg = f"[{timestamp}] " + " ".join(str(a) for a in args)
    print(msg)


def print_summary(info: str):
    RED = "\033[91m"
    YELLOW = "\033[93m"
    GREEN = "\033[92m"
    RESET = "\033[0m"

    log_print(f"\n# =========================== {info} =========================== #")

    def colorize(fsa: FSA) -> str:
        if (
                fsa.timeout_rate > fsa.max_timeout_rate
                or fsa.history_max_continuous_timeout_cnt > fsa.max_continuous_timeout_cnt
        ):
            return f"{RED}{fsa}{RESET}"
        elif fsa.timeout_cnt > 0:
            return f"{YELLOW}{fsa}{RESET}"
        else:
            return f"{GREEN}{fsa}{RESET}"

    if fsa_v2_list:
        print("[FSA V2]")
        for fsa in fsa_v2_list:
            print("  -", colorize(fsa))

    if fsa_v3_list:
        print("[FSA V3]")
        for fsa in fsa_v3_list:
            print("  -", colorize(fsa))

    if not fsa_v2_list and not fsa_v3_list:
        print("当前没有任何 FSA 设备.")

    print("# ================================================================ #\n")


class FSA:
    def __init__(self, ip: str):
        self.sock = ()
        self.addr = ()
        self.timeout = 0.05
        self.max_cnt = 100000
        self.max_continuous_timeout_cnt = 2
        self.max_timeout_rate = 1  # 万分之
        self.ip = ip
        self.all_cnt = 0
        self.timeout_cnt = 0
        self.continuous_timeout_cnt = 0
        self.history_max_continuous_timeout_cnt = 0
        self.timeout_rate = 0.0  # 万分之

    def __repr__(self):
        return f"FSA(ip: {self.ip}, 总包数: {self.all_cnt}, 丢包数: {self.timeout_cnt}, 最大连续丢包数: {self.history_max_continuous_timeout_cnt}, 丢包率: 万分之[{self.timeout_rate:.6f}])"


def boardcast(data: str, port=2334, timeout=1.0, retry=5):
    log_print(
        "# ------------------------------------ 广播 ------------------------------------ #"
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(timeout)
    addr = (BOARDCAST_IP, port)

    for i in range(retry):
        sock.sendto(data.encode("utf-8"), addr)
        start = time.time()
        while time.time() - start < timeout:
            if stop_event.is_set():
                break

            try:
                recv_data, from_addr = sock.recvfrom(1024)
                msg = recv_data.decode("utf-8")

                try:
                    json_data = json.loads(msg)
                except json.JSONDecodeError:
                    log_print(f"非JSON数据: {msg}")
                    continue

                ip = from_addr[0]
                type = json_data.get("type", "")
                if type == "Actuator":
                    ver = json_data.get("protocol_version", 2)
                    if ver == 3:
                        if ip not in [f.ip for f in fsa_v3_list]:
                            fsa_v3_list.append(FSA(ip))
                    else:
                        if ip not in [f.ip for f in fsa_v2_list]:
                            fsa_v2_list.append(FSA(ip))
            except socket.timeout:
                break

    sock.close()
    print_summary("搜索结果")

    discovered_ips = [f.ip for f in fsa_v2_list + fsa_v3_list]
    missing_ips = [ip for ip in GR3_IP_LIST if ip not in discovered_ips]

    if missing_ips:
        print("\033[91m未发现以下 FSA IP: \033[0m")  # 红色
        for ip in missing_ips:
            print(f"  - {ip}")
    else:
        print("\033[92m所有 FSA 均已搜索到!\033[0m")  # 绿色


def udp_timeout_test(fsa: FSA, msg):
    while not stop_event.is_set():
        fsa.sock.sendto(msg, fsa.addr)
        fsa.all_cnt += 1

        if fsa.all_cnt >= fsa.max_cnt:
            break

        try:
            recv_data, _ = fsa.sock.recvfrom(1024)
            fsa.continuous_timeout_cnt = 0
            fsa.timeout_rate = (fsa.timeout_cnt / fsa.all_cnt) * 10000
        except socket.timeout:
            fsa.timeout_cnt += 1
            fsa.continuous_timeout_cnt += 1
            if fsa.continuous_timeout_cnt > fsa.history_max_continuous_timeout_cnt:
                fsa.history_max_continuous_timeout_cnt = fsa.continuous_timeout_cnt
            fsa.timeout_rate = (fsa.timeout_cnt / fsa.all_cnt) * 10000
            print_summary("丢包记录")


def enable_v2(fsa: FSA, retry=5):
    # 使能
    timeout_flag = -1
    msg = bytes([0x51, 0x0, 0x0, 0x0, 0x0])
    for i in range(retry):
        try:
            fsa.sock.sendto(msg, fsa.addr)
            recv_data, _ = fsa.sock.recvfrom(1024)
            timeout_flag = 0
        except socket.timeout:
            continue
    if timeout_flag == -1:
        return timeout_flag

    # 速度模式
    timeout_flag = -1
    msg = bytes([0x5B, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0])
    for i in range(retry):
        try:
            fsa.sock.sendto(msg, fsa.addr)
            recv_data, _ = fsa.sock.recvfrom(1024)
            timeout_flag = 0
        except socket.timeout:
            continue
    if timeout_flag == -1:
        return timeout_flag

    # 速度设置为 0
    timeout_flag = -1
    msg = bytes([0x55, 0x0, 0x0, 0x0, 0x0])
    for i in range(retry):
        try:
            fsa.sock.sendto(msg, fsa.addr)
            recv_data, _ = fsa.sock.recvfrom(1024)
            timeout_flag = 0
        except socket.timeout:
            continue
    if timeout_flag == -1:
        return timeout_flag

    return 0


def disable_v2(fsa: FSA, retry=5):
    timeout_flag = -1
    msg = bytes([0x52, 0x0, 0x0, 0x0, 0x0])
    for i in range(retry):
        try:
            fsa.sock.sendto(msg, fsa.addr)
            recv_data, _ = fsa.sock.recvfrom(1024)
            timeout_flag = 0
        except socket.timeout:
            continue
    if timeout_flag == -1:
        return timeout_flag

    return 0


def get_pvct_v2(fsa: FSA):
    log_print(f"# ---------- 启动 V2 测试: {fsa.ip} ---------- #")

    msg = bytes([0x1D])
    udp_timeout_test(fsa, msg)


def enable_v3(fsa: FSA, retry=5):
    timeout_flag = -1
    msg = bytes(
        [
            0x0,
            0x58,
            0x0,
            0x0,
            0x62,
            0x81,
            0x0,
            0x0,
            0x0,
            0x0,
            0x2,
            0x82,
            0x0,
            0x0,
            0x0,
            0x0,
            0x2,
            0x80,
            0x5,
            0x0,
            0x0,
            0x0,
            0x22,
            0x80,
            0x2,
            0xF0,
            0x0,
            0x0,
        ]
    )
    for i in range(retry):
        try:
            fsa.sock.sendto(msg, fsa.addr)
            recv_data, _ = fsa.sock.recvfrom(1024)
            timeout_flag = 0
        except socket.timeout:
            continue
    if timeout_flag == -1:
        return timeout_flag

    return 0


def disable_v3(fsa: FSA, retry=5):
    timeout_flag = -1
    msg = bytes([0x0, 0x58, 0x0, 0x0, 0x22, 0x80, 0x1, 0xF0, 0x0, 0x0])
    for i in range(retry):
        try:
            fsa.sock.sendto(msg, fsa.addr)
            recv_data, _ = fsa.sock.recvfrom(1024)
            timeout_flag = 0
        except socket.timeout:
            continue
    if timeout_flag == -1:
        return timeout_flag

    return 0


def get_pvct_v3(fsa: FSA):
    log_print(f"# ---------- 启动 V3 测试: {fsa.ip} ---------- #")

    msg = bytes([0x0, 0x18, 0x0, 0x0, 0x30, 0x83])
    udp_timeout_test(fsa, msg)


def signal_handler(sig, frame):
    log_print("检测到 Ctrl+C, 即将退出测试")
    stop_event.set()
    # print_summary("测试结果")
    # log_print("程序已强制退出.")
    # os._exit(0)


if __name__ == "__main__":
    signal.signal(signal.SIGINT, signal_handler)

    msg = "Is any fourier smart server here?"
    boardcast(msg)

    log_print("测试开始, 按 Ctrl+C 可立即停止并打印当前状态.")

    threads = []

    for fsa in fsa_v2_list:
        fsa.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        fsa.sock.settimeout(fsa.timeout)
        fsa.addr = (fsa.ip, 2335)
        if enable_v2(fsa) == -1:
            log_print(f"# ---------- V2 {fsa.ip} 使能失败 ---------- #")
            continue
        t = threading.Thread(target=get_pvct_v2, args=(fsa,), daemon=True)
        t.start()
        threads.append(t)

    for fsa in fsa_v3_list:
        fsa.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        fsa.sock.settimeout(fsa.timeout)
        fsa.addr = (fsa.ip, 2340)
        if enable_v3(fsa) == -1:
            log_print(f"# ---------- V3 {fsa.ip} 使能失败 ---------- #")
            continue
        t = threading.Thread(target=get_pvct_v3, args=(fsa,), daemon=True)
        t.start()
        threads.append(t)

    try:
        while any(t.is_alive() for t in threads):
            time.sleep(1)
    except KeyboardInterrupt:
        signal_handler(None, None)
    finally:
        for fsa in fsa_v2_list:
            if disable_v2(fsa) == -1:
                log_print(f"# ---------- V2 {fsa.ip} 失能失败 ---------- #")
            fsa.sock.close()

        for fsa in fsa_v3_list:
            if disable_v3(fsa) == -1:
                log_print(f"# ---------- V3 {fsa.ip} 失能失败 ---------- #")
            fsa.sock.close()

        stop_event.set()
        print_summary("测试结果")
        log_print("测试结束, 程序退出.")
        sys.exit(0)
