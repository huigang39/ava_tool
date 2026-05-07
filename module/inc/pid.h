#ifndef PID_H
#define PID_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct pid_cfg {
        f32 fs;
        f32 kp, ki, kd;
        f32 ki_out_min, ki_out_max;
        f32 out_min, out_max;
        f32 ref_rate_max;
} pid_cfg_t;

typedef struct pid_in {
        f32 ref;
        f32 fdb;
        f32 ffd;
} pid_in_t;

typedef struct pid_out {
        f32 u_raw; // 未限幅
        f32 u;     // 限幅
} pid_out_t;

typedef struct pid_lo {
        pid_cfg_t cfg;

        f32 err, prev_err;
        f32 kp_out, ki_out, kd_out;
        f32 ref_change, ref_change_max;
        f32 ref_limited, prev_ref;
} pid_lo_t;

typedef struct pid_ctl {
        pid_cfg_t cfg;
        pid_in_t  in;
        pid_out_t out;
        pid_lo_t  lo;
} pid_ctl_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief PID 结构体初始化
 *
 * @param pid     PID 结构体
 * @param pid_cfg PID 配置
 * @return        void
 */
void pid_init(pid_ctl_t *pid, pid_cfg_t pid_cfg);

/**
 * @brief 并联 PID 运算
 *
 * @param pid PID 结构体
 * @return    void
 */
void pid_parallel_exec(pid_ctl_t *pid);

/**
 * @brief 串联 PID 运算
 *
 * @param pid PID 结构体
 * @return    void
 */
void pid_serial_exec(pid_ctl_t *pid);

/**
 * @brief 并联 PID 运算(带输入)
 *
 * @param pid PID 结构体
 * @param ref 参考值
 * @param fdb 反馈值
 * @param ffd 前馈值
 * @return    void
 */
void pid_parallel_exec_in(pid_ctl_t *pid, f32 ref, f32 fdb, f32 ffd);

/**
 * @brief 串联 PID 运算(带输入)
 *
 * @param pid PID 结构体
 * @param ref 参考值
 * @param fdb 反馈值
 * @param ffd 前馈值
 * @return    void
 */
void pid_serial_exec_in(pid_ctl_t *pid, f32 ref, f32 fdb, f32 ffd);

/**
 * @brief 并联 PD 运算(带输入)
 *
 * @param pid     PID 结构体
 * @param ref_pos 参考位置
 * @param fdb_pos 反馈位置
 * @param ref_vel 参考速度
 * @param fdb_vel 反馈速度
 * @param ffd_tor 前馈力矩
 * @return        void
 */
void pd_parallel_exec_in(pid_ctl_t *pid, f32 ref_pos, f32 fdb_pos, f32 ref_vel, f32 fdb_vel, f32 ffd_tor);

#ifdef __cplusplus
}
#endif

#endif // !PID_H
