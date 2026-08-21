# FSANeo SOEM master (Windows)

This program runs a cyclic SOEM master and exposes every slave's PDO image via
the `Local\\SOEM_PDO_FREERUN` Windows shared-memory block used by
`pdo_pid_param_test.c` and `ava_ecat.h`.

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 C/C++ workload
- CMake 3.28 or newer
- Npcap installed in WinPcap-compatible mode
- A dedicated wired Ethernet adapter, run from an Administrator terminal

SOEM is vendored at `third_party/SOEM` from the official
OpenEtherCATsociety/SOEM repository.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Run

```powershell
.\build\Release\fsa_soem_master.exe --list-adapters
.\build\Release\fsa_soem_master.exe --adapter "\\Device\\NPF_{GUID}" --cycle-us 1000
```

Stop the daemon with Ctrl+C. In another terminal, run the existing PID test:

```powershell
pdo_pid_param_test.exe read vel_kp
pdo_pid_param_test.exe write vel_kp 2.0
```

The daemon verifies FSANeo identity (`46494946:41434341`) and assigns the
extended process data before mapping:

- RxPDO: `0x1600`, `0x1603` (168 bytes)
- TxPDO: `0x1A00`, `0x1A03` (204 bytes)

The shared memory is deliberately a local-machine transport. Only one daemon
may own the EtherCAT adapter and shared-memory block at a time.

