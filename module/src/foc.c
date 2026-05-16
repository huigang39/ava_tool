#include "lut.h"
#include "mathdef.h"
#include "timeops.h"

#include "foc.h"

#define CUR_OFFSET_SAMPLE_MS (10)
static i32 foc_stator_init(foc_t *foc);

extern void foc_select_state(foc_t *foc);
static void foc_update_fdb(foc_t *foc);

static void foc_init_param(foc_t *foc);
static void foc_self_check_init(foc_t *foc);
static void foc_self_check_exec(foc_t *foc);
static void foc_rotor_init(foc_t *foc);
static void foc_get_adc_value(foc_t *foc);
static void foc_get_theta(foc_t *foc);
static void foc_get_tor(foc_t *foc);

void
foc_init(foc_t *foc, const foc_cfg_t foc_cfg)
{
        DECL(foc, cfg, in, lo);
        CFG_INIT(foc, foc_cfg);

        foc_init_param(foc);

        if (!CHECK_FUNC_PTR(cfg->func_cfg.f_load) || !CHECK_FUNC_PTR(cfg->func_cfg.f_init_periph) ||
            !CHECK_FUNC_PTR(cfg->func_cfg.f_init_tor_sensor) || !CHECK_FUNC_PTR(cfg->func_cfg.f_get_tor) ||
            !CHECK_FUNC_PTR(cfg->func_cfg.f_init_motor_sensor) || !CHECK_FUNC_PTR(cfg->func_cfg.f_trigger_motor_theta) ||
            !CHECK_FUNC_PTR(cfg->func_cfg.f_get_motor_theta) || !CHECK_FUNC_PTR(cfg->func_cfg.f_init_outshaft_sensor) ||
            !CHECK_FUNC_PTR(cfg->func_cfg.f_trigger_outshaft_theta) || !CHECK_FUNC_PTR(cfg->func_cfg.f_get_outshaft_theta) ||
            !CHECK_FUNC_PTR(cfg->func_cfg.f_set_drv_status) || !CHECK_FUNC_PTR(cfg->func_cfg.f_set_irq_status)) {
                lo->u_err.bit.null_ptr = TRUE;
                return;
        } else {
                cfg->func_cfg.f_load();

                cfg->func_cfg.f_init_periph();
                cfg->func_cfg.f_init_tor_sensor();
                cfg->func_cfg.f_init_motor_sensor();
                cfg->func_cfg.f_init_outshaft_sensor();

                cfg->func_cfg.f_get_motor_theta();
                cfg->func_cfg.f_get_outshaft_theta();
                cfg->func_cfg.f_get_tor();

                cfg->func_cfg.f_set_drv_status(TRUE);
                cfg->func_cfg.f_set_irq_status(TRUE);
        }

        in->rotor.sensor = lo->store.sensor;

        foc_self_check_init(foc);

        /* 默认电角度源 */
        lo->e_elec_theta = cfg->sensor_cfg.e_elec_theta;
}

void
foc_exec(foc_t *foc)
{
        DECL(foc, cfg, lo);

        lo->tick++;

        if (lo->u_err.bit.null_ptr) {
                cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, FALSE);
                return;
        }

        foc_self_check_exec(foc);

        foc_get_adc_value(foc);
        foc_get_theta(foc);
        foc_get_tor(foc);
        foc_select_state(foc);

        foc_update_fdb(foc);
}

static void
foc_self_check_init(foc_t *foc)
{
        DECL(foc, cfg, lo, tmp);

        if (!cfg->base_cfg.self_check_enable) {
                lo->is_ready = TRUE;
                return;
        }

        tmp->u_prev_obs_flag   = cfg->obs_cfg.u_obs_flag;
        tmp->e_prev_elec_theta = cfg->sensor_cfg.e_elec_theta;

        cfg->obs_cfg.u_obs_flag.bit.hfi = TRUE;
        cfg->sensor_cfg.e_elec_theta    = FOC_ELEC_THETA_SENSORLESS;
}

static void
foc_self_check_exec(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);

        if (!cfg->base_cfg.self_check_enable)
                return;

        if (!in->stator.is_init || !in->rotor.is_init || foc_is_ready(foc))
                return;

        if (lo->tick > MS2CNT(500, cfg->base_cfg.fs)) {
                lo->is_ready            = TRUE;
                cfg->obs_cfg.u_obs_flag = tmp->u_prev_obs_flag;
                lo->e_elec_theta = cfg->sensor_cfg.e_elec_theta = tmp->e_prev_elec_theta;
                lo->e_mode                                      = FOC_MODE_NONE;
                lo->e_state                                     = FOC_STATE_DISABLE;
                return;
        }

        if (!lo->is_ready)
                lo->e_state = FOC_STATE_ENABLE;
}

u32
foc_is_ready(const foc_t *foc)
{
        return foc->in.stator.is_init && foc->in.rotor.is_init && foc->lo.is_ready;
}

i32
foc_set_state(foc_t *foc, const foc_state_e e_state)
{
        foc->lo.e_state = e_state;
        return 0;
}

foc_state_e
foc_get_state(const foc_t *foc)
{
        return foc->lo.e_state;
}

i32
foc_set_mode(foc_t *foc, const foc_mode_e e_mode)
{
        if (foc->lo.e_state == FOC_STATE_CALI)
                return -MEINVAL;

        foc->lo.e_mode = e_mode;
        return 0;
}

foc_mode_e
foc_get_mode(const foc_t *foc)
{
        return foc->lo.e_mode;
}

i32
foc_set_elec_theta(foc_t *foc, const foc_elec_theta_e e_elec_theta)
{
        foc->lo.e_elec_theta = e_elec_theta;
        return 0;
}

foc_elec_theta_e
foc_get_elec_theta(const foc_t *foc)
{
        return foc->lo.e_elec_theta;
}

foc_temp_t
foc_get_temp(const foc_t *foc)
{
        return foc->in.temp;
}

f32
foc_get_vbus(const foc_t *foc)
{
        return foc->in.v_bus;
}

foc_store_t
foc_get_store(const foc_t *foc)
{
        return foc->lo.store;
}

i32
foc_set_outshaft_zero(foc_t *foc)
{
        DECL(foc, cfg, lo);

        lo->store.sensor.outshaft_offset_theta =
            TOGGLE_THETA(lo->store.sensor.outshaft_sensor_dir, cfg->func_cfg.f_get_outshaft_theta());

        cfg->func_cfg.f_store();
        return 0;
}

void
foc_set_cali_flag(foc_t *foc)
{
        foc->lo.u_cali_flag.bit.nonlinear_elec_theta = TRUE;
        foc->lo.u_cali_flag.bit.offset_elec_theta    = TRUE;
}

static void
foc_update_fdb(foc_t *foc)
{
        DECL(foc, cfg, in, lo, out);

        /* 使用电机端编码器角度计算的出轴角度作为位置反馈 */
        out->fdb_pvct.pos = in->rotor.outshaft_est_total_theta;
        out->fdb_pvct.vel = in->rotor.outshaft_est_omega;
        out->fdb_pvct.cur = in->stator.i_dq.q;

        /* 电磁力矩计算 */
        out->fdb_pvct.elec_tor =
            CPYSGN(poly_eval(cfg->base_cfg.motor.cur2tor, ARRAY_LEN(cfg->base_cfg.motor.cur2tor) - 1, ABS(in->stator.i_dq.q)),
                   in->stator.i_dq.q) *
            cfg->base_cfg.outshaft_ratio;

        out->fdb_pvct.load_tor = cfg->sensor_cfg.tor_sensor_enable ? in->load_tor : lo->luenberger.out.est_load_tor;
}

void
foc_set_pvct(foc_t *foc, foc_ref_pvct_t ref_pvct)
{
        DECL(foc, cfg, in, lo);

        switch (lo->e_mode) {
                case FOC_MODE_CUR: {
                        in->ref_pvct.cur = ref_pvct.cur * (f32)cfg->base_cfg.dir;
                        break;
                }
                case FOC_MODE_VEL: {
                        in->ref_pvct.vel     = ref_pvct.vel * (f32)cfg->base_cfg.dir;
                        in->ref_pvct.ffd_cur = ref_pvct.ffd_cur * (f32)cfg->base_cfg.dir;
                        in->ref_pvct.ffd_tor = ref_pvct.ffd_tor * (f32)cfg->base_cfg.dir;
                        break;
                }
                case FOC_MODE_POS:
                case FOC_MODE_PD: {
                        in->ref_pvct.pos     = ref_pvct.pos * (f32)cfg->base_cfg.dir;
                        in->ref_pvct.ffd_vel = ref_pvct.ffd_vel * (f32)cfg->base_cfg.dir;
                        in->ref_pvct.ffd_cur = ref_pvct.ffd_cur * (f32)cfg->base_cfg.dir;
                        in->ref_pvct.ffd_tor = ref_pvct.ffd_tor * (f32)cfg->base_cfg.dir;
                        break;
                }
                default:
                        break;
        }
}

void
foc_set_force_vel_ref(foc_t *foc, const f32 ref_id, const f32 ref_vel, const f32 exec_freq)
{
        DECL(foc, cfg, in, lo);

        lo->ref_i_dq.d   = ref_id;
        in->ref_pvct.vel = ref_vel;

        in->rotor.elec_force_omega = OUTSHAFT2ELEC(in->ref_pvct.vel, cfg->base_cfg.outshaft_ratio, cfg->base_cfg.motor.npp);
        WARP_TAU(in->rotor.elec_force_theta, in->rotor.elec_force_theta + in->rotor.elec_force_omega / exec_freq);
}

foc_fdb_pvct_t
foc_get_fdb_pvct(foc_t *foc)
{
        DECL(foc, cfg, in, lo, out);

        foc_fdb_pvct_t fdb_pvct = {0};
        fdb_pvct.pos            = out->fdb_pvct.pos * (f32)cfg->base_cfg.dir;
        fdb_pvct.vel            = out->fdb_pvct.vel * (f32)cfg->base_cfg.dir;
        fdb_pvct.cur            = out->fdb_pvct.cur * (f32)cfg->base_cfg.dir;
        fdb_pvct.elec_tor       = out->fdb_pvct.elec_tor * (f32)cfg->base_cfg.dir;
        fdb_pvct.load_tor       = cfg->sensor_cfg.tor_sensor_enable ? in->load_tor : out->fdb_pvct.load_tor;
        return fdb_pvct;
}

foc_ref_pvct_t
foc_get_ref_pvct(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        foc_ref_pvct_t ref_pvct = {0};
        ref_pvct.pos            = in->ref_pvct.pos * (f32)cfg->base_cfg.dir;
        ref_pvct.vel            = in->ref_pvct.vel * (f32)cfg->base_cfg.dir;
        ref_pvct.cur            = in->ref_pvct.cur * (f32)cfg->base_cfg.dir;
        ref_pvct.tor            = in->ref_pvct.tor * (f32)cfg->base_cfg.dir;
        ref_pvct.ffd_vel        = in->ref_pvct.ffd_vel * (f32)cfg->base_cfg.dir;
        ref_pvct.ffd_cur        = in->ref_pvct.ffd_cur * (f32)cfg->base_cfg.dir;
        ref_pvct.ffd_tor        = in->ref_pvct.ffd_tor * (f32)cfg->base_cfg.dir;
        return ref_pvct;
}

/**
 * @brief 获取 ADC 采样值
 *
 * @param foc FOC 结构体
 * @return    void
 */
static void
foc_get_adc_value(foc_t *foc)
{
        DECL(foc, cfg, in, out, lo);

        /* ADC 采样 */
        in->adc_raw = cfg->func_cfg.f_get_adc_raw();

        /* 相电流 */
        UVW_MUL(in->stator.f32_i_uvw_raw, in->adc_raw.i32_i_uvw, cfg->base_cfg.periph.adc2cur);
        if (in->stator.is_init)
                UVW_SUB_VEC(in->stator.f32_i_uvw, in->stator.f32_i_uvw_raw, in->stator.cur_offset);
        else
                foc_stator_init(foc);

        if (cfg->sensor_cfg.terminal_volt_sample_enable)
                /* 端电压 */
                UVW_MUL(in->stator.f32_v_uvw, in->adc_raw.i32_v_uvw, cfg->base_cfg.periph.adc2volt);
        else
                in->stator.f32_v_uvw = out->f32_v_uvw;

        /* 线电压 */
        in->stator.f32_line_v_uvw.u = in->stator.f32_v_uvw.u - in->stator.f32_v_uvw.v;
        in->stator.f32_line_v_uvw.v = in->stator.f32_v_uvw.v - in->stator.f32_v_uvw.w;
        in->stator.f32_line_v_uvw.w = in->stator.f32_v_uvw.w - in->stator.f32_v_uvw.u;

        /* 母线电压 */
        in->v_bus = (f32)in->adc_raw.i32_v_bus * cfg->base_cfg.periph.adc2vbus;
}

/**
 * @brief 获取角度(电气角度、机械角度、出轴角度)
 *
 * @param foc FOC 结构体
 * @return    void
 */
static void
foc_get_theta(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);

        /* -------------------------------------------------------------------------- */
        /*                                  电机角度                                  */
        /* -------------------------------------------------------------------------- */

        /* 电机角度获取 */
        if (++tmp->freq_div_cnt.motor_sensor >= cfg->ctl_cfg.div.motor_sensor) {
                cfg->func_cfg.f_trigger_motor_theta();
                tmp->freq_div_cnt.motor_sensor = 0;
        }

        in->rotor.motor_theta_raw = cfg->func_cfg.f_get_motor_theta();

        /* 电机角速度计算 */
        RENAME(&lo->omega_pll, omega_pll)
        pll_exec_theta_in(omega_pll, in->rotor.motor_theta_raw);
        in->rotor.motor_omega = TOGGLE_OMEGA(in->rotor.sensor.motor_sensor_dir, omega_pll->out.lpf_omega);

        /* 电机角度补偿 */
        in->rotor.motor_comp_theta = cfg->sensor_cfg.motor_theta_delay_comp_cycle * in->rotor.motor_omega / cfg->base_cfg.fs;
        in->rotor.motor_theta =
            TOGGLE_THETA(in->rotor.sensor.motor_sensor_dir, in->rotor.motor_theta_raw + in->rotor.motor_comp_theta);

        /* 电机角度累计 */
        CYCLE_CNT(in->rotor.motor_cycle_cnt, in->rotor.motor_theta, in->rotor.motor_prev_theta);
        in->rotor.motor_total_theta =
            (f32)in->rotor.motor_cycle_cnt * TAU + in->rotor.motor_theta - in->rotor.motor_offset_theta;

        /* 始终读取角度传感器反馈的角度 */
        switch (cfg->sensor_cfg.e_sensor) {
                case FOC_SENSOR_ELEC: {
                        /* 电气角度 -> 机械角度 */
                        WARP_TAU(in->rotor.motor_elec_theta, in->rotor.motor_total_theta);
                        WARP_TAU(in->rotor.motor_mech_theta, ELEC2MECH(in->rotor.motor_elec_theta, cfg->base_cfg.motor.npp));

                        in->rotor.motor_mech_total_theta = ELEC2MECH(in->rotor.motor_total_theta, cfg->base_cfg.motor.npp);

                        in->rotor.motor_elec_omega = in->rotor.motor_omega;
                        in->rotor.motor_mech_omega = ELEC2MECH(in->rotor.motor_elec_omega, cfg->base_cfg.motor.npp);
                        break;
                }
                case FOC_SENSOR_MECH: {
                        /* 机械角度 -> 电气角度 */
                        WARP_TAU(in->rotor.motor_mech_theta, in->rotor.motor_total_theta);
                        WARP_TAU(in->rotor.motor_elec_theta, MECH2ELEC(in->rotor.motor_mech_theta, cfg->base_cfg.motor.npp));

                        in->rotor.motor_mech_total_theta = in->rotor.motor_total_theta;

                        in->rotor.motor_mech_omega = in->rotor.motor_omega;
                        in->rotor.motor_elec_omega = MECH2ELEC(in->rotor.motor_mech_omega, cfg->base_cfg.motor.npp);
                        break;
                }
                default:
                        break;
        }

        /* 无感或强拖时将估测电角度转换为机械角度反馈到上层 */
        if (lo->e_elec_theta == FOC_ELEC_THETA_SENSOR) {
                in->rotor.mech_total_theta = in->rotor.motor_mech_total_theta;
                in->rotor.mech_theta       = in->rotor.motor_mech_theta;
                in->rotor.mech_omega       = in->rotor.motor_mech_omega;
        } else {
                in->rotor.mech_total_theta = ELEC2MECH(in->rotor.elec_total_theta, cfg->base_cfg.motor.npp);
                WARP_TAU(in->rotor.mech_theta, in->rotor.mech_total_theta);
                in->rotor.mech_omega = ELEC2MECH(in->rotor.elec_omega, cfg->base_cfg.motor.npp);
        }

        f32 acc = 0;
        DERIVATIVE(acc, in->rotor.mech_omega, in->rotor.mech_prev_omega, 1.0f, cfg->base_cfg.fs);
        LOWPASS(in->rotor.mech_acc, acc, 10.0f, cfg->base_cfg.fs);

        /* -------------------------------------------------------------------------- */
        /*                                  出轴角度                                  */
        /* -------------------------------------------------------------------------- */

        if (cfg->sensor_cfg.outshaft_sensor_enable) {
                /* 出轴角度获取 */
                if (++tmp->freq_div_cnt.outshaft_sensor >= cfg->ctl_cfg.div.outshaft_sensor) {
                        cfg->func_cfg.f_trigger_outshaft_theta();
                        tmp->freq_div_cnt.outshaft_sensor = 0;
                }

                in->rotor.outshaft_theta_raw = cfg->func_cfg.f_get_outshaft_theta();

                in->rotor.outshaft_comp_theta = cfg->sensor_cfg.outshaft_sensor_comp_enable && in->rotor.is_init
                                                    ? TOGGLE_THETA(in->rotor.sensor.outshaft_sensor_dir,
                                                                   lut_idx((f32 *)&lo->store.table.outshaft_theta,
                                                                           in->rotor.outshaft_theta_raw,
                                                                           ARRAY_LEN(lo->store.table.outshaft_theta)))
                                                    : 0.0f;
                WARP_TAU(in->rotor.outshaft_theta,
                         TOGGLE_THETA(in->rotor.sensor.outshaft_sensor_dir,
                                      in->rotor.outshaft_theta_raw + in->rotor.outshaft_comp_theta));

                /* 出轴角速度计算 */
                in->rotor.outshaft_omega = MECH2OUTSHAFT(in->rotor.mech_omega, cfg->base_cfg.outshaft_ratio);

                /* 出轴角度累计 */
                CYCLE_CNT(in->rotor.outshaft_cycle_cnt, in->rotor.outshaft_theta, in->rotor.outshaft_prev_theta);
                in->rotor.outshaft_total_theta = (f32)in->rotor.outshaft_cycle_cnt * TAU + in->rotor.outshaft_theta -
                                                 lo->store.sensor.outshaft_offset_theta - in->rotor.outshaft_offset_theta;
        }

        in->rotor.outshaft_est_total_theta =
            MECH2OUTSHAFT(in->rotor.mech_total_theta - in->rotor.motor_init_theta, cfg->base_cfg.outshaft_ratio) +
            in->rotor.outshaft_init_theta;
        WARP_PI(in->rotor.outshaft_est_theta, in->rotor.outshaft_est_total_theta);

        /* 电机机械角速度 -> 出轴角速度 */
        in->rotor.outshaft_est_omega = MECH2OUTSHAFT(in->rotor.mech_omega, cfg->base_cfg.outshaft_ratio);

        /* 更新双编误差 */
        f32 outshaft_theta_err_max = 0, outshaft_theta_err_min = 0;
        WARP_PI(in->rotor.outshaft_theta_err, in->rotor.outshaft_est_theta - in->rotor.outshaft_theta);
        outshaft_theta_err_max          = MAX(in->rotor.outshaft_theta_err, outshaft_theta_err_max);
        outshaft_theta_err_min          = MIN(in->rotor.outshaft_theta_err, outshaft_theta_err_min);
        in->rotor.outshaft_theta_err_pp = outshaft_theta_err_max - outshaft_theta_err_min;

        foc_rotor_init(foc);
}

static void
foc_get_tor(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);

        if (++tmp->freq_div_cnt.tor_sensor >= cfg->ctl_cfg.div.tor_sensor)
                tmp->freq_div_cnt.tor_sensor = 0;

        in->load_tor = cfg->func_cfg.f_get_tor();
}

/**
 * @brief FOC 结构体参数初始化
 *
 * @param foc FOC 结构体
 * @return    void
 */
static void
foc_init_param(foc_t *foc)
{
        DECL(foc, cfg, in, lo);
        CFG_INIT(&lo->omega_pll, cfg->obs_cfg.omega_pll);
        CFG_INIT(&lo->luenberger, cfg->obs_cfg.luenberger);
        CFG_INIT(&lo->hfi, cfg->obs_cfg.hfi);
        CFG_INIT(&lo->smo, cfg->obs_cfg.smo);

        /* 电流采样转换系数 */
        cfg->base_cfg.periph.adc2cur = 2.0f * cfg->base_cfg.periph.cur_max / (f32)cfg->base_cfg.periph.adc_full_cnt;

        /* PWM 周期计算 */
        cfg->base_cfg.periph.pwm_full_cnt = cfg->base_cfg.periph.timer_freq / cfg->base_cfg.periph.pwm_freq;

        /* 电流环 PI 参数 */
        CUR_KP(cfg->ctl_cfg.id_pi.kp, cfg->ctl_cfg.cur_wc, cfg->base_cfg.motor.ld);
        CUR_KI(cfg->ctl_cfg.id_pi.ki, cfg->ctl_cfg.cur_wc, cfg->base_cfg.motor.rs);

        CUR_KP(cfg->ctl_cfg.iq_pi.kp, cfg->ctl_cfg.cur_wc, cfg->base_cfg.motor.lq);
        CUR_KI(cfg->ctl_cfg.iq_pi.ki, cfg->ctl_cfg.cur_wc, cfg->base_cfg.motor.rs);

        /* 速度环输出限幅 */
        cfg->ctl_cfg.vel_pi.out_max = cfg->ctl_cfg.vel_pi.ki_out_max =
            MIN(cfg->base_cfg.motor.cur_peak, cfg->base_cfg.periph.cur_max);
        cfg->ctl_cfg.vel_pi.out_min = cfg->ctl_cfg.vel_pi.ki_out_min = -cfg->ctl_cfg.vel_pi.out_max;
        cfg->ctl_cfg.vel_pi.ref_rate_max                             = cfg->base_cfg.acc_max;

        /* 位置环输出限幅 */
        cfg->ctl_cfg.pos_p.out_max = cfg->ctl_cfg.pos_p.ki_out_max = cfg->base_cfg.motor.vel_peak;
        cfg->ctl_cfg.pos_p.out_min = cfg->ctl_cfg.pos_p.ki_out_min = -cfg->ctl_cfg.pos_p.out_max;

        /* PD 环输出限幅 */
        cfg->ctl_cfg.pos_vel_pd.out_max = cfg->ctl_cfg.pos_vel_pd.ki_out_max =
            cfg->base_cfg.motor.tor_peak * cfg->base_cfg.outshaft_ratio;
        cfg->ctl_cfg.pos_vel_pd.out_min = cfg->ctl_cfg.pos_vel_pd.ki_out_min = -cfg->ctl_cfg.pos_vel_pd.out_max;

        /* 电流环 PI 控制器初始化 */
        cfg->ctl_cfg.id_pi.fs = cfg->ctl_cfg.iq_pi.fs = cfg->base_cfg.fs / cfg->ctl_cfg.div.cur;
        pid_init(&lo->id_pi, cfg->ctl_cfg.id_pi);
        pid_init(&lo->iq_pi, cfg->ctl_cfg.iq_pi);

        /* 力矩环 PI 控制器初始化 */
        cfg->ctl_cfg.tor_pi.fs = cfg->base_cfg.fs / cfg->ctl_cfg.div.tor;
        pid_init(&lo->tor_pi, cfg->ctl_cfg.tor_pi);

        /* 弱磁环 PI 控制器初始化 */
        cfg->ctl_cfg.flux_week_pi.fs = cfg->base_cfg.fs / cfg->ctl_cfg.div.flux_week;
        pid_init(&lo->flux_week_pi, cfg->ctl_cfg.flux_week_pi);

        /* 速度环 PI 控制器初始化 */
        cfg->ctl_cfg.vel_pi.fs = cfg->base_cfg.fs / cfg->ctl_cfg.div.vel;
        pid_init(&lo->vel_pi, cfg->ctl_cfg.vel_pi);

        /* 位置环 P 控制器初始化 */
        cfg->ctl_cfg.pos_p.fs = cfg->base_cfg.fs / cfg->ctl_cfg.div.pos;
        pid_init(&lo->pos_p, cfg->ctl_cfg.pos_p);

        /* PD 环 PD 控制器初始化 */
        cfg->ctl_cfg.pos_vel_pd.fs = cfg->base_cfg.fs / cfg->ctl_cfg.div.pd;
        pid_init(&lo->pos_vel_pd, cfg->ctl_cfg.pos_vel_pd);

        /* 电机参数设置 */
        cfg->obs_cfg.luenberger.motor = cfg->obs_cfg.smo.motor = &cfg->base_cfg.motor;

        cfg->obs_cfg.omega_pll.fs = cfg->obs_cfg.luenberger.fs = cfg->obs_cfg.smo.fs = cfg->obs_cfg.hfi.fs = cfg->base_cfg.fs;

        pll_init(&lo->omega_pll, cfg->obs_cfg.omega_pll);
        luenberger_init(&lo->luenberger, cfg->obs_cfg.luenberger);
        smo_init(&lo->smo, cfg->obs_cfg.smo);
        hfi_init(&lo->hfi, cfg->obs_cfg.hfi);
        rls_init(&lo->rls, cfg->obs_cfg.rls);
}

/**
 * @brief 获取电机轴/出轴编码器上电初始位置
 *
 * @param foc FOC 结构体
 * @return    void
 */
static void
foc_rotor_init(foc_t *foc)
{
        DECL(foc, in);

        if (in->rotor.is_init)
                return;

        WARP_PI(in->rotor.motor_init_theta, in->rotor.motor_total_theta);
        WARP_PI(in->rotor.outshaft_init_theta, in->rotor.outshaft_total_theta);

        in->rotor.motor_offset_theta = (in->rotor.motor_total_theta > PI || in->rotor.motor_total_theta < -PI) ? TAU : 0.0f;
        in->rotor.outshaft_offset_theta =
            (in->rotor.outshaft_total_theta > PI || in->rotor.outshaft_total_theta < -PI) ? TAU : 0.0f;

        in->rotor.is_init = TRUE;
}

/**
 * @brief 三相电流偏置校准
 *
 * @param foc FOC 结构体
 * @return    int 状态码
 */
static i32
foc_stator_init(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);

        cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, TRUE);

        /* ADC 还未采样到电流 */
        if (in->adc_raw.i32_i_uvw.u == 0.0f || in->adc_raw.i32_i_uvw.v == 0.0f || in->adc_raw.i32_i_uvw.w == 0.0f)
                return -MEBUSY;

        UVW_ADD_VEC(tmp->cur_offset_sum, tmp->cur_offset_sum, in->stator.f32_i_uvw_raw);

        if (++tmp->cur_offset_cali_cnt >= MS2CNT(CUR_OFFSET_SAMPLE_MS, cfg->base_cfg.fs)) {
                UVW_DIV(in->stator.cur_offset, tmp->cur_offset_sum, tmp->cur_offset_cali_cnt);
                in->stator.is_init = TRUE;
                cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, FALSE);
                return 0;
        }
        return -MEBUSY;
}
