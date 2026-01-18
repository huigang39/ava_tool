#include "mathdef.h"

#include "foc.h"

extern void foc_cali(foc_t *foc);
extern void foc_ready(foc_t *foc);
extern void foc_disable(foc_t *foc);
extern void foc_enable(foc_t *foc);
extern void foc_load_by_cali_bit(foc_t *foc);
extern f32  foc_nonlinear_lookup(f32 theta, const f16 *table, usize table_size);

static void foc_init_param(foc_t *foc);
static void foc_get_adc(foc_t *foc);
static void foc_get_theta(foc_t *foc);

void
foc_init(foc_t *foc, const foc_cfg_t foc_cfg)
{
        CFG_INIT(foc, foc_cfg);
        DECL(foc, cfg, in, lo);

        foc_init_param(foc);
        lo->u_error.bits.null_ptr = !RUN_FUNC_PTR(cfg->func_cfg.f_init_motor_theta_sensor);
        lo->u_error.bits.null_ptr = !RUN_FUNC_PTR(cfg->func_cfg.f_init_outshaft_theta_sensor);
        lo->u_error.bits.null_ptr = !RUN_FUNC_PTR(cfg->func_cfg.f_set_drv_status, 1);
        lo->u_error.bits.null_ptr = !RUN_FUNC_PTR(cfg->func_cfg.f_init_periph);
        foc_load_by_cali_bit(foc);
}

void
foc_exec(foc_t *foc)
{
        DECL(foc, lo);

        lo->exec_cnt++;

        if (lo->u_error.bits.null_ptr)
                return;

        foc_get_adc(foc);
        foc_get_theta(foc);

        switch (lo->e_state) {
                case FOC_STATE_CALI: {
                        foc_cali(foc);
                        break;
                }
                case FOC_STATE_READY: {
                        foc_ready(foc);
                        break;
                }
                case FOC_STATE_DISABLE: {
                        foc_disable(foc);
                        break;
                }
                case FOC_STATE_ENABLE: {
                        foc_enable(foc);
                        break;
                }
                default:
                        break;
        }

        foc_get_fdb(foc);
}

void
foc_set_ref(foc_t *foc, foc_ref_pvct_t ref_pvct)
{
        DECL(foc, cfg, lo);

        switch (lo->e_mode) {
                case FOC_MODE_CUR: {
                        CLAMP(ref_pvct.cur,
                              -MIN(cfg->base_cfg.motor.cur_max, cfg->base_cfg.periph.cur_max),
                              MIN(cfg->base_cfg.motor.cur_max, cfg->base_cfg.periph.cur_max));
                        lo->ref_pvct.cur = ref_pvct.cur * cfg->base_cfg.dir;
                        break;
                }
                case FOC_MODE_VEL: {
                        CLAMP(ref_pvct.vel,
                              -MECH2OUTSHAFT(cfg->base_cfg.motor.vel_max, cfg->base_cfg.outshaft_ratio),
                              MECH2OUTSHAFT(cfg->base_cfg.motor.vel_max, cfg->base_cfg.outshaft_ratio));
                        lo->ref_pvct.vel     = ref_pvct.vel * cfg->base_cfg.dir;
                        lo->ref_pvct.ffd_cur = ref_pvct.ffd_cur * cfg->base_cfg.dir;
                        lo->ref_pvct.ffd_tor = ref_pvct.ffd_tor * cfg->base_cfg.dir;
                        break;
                }
                case FOC_MODE_POS:
                case FOC_MODE_PD: {
                        lo->ref_pvct.pos     = ref_pvct.pos * cfg->base_cfg.dir;
                        lo->ref_pvct.ffd_vel = ref_pvct.ffd_vel * cfg->base_cfg.dir;
                        lo->ref_pvct.ffd_cur = ref_pvct.ffd_cur * cfg->base_cfg.dir;
                        lo->ref_pvct.ffd_tor = ref_pvct.ffd_tor * cfg->base_cfg.dir;
                        break;
                }
                default:
                        break;
        }
}

foc_fdb_pvct_t
foc_get_fdb(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        // 使用电机端编码器角度计算的出轴角度作为位置反馈
        lo->fdb_pvct.pos = in->rotor.est_outshaft_total_theta;
        lo->fdb_pvct.vel = in->rotor.est_outshaft_omega;
        lo->fdb_pvct.cur = in->i_dq.q;

        // 电磁力矩计算
        lo->fdb_pvct.elec_tor =
            CPYSGN(poly_eval(cfg->base_cfg.motor.cur2tor, ARRAY_LEN(cfg->base_cfg.motor.cur2tor) - 1, ABS(in->i_dq.q)),
                   in->i_dq.q) *
            cfg->base_cfg.outshaft_ratio;

        const foc_fdb_pvct_t fdb_pvct = {
            .pos      = lo->fdb_pvct.pos * cfg->base_cfg.dir,
            .vel      = lo->fdb_pvct.vel * cfg->base_cfg.dir,
            .cur      = lo->fdb_pvct.cur * cfg->base_cfg.dir,
            .elec_tor = lo->fdb_pvct.elec_tor * cfg->base_cfg.dir,
        };

        return fdb_pvct;
}

static void
foc_get_theta(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        /* -------------------------------------------------------------------------- */
        /*                                  电机角度                                  */
        /* -------------------------------------------------------------------------- */

        // 电机角度获取
        in->rotor.motor_theta_raw = cfg->func_cfg.f_get_motor_theta();
        in->rotor.motor_theta     = TOGGLE_THETA(cfg->sensor_cfg.theta_dir, in->rotor.motor_theta_raw);

        // 电机角速度计算
        RENAME(&lo->pll, pll)
        pll_exec_theta_in(pll, in->rotor.motor_theta);
        in->rotor.motor_omega = pll->out.lpf_omega;

        in->rotor.motor_comp_theta = cfg->sensor_cfg.motor_theta_comp_gain * in->rotor.motor_omega / cfg->base_cfg.exec_freq;

        // 默认使用传感器角度
        switch (cfg->sensor_cfg.e_sensor) {
                case FOC_SENSOR_ELEC: {
                        // 电气角度 -> 机械角度
                        WARP_TAU(in->rotor.elec_theta, in->rotor.motor_theta + in->rotor.motor_comp_theta);
                        WARP_TAU(in->rotor.mech_theta, ELEC2MECH(in->rotor.elec_theta, cfg->base_cfg.motor.npp));

                        in->rotor.elec_omega = in->rotor.motor_omega;
                        in->rotor.mech_omega = ELEC2MECH(in->rotor.elec_omega, cfg->base_cfg.motor.npp);
                        break;
                }
                case FOC_SENSOR_MECH: {
                        // 机械角度 -> 电气角度
                        WARP_TAU(in->rotor.mech_theta, in->rotor.motor_theta + in->rotor.motor_comp_theta);
                        WARP_TAU(in->rotor.elec_theta, MECH2ELEC(in->rotor.mech_theta, cfg->base_cfg.motor.npp));

                        in->rotor.mech_omega = in->rotor.motor_omega;
                        in->rotor.elec_omega = MECH2ELEC(in->rotor.mech_omega, cfg->base_cfg.motor.npp);
                        break;
                }
                default:
                        break;
        }

        if (lo->e_theta == FOC_THETA_SENSOR) {
                WARP_TAU(in->rotor.theta, in->rotor.elec_theta - lo->store.offset.theta);
                in->rotor.omega = in->rotor.elec_omega;
        }

        // 角度累计
        CYCLE_CNT(in->rotor.theta_cycle_cnt, in->rotor.theta, in->rotor.prev_theta);
        in->rotor.total_theta      = (f32)in->rotor.theta_cycle_cnt * TAU + in->rotor.theta;
        in->rotor.mech_total_theta = ELEC2MECH(in->rotor.total_theta, cfg->base_cfg.motor.npp);

        /* -------------------------------------------------------------------------- */
        /*                                  出轴角度                                  */
        /* -------------------------------------------------------------------------- */

        // 出轴角度获取
        in->rotor.outshaft_theta_raw = cfg->func_cfg.f_get_outshaft_theta();
        in->rotor.outshaft_comp_theta =
            (cfg->sensor_cfg.outshaft_sensor_comp_enable && lo->store.info.cali_flag.bits.nonlinear_outshaft_theta)
                ? TOGGLE_THETA(cfg->sensor_cfg.outshaft_theta_dir,
                               foc_nonlinear_lookup(in->rotor.outshaft_theta_raw,
                                                    lo->store.nonlinear.outshaft_theta,
                                                    ARRAY_LEN(lo->store.nonlinear.outshaft_theta)))
                : 0.0f;
        WARP_TAU(
            in->rotor.outshaft_theta,
            TOGGLE_THETA(cfg->sensor_cfg.outshaft_theta_dir, in->rotor.outshaft_theta_raw + in->rotor.outshaft_comp_theta));

        // 出轴角速度计算
        in->rotor.outshaft_omega = MECH2OUTSHAFT(in->rotor.mech_omega, cfg->base_cfg.outshaft_ratio);

        // 出轴角度累计
        CYCLE_CNT(in->rotor.outshaft_cycle_cnt, in->rotor.outshaft_theta, in->rotor.prev_outshaft_theta);
        in->rotor.outshaft_total_theta =
            (f32)in->rotor.outshaft_cycle_cnt * TAU + in->rotor.outshaft_theta - lo->store.offset.outshaft_theta;

        // 上电对齐电机角度传感器角度
        in->rotor.motor_theta_offset =
            in->rotor.motor_theta_offset == 0.0f ? in->rotor.mech_total_theta : in->rotor.motor_theta_offset;
        in->rotor.est_outshaft_total_theta =
            MECH2OUTSHAFT(in->rotor.mech_total_theta - in->rotor.motor_theta_offset, cfg->base_cfg.outshaft_ratio);

        // 上电对齐出轴角度传感器角度
        in->rotor.outshaft_theta_offset =
            in->rotor.outshaft_theta_offset == 0.0f ? in->rotor.outshaft_theta : in->rotor.outshaft_theta_offset;
        in->rotor.est_outshaft_total_theta +=
            cfg->sensor_cfg.outshaft_sensor_enable ? in->rotor.outshaft_theta_offset - lo->store.offset.outshaft_theta : 0;
        WARP_TAU(in->rotor.est_outshaft_theta, in->rotor.est_outshaft_total_theta + lo->store.offset.outshaft_theta);

        // 电机角速度 -> 出轴角速度
        in->rotor.est_outshaft_omega = MECH2OUTSHAFT(in->rotor.mech_omega, cfg->base_cfg.outshaft_ratio);

        // 更新双编误差
        WARP_PI(in->rotor.outshaft_theta_err, in->rotor.est_outshaft_theta - in->rotor.outshaft_theta);
        in->rotor.outshaft_theta_err_max = MAX(in->rotor.outshaft_theta_err, in->rotor.outshaft_theta_err_max);
        in->rotor.outshaft_theta_err_min = MIN(in->rotor.outshaft_theta_err, in->rotor.outshaft_theta_err_min);
        in->rotor.outshaft_theta_err_pp  = in->rotor.outshaft_theta_err_max - in->rotor.outshaft_theta_err_min;
}

static void
foc_get_adc(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        // ADC采样
        in->adc_raw = cfg->func_cfg.f_get_adc();
        UVW_SUB_VEC(in->adc_raw.i32_i_uvw, in->adc_raw.i32_i_uvw, lo->store.offset.adc.i32_i_uvw);
        UVW_MUL(in->f32_i_uvw, in->adc_raw.i32_i_uvw, cfg->base_cfg.periph.adc2cur);
        in->v_bus = (f32)in->adc_raw.i32_v_bus * cfg->base_cfg.periph.adc2vbus;
}

static void
foc_init_param(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        cfg->base_cfg.periph.adc2cur      = 2.0f * cfg->base_cfg.periph.cur_max / cfg->base_cfg.periph.adc_full_cnt;
        cfg->base_cfg.periph.adc2vbus     = cfg->base_cfg.periph.vbus_max / cfg->base_cfg.periph.adc_full_cnt;
        cfg->base_cfg.periph.pwm_full_cnt = cfg->base_cfg.periph.timer_freq / cfg->base_cfg.periph.pwm_freq;

        lo->pll.cfg.fs    = cfg->base_cfg.exec_freq;
        lo->smo.cfg.fs    = cfg->base_cfg.exec_freq;
        lo->smo.cfg.motor = cfg->base_cfg.motor;
        lo->hfi.cfg.fs    = cfg->base_cfg.exec_freq;
        lo->lbg.cfg.fs    = cfg->base_cfg.exec_freq;
        lo->lbg.cfg.motor = cfg->base_cfg.motor;

        cfg->ctl_cfg.cur.fs = cfg->base_cfg.exec_freq / cfg->ctl_cfg.div.cur;
        cfg->ctl_cfg.vel.fs = cfg->base_cfg.exec_freq / cfg->ctl_cfg.div.vel;
        cfg->ctl_cfg.pos.fs = cfg->base_cfg.exec_freq / cfg->ctl_cfg.div.pos;
        cfg->ctl_cfg.pd.fs  = cfg->base_cfg.exec_freq / cfg->ctl_cfg.div.pd;

        cfg->ctl_cfg.vel.ki_out_max = cfg->ctl_cfg.vel.out_max = MIN(cfg->base_cfg.motor.cur_max, cfg->base_cfg.periph.cur_max);
        cfg->ctl_cfg.vel.ref_rate_max                          = cfg->base_cfg.acc_max;
        cfg->ctl_cfg.pos.ki_out_max = cfg->ctl_cfg.pos.out_max = cfg->base_cfg.motor.vel_max;
        cfg->ctl_cfg.pd.ki_out_max = cfg->ctl_cfg.pd.out_max = cfg->base_cfg.motor.tor_max * cfg->base_cfg.outshaft_ratio;

        pid_init(&lo->pd_pid, cfg->ctl_cfg.pd);
        pid_init(&lo->vel_pid, cfg->ctl_cfg.vel);
        pid_init(&lo->pos_pid, cfg->ctl_cfg.pos);
        pid_init(&lo->id_pid, cfg->ctl_cfg.cur);
        pid_init(&lo->iq_pid, cfg->ctl_cfg.cur);

        pll_init(&lo->pll, lo->pll.cfg);
        smo_init(&lo->smo, lo->smo.cfg);
        hfi_init(&lo->hfi, lo->hfi.cfg);
        lbg_init(&lo->lbg, lo->lbg.cfg);
}
