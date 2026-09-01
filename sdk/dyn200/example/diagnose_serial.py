"""Windows active-stream diagnostics: reads only, never sends sensor commands.

Print inherited serial settings and byte delivery statistics. Restore DCB,
timeouts and event mask before closing. Close other apps using this COM port.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import statistics
import sys
import time


class DCB(C.Structure):
    _fields_ = [("length", W.DWORD), ("baud", W.DWORD), ("flags", W.DWORD),
                ("reserved", W.WORD), ("xon_limit", W.WORD), ("xoff_limit", W.WORD),
                ("byte_size", W.BYTE), ("parity", W.BYTE), ("stop_bits", W.BYTE),
                ("xon", W.BYTE), ("xoff", W.BYTE), ("error_char", W.BYTE),
                ("eof_char", W.BYTE), ("event_char", W.BYTE), ("reserved1", W.WORD)]


class Timeouts(C.Structure):
    _fields_ = [(name, W.DWORD) for name in
                ("interval", "read_multiplier", "read_constant", "write_multiplier", "write_constant")]


class Comstat(C.Structure):
    _fields_ = [("flags", W.DWORD), ("input_bytes", W.DWORD), ("output_bytes", W.DWORD)]


def crc(data):
    value = 0xffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xa001 if value & 1 else 0)
    return value


def main():
    if sys.platform != "win32":
        raise SystemExit("This diagnostic requires Windows.")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--frame-bytes", type=int, choices=(6, 8), default=6)
    parser.add_argument("--normalize", action="store_true", help="fully initialize binary/no-flow DCB")
    parser.add_argument("--rx-event", action="store_true", help="enable documented EV_RXCHAR/EV_ERR events")
    parser.add_argument("--rx-flag", action="store_true", help="enable EV_RXFLAG event-character delivery")
    parser.add_argument("--event-char", type=int, choices=range(256), metavar="0..255")
    args = parser.parse_args()
    if not 0 < args.seconds <= 60:
        parser.error("seconds must be in (0, 60]")
    if args.baud <= 0:
        parser.error("baud must be positive")
    k = C.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
    k.CreateFileW.restype = W.HANDLE
    for name, struct in (("GetCommState", DCB), ("SetCommState", DCB),
                         ("GetCommTimeouts", Timeouts), ("SetCommTimeouts", Timeouts)):
        getattr(k, name).argtypes = [W.HANDLE, C.POINTER(struct)]
    k.GetCommMask.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
    k.SetCommMask.argtypes = [W.HANDLE, W.DWORD]
    k.ClearCommError.argtypes = [W.HANDLE, C.POINTER(W.DWORD), C.POINTER(Comstat)]
    k.ReadFile.argtypes = [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
    k.CloseHandle.argtypes = [W.HANDLE]

    def checked(ok):
        if not ok:
            raise C.WinError(C.get_last_error())

    handle = k.CreateFileW("\\\\.\\" + args.port, 0xc0000000, 0, None, 3, 0, None)
    if handle == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    original, original_timeouts, original_mask = DCB(), Timeouts(), W.DWORD()
    saved = False
    try:
        original.length = C.sizeof(DCB)
        checked(k.GetCommState(handle, C.byref(original)))
        checked(k.GetCommTimeouts(handle, C.byref(original_timeouts)))
        checked(k.GetCommMask(handle, C.byref(original_mask)))
        saved = True
        print(json.dumps({"port": args.port, "inherited_dcb": dict((n, getattr(original, n)) for n, _ in DCB._fields_),
                          "inherited_timeouts": dict((n, getattr(original_timeouts, n)) for n, _ in Timeouts._fields_),
                          "inherited_mask": original_mask.value}), flush=True)
        configured = DCB.from_buffer_copy(original)
        configured.baud, configured.byte_size, configured.parity, configured.stop_bits = args.baud, 8, 0, 0
        configured.flags = (configured.flags & ~0x333e) | 1  # previous SDK's partial DCB initialization
        if args.normalize:
            configured = DCB()
            configured.length = C.sizeof(DCB)
            configured.baud, configured.byte_size = args.baud, 8
            configured.flags = 0x81  # binary, TXContinueOnXoff; no filtering/handshaking/abort
            configured.xon, configured.xoff = 0x11, 0x13
            configured.xon_limit = configured.xoff_limit = 128
        if args.event_char is not None:
            configured.event_char = args.event_char
        checked(k.SetCommState(handle, C.byref(configured)))
        timeout = Timeouts(0xffffffff, 0xffffffff, 200, 0, 200)
        checked(k.SetCommTimeouts(handle, C.byref(timeout)))
        if args.rx_event or args.rx_flag:
            checked(k.SetCommMask(handle, (0x81 if args.rx_event else 0) | (0x2 if args.rx_flag else 0)))
        print(json.dumps({"configured_flags": configured.flags, "event_char": configured.event_char,
                          "normalize": args.normalize, "rx_event": args.rx_event, "rx_flag": args.rx_flag}), flush=True)
        data = C.create_string_buffer(16384)
        received, error, status = W.DWORD(), W.DWORD(), Comstat()
        sizes, gaps, pending = [], [], bytearray()
        calls = frames = dropped = error_bits = total = 0
        started = last = time.perf_counter()
        while time.perf_counter() - started < args.seconds:
            checked(k.ClearCommError(handle, C.byref(error), C.byref(status)))
            error_bits |= error.value
            checked(k.ReadFile(handle, data, len(data), C.byref(received), None))
            calls += 1
            now = time.perf_counter()
            if received.value:
                sizes.append(received.value)
                gaps.append((now - last) * 1000)
                last = now
                total += received.value
                pending.extend(data.raw[:received.value])
                position = 0
                while len(pending) - position >= args.frame_bytes:
                    frame = pending[position:position + args.frame_bytes]
                    if crc(frame[:-2]) == int.from_bytes(frame[-2:], "little"):
                        frames += 1
                        position += args.frame_bytes
                    else:
                        dropped += 1
                        position += 1
                del pending[:position]
        elapsed = time.perf_counter() - started
        print(json.dumps({"elapsed_s": round(elapsed, 3), "bytes_per_s": round(total / elapsed, 1),
                          "valid_frames_per_s": round(frames / elapsed, 1), "batches_per_s": round(len(sizes) / elapsed, 1),
                          "read_calls": calls, "batch_min": min(sizes, default=0), "batch_max": max(sizes, default=0),
                          "batch_median": statistics.median(sizes) if sizes else 0,
                          "gap_median_ms": round(statistics.median(gaps), 3) if gaps else 0,
                          "gap_max_ms": round(max(gaps, default=0), 3), "discarded_bytes": dropped,
                          "serial_error_bits": error_bits}), flush=True)
    finally:
        if saved:
            for name, value in (("SetCommState", C.byref(original)),
                                ("SetCommTimeouts", C.byref(original_timeouts)), ("SetCommMask", original_mask.value)):
                if not getattr(k, name)(handle, value):
                    print(f"WARNING: {name} restore failed: {C.get_last_error()}", file=sys.stderr)
        k.CloseHandle(handle)


if __name__ == "__main__":
    main()
