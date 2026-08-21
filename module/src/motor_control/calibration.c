#include "errdef.h"
#include "macrodef.h"
#include "mathdef.h"
#include "timeops.h"

#include "motor_control/foc.h"
#include "motor_control/focdef.h"
#include <stddef.h>
#include <stdint.h>

#define ELEC_THETA_OFFSET_DELAY_SMAPLE_MS (1000)
#define RESISTANCE_CALI_RAMP_TIMEOUT_MS   (2000)

extern void foc_disable_rt(struct foc *foc);
extern void foc_enable_rt(struct foc *foc);

static int foc_offset_elec_theta_cali(struct foc *foc);
static int foc_nonlinear_motor_theta_cali(struct foc *foc);
static int foc_nonlinear_outshaft_theta_cali(struct foc *foc);

static int foc_resistance_cali(struct foc *foc);
static int foc_inductance_cali(struct foc *foc);

static void
foc_set_if_acc_limit(struct foc *foc, const float32_t acc_max)
{
    foc->lo.if_ctl.vel_acc_max = ABS(acc_max);
}

static void
foc_start_nonlinear_cali(struct foc *foc)
{
    foc_set_if_acc_limit(foc, foc->cfg.cali_cfg.nonlinear_theta_cali_acc_max);
    foc->lo.if_ctl.vel_ref_limited = 0.0F;
    foc->in.ref_pvct.vel           = 0.0F;
    foc->in.rotor.elec_omega_force = 0.0F;
}

static uint32_t
foc_stop_nonlinear_cali(struct foc *foc)
{
    foc->in.ref_pvct.cur = foc->cfg.cali_cfg.force_id;
    foc->in.ref_pvct.vel = 0.0F;
    foc_enable_rt(foc);
    return foc->lo.if_ctl.vel_ref_limited == 0.0F;
}

static void
foc_restore_if_acc_limit(struct foc *foc)
{
    foc_set_if_acc_limit(foc, foc->cfg.base_cfg.acc_max);
}

static void
foc_set_if_motor_vel_ref(struct foc *foc, const float32_t ref_id, const float32_t ref_motor_vel)
{
    const float32_t ref_outshaft_vel =
        MECH2OUTSHAFT(ref_motor_vel, foc->cfg.base_cfg.reducer.outshaft_ratio);
    foc->in.ref_pvct.cur = ref_id;
    foc->in.ref_pvct.vel = foc_vel_from_rads_rt(ref_outshaft_vel, foc->cfg.base_cfg.e_vel_unit);
}

static int
foc_finish_cali_step(struct foc *foc)
{
    DECL(foc, cfg);

    foc_disable_rt(foc);
    return cfg->func_cfg.f_store();
}

static int
foc_wait_next_cali_step(struct foc *foc)
{
    DECL(foc, cfg, tmp);

    if (tmp->cali_step_delay_cnt <= 0)
        return 0;

    foc_disable_rt(foc);
    tmp->cali_step_delay_cnt--;
    return -MEBUSY;
}

static void
foc_start_cali_step_delay(struct foc *foc)
{
    DECL(foc, cfg, tmp);

    tmp->cali_step_delay_cnt = (int32_t)MS2CNT(cfg->cali_cfg.step_delay_ms, cfg->ctl_cfg.iq_pi.fs);
}

/**
 * @brief 电气角度偏置校准
 *
 * @param foc FOC 结构体
 * @return    int 状态码
 */
int
foc_offset_elec_theta_cali(struct foc *foc)
{
    DECL(foc, cfg, in, lo, tmp);

    switch (lo->e_cali_state) {
        case FOC_CALI_STATE_INIT: {
            tmp->elec_theta_offset_sum                   = 0.0F;
            tmp->elec_theta_dir_cali_cnt                 = 0;
            tmp->elec_theta_offset_cali_cycle_cnt        = 0;
            tmp->elec_theta_offset_cali_sample_delay_cnt = 0;
            tmp->outshaft_theta_dir_cali_cnt             = 0;
            tmp->prev_outshaft_theta_raw                 = in->rotor.outshaft_theta_raw;
            in->rotor.sensor.elec_theta_offset           = 0.0F;
            in->rotor.sensor.motor_sensor_dir            = DIR_NONE;
            in->rotor.sensor.outshaft_sensor_dir         = DIR_NONE;
            lo->ref_i_dq.d                               = cfg->cali_cfg.force_id;
            in->rotor.elec_omega_force = cfg->cali_cfg.offset_elec_theta_cali_elec_vel;
            foc_set_mode(foc, FOC_MODE_CUR);
            foc_set_elec_theta(foc, FOC_ELEC_THETA_FORCE);
            lo->e_cali_state = FOC_CALI_STATE_CW;
            break;
        }
        case FOC_CALI_STATE_CW: {
            foc_enable_rt(foc);

            if (in->rotor.elec_theta_force >= TAU) {
                in->rotor.elec_theta_force = TAU;

                if (++tmp->elec_theta_offset_cali_sample_delay_cnt >=
                    MS2CNT(ELEC_THETA_OFFSET_DELAY_SMAPLE_MS, cfg->ctl_cfg.iq_pi.fs)) {
                    tmp->elec_theta_offset_sum += in->rotor.motor_theta_elec;

                    tmp->elec_theta_offset_cali_sample_delay_cnt = 0;
                    if (++tmp->elec_theta_offset_cali_cycle_cnt >= cfg->base_cfg.motor.npp) {
                        in->rotor.elec_omega_force = -in->rotor.elec_omega_force;
                        lo->e_cali_state           = FOC_CALI_STATE_CCW;
                    } else
                        in->rotor.elec_theta_force = 0.0F;
                }
            } else {
                if (ABS(in->rotor.motor_omega_elec) > in->rotor.elec_theta_force)
                    tmp->elec_theta_dir_cali_cnt +=
                        IS_SAME_DIR(in->rotor.motor_omega_elec, in->rotor.elec_omega_force)
                            ? DIR_FORWARD
                            : DIR_REVERSE;

                if (cfg->sensor_cfg.outshaft_sensor_enable) {
                    float32_t delta_outshaft_raw;
                    WARP_PI(delta_outshaft_raw,
                            in->rotor.outshaft_theta_raw - tmp->prev_outshaft_theta_raw);
                    tmp->prev_outshaft_theta_raw = in->rotor.outshaft_theta_raw;
                    if (delta_outshaft_raw != 0.0F)
                        tmp->outshaft_theta_dir_cali_cnt +=
                            IS_SAME_DIR(delta_outshaft_raw, in->rotor.elec_omega_force)
                                ? DIR_FORWARD
                                : DIR_REVERSE;
                }

                in->rotor.elec_theta_force += in->rotor.elec_omega_force / cfg->ctl_cfg.iq_pi.fs;
            }
            break;
        }
        case FOC_CALI_STATE_CCW: {
            foc_enable_rt(foc);

            if (in->rotor.elec_theta_force <= 0.0F) {
                in->rotor.elec_theta_force = 0.0F;

                if (++tmp->elec_theta_offset_cali_sample_delay_cnt >=
                    MS2CNT(ELEC_THETA_OFFSET_DELAY_SMAPLE_MS, cfg->ctl_cfg.iq_pi.fs)) {
                    tmp->elec_theta_offset_sum += in->rotor.motor_theta_elec;

                    tmp->elec_theta_offset_cali_sample_delay_cnt = 0;
                    if (++tmp->elec_theta_offset_cali_cycle_cnt >= cfg->base_cfg.motor.npp * 2)
                        lo->e_cali_state = FOC_CALI_STATE_FINISH;
                    else
                        in->rotor.elec_theta_force = TAU;
                }
            } else {
                if (ABS(in->rotor.motor_omega_elec) > in->rotor.elec_theta_force)
                    tmp->elec_theta_dir_cali_cnt +=
                        IS_SAME_DIR(in->rotor.motor_omega_elec, in->rotor.elec_omega_force)
                            ? DIR_FORWARD
                            : DIR_REVERSE;

                if (cfg->sensor_cfg.outshaft_sensor_enable) {
                    float32_t delta_outshaft_raw;
                    WARP_PI(delta_outshaft_raw,
                            in->rotor.outshaft_theta_raw - tmp->prev_outshaft_theta_raw);
                    tmp->prev_outshaft_theta_raw = in->rotor.outshaft_theta_raw;
                    if (delta_outshaft_raw != 0.0F)
                        tmp->outshaft_theta_dir_cali_cnt +=
                            IS_SAME_DIR(delta_outshaft_raw, in->rotor.elec_omega_force)
                                ? DIR_FORWARD
                                : DIR_REVERSE;
                }

                in->rotor.elec_theta_force += in->rotor.elec_omega_force / cfg->ctl_cfg.iq_pi.fs;
            }
            break;
        }
        case FOC_CALI_STATE_FINISH: {
            in->rotor.sensor.motor_sensor_dir = lo->store.sensor.motor_sensor_dir =
                tmp->elec_theta_dir_cali_cnt > 0 ? DIR_FORWARD : DIR_REVERSE;
            in->rotor.sensor.elec_theta_offset = lo->store.sensor.elec_theta_offset =
                TOGGLE_THETA(lo->store.sensor.motor_sensor_dir,
                             tmp->elec_theta_offset_sum / tmp->elec_theta_offset_cali_cycle_cnt);
            if (cfg->sensor_cfg.outshaft_sensor_enable)
                in->rotor.sensor.outshaft_sensor_dir = lo->store.sensor.outshaft_sensor_dir =
                    tmp->outshaft_theta_dir_cali_cnt > 0 ? DIR_FORWARD : DIR_REVERSE;

            lo->ref_i_dq.d             = 0.0F;
            in->rotor.elec_theta_force = 0.0F;
            in->rotor.elec_omega_force = 0.0F;
            in->rotor.elec_theta       = 0.0F;
            in->rotor.elec_omega       = 0.0F;

            memset(&lo->id_pi.lo, 0, sizeof(lo->id_pi.lo));
            memset(&lo->iq_pi.lo, 0, sizeof(lo->iq_pi.lo));
            RESET(&lo->id_pi, out);
            RESET(&lo->iq_pi, out);
            RESET(foc, out);

            lo->e_cali_state = FOC_CALI_STATE_INIT;
            foc_disable_rt(foc);
            return 0;
        }
        default:
            break;
    }
    return -MEBUSY;
}

/**
 * @brief 电机轴角度非线性校准(仅绝对值编码器可以做非线性校准)
 *
 * @param foc FOC 结构体
 * @return    int 状态码
 */
int
foc_nonlinear_motor_theta_cali(struct foc *foc)
{
    DECL(foc, cfg, in, lo);

    if (cfg->sensor_cfg.e_motor_sensor == FOC_SENSOR_ELEC)
        return -MEACCES;

    switch (lo->e_cali_state) {
        case FOC_CALI_STATE_INIT: {
            foc_start_nonlinear_cali(foc);
            foc_set_mode(foc, FOC_MODE_IF);
            lo->e_cali_state = FOC_CALI_STATE_CW;
            break;
        }
        case FOC_CALI_STATE_CW: {
            foc_enable_rt(foc);

            foc_set_if_motor_vel_ref(
                foc, cfg->cali_cfg.force_id, cfg->cali_cfg.nonlinear_motor_theta_cali_motor_vel);
            break;
        }
        case FOC_CALI_STATE_CCW: {
            break;
        }
        case FOC_CALI_STATE_FINISH: {
            if (!foc_stop_nonlinear_cali(foc))
                break;

            memset(&in->ref_pvct, 0, sizeof(in->ref_pvct));
            foc_restore_if_acc_limit(foc);
            lo->e_cali_state = FOC_CALI_STATE_INIT;
            foc_disable_rt(foc);
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
int
foc_nonlinear_outshaft_theta_cali(struct foc *foc)
{
    DECL(foc, cfg, in, lo);

    switch (lo->e_cali_state) {
        case FOC_CALI_STATE_INIT: {
            foc_start_nonlinear_cali(foc);
            foc_set_mode(foc, FOC_MODE_IF);
            lo->e_cali_state = FOC_CALI_STATE_CW;
            break;
        }
        case FOC_CALI_STATE_CW: {
            foc_enable_rt(foc);

            in->ref_pvct.cur = cfg->cali_cfg.force_id;
            in->ref_pvct.vel = cfg->cali_cfg.nonlinear_outshaft_theta_cali_outshaft_vel;
            break;
        }
        case FOC_CALI_STATE_CCW: {
            break;
        }
        case FOC_CALI_STATE_FINISH: {
            if (!foc_stop_nonlinear_cali(foc))
                break;

            memset(&in->ref_pvct, 0, sizeof(in->ref_pvct));
            foc_restore_if_acc_limit(foc);
            lo->e_cali_state = FOC_CALI_STATE_INIT;
            foc_disable_rt(foc);
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
int
foc_cali(struct foc *foc)
{
    DECL(foc, lo, tmp);

    int ret;

    ret = foc_wait_next_cali_step(foc);
    if (ret < 0)
        return ret;

    /* 如果正在电感离线辨识中, 继续执行 */
    if (lo->u_cali_flag.bit.inductance) {
        ret = foc_inductance_cali(foc);
        if (ret < 0)
            return ret;

        /* 校准结束 */
        lo->u_cali_flag.bit.inductance = false;
        ret                            = foc_finish_cali_step(foc);
        if (ret < 0)
            return ret;
        if (lo->u_cali_flag.val != 0) {
            foc_start_cali_step_delay(foc);
            return -MEBUSY;
        }
    }

    /* 如果正在电阻离线辨识中, 继续执行 */
    if (lo->u_cali_flag.bit.resistance) {
        ret = foc_resistance_cali(foc);
        if (ret < 0)
            return ret;

        /* 校准结束 */
        lo->u_cali_flag.bit.resistance = false;
        ret                            = foc_finish_cali_step(foc);
        if (ret < 0)
            return ret;
        if (lo->u_cali_flag.val != 0) {
            foc_start_cali_step_delay(foc);
            return -MEBUSY;
        }
    }

    /* 如果正在 MOTOR_THETA 非线性校准中, 继续执行 */
    if (lo->u_cali_flag.bit.nonlinear_elec_theta) {
        ret = foc_nonlinear_motor_theta_cali(foc);
        if (ret < 0)
            return ret;

        /* 校准结束 */
        lo->u_cali_flag.bit.nonlinear_elec_theta = false;
        ret                                      = foc_finish_cali_step(foc);
        if (ret < 0)
            return ret;
        if (lo->u_cali_flag.val != 0) {
            foc_start_cali_step_delay(foc);
            return -MEBUSY;
        }
    }

    /* 如果正在 OUTSHAFT_THETA 非线性校准中, 继续执行 */
    if (lo->u_cali_flag.bit.nonlinear_outshaft_theta) {
        ret = foc_nonlinear_outshaft_theta_cali(foc);
        if (ret < 0)
            return ret;

        /* 校准结束 */
        lo->u_cali_flag.bit.nonlinear_outshaft_theta = false;
        ret                                          = foc_finish_cali_step(foc);
        if (ret < 0)
            return ret;
        if (lo->u_cali_flag.val != 0) {
            foc_start_cali_step_delay(foc);
            return -MEBUSY;
        }
    }

    /* 如果正在 ELEC_THETA 偏置校准中, 继续执行 */
    if (lo->u_cali_flag.bit.offset_elec_theta) {
        ret = foc_offset_elec_theta_cali(foc);
        if (ret < 0)
            return ret;

        /* 校准结束 */
        lo->u_cali_flag.bit.offset_elec_theta = false;
        ret                                   = foc_finish_cali_step(foc);
        if (ret < 0)
            return ret;
        if (lo->u_cali_flag.val != 0) {
            foc_start_cali_step_delay(foc);
            return -MEBUSY;
        }
    }

    if (lo->u_cali_flag.val == 0) {
        lo->e_cali_state = FOC_CALI_STATE_INIT;
        (void)foc_set_mode(foc, tmp->e_cali_prev_mode);
        (void)foc_set_elec_theta(foc, tmp->e_cali_prev_elec_theta);
        foc_set_state(foc, FOC_STATE_DISABLE);
        return 0;
    }
    return -MEBUSY;
}

int
foc_resistance_cali(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo, tmp);

    switch (lo->e_cali_state) {
        case FOC_CALI_STATE_INIT: {
            foc_set_elec_theta(foc, FOC_ELEC_THETA_NONE);
            foc_set_mode(foc, FOC_MODE_VOL);
            lo->e_cali_state = FOC_CALI_STATE_CW;
            lo->identify.rs  = 0.0F;

            tmp->cali_cnt = MS2CNT(1000, cfg->ctl_cfg.iq_pi.fs);
            tmp->resistance_cali_timeout_cnt =
                MS2CNT(RESISTANCE_CALI_RAMP_TIMEOUT_MS, cfg->ctl_cfg.iq_pi.fs);
            tmp->lpf_is                           = 0.0F;
            tmp->lpf_vd                           = 0.0F;
            out->v_dq.d                           = 0.0F;
            lo->u_err.bit.resistance_cali_timeout = false;
            break;
        }
        case FOC_CALI_STATE_CW: {
            foc_enable_rt(foc);

            if (in->stator.i_s < 0.5F * cfg->base_cfg.motor.cur_rated) {
                if (--tmp->resistance_cali_timeout_cnt <= 0) {
                    out->v_dq.d                           = 0.0F;
                    lo->u_err.bit.resistance_cali_timeout = true;
                    (void)foc_set_state(foc, FOC_STATE_DISABLE);
                    return -METIMEOUT;
                }

                const float32_t volt_max =
                    MAX(0.0F, in->v_bus * DIV_1_BY_SQRT_3 * cfg->base_cfg.periph.f32_pwm_duty_max);
                out->v_dq.d = MIN(out->v_dq.d + 0.01F, volt_max);
            } else {
                lo->e_cali_state = FOC_CALI_STATE_CCW;
            }

            break;
        }
        case FOC_CALI_STATE_CCW: {
            foc_enable_rt(foc);

            if (--tmp->cali_cnt > 0) {
                LOWPASS(tmp->lpf_is, in->stator.i_s, 100.0F, cfg->ctl_cfg.iq_pi.fs);
                LOWPASS(tmp->lpf_vd, in->stator.v_dq.d, 100.0F, cfg->ctl_cfg.iq_pi.fs);
            } else {
                lo->identify.rs  = tmp->lpf_vd / tmp->lpf_is;
                lo->e_cali_state = FOC_CALI_STATE_FINISH;
            }

            break;
        }
        case FOC_CALI_STATE_FINISH: {
            out->v_dq.d      = 0;
            lo->e_cali_state = FOC_CALI_STATE_INIT;
            foc_disable_rt(foc);
            return 0;
        }
        default:
            break;
    }
    return -MEBUSY;
}

int
foc_inductance_cali(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo, tmp);

    switch (lo->e_cali_state) {
        case FOC_CALI_STATE_INIT: {
            foc_set_elec_theta(foc, FOC_ELEC_THETA_NONE);
            foc_set_mode(foc, FOC_MODE_VOL);
            lo->e_cali_state = FOC_CALI_STATE_CW;
            lo->identify.ld  = 0.0F;
            lo->identify.lq  = 0.0F;

            tmp->cali_cnt = MS2CNT(1, cfg->ctl_cfg.iq_pi.fs);
            break;
        }
        case FOC_CALI_STATE_CW: {
            foc_enable_rt(foc);

            out->v_dq.d      = 0.3F * in->v_bus;
            lo->e_cali_state = FOC_CALI_STATE_CCW;
            break;
        }
        case FOC_CALI_STATE_CCW: {
            foc_enable_rt(foc);

            if (--tmp->cali_cnt > 0) {
                tmp->lpf_is = in->stator.i_s;
                tmp->lpf_vd = in->stator.v_dq.d;
            } else {
                lo->identify.ld = lo->identify.lq = tmp->lpf_vd / (tmp->lpf_is / MS2S(1));
                lo->e_cali_state                  = FOC_CALI_STATE_FINISH;
            }

            break;
        }
        case FOC_CALI_STATE_FINISH: {
            out->v_dq.d      = 0;
            lo->e_cali_state = FOC_CALI_STATE_INIT;
            foc_disable_rt(foc);
            return 0;
        }
        default:
            break;
    }
    return -MEBUSY;
}
