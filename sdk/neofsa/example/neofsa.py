"""neofsa C ABI v2 的 ctypes 封装，仅依赖 Python 标准库。"""
import ctypes as C
from pathlib import Path

U = C.c_uint32
I = C.c_int32
F = C.c_float
Q = C.c_uint64

class Param(C.Structure):
    _fields_ = [("runtime_dll", C.c_char * 1024), ("adapter", C.c_char * 1024),
                ("backend", I), ("axes", U), ("cycle_us", U), ("watchdog_ms", U),
                ("master_action", I), ("master_running", U), ("capabilities", U), ("online", U)]

class Adapter(C.Structure):
    _fields_ = [("runtime_dll", C.c_char * 1024), ("backend", I), ("index", U),
                ("count", U), ("name", C.c_char * 1024)]

class Target(C.Structure):
    _fields_ = [("mode", I), ("position", F), ("velocity", F), ("current", F), ("torque", F)]

class Feedback(C.Structure):
    _fields_ = [("flags", U), ("error_code", U), ("mode", I), ("special_cmd", U),
                ("position", F), ("velocity", F), ("current", F), ("torque", F),
                ("electromagnetic_torque", F), ("vbus", F), ("motor_temperature", F),
                ("drive_temperature", F), ("timestamp_ns", Q), ("accepted_sequence", Q), ("applied_sequence", Q)]

class Realtime(C.Structure):
    _fields_ = [("axis", U), ("write_target", U), ("action", I), ("target", Target), ("feedback", Feedback)]

class PdParam(C.Structure):
    _fields_ = [("axis", U), ("kp", F), ("kd", F), ("state", U), ("result", U),
                ("sequence", Q), ("sent_kp", F), ("sent_kd", F), ("message", C.c_char * 512)]


class NeoFsa:
    def __init__(self, dll=None):
        self.dll = C.CDLL(str(Path(dll or Path(__file__).resolve().parents[1] / "lib/win/neofsa_c.dll").resolve()))
        for name, typ in (("read_param", Param), ("write_param", Param),
                          ("read_pd_param", PdParam), ("write_pd_param", PdParam),
                          ("read_adapter_param", Adapter), ("read_rt", Realtime), ("write_rt", Realtime)):
            fn = getattr(self.dll, "neofsa_" + name)
            fn.argtypes = [C.POINTER(typ)]
            fn.restype = C.c_int
        self.dll.neofsa_last_error.argtypes = []
        self.dll.neofsa_last_error.restype = C.c_char_p
        self.dll.neofsa_shutdown.argtypes = []
        self.dll.neofsa_shutdown.restype = C.c_int

    def call(self, name, value):
        code = getattr(self.dll, "neofsa_" + name)(C.byref(value))
        if code:
            message = self.dll.neofsa_last_error().decode("utf-8", errors="replace")
            raise RuntimeError(f"{name} 失败 ({code})：{message}")
        return value

    def close(self):
        code = self.dll.neofsa_shutdown()
        if code:
            raise RuntimeError(self.dll.neofsa_last_error().decode("utf-8", errors="replace"))
