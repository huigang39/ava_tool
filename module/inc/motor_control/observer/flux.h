#ifndef FLUX_H
#define FLUX_H

#include "../pll.h"

#ifdef __cplusplus
extern "C" {
#endif

struct flux_cfg {
    float32_t fs;                    // 采样频率 (Hz)
    float32_t wc;                    // 磁链幅值校正带宽 (rad/s)
    float32_t sample_volt_max_omega; // 全部使用端电压采样的最高电角速度 (rad/s)
    float32_t out_volt_min_omega;    // 全部使用输出电压的最低电角速度 (rad/s)

    struct pll_cfg pll; // 转子磁链锁相环

    struct motor_cfg *motor;
};

struct flux_in {
    struct f32_ab i_ab;
    struct f32_ab v_ab;
};

struct flux_out {
    struct f32_ab est_stator_flux_ab; // 估算定子磁链 (Wb)
    struct f32_ab est_rotor_flux_ab;  // 估算转子/有功磁链 (Wb)
    float32_t     est_flux;           // 估算转子/有功磁链幅值 (Wb)
    float32_t     est_theta;          // 估算电角度 (rad)
    float32_t     est_omega;          // 估算电角速度 (rad/s)
};

struct flux_lo {
    float32_t         target_flux;
    struct pll_filter pll;
};

struct flux_obs {
    struct flux_cfg cfg;
    struct flux_in  in;
    struct flux_out out;
    struct flux_lo  lo;
};

void flux_init(struct flux_obs *flux, struct flux_cfg flux_cfg);
void flux_exec_rt(struct flux_obs *flux);
void flux_exec_in_rt(struct flux_obs *flux, struct f32_ab i_ab, struct f32_ab v_ab);

#ifdef __cplusplus
}
#endif

#endif // !FLUX_H
