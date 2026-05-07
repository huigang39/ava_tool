#ifndef HFI_H
#define HFI_H

#include "iir.h"
#include "pll.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef enum hfi_palar_idf {
        HFI_POLAR_IDF_READY,
        HFI_POLAR_IDF_POSITIVE,
        HFI_POLAR_IDF_NEGATIVE,
        HFI_POLAR_IDF_FINISH,
} hfi_polar_idf_e;

typedef enum hfi_type {
        HFI_TYPE_PULSATING_SINE,   // 脉振正弦注入
        HFI_TYPE_PULSATING_SQUARE, // 脉振方波注入
        HFI_TYPE_ROTATING_SINE,    // 旋转正弦注入
        HFI_TYPE_ROTATING_SQUARE,  // 旋转方波注入
} hfi_type_e;

typedef struct hfi_cfg {
        hfi_type_e e_type;
        f32        fs;
        f32        fi;
        f32        hfi_vd, hfi_id;
        f32_dq_t   lpf_wc_dq;
        u32        polar_idf_ms;

        iir_cfg_t id_bpf, iq_bpf;
        pll_cfg_t pll;
} hfi_cfg_t;

typedef struct hfi_in {
        f32_ab_t i_ab;
} hfi_in_t;

typedef struct hfi_out {
        f32 est_theta;
        f32 est_omega;
        f32 id;
        f32 vd;
} hfi_out_t;

typedef struct hfi_lo {
        hfi_cfg_t cfg;

        f32_dq_t est_i_dq;

        f32      hfi_theta;
        f32_dq_t hfi_i_dq;
        f32_dq_t lpf_i_dq;

        // 方波解调 (脉振方波注入)
        dir_e    sq_sign;
        f32_dq_t prev_est_i_dq;

        // 极性辨识
        hfi_polar_idf_e e_polar_idf;
        u32             polar_cnt;
        f32             id_pos, id_neg;
        f32             polar_offset;

        iir_filter_t id_bpf, iq_bpf;
        pll_filter_t pll;
} hfi_lo_t;

typedef struct hfi_obs {
        hfi_cfg_t cfg;
        hfi_in_t  in;
        hfi_out_t out;
        hfi_lo_t  lo;
} hfi_obs_t;

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
void hfi_init(hfi_obs_t *hfi, hfi_cfg_t hfi_cfg);

/**
 * @brief 高频注入单次执行计算
 *
 * @param hfi 高频注入结构体
 * @return    void
 */
void hfi_exec(hfi_obs_t *hfi);

/**
 * @brief 高频注入单次执行计算(带输入)
 *
 * @param hfi  高频注入结构体
 * @param i_ab alpha-beta 轴电流
 * @return     void
 */
void hfi_exec_in(hfi_obs_t *hfi, f32_ab_t i_ab);

#ifdef __cplusplus
}
#endif

#endif // !HFI_H
