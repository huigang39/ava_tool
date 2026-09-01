#pragma once
#include "types.h"

#define AVA_ETH_ABI                  1u
#define AVA_ETH_MAX_AXES             64u
#define AVA_ETH_CAP_SIM              1u
#define AVA_ETH_CAP_HARDWARE         2u
#define AVA_ETH_CAP_INTERPOLATION    4u
#define AVA_ETH_CAP_SPECIAL_CMD      8u
#define AVA_ETH_ONLINE               1u
#define AVA_ETH_FEEDBACK_VALID       2u
#define AVA_ETH_ENABLED              4u
#define AVA_ETH_FAULT                8u
#define AVA_ETH_COMMAND_EXPIRED      16u
#define AVA_ETH_TEMPERATURE_VALID    32u
#define AVA_ETH_SIMULATED            64u
#define AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID 128u
// Controller errors, distinct from the slave's standard 0x603F error object
#define AVA_ETH_ERR_RX_TIMEOUT       0x80000001u
#define AVA_ETH_ERR_WKC              0x80000002u
#define AVA_ETH_ERR_AL               0x80000003u
#define AVA_ETH_ERR_SEND             0x80000004u
#define AVA_ETH_ERR_DATA             0x80000005u
#define AVA_ETH_ERR_DRIVE_FAULT      0x80000008u

#define AVA_ETH_CONTROLLER_GUID_INIT {0x350a986b, 0xb8cf, 0x49be, {0x86, 0xe1, 0xe5, 0x5e, 0xdb, 0x43, 0x72, 0x12}}
#define AVA_ETH_CTL(n)               ((0x8001u << 16) | (3u << 14) | ((n) << 2))
#define AVA_ETH_IOCTL_INFO           AVA_ETH_CTL(0x800u)
#define AVA_ETH_IOCTL_INTERPOLATION  AVA_ETH_CTL(0x808u)
#define AVA_ETH_IOCTL_CONFIG         AVA_ETH_CTL(0x801u)
#define AVA_ETH_IOCTL_TARGETS        AVA_ETH_CTL(0x802u)
#define AVA_ETH_IOCTL_ACTION         AVA_ETH_CTL(0x803u)
#define AVA_ETH_IOCTL_FEEDBACK       AVA_ETH_CTL(0x804u)
#define AVA_ETH_IOCTL_SIM_LINK       AVA_ETH_CTL(0x805u)
#define AVA_ETH_IOCTL_DIAGNOSTICS    AVA_ETH_CTL(0x807u)
#define AVA_ETH_IOCTL_SIM_FAULT      AVA_ETH_CTL(0x806u)
#define AVA_ETH_ENABLE               1u
#define AVA_ETH_DISABLE              2u
#define AVA_ETH_RESET_FAULT          3u
#define AVA_ETH_KEEPALIVE            4u
#define AVA_ETH_STOP                 5u
// FocDriver cia402_special_cmd, carried by PDO object 0x2080
#define AVA_ETH_SPECIAL_BASE         0x100u
#define AVA_ETH_SPECIAL_NONE         0u
#define AVA_ETH_SPECIAL_DISABLE      1u
#define AVA_ETH_SPECIAL_ENABLE       2u
#define AVA_ETH_SPECIAL_CALI         3u
#define AVA_ETH_SPECIAL_SENSORLESS   4u
#define AVA_ETH_SPECIAL_OUT_ZERO     5u
#define AVA_ETH_SPECIAL_CLEAR_FAULT  6u
#define AVA_ETH_SPECIAL_TORQUE_ZERO  7u

enum AvaEthResult {
        AVA_ETH_OK,
        AVA_ETH_INVALID,
        AVA_ETH_UNSUPPORTED,
        AVA_ETH_STATE,
        AVA_ETH_DEVICE_FAULT,
        AVA_ETH_OFFLINE,
        AVA_ETH_EXPIRED,
        AVA_ETH_ACCESS,
        AVA_ETH_IO,
        AVA_ETH_MEMORY,
        AVA_ETH_BUSY
};

#pragma pack(push, 8)

struct AvaEthConfig {
        uint32_t version, size, backend, axes, period_us, watchdog_ms, reserved[2];
};

struct AvaEthInfo {
        uint32_t version, size, capabilities, axes, period_us, watchdog_ms, online, reserved;
};

struct AvaEthTarget {
        uint32_t axis;
        int32_t  mode;
        float    position, velocity, current, torque;
};

// Per-axis CSP/CSPC trapezoidal position trajectory. Configure only while disabled
struct AvaEthInterpolation {
        uint32_t version, size, axis, enabled;
        float    max_velocity, max_acceleration;
};

struct AvaEthTargets {
        uint32_t            version, size, count, reserved;
        struct AvaEthTarget targets[AVA_ETH_MAX_AXES];
};

struct AvaEthAction {
        uint32_t version, size, axis, action;
};

struct AvaEthReply {
        uint32_t result, reserved;
        uint64_t sequence;
};

struct AvaEthFeedback {
        uint32_t axis, flags, error_code, reserved;
        int32_t  mode;
        uint32_t special_cmd; // Last acknowledged PDO value; requires AVA_ETH_CAP_SPECIAL_CMD
        // torque is the output-shaft LOAD torque from NEO PDO 0x2008, in Nm
        float    position, velocity, current, torque, vbus, motor_temperature, drive_temperature;
        union {
                float electromagnetic_torque; // PDO 0x200A, Nm; requires ELECTROMAGNETIC_TORQUE_VALID
                float reserved3;              // ABI 1 compatibility: same size and offset
        };
        uint64_t timestamp_ns, accepted_sequence, applied_sequence;
};

struct AvaEthSnapshot {
        uint32_t              version, size, count, reserved;
        struct AvaEthFeedback axes[AVA_ETH_MAX_AXES];
};

struct AvaEthDiagnostics {
        uint32_t version, size, status_word, al_status, al_code, wkc, expected_wkc, receive_timeout_us;
        uint32_t error_pages[8]; // Raw PDO objects 0x2030..0x204C, not standard 0x603F
        uint64_t sent, received, receive_probes, timeouts, unmatched, late, bad_wkc, bad_al, max_rtt_ns, missed_periods;
};

struct AvaEthSimFault {
        uint32_t axis, error_code;
};

#pragma pack(pop)
