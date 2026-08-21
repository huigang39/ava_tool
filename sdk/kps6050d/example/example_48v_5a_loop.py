import math
import signal
import threading
import time
import serial
import serial.tools.list_ports
from dataclasses import dataclass


@dataclass
class Kps6050dState:
    voltage: float = 0.0
    current: float = 0.0
    set_voltage: float = 0.0
    set_current: float = 0.0
    max_voltage: float = 0.0
    max_current: float = 0.0
    output_on: int = 0
    ocp_on: int = 0
    remote_locked: int = 0
    constant_current: int = 0
    alarm: int = 0


class Kps6050d:
    def __init__(self):
        self._port = None
        self._id = 0
        self._error_code = 0
        self._error = "OK"
        self._state = Kps6050dState()
        self._big_endian = 0
        self._lock = threading.Lock()

    def _set_error(self, code, text):
        self._error_code = code
        self._error = text
        return code

    def _set_ok(self):
        self._error_code = 0
        self._error = "OK"
        return 0

    @staticmethod
    def _crc16(data):
        crc = 0xFFFF
        for b in data:
            crc ^= b
            for _ in range(8):
                crc = ((crc >> 1) ^ 0xA001) if (crc & 1) else (crc >> 1)
        return crc & 0xFFFF

    @classmethod
    def _add_crc(cls, data):
        crc = cls._crc16(data)
        data.append(crc & 0xFF)
        data.append((crc >> 8) & 0xFF)

    @staticmethod
    def _swap16(v):
        return ((v << 8) | (v >> 8)) & 0xFFFF

    def _port_is_open(self):
        return self._port is not None and self._port.is_open

    def _port_close(self):
        if self._port_is_open():
            self._port.close()
        self._port = None

    def _transact(self, tx, rx_len):
        if not self._port_is_open():
            return self._set_error(-4, "serial port is not open"), None
        try:
            self._port.reset_input_buffer()
            self._port.reset_output_buffer()
            self._port.write(bytes(tx))
            self._port.flush()
        except serial.SerialException:
            return self._set_error(-5, "serial write failed"), None

        try:
            rx = self._port.read(rx_len)
        except serial.SerialException:
            return self._set_error(-7, "serial read failed"), None

        if len(rx) != rx_len:
            return self._set_error(-6, "serial response timeout"), None

        expected_crc = rx[-2] | (rx[-1] << 8)
        if self._crc16(rx[:-2]) != expected_crc:
            return self._set_error(-8, "Modbus CRC mismatch"), None

        if rx[0] != self._id:
            return self._set_error(-7, "unexpected device ID"), None

        if rx[1] & 0x80:
            return self._set_error(-9, "Modbus exception response"), None

        return 0, rx

    def _read_unlocked(self):
        tx = [self._id, 3, 0, 0, 0, 8]
        self._add_crc(tx)

        rc, rx = self._transact(tx, 21)
        if rc:
            return rc, None

        if rx[1] != 3 or rx[2] != 16:
            return self._set_error(-7, "invalid read response"), None

        r = []
        for i in range(8):
            value = (rx[3 + i * 2] << 8) | rx[4 + i * 2]
            r.append(value)

        status = (r[0] >> 8) & 0xFF
        big = 1 if (status & 8) else 0
        self._big_endian = big

        def reg(v):
            return v if big else self._swap16(v)

        vs = 0.1 if (r[0] & 0x80) else 0.01
        cs = 0.01

        state = Kps6050dState()
        state.output_on = 1 if (status & 1) else 0
        state.ocp_on = 1 if (status & 2) else 0
        state.remote_locked = 1 if (status & 4) else 0
        state.constant_current = 1 if (status & 0x10) else 0
        state.alarm = 1 if (status & 0x20) else 0
        state.voltage = reg(r[2]) * vs
        state.current = reg(r[3]) * cs
        state.set_voltage = reg(r[4]) * vs
        state.set_current = reg(r[5]) * cs
        state.max_voltage = reg(r[6]) * vs
        state.max_current = reg(r[7]) * cs

        self._state = state
        self._set_ok()
        return 0, state

    def _write_unlocked(self, state):
        flags = (
            (1 if state.output_on else 0)
            | (2 if state.ocp_on else 0)
            | (4 if state.remote_locked else 0)
            | (8 if self._big_endian else 0)
        )

        v = [
            (flags << 8) & 0xFFFF,
            round(state.set_voltage * 100.0) & 0xFFFF,
            round(state.set_current * 100.0) & 0xFFFF,
        ]

        if not self._big_endian:
            v[1] = self._swap16(v[1])
            v[2] = self._swap16(v[2])

        tx = [self._id, 0x10, 0, 0, 0, 3, 6]

        for value in v:
            tx.append((value >> 8) & 0xFF)
            tx.append(value & 0xFF)

        self._add_crc(tx)

        rc, rx = self._transact(tx, 8)
        if rc:
            return rc

        if rx[1] != 0x10 or rx[5] != 3:
            return self._set_error(-7, "invalid write response")

        self._state = state
        return self._set_ok()

    def _update_unlocked(self, field, value=0, fvalue=0.0):
        rc, state = self._read_unlocked()
        if rc:
            return rc

        if field == 0:
            state.set_voltage = fvalue
        elif field == 1:
            state.set_current = fvalue
        elif field == 2:
            state.output_on = 1 if value else 0
        elif field == 3:
            state.ocp_on = 1 if value else 0
        else:
            state.remote_locked = 1 if value else 0

        return self._write_unlocked(state)

    def open(self, port, baud_rate, device_id):
        with self._lock:
            if (
                not port
                or device_id < 0
                or device_id > 31
                or baud_rate not in (2400, 4800, 9600, 19200)
            ):
                return self._set_error(-1, "invalid connection settings")

            self._port_close()

            try:
                self._port = serial.Serial(
                    port=port,
                    baudrate=baud_rate,
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_NONE,
                    stopbits=serial.STOPBITS_ONE,
                    timeout=0.5,
                    write_timeout=0.5,
                )
            except serial.SerialException:
                self._port = None
                return self._set_error(-2, "cannot open RS232 port")

            self._id = device_id

            rc, state = self._read_unlocked()
            if rc:
                self._port_close()
                return rc

            self._state = state
            return rc

    def close(self):
        with self._lock:
            self._port_close()
            return self._set_ok()

    def is_open(self):
        with self._lock:
            return 1 if self._port_is_open() else 0

    def read(self):
        with self._lock:
            return self._read_unlocked()

    def get_voltage(self):
        with self._lock:
            rc, state = self._read_unlocked()
            return -1.0 if rc else state.voltage

    def get_current(self):
        with self._lock:
            rc, state = self._read_unlocked()
            return -1.0 if rc else state.current

    def set_voltage(self, voltage):
        with self._lock:
            if not math.isfinite(voltage) or voltage < 0 or voltage > 60:
                return self._set_error(-1, "voltage must be 0..60 V")
            return self._update_unlocked(0, fvalue=voltage)

    def set_current(self, current):
        with self._lock:
            if not math.isfinite(current) or current < 0 or current > 50:
                return self._set_error(-1, "current must be 0..50 A")
            return self._update_unlocked(1, fvalue=current)

    def set_output(self, enabled):
        with self._lock:
            return self._update_unlocked(2, value=enabled)

    def set_ocp(self, enabled):
        with self._lock:
            return self._update_unlocked(3, value=enabled)

    def set_remote_lock(self, enabled):
        with self._lock:
            return self._update_unlocked(4, value=enabled)

    def last_error_code(self):
        return self._error_code

    def last_error(self):
        return self._error


g_stop = False


def on_signal(sig, frame):
    global g_stop
    g_stop = True


def check(psu, rc, op):
    if rc == 0:
        return True
    print(f"{op} failed: {psu.last_error_code()}, {psu.last_error()}")
    return False


def monitor(psu, phase):
    global g_stop
    for sec in range(1, 11):
        if g_stop:
            break

        rc, state = psu.read()
        if not check(psu, rc, "read"):
            return False

        print(
            f"[{phase} {sec:2d}/10 s] "
            f"actual={state.voltage:.2f} V / {state.current:.3f} A, "
            f"limit={state.set_voltage:.2f} V / {state.set_current:.2f} A, "
            f"mode={'CC' if state.constant_current else 'CV'}"
        )

        time.sleep(1)

    return True


def select_port():
    ports = list(serial.tools.list_ports.comports())

    if ports:
        print("Available COM ports:")
        for i, port in enumerate(ports):
            print(f"{i}: {port.device}  {port.description}")

        while True:
            value = input(
                "Select COM port number, or enter COM name directly: "
            ).strip()

            if value.isdigit():
                index = int(value)
                if 0 <= index < len(ports):
                    return ports[index].device
            elif value:
                return value

            print("Invalid selection.")
    else:
        return input("Enter COM port, e.g. COM7: ").strip()


def main():
    global g_stop

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    port = select_port()

    baud_text = input("Baud rate [2400]: ").strip()
    baud = int(baud_text) if baud_text else 2400

    id_text = input("Device ID [0]: ").strip()
    device_id = int(id_text) if id_text else 0

    psu = Kps6050d()

    connected = False
    output = False
    result = 1

    print(f"Connecting on {port}, {baud} baud, ID {device_id}...")

    if not check(psu, psu.open(port, baud, device_id), "connect"):
        return result

    connected = True

    if (
        not check(psu, psu.set_output(0), "disable output")
        or not check(psu, psu.set_voltage(48.0), "set voltage")
        or not check(psu, psu.set_current(5.0), "set current")
    ):
        if connected:
            psu.close()
        return result

    print("Cycling: ON 10 s, OFF 10 s. Press Ctrl+C to stop.")

    try:
        while not g_stop:
            if not check(psu, psu.set_output(1), "enable output"):
                break

            output = True

            if not monitor(psu, "ON "):
                break

            if not check(psu, psu.set_output(0), "disable output"):
                break

            output = False

            if not monitor(psu, "OFF"):
                break
        else:
            result = 0

        if g_stop:
            result = 0

    finally:
        if connected and output:
            psu.set_output(0)

        if connected:
            psu.close()

    return result


if __name__ == "__main__":
    raise SystemExit(main())
