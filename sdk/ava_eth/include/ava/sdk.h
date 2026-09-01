#pragma once
#include "controller.h"
#ifdef _WIN32
#ifdef AVA_ETH_SDK_BUILD
#define AVA_ETH_API __declspec(dllexport)
#else
#define AVA_ETH_API __declspec(dllimport)
#endif
#else
#define AVA_ETH_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AvaEthHandleImpl *AvaEthHandle;

enum AvaEthBackend { AVA_ETH_USER_SIM = 1, AVA_ETH_DEVICE_SIM = 2, AVA_ETH_DEVICE_HARDWARE = 3, AVA_ETH_NPCAP = 4 };

#define AVA_ETH_MANUAL_CLOCK    1u
#define AVA_ETH_RELAXED_RECEIVE 2u // Hardware only: 20 ms instead of the strict 5 ms receive budget

struct AvaEthOpenOptions {
        uint32_t    size, backend, axes, period_us, watchdog_ms, flags;
        const char *device_path;
};

struct AvaEthDeviceInfo {
        char     path[1024];
        uint32_t reserved[4];
};

// Runtime gains in CM4 OD order: pd_kp, pd_kd, pos_tor_kp, vel_tor_kp,
// vel_tor_ki, pos_cur_kp, vel_cur_kp, vel_cur_ki. First two use IEEE binary16
struct AvaEthPidParameters { float values[8]; };
enum AvaEthPidState { AVA_ETH_PID_IDLE = 0, AVA_ETH_PID_PENDING = 1, AVA_ETH_PID_SENT = 2, AVA_ETH_PID_FAILED = 3 };
struct AvaEthPidStatus {
        uint32_t size, state, mask, result;
        uint64_t sequence;
        struct AvaEthPidParameters values; // Transmitted precision, not device readback
        char message[512];
};
// Asynchronous online update; keep servicing the existing session/keepalive
// Success queues a request; poll status until SENT/FAILED. Never changes enable/mode
AVA_ETH_API int ava_eth_set_pid(AvaEthHandle, uint32_t axis, uint32_t mask,
                               const struct AvaEthPidParameters *, uint64_t *sequence);
AVA_ETH_API int ava_eth_get_pid_status(AvaEthHandle, struct AvaEthPidStatus *);
// Legacy SDO API returns UNSUPPORTED: existing CM4 scalar callbacks are stubs
AVA_ETH_API int ava_eth_configure_pid(const struct AvaEthOpenOptions *, uint32_t axis, uint32_t mask,
                                     const struct AvaEthPidParameters *expected, struct AvaEthPidParameters *values);
// Disconnect cyclic control first. Single actuator (axis 0), Npcap only on hardware
// Sends selected runtime gains via existing RxPDO with controlword/special_cmd zero,
// then restores the normal mapping in PREOP. No gain SDO, readback, persistence or retry
// Returned values are transmitted precision, NOT measured device values. A failure
// may leave a partial parameter update; error text reports mapping restoration failure
AVA_ETH_API int ava_eth_write_pid_pdo(const struct AvaEthOpenOptions *, uint32_t axis, uint32_t mask,
                                    struct AvaEthPidParameters *values);

AVA_ETH_API int         ava_eth_enumerate_npcap(struct AvaEthDeviceInfo *, uint32_t, uint32_t *);
AVA_ETH_API int         ava_eth_enumerate(struct AvaEthDeviceInfo *, uint32_t, uint32_t *);
AVA_ETH_API int         ava_eth_open(const struct AvaEthOpenOptions *, AvaEthHandle *);
AVA_ETH_API void        ava_eth_close(AvaEthHandle);
AVA_ETH_API int         ava_eth_get_info(AvaEthHandle, struct AvaEthInfo *);
AVA_ETH_API int         ava_eth_set_interpolation(AvaEthHandle, uint32_t, uint32_t, float, float);
AVA_ETH_API int         ava_eth_set_targets(AvaEthHandle, const struct AvaEthTarget *, uint32_t, uint64_t *);
AVA_ETH_API int         ava_eth_set_mode(AvaEthHandle, uint32_t, int32_t);
// Atomic mode/target switch, preserving enable. Returns the applied initial target
// Position modes start at fresh feedback; other command components start at zero
AVA_ETH_API int         ava_eth_switch_mode(AvaEthHandle, struct AvaEthTarget *, const struct AvaEthInterpolation *);
AVA_ETH_API int         ava_eth_set_position(AvaEthHandle, uint32_t, float);
AVA_ETH_API int         ava_eth_set_velocity(AvaEthHandle, uint32_t, float);
AVA_ETH_API int         ava_eth_set_current(AvaEthHandle, uint32_t, float);
AVA_ETH_API int         ava_eth_set_torque(AvaEthHandle, uint32_t, float);
AVA_ETH_API int         ava_eth_get_feedback(AvaEthHandle, uint32_t, struct AvaEthFeedback *);
AVA_ETH_API int         ava_eth_enable(AvaEthHandle, uint32_t);
AVA_ETH_API int         ava_eth_disable(AvaEthHandle, uint32_t);
AVA_ETH_API int         ava_eth_keepalive(AvaEthHandle, uint32_t);
AVA_ETH_API int         ava_eth_reset_fault(AvaEthHandle, uint32_t);
// Sets the persistent wire value. Success acknowledges PDO transport, not calibration completion
AVA_ETH_API int         ava_eth_set_special_cmd(AvaEthHandle, uint32_t, uint32_t);
AVA_ETH_API int         ava_eth_stop(AvaEthHandle);
AVA_ETH_API int         ava_eth_get_snapshot(AvaEthHandle, struct AvaEthSnapshot *);
AVA_ETH_API int         ava_eth_get_diagnostics(AvaEthHandle, struct AvaEthDiagnostics *);
AVA_ETH_API int         ava_eth_get_temperature(AvaEthHandle, uint32_t, float *, float *);
AVA_ETH_API int         ava_eth_sim_link(AvaEthHandle, uint32_t);
AVA_ETH_API int         ava_eth_sim_fault(AvaEthHandle, uint32_t, uint32_t);
AVA_ETH_API int         ava_eth_sim_advance(AvaEthHandle, uint64_t);
AVA_ETH_API const char *ava_eth_result_string(int);
AVA_ETH_API uint32_t    ava_eth_last_os_error(void);
AVA_ETH_API const char *ava_eth_last_error_message(void);
#ifdef __cplusplus
}
#endif
