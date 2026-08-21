#ifndef HFI_H
#define HFI_H

#include "../../iir.h"
#include <stddef.h>
#include <stdint.h>

#include "../pll.h"
#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

enum hfi_palar_idf {
    HFI_POLAR_IDF_READY,
    HFI_POLAR_IDF_POSITIVE,
    HFI_POLAR_IDF_NEGATIVE,
    HFI_POLAR_IDF_FINISH,
};

enum hfi_type {
    HFI_TYPE_PULSATING_SINE,   // 脉振正弦注入
    HFI_TYPE_PULSATING_SQUARE, // 脉振方波注入
    HFI_TYPE_ROTATING_SINE,    // 旋转正弦注入
    HFI_TYPE_ROTATING_SQUARE,  // 旋转方波注入
};

struct hfi_cfg {
    enum hfi_type e_type;
    float32_t     fs;
    float32_t     fi;
    float32_t     hfi_vd, hfi_id;
    uint32_t      alarm_enable;
    float32_t     alarm_fi, alarm_vd;
    struct f32_dq lpf_wc_dq;
    uint32_t      polar_idf_ms;

    struct iif_cfg id_bpf, iq_bpf;
    struct pll_cfg pll;
};

struct hfi_in {
    struct f32_ab i_ab;
};

struct hfi_out {
    float32_t est_theta;
    float32_t est_omega;
    float32_t id;
    float32_t vd;
};

struct hfi_lo {
    struct f32_dq est_i_dq;

    float32_t     hfi_theta;
    float32_t     alarm_theta;
    struct f32_dq hfi_i_dq;
    struct f32_dq lpf_i_dq;

    // 方波解调 (脉振方波注入)
    enum dir      sq_sign;
    struct f32_dq prev_est_i_dq;

    // 极性辨识
    enum hfi_palar_idf e_polar_idf;
    uint32_t           polar_cnt;
    float32_t          id_pos, id_neg;
    float32_t          polar_offset;

    struct iir_filter id_bpf, iq_bpf;
    struct pll_filter pll;
};

struct hfi_obs {
    struct hfi_cfg cfg;
    struct hfi_in  in;
    struct hfi_out out;
    struct hfi_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 高频注入结构体初始化
 *
 * @param hfi     高频注入结构体
 * @param hfi_cfg 高频注入配置
 * @return        void
 */
void hfi_init(struct hfi_obs *hfi, struct hfi_cfg hfi_cfg);

/**
 * @brief 高频注入单次执行计算
 *
 * @param hfi 高频注入结构体
 * @return    void
 */
void hfi_exec_rt(struct hfi_obs *hfi);

/**
 * @brief 高频注入单次执行计算(带输入)
 *
 * @param hfi  高频注入结构体
 * @param i_ab alpha-beta 轴电流
 * @return     void
 */
void hfi_exec_in_rt(struct hfi_obs *hfi, struct f32_ab i_ab);

/** 告警音仅生成高频注入电压，不执行电流解调和位置观测。 */
void hfi_alarm_set(struct hfi_obs *hfi, uint32_t enable);

#ifdef __cplusplus
}
#endif

#endif // !HFI_H
