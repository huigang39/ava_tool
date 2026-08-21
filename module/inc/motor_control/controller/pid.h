#ifndef PID_H
#define PID_H
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

struct pid_cfg {
    float32_t fs;
    float32_t kp, ki, kd;
    float32_t ki_out_min, ki_out_max;
    float32_t pid_out_min, pid_out_max;
    float32_t out_min, out_max;
    float32_t ref_rate_max;
    float32_t ref_change_max;
};

struct pid_in {
    float32_t ref;
    float32_t fdb;
    float32_t ffd;
};

struct pid_out {
    float32_t pid_raw; // 未限幅的 Kp+Ki+Kd
    float32_t pid;     // 限幅后的 Kp+Ki+Kd
    float32_t u_raw;   // pid+ffd,最终输出限幅前
    float32_t u;       // 最终输出
};

struct pid_lo {
    float32_t err, prev_err;
    float32_t kp_out, ki_out, kd_out;
    float32_t ref_change;
    float32_t ref_limited, prev_ref;
};

struct pid_ctl {
    struct pid_cfg cfg;
    struct pid_in  in;
    struct pid_out out;
    struct pid_lo  lo;
};

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
void pid_init(struct pid_ctl *pid, struct pid_cfg pid_cfg);

/** 设置总输出,Kp+Ki+Kd 修正量及积分输出的上下限. */
void pid_set_out_limit(struct pid_ctl *pid,
                       float32_t       out_max,
                       float32_t       pid_out_max,
                       float32_t       ki_out_max,
                       float32_t       out_min,
                       float32_t       pid_out_min,
                       float32_t       ki_out_min);

/**
 * @brief 并联 PID 运算
 *
 * @param pid PID 结构体
 * @return    void
 */
void pid_parallel_exec_rt(struct pid_ctl *pid);

/**
 * @brief 串联 PID 运算
 *
 * @param pid PID 结构体
 * @return    void
 */
void pid_serial_exec_rt(struct pid_ctl *pid);

/**
 * @brief 并联 PID 运算(带输入)
 *
 * @param pid PID 结构体
 * @param ref 参考值
 * @param fdb 反馈值
 * @param ffd 前馈值
 * @return    void
 */
void pid_parallel_exec_in_rt(struct pid_ctl *pid, float32_t ref, float32_t fdb, float32_t ffd);

/**
 * @brief 串联 PID 运算(带输入)
 *
 * @param pid PID 结构体
 * @param ref 参考值
 * @param fdb 反馈值
 * @param ffd 前馈值
 * @return    void
 */
void pid_serial_exec_in_rt(struct pid_ctl *pid, float32_t ref, float32_t fdb, float32_t ffd);

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
void pd_parallel_exec_in_rt(struct pid_ctl *pid,
                            float32_t       ref_pos,
                            float32_t       fdb_pos,
                            float32_t       ref_vel,
                            float32_t       fdb_vel,
                            float32_t       ffd_tor);

#ifdef __cplusplus
}
#endif

#endif // !PID_H
