#ifndef FSAV3_C_H
#define FSAV3_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Values for fsav3_param.control_action
typedef enum fsav3_control_action {
    FSAV3_CONTROL_NONE = 0,
    FSAV3_CONTROL_ENABLE = 1,
    FSAV3_CONTROL_DISABLE = 2
} fsav3_control_action;

// Values for fsav3_param.command_action
typedef enum fsav3_command_action {
    FSAV3_COMMAND_NONE = 0,
    FSAV3_COMMAND_POSITION = 1,
    FSAV3_COMMAND_VELOCITY = 2,
    FSAV3_COMMAND_TORQUE = 3,
    FSAV3_COMMAND_CURRENT = 4,
    FSAV3_COMMAND_PD = 5
} fsav3_command_action;

typedef enum comm_shm_v25_control_word {
    COMM_SHM_V25_CTRL_NONE = 0x0000U,
    COMM_SHM_V25_CTRL_SERVO_OFF = 0xF001U,
    COMM_SHM_V25_CTRL_SERVO_ON = 0xF002U,
    COMM_SHM_V25_CTRL_CLEAR_FAULT = 0xF086U,
    COMM_SHM_V25_CTRL_CALIBRATE_ENCODER = 0x101FU,
    COMM_SHM_V25_CTRL_RETURN_ZERO = 0x102FU,
    COMM_SHM_V25_CTRL_POSITION_SET_ZERO = 0x104FU
} comm_shm_v25_control_word;

typedef enum comm_shm_v25_control_mode {
    COMM_SHM_V25_MODE_NONE = 0,
    COMM_SHM_V25_MODE_CURRENT = 1,
    COMM_SHM_V25_MODE_VELOCITY = 2,
    COMM_SHM_V25_MODE_POSITION = 3,
    COMM_SHM_V25_MODE_TORQUE = 4,
    COMM_SHM_V25_MODE_TORQUE_VELOCITY = 5,
    COMM_SHM_V25_MODE_TORQUE_POSITION = 6,
    COMM_SHM_V25_MODE_TORQUE_PD = 7,
    COMM_SHM_V25_MODE_BRAKE = 8,
    COMM_SHM_V25_MODE_ASC = 9,
    COMM_SHM_V25_MODE_OPENLOOP = 0xFF
} comm_shm_v25_control_mode;

/*
 * Automatic parameter table. The type and its read/write functions deliberately
 * end in "param", which is the strict naming convention used by ava_tool.
 *
 * write_pid/write_pd/control_action/command_action are explicit gates. Their
 * zero defaults make applying a newly-created table safe: it connects to the
 * device but does not enable the actuator or send a motion command.
 */
typedef struct fsav3_param {
    char ip[64];
    uint32_t timeout_ms;
    uint32_t max_retry;

    uint32_t write_pid;
    uint32_t write_pd;
    fsav3_control_action control_action;
    comm_shm_v25_control_mode control_mode;
    fsav3_command_action command_action;

    double pos_kp;
    double vel_kp;
    double vel_ki;
    double pd_kp;
    double pd_kd;

    double position_rad;
    double velocity_radps;
    double torque_nm;
    double current_a;
} fsav3_param;

// V2.5 realtime-control layout, matching FocDriver/app/src/comm_shm.c
struct comm_shm_v25_control_config {
    comm_shm_v25_control_mode set_control_mode;
    comm_shm_v25_control_word set_control_word;
    uint32_t get_work_mode;
    uint32_t reserved[7];
};

struct comm_shm_v25_set_pvct {
    float pos;
    float vel;
    float cur;
    float tor;
    float vel_ff;
    float cur_ff;
    float tor_ff;
    uint32_t reserved[8];
};

struct comm_shm_v25_get_pvct {
    float pos;
    float vel;
    float cur;
    float load_tor;
    float elec_tor;
    uint32_t reserved[7];
};

struct comm_shm_v25_pid_param {
    float tor_pos_kp;
    float tor_vel_kp;
    float tor_vel_ki;
    float pd_kp;
    float pd_kd;
    float cur_pos_kp;
    float cur_vel_kp;
    float cur_vel_ki;
    uint32_t reserved[5];
};

struct comm_shm_v25_inverter_status {
    float mos;
    float coil;
    float vbus;
    float coil_2;
    uint32_t reserved[7];
};

struct comm_shm_v25_comm_timeout {
    uint32_t timeout_ms;
    float tor_pos_kp;
    float tor_vel_kp;
    float tor_vel_ki;
    uint32_t reserved[8];
};

/*
 * Automatic realtime table.  Transport fields precede the V2.5 control area;
 * the SDK caller flattens embedded structs into names such as control.xxx,
 * set_pvct.xxx and get_pvct.xxx.
 */
typedef struct fsav3_rt {
    char ip[64];
    uint32_t timeout_ms;
    uint32_t max_retry;
    uint32_t write_pid;
    uint32_t write_comm_timeout;
    uint64_t sample_count;

    struct comm_shm_v25_control_config control;
    struct comm_shm_v25_set_pvct set_pvct;
    struct comm_shm_v25_get_pvct get_pvct;
    struct comm_shm_v25_pid_param pid;
    uint32_t err_code[8];
    struct comm_shm_v25_inverter_status inverter;
    struct comm_shm_v25_comm_timeout comm_timeout;
    uint8_t reserved[0x400U - 0x144U];
} fsav3_rt;

/**
 * @brief 0=success, -302=timeout, other values are fsav3 return codes
 */
int fsav3_read_param(fsav3_param *param);
int fsav3_write_param(fsav3_param *param);
int fsav3_read_rt(fsav3_rt *rt);
int fsav3_write_rt(fsav3_rt *rt);

/**
 * @brief Backward-compatible narrow helper used by the KPS6050D example
 */
int fsav3_c_read_error3(const char *ip, int timeout_ms, uint32_t *error3);

#ifdef __cplusplus
}
#endif
#endif
