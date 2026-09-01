"""将 Npcap 路径按 GUID 精确匹配到 Windows 网卡名称，不调用 PowerShell。"""
import ctypes as C
import os
import re


class _Addresses(C.Structure):
    pass


_Addresses._fields_ = [
    ("alignment", C.c_uint64),  # Length + IfIndex union
    ("next", C.POINTER(_Addresses)),
    ("adapter_name", C.c_char_p),
    ("first_unicast", C.c_void_p), ("first_anycast", C.c_void_p),
    ("first_multicast", C.c_void_p), ("first_dns", C.c_void_p),
    ("dns_suffix", C.c_wchar_p), ("description", C.c_wchar_p), ("friendly_name", C.c_wchar_p),
    ("physical_address", C.c_ubyte * 8), ("physical_address_length", C.c_uint32),
    ("flags", C.c_uint32), ("mtu", C.c_uint32), ("if_type", C.c_uint32),
    ("oper_status", C.c_uint32),
]


def adapter_id(path):
    match = re.search(r"\{([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\}", path, re.I)
    return match.group(1).lower() if match else None


def windows_adapters():
    if os.name != "nt":
        return {}
    api = C.WinDLL("iphlpapi.dll").GetAdaptersAddresses
    api.argtypes = [C.c_uint32, C.c_uint32, C.c_void_p, C.c_void_p, C.POINTER(C.c_uint32)]
    api.restype = C.c_uint32
    size = C.c_uint32(16384)
    for _ in range(4):
        buffer = C.create_string_buffer(size.value)
        # Skip anycast/multicast/DNS-server addresses, retain friendly names.
        result = api(0, 0x2 | 0x4 | 0x8, None, buffer, C.byref(size))
        if result == 111:  # ERROR_BUFFER_OVERFLOW: adapter list may change between calls.
            continue
        if result == 232:  # ERROR_NO_DATA
            return {}
        if result:
            raise OSError(result, "读取 Windows 网卡名称失败")
        names = {}
        node = C.cast(buffer, C.POINTER(_Addresses))
        while node:
            item = node.contents
            guid = adapter_id((item.adapter_name or b"").decode("ascii", errors="replace"))
            if guid:
                names[guid] = {
                    "name": item.friendly_name or item.description or "未命名网卡",
                    "description": item.description or "",
                    "connected": item.oper_status == 1,
                }
            node = item.next
        return names
    raise OSError("网卡列表持续变化，请重新扫描")


def resolve_names():
    try:
        return windows_adapters()
    except OSError as exc:
        print(f"提示：{exc}；仍显示 Npcap 路径，可继续选择。")
        return {}


def adapter_label(path, names):
    info = names.get(adapter_id(path))
    if not info:
        return "未解析名称 · " + path
    description = info["description"]
    label = info["name"]
    if description and description != label:
        label += " · " + description
    label += " [已连接]" if info["connected"] else " [未连接]"
    return label
