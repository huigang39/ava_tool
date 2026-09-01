#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PD 模式正弦运动后失能，重启执行器并比较重启前后的角度。

流程：
1. 自动扫描执行器（也可用 --ip 指定）。
2. 进入 PD 模式，在 ±180° 内执行正弦轨迹。
3. 随机运行 1~3 秒后失能。
4. 等待机构停止，读取并记录角度。
5. 重启执行器，重新上线后读取角度。
6. 比较角度误差并检查阈值。

仅使用 Python 标准库，直接实现 FSA v3 的 UDP 协议。
"""

from __future__ import annotations

import argparse
import json
import math
import random
import select
import socket
import struct
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from typing import Iterable, List, Optional, Sequence, Tuple

COMM_PORT = 2334
RMAIO_PORT = 2340
UDP_BUFFER_SIZE = 1400

RMAIO_REAL_TIME_BASE = 1024
RMAIO_CONTROL_MODE = RMAIO_REAL_TIME_BASE + 0
RMAIO_CONTROL_WORD = RMAIO_REAL_TIME_BASE + 1
RMAIO_SET_POSITION = RMAIO_REAL_TIME_BASE + 10
RMAIO_SET_VELOCITY = RMAIO_REAL_TIME_BASE + 11
RMAIO_SET_TORQUE_FF = RMAIO_REAL_TIME_BASE + 16
RMAIO_GET_POSITION = RMAIO_REAL_TIME_BASE + 25

# 固件转矩 PD 模式。
PD_CONTROL_MODE = 7
SERVO_ON = 0xF002
SERVO_OFF = 0xF001

RMAIO_READ = 0
RMAIO_WRITE_ONLY = 1
RMAIO_WRITE_FEEDBACK = 2
RMAIO_ILLEGAL_ACCESS = 3
ATOMIC_WAIT_50US = 3

DEFAULT_SCAN_TIMEOUT = 3.0
DEFAULT_REBOOT_TIMEOUT = 30.0
DEFAULT_THRESHOLD_DEG = 2.0
DEFAULT_AMPLITUDE_DEG = 160.0
DEFAULT_FREQUENCY_HZ = 0.25
POSITION_RAMP_SECONDS = 1.0
REBOOT_INITIALIZE_SECONDS = 3.0


class FsaError(RuntimeError):
    pass


class ProtocolError(FsaError):
    pass


class ResponseTimeout(FsaError):
    pass


class ThresholdExceeded(FsaError):
    pass


def log(message: str, error: bool = False, success: bool = False) -> None:
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")
    level = "ERROR " if error else ""
    stream = sys.stderr if error else sys.stdout
    color = ""
    reset = ""
    if stream.isatty():
        if error:
            color = "\033[31m"
        elif success:
            color = "\033[32m"
        reset = "\033[0m" if color else ""
    print(
        f"{color}[{timestamp}] {level}{message}{reset}",
        file=stream,
        flush=True,
    )


@dataclass(frozen=True)
class DeviceInfo:
    ip: str
    name: str = ""
    device_type: str = ""
    firmware: str = ""
    serial_number: str = ""


@dataclass(frozen=True)
class RmaChunk:
    command: int
    address: int
    word_count: int
    words: Tuple[int, ...] = ()
    feedback: int = 0


@dataclass(frozen=True)
class RmaResponse:
    counter: int
    chunks: Tuple[RmaChunk, ...]


def _frame_head(counter: int, direct_write: bool, no_response: bool) -> bytes:
    value = (
        (ATOMIC_WAIT_50US << 11)
        | (int(direct_write) << 14)
        | (int(no_response) << 15)
        | ((counter & 0xFFFF) << 16)
    )
    return struct.pack("<I", value)


def _data_head(command: int, address: int, word_count: int = 1) -> bytes:
    if not 1 <= word_count <= 8:
        raise ValueError("RMAIO word_count 必须在 1..8 范围内")
    if not 0 <= address < 2048:
        raise ValueError("RMAIO 地址必须在 0..2047 范围内")
    value = (command & 0x3) | ((word_count - 1) & 0x7) << 2 | ((address & 0x7FF) << 5)
    return struct.pack("<H", value)


def _decode_response(payload: bytes) -> RmaResponse:
    if len(payload) < 4:
        raise ProtocolError("RMAIO 响应帧头不完整")

    frame = struct.unpack_from("<I", payload, 0)[0]
    version = frame & 0xF
    frame_type = (frame >> 4) & 0xF
    encrypt = (frame >> 8) & 0x7
    counter = (frame >> 16) & 0xFFFF
    if version != 0 or frame_type != 0 or encrypt != 0:
        raise ProtocolError("RMAIO 响应帧头无效")

    chunks: List[RmaChunk] = []
    offset = 4
    while offset < len(payload):
        if len(payload) - offset < 2:
            raise ProtocolError("RMAIO 响应数据头不完整")
        head = struct.unpack_from("<H", payload, offset)[0]
        offset += 2

        command = head & 0x3
        word_count = ((head >> 2) & 0x7) + 1
        address = (head >> 5) & 0x7FF

        if command == RMAIO_READ:
            size = word_count * 4
            if len(payload) - offset < size:
                raise ProtocolError("RMAIO 读响应数据不完整")
            words = struct.unpack_from("<" + ("I" * word_count), payload, offset)
            offset += size
            chunks.append(RmaChunk(command, address, word_count, words=tuple(words)))
        elif command in (RMAIO_WRITE_FEEDBACK, RMAIO_ILLEGAL_ACCESS):
            if len(payload) - offset < 4:
                raise ProtocolError("RMAIO 写反馈不完整")
            feedback = struct.unpack_from("<I", payload, offset)[0]
            offset += 4
            chunks.append(
                RmaChunk(
                    command,
                    address,
                    word_count,
                    feedback=feedback,
                )
            )
        else:
            raise ProtocolError(f"RMAIO 响应命令无效：{command}")

    if not chunks:
        raise ProtocolError("RMAIO 响应没有数据")
    return RmaResponse(counter, tuple(chunks))


def _float_to_u32(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", float(value)))[0]


def _u32_to_float(value: int) -> float:
    return struct.unpack("<f", struct.pack("<I", value & 0xFFFFFFFF))[0]


def _normalize_angle(angle_rad: float) -> float:
    return math.atan2(math.sin(angle_rad), math.cos(angle_rad))


def _wrapped_difference(start_rad: float, end_rad: float) -> float:
    return _normalize_angle(end_rad - start_rad)


class FsaClient:
    def __init__(self, ip: str, timeout_seconds: float = 0.25, retries: int = 2):
        self.ip = ip
        self.timeout_seconds = timeout_seconds
        self.retries = retries
        self.counter = 0
        self.sock: Optional[socket.socket] = None
        self._connect()

    def _connect(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(self.timeout_seconds)
        sock.connect((self.ip, RMAIO_PORT))
        self.sock = sock

    def reconnect(self) -> None:
        if self.sock is not None:
            self.sock.close()
        self._connect()

    def close(self) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def _next_counter(self) -> int:
        value = self.counter
        self.counter = (self.counter + 1) & 0xFFFF
        return value

    def _drain(self) -> None:
        if self.sock is None:
            return
        self.sock.setblocking(False)
        try:
            while True:
                try:
                    self.sock.recv(UDP_BUFFER_SIZE)
                except (BlockingIOError, socket.timeout, OSError):
                    break
        finally:
            if self.sock is not None:
                self.sock.settimeout(self.timeout_seconds)

    def _exchange(self, request: bytes, counter: int) -> RmaResponse:
        last_error: Optional[Exception] = None
        for _ in range(self.retries + 1):
            try:
                if self.sock is None:
                    self._connect()
                assert self.sock is not None
                self._drain()
                self.sock.sendall(request)
                payload = self.sock.recv(UDP_BUFFER_SIZE)
                response = _decode_response(payload)
                if response.counter != counter:
                    raise ProtocolError(
                        f"RMAIO cnt 不匹配：{counter} != {response.counter}"
                    )
                return response
            except socket.timeout:
                last_error = ResponseTimeout("等待 RMAIO 响应超时")
            except ProtocolError as exc:
                last_error = exc
            except OSError as exc:
                last_error = FsaError(f"RMAIO 通信错误：{exc}")
                try:
                    self.reconnect()
                except OSError:
                    pass
        assert last_error is not None
        raise last_error

    def read_words(self, address: int, count: int) -> List[int]:
        counter = self._next_counter()
        request = _frame_head(counter, False, False) + _data_head(
            RMAIO_READ, address, count
        )
        response = self._exchange(request, counter)
        values = {}
        for chunk in response.chunks:
            if chunk.command == RMAIO_ILLEGAL_ACCESS:
                raise ProtocolError(f"非法读取地址：0x{chunk.address:04X}")
            if chunk.command != RMAIO_READ:
                raise ProtocolError("读请求收到非读响应")
            for index, value in enumerate(chunk.words):
                values[chunk.address + index] = value

        try:
            return [values[address + index] for index in range(count)]
        except KeyError as exc:
            raise ProtocolError(f"读响应缺少地址：0x{int(exc.args[0]):04X}") from exc

    def read_position(self) -> float:
        value = self.read_words(RMAIO_GET_POSITION, 1)[0]
        position = _u32_to_float(value)
        if not math.isfinite(position):
            raise ProtocolError("位置反馈不是有限数")
        return position

    def write_feedback(self, operations: Iterable[Tuple[int, int]]) -> None:
        operations = list(operations)
        counter = self._next_counter()
        request = [
            _frame_head(counter, True, False),
        ]
        addresses = []
        for address, value in operations:
            request.append(_data_head(RMAIO_WRITE_FEEDBACK, address, 1))
            request.append(struct.pack("<I", value & 0xFFFFFFFF))
            addresses.append(address)

        response = self._exchange(b"".join(request), counter)
        statuses = {}
        for chunk in response.chunks:
            if chunk.command == RMAIO_READ:
                raise ProtocolError("写请求收到读响应")
            if chunk.command not in (RMAIO_WRITE_FEEDBACK, RMAIO_ILLEGAL_ACCESS):
                raise ProtocolError("写请求收到无效响应")
            for index in range(chunk.word_count):
                status = (chunk.feedback >> (index * 4)) & 0xF
                if status != 0xF:
                    statuses[chunk.address + index] = (
                        chunk.command,
                        status,
                    )

        for address in addresses:
            command, status = statuses.get(address, (RMAIO_ILLEGAL_ACCESS, 0xF))
            if command == RMAIO_ILLEGAL_ACCESS:
                raise ProtocolError(f"RMAIO 拒绝写入：0x{address:04X}")
            if status != 0:
                raise ProtocolError(
                    f"RMAIO 写入失败：addr=0x{address:04X}, status={status:X}"
                )

    def write_no_ack(self, operations: Iterable[Tuple[int, int]]) -> None:
        counter = self._next_counter()
        request = [_frame_head(counter, True, True)]
        for address, value in operations:
            request.append(_data_head(RMAIO_WRITE_ONLY, address, 1))
            request.append(struct.pack("<I", value & 0xFFFFFFFF))
        if self.sock is None:
            self._connect()
        assert self.sock is not None
        self.sock.sendall(b"".join(request))

    def enable_pd_mode(self, current_position: float) -> None:
        self.write_feedback(
            (
                (RMAIO_SET_POSITION, _float_to_u32(current_position)),
                (RMAIO_SET_VELOCITY, _float_to_u32(0.0)),
                (RMAIO_SET_TORQUE_FF, _float_to_u32(0.0)),
                (RMAIO_CONTROL_MODE, PD_CONTROL_MODE),
                (RMAIO_CONTROL_WORD, SERVO_ON),
            )
        )
        mode = self.read_words(RMAIO_CONTROL_MODE, 1)[0]
        if mode != PD_CONTROL_MODE:
            raise ProtocolError(
                f"执行器未进入 PD 模式：expected={PD_CONTROL_MODE}, actual={mode}"
            )

    def send_pd_target(self, position: float, velocity: float) -> None:
        self.write_feedback(
            (
                (RMAIO_SET_POSITION, _float_to_u32(position)),
                (RMAIO_SET_VELOCITY, _float_to_u32(velocity)),
                (RMAIO_SET_TORQUE_FF, _float_to_u32(0.0)),
            )
        )

    def send_pd_target_no_ack(self, position: float, velocity: float) -> None:
        self.write_no_ack(
            (
                (RMAIO_SET_POSITION, _float_to_u32(position)),
                (RMAIO_SET_VELOCITY, _float_to_u32(velocity)),
                (RMAIO_SET_TORQUE_FF, _float_to_u32(0.0)),
            )
        )

    def servo_off(self) -> None:
        self.write_feedback(((RMAIO_CONTROL_WORD, SERVO_OFF),))


def _local_ipv4_addresses() -> List[str]:
    addresses = set()
    try:
        for item in socket.getaddrinfo(
            socket.gethostname(), None, socket.AF_INET, socket.SOCK_DGRAM
        ):
            address = item[4][0]
            if address and address != "0.0.0.0" and not address.startswith("127."):
                addresses.add(address)
    except OSError:
        pass

    for target in ("192.0.2.1", "8.8.8.8"):
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.connect((target, 9))
            address = probe.getsockname()[0]
            if address and address != "0.0.0.0" and not address.startswith("127."):
                addresses.add(address)
        except OSError:
            pass
        finally:
            probe.close()
    return sorted(addresses, key=socket.inet_aton)


def _scan_targets(extra: Sequence[str]) -> List[str]:
    targets = {"255.255.255.255", *extra}
    for address in _local_ipv4_addresses():
        parts = address.split(".")
        if len(parts) == 4:
            targets.add(".".join(parts[:3] + ["255"]))
    return sorted(targets, key=socket.inet_aton)


def _discovery_request() -> bytes:
    request = {
        "method": "GET",
        "reqTarget": "/custom",
        "cnt": "00000",
        "name": True,
        "type": True,
        "mcu_fw_version": True,
        "sn": True,
    }
    return json.dumps(request, separators=(",", ":")).encode("utf-8")


def _parse_device(payload: bytes, source_ip: str) -> Optional[DeviceInfo]:
    try:
        data = json.loads(payload.decode("utf-8").rstrip("\x00"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(data, dict):
        return None
    if data.get("method") != "GET" or data.get("reqTarget") != "/custom":
        return None
    if not any(data.get(key) for key in ("name", "type", "mcu_fw_version", "sn")):
        return None
    return DeviceInfo(
        ip=source_ip,
        name=str(data.get("name", "")),
        device_type=str(data.get("type", "")),
        firmware=str(data.get("mcu_fw_version", "")),
        serial_number=str(data.get("sn", "")),
    )


def discover_devices(
    extra_targets: Sequence[str],
    timeout_seconds: float = DEFAULT_SCAN_TIMEOUT,
) -> List[DeviceInfo]:
    targets = _scan_targets(extra_targets)
    request = _discovery_request()
    devices = {}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("", 0))
        sock.setblocking(False)
        deadline = time.monotonic() + timeout_seconds
        next_send = 0.0

        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            if now >= next_send:
                for target in targets:
                    try:
                        sock.sendto(request, (target, COMM_PORT))
                    except OSError:
                        pass
                next_send = now + 0.35

            readable, _, _ = select.select([sock], [], [], min(0.05, deadline - now))
            if not readable:
                continue
            while True:
                try:
                    payload, address = sock.recvfrom(UDP_BUFFER_SIZE)
                except (BlockingIOError, OSError):
                    break
                device = _parse_device(payload, address[0])
                if device is not None:
                    devices[device.ip] = device
    finally:
        sock.close()

    return sorted(devices.values(), key=lambda item: socket.inet_aton(item.ip))


def choose_device(
    ip: Optional[str],
    device_index: Optional[int],
    extra_targets: Sequence[str],
) -> str:
    if ip is not None:
        return ip

    log("正在扫描执行器...")
    devices = discover_devices(extra_targets)
    if not devices:
        raise FsaError("未发现执行器，请检查网线/同网段或使用 --ip")

    if len(devices) == 1:
        log(f"发现执行器：{devices[0].ip}")
    else:
        log(f"发现 {len(devices)} 台执行器：")
        for index, device in enumerate(devices, 1):
            log(f"  [{index}] {device.ip}")

    if device_index is not None:
        if not 1 <= device_index <= len(devices):
            raise FsaError(f"--device-index 超出范围：1..{len(devices)}")
        return devices[device_index - 1].ip
    if len(devices) == 1:
        return devices[0].ip

    try:
        choice = int(input(f"选择执行器序号 [1-{len(devices)}]: ").strip())
    except (EOFError, ValueError):
        raise FsaError("发现多台执行器，请使用 --device-index") from None
    if not 1 <= choice <= len(devices):
        raise FsaError(f"执行器序号超出范围：{choice}")
    return devices[choice - 1].ip


def request_reboot(ip: str, timeout_seconds: float = 0.25) -> str:
    request = b'{"method": "SET","reqTarget": "/reboot"}'
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.settimeout(timeout_seconds)
        sock.connect((ip, COMM_PORT))
        sock.sendall(request)
        payload = sock.recv(UDP_BUFFER_SIZE)
        if not payload:
            raise FsaError("重启请求没有收到响应")
        return payload.decode("utf-8", errors="replace").rstrip("\x00")
    finally:
        sock.close()


def wait_for_reboot(
    client: FsaClient,
    timeout_seconds: float = DEFAULT_REBOOT_TIMEOUT,
) -> float:
    deadline = time.monotonic() + timeout_seconds
    observed_down = False
    successes = 0

    while time.monotonic() < deadline:
        try:
            position = client.read_position()
            successes += 1
            if observed_down and successes >= 2:
                return position
        except FsaError:
            observed_down = True
            successes = 0
            try:
                client.reconnect()
            except OSError:
                pass
        time.sleep(0.1)

    if not observed_down:
        raise ResponseTimeout("重启超时：没有检测到执行器下线")
    raise ResponseTimeout("重启超时：执行器没有重新上线")


def run_sine_motion(
    client: FsaClient, amplitude_deg: float, frequency_hz: float
) -> float:
    initial_position = client.read_position()
    amplitude_rad = math.radians(amplitude_deg)
    initial_normalized = _normalize_angle(initial_position)
    sine_start = 0.0
    omega = 2.0 * math.pi * frequency_hz

    log(
        f"初始位置：{math.degrees(initial_position):.3f} deg，"
        f"PD 正弦：±{amplitude_deg:.1f} deg / {frequency_hz:.3f} Hz"
    )
    client.enable_pd_mode(initial_position)

    ramp_velocity = (sine_start - initial_normalized) / POSITION_RAMP_SECONDS
    log(
        f"位置斜坡：{math.degrees(initial_normalized):.3f} -> "
        f"{math.degrees(sine_start):.3f} deg / {POSITION_RAMP_SECONDS:.1f} s"
    )
    client.send_pd_target(initial_normalized, ramp_velocity)

    ramp_started = time.monotonic()
    next_command = ramp_started + 0.01
    while True:
        elapsed = time.monotonic() - ramp_started
        if elapsed >= POSITION_RAMP_SECONDS:
            break
        target = initial_normalized + ramp_velocity * elapsed
        client.send_pd_target_no_ack(target, ramp_velocity)
        next_command += 0.01
        remaining = next_command - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)
    client.send_pd_target(sine_start, amplitude_rad * omega)

    run_seconds = random.uniform(1.0, 3.0)
    log(f"随机运行时间：{run_seconds:.3f} s")

    started = time.monotonic()
    next_command = started + 0.01
    while True:
        elapsed = time.monotonic() - started
        if elapsed >= run_seconds:
            break
        phase = omega * elapsed
        target = amplitude_rad * math.sin(phase)
        velocity = amplitude_rad * omega * math.cos(phase)
        client.send_pd_target_no_ack(target, velocity)
        next_command += 0.01
        remaining = next_command - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)

    log("失能执行器")
    client.servo_off()
    time.sleep(1.0)
    return client.read_position()


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="PD 模式正弦运动，随机失能后重启并比较角度"
    )
    parser.add_argument("--ip", help="指定执行器 IP；不填则自动扫描")
    parser.add_argument("--device-index", type=int, help="多台执行器时选择序号")
    parser.add_argument(
        "--scan-broadcast",
        action="append",
        default=[],
        help="额外广播地址，可重复",
    )
    parser.add_argument(
        "--amplitude-deg",
        type=float,
        default=DEFAULT_AMPLITUDE_DEG,
        help="正弦位置幅度，deg",
    )
    parser.add_argument(
        "--frequency-hz",
        type=float,
        default=DEFAULT_FREQUENCY_HZ,
        help="正弦运动频率，Hz",
    )
    parser.add_argument(
        "--threshold-deg",
        type=float,
        default=DEFAULT_THRESHOLD_DEG,
        help="重启前后允许角度误差，deg",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=3,
        help="连续测试次数，全部通过才算成功",
    )
    args = parser.parse_args(argv)

    if not 0 < args.amplitude_deg < 180:
        parser.error("--amplitude-deg 必须在 (0, 180) 范围内")
    if not math.isfinite(args.frequency_hz) or args.frequency_hz <= 0:
        parser.error("--frequency-hz 必须是正数")
    if not math.isfinite(args.threshold_deg) or args.threshold_deg < 0:
        parser.error("--threshold-deg 必须大于等于 0")
    if args.repeat < 1:
        parser.error("--repeat 必须大于等于 1")
    if args.device_index is not None and args.device_index < 1:
        parser.error("--device-index 必须从 1 开始")
    return args


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    client = None
    control_enabled = False

    try:
        ip = choose_device(args.ip, args.device_index, args.scan_broadcast)
        if args.ip is not None:
            log(f"执行器：{ip}")
        client = FsaClient(ip)

        control_enabled = True
        passed_count = 0
        for test_index in range(1, args.repeat + 1):
            log(f"测试 {test_index}/{args.repeat}")
            before_deg = math.degrees(
                run_sine_motion(
                    client,
                    args.amplitude_deg,
                    args.frequency_hz,
                )
            )
            log(f"失能后位置：{before_deg:.6f} deg")
            if abs(before_deg) > 180:
                raise FsaError(f"失能后位置超出 ±180 deg：{before_deg:.6f} deg")

            log("重启执行器")
            request_reboot(ip)
            wait_for_reboot(client)
            time.sleep(REBOOT_INITIALIZE_SECONDS)

            after_deg = math.degrees(client.read_position())
            error_deg = abs(
                math.degrees(
                    _wrapped_difference(
                        math.radians(before_deg),
                        math.radians(after_deg),
                    )
                )
            )
            log(f"重启后位置：{after_deg:.6f} deg")
            log(
                f"角度误差：{error_deg:.6f} deg " f"(阈值 {args.threshold_deg:.6f} deg)"
            )

            if error_deg > args.threshold_deg:
                raise ThresholdExceeded(
                    f"第 {test_index} 次测试角度误差 {error_deg:.6f} deg "
                    f"超过阈值 {args.threshold_deg:.6f} deg"
                )
            passed_count += 1

        log(f"连续 {passed_count} 次测试通过", success=True)
        return 0
    except ThresholdExceeded as exc:
        log(str(exc), error=True)
        return 2
    except (FsaError, OSError) as exc:
        log(str(exc), error=True)
        return 1
    except KeyboardInterrupt:
        log("用户中断", error=True)
        return 130
    finally:
        if client is not None:
            if control_enabled:
                try:
                    client.servo_off()
                except Exception:
                    pass
            client.close()


if __name__ == "__main__":
    raise SystemExit(main())
