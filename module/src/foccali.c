#include "errdef.h"
#include "focdef.h"
#include "macrodef.h"
#include "mathdef.h"
#include "timeops.h"

#define ELEC_THETA_OFFSET_DELAY_SMAPLE_MS (1000)

extern void foc_disable(foc_t *foc);
extern void foc_enable(foc_t *foc);

extern void foc_set_force_vel_ref(foc_t *foc, f32 ref_id, f32 ref_vel, f32 exec_freq);

static i32 foc_offset_elec_theta_cali(foc_t *foc);
static i32 foc_nonlinear_elec_theta_cali(foc_t *foc);
static i32 foc_nonlinear_outshaft_theta_cali(foc_t *foc);

// TODO 出轴方向辨识

/**
 * @brief 电气角度偏置校准
 *
 * @param foc FOC 结构体
 * @return    int 状态码
 */
i32
foc_offset_elec_theta_cali(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);

        switch (lo->e_cali_state) {
                case FOC_CALI_STATE_INIT: {
                        tmp->elec_theta_offset_sum                   = 0.0f;
                        tmp->elec_theta_dir_cali_cnt                 = 0;
                        tmp->elec_theta_offset_cali_cycle_cnt        = 0;
                        tmp->elec_theta_offset_cali_sample_delay_cnt = 0;
                        tmp->e_prev_elec_theta                       = lo->e_elec_theta;
                        tmp->e_prev_mode                             = lo->e_mode;
                        in->rotor.sensor.elec_offset_theta           = 0.0f;
                        in->rotor.sensor.motor_sensor_dir            = DIR_NONE;
                        lo->ref_i_dq.d                               = cfg->cali_cfg.force_id;
                        in->rotor.elec_force_omega                   = OUTSHAFT2ELEC(
                            cfg->cali_cfg.elec_theta_offset_cali_vel, cfg->base_cfg.outshaft_ratio, cfg->base_cfg.motor.npp);
                        lo->e_mode       = FOC_MODE_CUR;
                        lo->e_elec_theta = FOC_ELEC_THETA_FORCE;
                        lo->e_cali_state = FOC_CALI_STATE_CW;
                        break;
                }
                case FOC_CALI_STATE_CW: {
                        foc_enable(foc);

                        if (in->rotor.elec_force_theta >= TAU) {
                                in->rotor.elec_force_theta = TAU;

                                if (++tmp->elec_theta_offset_cali_sample_delay_cnt >=
                                    MS2CNT(ELEC_THETA_OFFSET_DELAY_SMAPLE_MS, cfg->base_cfg.fs)) {
                                        tmp->elec_theta_offset_sum += in->rotor.motor_elec_theta;

                                        tmp->elec_theta_offset_cali_sample_delay_cnt = 0;
                                        if (++tmp->elec_theta_offset_cali_cycle_cnt >= cfg->base_cfg.motor.npp) {
                                                in->rotor.elec_force_omega = -in->rotor.elec_force_omega;
                                                lo->e_cali_state           = FOC_CALI_STATE_CCW;
                                        } else
                                                in->rotor.elec_force_theta = 0.0f;
                                }
                        } else {
                                if (ABS(in->rotor.motor_elec_omega) > in->rotor.elec_force_theta)
                                        tmp->elec_theta_dir_cali_cnt +=
                                            IS_SAME_DIR(in->rotor.motor_elec_omega, in->rotor.elec_force_omega) ? DIR_FORWARD
                                                                                                                : DIR_REVERSE;

                                in->rotor.elec_force_theta += in->rotor.elec_force_omega / cfg->base_cfg.fs;
                        }
                        break;
                }
                case FOC_CALI_STATE_CCW: {
                        foc_enable(foc);

                        if (in->rotor.elec_force_theta <= 0.0f) {
                                in->rotor.elec_force_theta = 0.0f;

                                if (++tmp->elec_theta_offset_cali_sample_delay_cnt >=
                                    MS2CNT(ELEC_THETA_OFFSET_DELAY_SMAPLE_MS, cfg->base_cfg.fs)) {
                                        tmp->elec_theta_offset_sum += in->rotor.motor_elec_theta;

                                        tmp->elec_theta_offset_cali_sample_delay_cnt = 0;
                                        if (++tmp->elec_theta_offset_cali_cycle_cnt >= cfg->base_cfg.motor.npp * 2)
                                                lo->e_cali_state = FOC_CALI_STATE_FINISH;
                                        else
                                                in->rotor.elec_force_theta = TAU;
                                }
                        } else {
                                if (ABS(in->rotor.motor_elec_omega) > in->rotor.elec_force_theta)
                                        tmp->elec_theta_dir_cali_cnt +=
                                            IS_SAME_DIR(in->rotor.motor_elec_omega, in->rotor.elec_force_omega) ? DIR_FORWARD
                                                                                                                : DIR_REVERSE;

                                in->rotor.elec_force_theta += in->rotor.elec_force_omega / cfg->base_cfg.fs;
                        }
                        break;
                }
                case FOC_CALI_STATE_FINISH: {
                        in->rotor.sensor.motor_sensor_dir = lo->store.sensor.motor_sensor_dir =
                            tmp->elec_theta_dir_cali_cnt > 0 ? DIR_FORWARD : DIR_REVERSE;
                        in->rotor.sensor.elec_offset_theta = lo->store.sensor.elec_offset_theta =
                            TOGGLE_THETA(lo->store.sensor.motor_sensor_dir,
                                         tmp->elec_theta_offset_sum / tmp->elec_theta_offset_cali_cycle_cnt);

                        lo->ref_i_dq.d             = 0.0f;
                        in->rotor.elec_force_theta = 0.0f;
                        in->rotor.elec_force_omega = 0.0f;
                        in->rotor.elec_theta       = 0.0f;
                        in->rotor.elec_omega       = 0.0f;

                        memset(&lo->id_pi.lo, 0, sizeof(lo->id_pi.lo));
                        memset(&lo->iq_pi.lo, 0, sizeof(lo->iq_pi.lo));
                        RESET(&lo->id_pi, out);
                        RESET(&lo->iq_pi, out);
                        RESET(foc, out);

                        lo->e_mode       = tmp->e_prev_mode;
                        lo->e_elec_theta = tmp->e_prev_elec_theta;
                        lo->e_cali_state = FOC_CALI_STATE_INIT;
                        return 0;
                }
                default:
                        break;
        }
        return -MEBUSY;
}

/**
 * @brief 电气角度非线性校准(仅绝对值编码器可以做非线性校准)
 *
 * @param foc FOC 结构体
 * @return    int 状态码
 */
i32
foc_nonlinear_elec_theta_cali(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);

        if (cfg->sensor_cfg.e_sensor == FOC_SENSOR_ELEC)
                return -MEACCES;

        switch (lo->e_cali_state) {
                case FOC_CALI_STATE_INIT: {
                        tmp->e_prev_elec_theta             = lo->e_elec_theta;
                        tmp->e_prev_mode                   = lo->e_mode;
                        in->rotor.sensor.elec_offset_theta = 0.0f;
                        in->rotor.sensor.motor_sensor_dir  = DIR_NONE;
                        lo->e_elec_theta                   = FOC_ELEC_THETA_FORCE;
                        lo->e_mode                         = FOC_MODE_VEL;
                        lo->e_cali_state                   = FOC_CALI_STATE_CW;
                        break;
                }
                case FOC_CALI_STATE_CW: {
                        foc_enable(foc);

                        foc_set_force_vel_ref(
                            foc, cfg->cali_cfg.force_id, cfg->cali_cfg.elec_theta_nonlinear_cali_vel, cfg->base_cfg.fs);
                        break;
                }
                case FOC_CALI_STATE_CCW: {
                        break;
                }
                case FOC_CALI_STATE_FINISH: {
                        memset(&in->ref_pvct, 0, sizeof(in->ref_pvct));
                        lo->e_elec_theta = tmp->e_prev_elec_theta;
                        lo->e_mode       = tmp->e_prev_mode;
                        lo->e_cali_state = FOC_CALI_STATE_INIT;
                        return 0;
                }
                default:
                        break;
        }
        return -MEBUSY;
}

/**
 * @brief 出轴角度非线性校准
 *
 * @param foc FOC 结构体
 * @return    int 状态码
 */
i32
foc_nonlinear_outshaft_theta_cali(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);

        switch (lo->e_cali_state) {
                case FOC_CALI_STATE_INIT: {
                        tmp->e_prev_elec_theta             = lo->e_elec_theta;
                        tmp->e_prev_mode                   = lo->e_mode;
                        in->rotor.sensor.elec_offset_theta = 0.0f;
                        in->rotor.sensor.motor_sensor_dir  = DIR_NONE;
                        lo->e_elec_theta                   = FOC_ELEC_THETA_FORCE;
                        lo->e_mode                         = FOC_MODE_VEL;
                        lo->e_cali_state                   = FOC_CALI_STATE_CW;
                        break;
                }
                case FOC_CALI_STATE_CW: {
                        foc_enable(foc);

                        foc_set_force_vel_ref(
                            foc, cfg->cali_cfg.force_id, cfg->cali_cfg.elec_theta_nonlinear_cali_vel, cfg->base_cfg.fs);
                        break;
                }
                case FOC_CALI_STATE_CCW: {
                        break;
                }
                case FOC_CALI_STATE_FINISH: {
                        memset(&in->ref_pvct, 0, sizeof(in->ref_pvct));
                        lo->e_elec_theta = tmp->e_prev_elec_theta;
                        lo->e_mode       = tmp->e_prev_mode;
                        lo->e_cali_state = FOC_CALI_STATE_INIT;
                        return 0;
                }
                default:
                        break;
        }
        return -MEBUSY;
}

/**
 * @brief FOC 校准状态
 *
 * @param foc FOC 结构体
 * @return    int 状态码
 */
i32
foc_cali(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);

        i32 ret;

        /* ---------------------------------- 电角度校准 --------------------------------- */

        /* 如果正在 ELEC_THETA 非线性校准中, 继续执行 */
        if (lo->u_cali_flag.bit.nonlinear_elec_theta) {
                ret = foc_nonlinear_elec_theta_cali(foc);
                if (ret < 0)
                        return ret;

                /* 校准结束 */
                lo->u_cali_flag.bit.nonlinear_elec_theta = FALSE;
        }

        /* 如果正在 ELEC_THETA 偏置校准中, 继续执行 */
        if (lo->u_cali_flag.bit.offset_elec_theta) {
                ret = foc_offset_elec_theta_cali(foc);
                if (ret < 0)
                        return ret;

                /* 校准结束 */
                lo->u_cali_flag.bit.offset_elec_theta = FALSE;
        }

        /* ---------------------------------- 电角度校准 --------------------------------- */

        /* --------------------------------- 出轴角度校准 --------------------------------- */

        /* 如果正在 OUTSHAFT_THETA 非线性校准中, 继续执行 */
        if (lo->u_cali_flag.bit.nonlinear_outshaft_theta) {
                ret = foc_nonlinear_outshaft_theta_cali(foc);
                if (ret < 0)
                        return ret;

                /* 校准结束 */
                lo->u_cali_flag.bit.nonlinear_outshaft_theta = FALSE;
        }

        /* --------------------------------- 出轴角度校准 --------------------------------- */

        if (lo->u_cali_flag.val == 0) {
                ret         = cfg->func_cfg.f_store();
                lo->e_state = FOC_STATE_DISABLE;
                return ret;
        }
        return -MEBUSY;
}
