#include "benchmark.h"
#include "errdef.h"
#include "fir.h"
#include "lut.h"
#include "macrodef.h"
#include "mathdef.h"
#include "motor_control/focdef.h"
#include "timeops.h"
#include <stddef.h>
#include <stdint.h>

#include "motor_control/foc.h"

#define CUR_OFFSET_SAMPLE_MS  (10)
#define OFFSET_CALI_SETTLE_MS (1)
static int foc_stator_init(struct foc *foc);

extern void foc_update_state_rt(struct foc *foc);
static void foc_update_fdb_rt(struct foc *foc);

static void foc_init_param(struct foc *foc);
static void foc_self_check_init(struct foc *foc);
static void foc_self_check_exec_rt(struct foc *foc);
static void foc_rotor_init(struct foc *foc);
static void foc_get_adc_value_rt(struct foc *foc);
static void foc_get_theta_rt(struct foc *foc);
static void foc_get_tor_rt(struct foc *foc);

static inline void
foc_update_stage_time_rt(struct foc_stage_time *time, const uint32_t start_cyccnt)
{
    const uint32_t elapsed = benchmark_read_cyccnt_rt() - start_cyccnt;
    time->last_cyccnt      = elapsed;
    if (elapsed > time->max_cyccnt)
        time->max_cyccnt = elapsed;
}

static inline void
foc_update_state_time_rt(struct foc_state_time *time, const uint32_t start_cyccnt)
{
    const uint32_t elapsed = benchmark_read_cyccnt_rt() - start_cyccnt;
    time->last_cyccnt      = elapsed;
    if (elapsed > time->max_cyccnt)
        time->max_cyccnt = elapsed;
}

void
foc_init(struct foc *foc, const struct foc_cfg foc_cfg)
{
    RESET(foc, cfg, in, out, lo, tmp);
    CFG_INIT(foc, foc_cfg);
    DECL(foc, cfg, in, lo, tmp);

    /* 不含 CFG_INIT, 复用 foc->cfg 上的在线修改, 供失能态热重载参数 */
    if (cfg->func_cfg.f_type_resolve)
        cfg->func_cfg.f_type_resolve(&foc->cfg);
    else
        goto null_ptr;

    if (cfg->func_cfg.f_load)
        cfg->func_cfg.f_load();
    else
        goto null_ptr;

    if (!cfg->func_cfg.f_store)
        goto null_ptr;

    if (cfg->func_cfg.f_init_periph)
        cfg->func_cfg.f_init_periph();
    else
        goto null_ptr;

    if (cfg->func_cfg.f_init_motor_sensor)
        cfg->func_cfg.f_init_motor_sensor();
    else
        goto null_ptr;

    if (!cfg->func_cfg.f_trigger_motor_theta || !cfg->func_cfg.f_get_motor_theta)
        goto null_ptr;

    if (cfg->sensor_cfg.outshaft_sensor_enable) {
        if (!cfg->func_cfg.f_init_outshaft_sensor)
            goto null_ptr;

        /* 同一物理传感器可能同时提供电机端和出轴端角度(例如 GW).
         *
         * 两个无参初始化回调相同时只初始化一次,避免重复 Abort/重启同一 UART DMA. */
        if (cfg->func_cfg.f_init_outshaft_sensor != cfg->func_cfg.f_init_motor_sensor)
            cfg->func_cfg.f_init_outshaft_sensor();

        if (!cfg->func_cfg.f_trigger_outshaft_theta || !cfg->func_cfg.f_get_outshaft_theta)
            goto null_ptr;
    }

    if (cfg->sensor_cfg.tor_sensor_enable) {
        if (cfg->func_cfg.f_init_tor_sensor)
            cfg->func_cfg.f_init_tor_sensor();
        else
            goto null_ptr;

        if (!cfg->func_cfg.f_get_tor)
            goto null_ptr;
    }

    if (cfg->func_cfg.f_set_drv_status)
        cfg->func_cfg.f_set_drv_status(true);
    else
        goto null_ptr;

    /* 加载保存的传感器校准数据 */
    if (IS_NAN(lo->store.sensor.elec_theta_offset))
        lo->store.sensor.elec_theta_offset = 0.0F;
    if (IS_NAN(lo->store.sensor.outshaft_theta_offset))
        lo->store.sensor.outshaft_theta_offset = 0.0F;
    if (IS_NAN(lo->store.sensor.tor_offset))
        lo->store.sensor.tor_offset = 0.0F;

    in->rotor.sensor = lo->store.sensor;

    /* 复位上电初始化的运行时状态: 热重载后需重新做电流偏置校准与转子初始位置获取
       (冷启动时这些靠 g_foc 零初始化保证, 重载必须显式复位, 否则沿用旧偏置) */
    in->stator.is_init            = false;
    in->rotor.is_init             = false;
    lo->is_ready                  = false;
    tmp->cur_offset_cali_cnt      = 0;
    tmp->offset_cali_settle_cnt   = 0;
    tmp->cur_offset_sum           = (struct f32_uvw){0};
    tmp->volt_offset_sum          = (struct f32_uvw){0};
    tmp->outshaft_theta_est_valid = false;
    in->stator.volt_offset        = (struct f32_uvw){0};

    foc_init_param(foc);
    foc_self_check_init(foc);
    foc_set_cali_flag(foc);

    /* 默认电角度源 */
    foc_set_elec_theta(foc, cfg->sensor_cfg.e_elec_theta);

    if (cfg->func_cfg.f_set_irq_status)
        cfg->func_cfg.f_set_irq_status(true);
    else
        goto null_ptr;

    return;

null_ptr:
    lo->u_err.bit.null_ptr = true;
}

void
foc_exec_rt(struct foc *foc)
{
    DECL(foc, cfg, lo);
    uint32_t start_cyccnt;

    lo->tick++;

    if (lo->u_err.bit.null_ptr) {
        cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, false);
        return;
    }

    start_cyccnt = benchmark_read_cyccnt_rt();
    foc_self_check_exec_rt(foc);
    foc_update_stage_time_rt(&lo->exec_time.self_check, start_cyccnt);

    start_cyccnt = benchmark_read_cyccnt_rt();
    foc_get_adc_value_rt(foc);
    foc_update_stage_time_rt(&lo->exec_time.adc, start_cyccnt);

    start_cyccnt = benchmark_read_cyccnt_rt();
    foc_get_theta_rt(foc);
    foc_update_stage_time_rt(&lo->exec_time.theta, start_cyccnt);

    start_cyccnt = benchmark_read_cyccnt_rt();
    foc_get_tor_rt(foc);
    foc_update_stage_time_rt(&lo->exec_time.tor, start_cyccnt);

    start_cyccnt = benchmark_read_cyccnt_rt();
    foc_update_state_rt(foc);
    foc_update_state_time_rt(&lo->exec_time.state, start_cyccnt);

    start_cyccnt = benchmark_read_cyccnt_rt();
    foc_update_fdb_rt(foc);
    foc_update_stage_time_rt(&lo->exec_time.fdb, start_cyccnt);
}

static void
foc_self_check_init(struct foc *foc)
{
    DECL(foc, cfg, lo, tmp);

    if (!cfg->base_cfg.self_check_enable) {
        lo->is_ready = true;
        return;
    }

    tmp->u_prev_obs_flag   = cfg->obs_cfg.u_obs_flag;
    tmp->e_prev_elec_theta = cfg->sensor_cfg.e_elec_theta;

    cfg->obs_cfg.u_obs_flag.bit.hfi = true;
    cfg->sensor_cfg.e_elec_theta    = FOC_ELEC_THETA_SENSORLESS;
}

void
foc_self_check_exec_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, tmp);

    if (!cfg->base_cfg.self_check_enable)
        return;

    if (!in->stator.is_init || foc_is_ready(foc))
        return;

    if (lo->tick > MS2CNT(500, cfg->ctl_cfg.iq_pi.fs)) {
        lo->is_ready                 = true;
        cfg->obs_cfg.u_obs_flag      = tmp->u_prev_obs_flag;
        cfg->sensor_cfg.e_elec_theta = tmp->e_prev_elec_theta;
        foc_set_elec_theta(foc, tmp->e_prev_elec_theta);
        foc_set_mode(foc, FOC_MODE_NONE);
        foc_set_state(foc, FOC_STATE_DISABLE);
        return;
    }

    if (!lo->is_ready)
        foc_set_state(foc, FOC_STATE_ENABLE);
}

uint32_t
foc_is_ready(const struct foc *foc)
{
    return foc->in.stator.is_init && foc->lo.is_ready;
}

int
foc_set_state(struct foc *foc, const enum foc_update_state e_state)
{
    if (e_state == foc->lo.e_state)
        return -MEINVAL;

    const enum foc_update_state e_prev_state = foc->lo.e_state;

    if (e_state == FOC_STATE_CALI) {
        foc->tmp.e_cali_prev_mode       = foc->lo.e_mode;
        foc->tmp.e_cali_prev_elec_theta = foc->lo.e_elec_theta;
        foc->tmp.cali_step_delay_cnt    = 0;
    } else if (e_prev_state == FOC_STATE_CALI) {
        /* 无论正常结束、故障还是外部失能, 只要离开 CALI 就统一中止

         * 未完成步骤并恢复进入校准前的模式和电角度源. */
        foc->lo.u_cali_flag.val      = 0;
        foc->lo.e_cali_state         = FOC_CALI_STATE_INIT;
        foc->tmp.cali_step_delay_cnt = 0;
        (void)foc_set_mode(foc, foc->tmp.e_cali_prev_mode);
        (void)foc_set_elec_theta(foc, foc->tmp.e_cali_prev_elec_theta);
    }

    foc->tmp.e_prev_state = e_prev_state;
    foc->lo.e_state       = e_state;

    if (foc->tmp.e_prev_state == FOC_STATE_DISABLE && foc->lo.e_state == FOC_STATE_ENABLE) {
        foc->in.stator.is_init          = false;
        foc->tmp.cur_offset_cali_cnt    = 0;
        foc->tmp.offset_cali_settle_cnt = 0;
        foc->tmp.cur_offset_sum         = (struct f32_uvw){0};
        foc->tmp.volt_offset_sum        = (struct f32_uvw){0};
    }

    return 0;
}

enum foc_update_state
foc_get_state(const struct foc *foc)
{
    return foc->lo.e_state;
}

int
foc_set_mode(struct foc *foc, const enum foc_mode e_mode)
{
    if (e_mode == foc->lo.e_mode)
        return -MEINVAL;

    const enum foc_mode e_prev_mode = foc->lo.e_mode;

    /* foc_set_elec_theta() 将进入开环前的角度源统一保存在
       tmp.e_prev_elec_theta；I/F 与 V/F
     * 互切时当前已经是 FORCE,
       函数会直接返回, 因此不会覆盖原角度源. */
    if (e_mode == FOC_MODE_IF || e_mode == FOC_MODE_VF)
        (void)foc_set_elec_theta(foc, FOC_ELEC_THETA_FORCE);

    foc->tmp.e_prev_mode = e_prev_mode;
    foc->lo.e_mode       = e_mode;
    return 0;
}

enum foc_mode
foc_get_mode(const struct foc *foc)
{
    return foc->lo.e_mode;
}

int
foc_set_elec_theta(struct foc *foc, const enum foc_theta e_elec_theta)
{
    if (e_elec_theta == foc->lo.e_elec_theta)
        return -MEINVAL;

    foc->tmp.e_prev_elec_theta = foc->lo.e_elec_theta;
    foc->lo.e_elec_theta       = e_elec_theta;
    rls_reset(&foc->lo.mech_acc_rls);
    foc->tmp.prev_mech_omega    = 0.0F;
    foc->tmp.mech_acc_rls_valid = false;
    foc->in.rotor.mech_acc      = 0.0F;
    return 0;
}

enum foc_theta
foc_get_elec_theta(const struct foc *foc)
{
    return foc->lo.e_elec_theta;
}

struct foc_temp
foc_get_temp(const struct foc *foc)
{
    return foc->in.temp;
}

float32_t
foc_get_vbus(const struct foc *foc)
{
    return foc->in.v_bus;
}

struct foc_store
foc_get_store(const struct foc *foc)
{
    return foc->lo.store;
}

int
foc_set_outshaft_zero(struct foc *foc)
{
    DECL(foc, cfg, lo);

    const float32_t outshaft_theta_raw = f32_finite_or_rt(cfg->func_cfg.f_get_outshaft_theta(),
                                                          lo->store.sensor.outshaft_theta_offset);
    lo->store.sensor.outshaft_theta_offset =
        f32_finite_or_rt(TOGGLE_THETA(lo->store.sensor.outshaft_sensor_dir, outshaft_theta_raw),
                         lo->store.sensor.outshaft_theta_offset);

    cfg->func_cfg.f_store();
    return 0;
}

int
foc_set_tor_zero(struct foc *foc)
{
    DECL(foc, cfg, lo);

    /* 捕获当前扭矩传感器原始读数作为零偏, 之后 in->load_tor 会被扣减为 0 */
    lo->store.sensor.tor_offset = cfg->func_cfg.f_get_tor();

    cfg->func_cfg.f_store();
    return 0;
}

void
foc_set_cali_flag(struct foc *foc)
{
    if (foc->cfg.cali_cfg.nonlinear_motor_theta_cali_motor_vel != 0.0F)
        foc->lo.u_cali_flag.bit.nonlinear_elec_theta =
            foc->cfg.cali_cfg.u_cali_flag.bit.nonlinear_elec_theta;

    foc->lo.u_cali_flag.bit.offset_elec_theta = foc->cfg.cali_cfg.u_cali_flag.bit.offset_elec_theta;

    if (foc->cfg.sensor_cfg.outshaft_sensor_enable &&
        foc->cfg.cali_cfg.nonlinear_outshaft_theta_cali_outshaft_vel != 0.0F)
        foc->lo.u_cali_flag.bit.nonlinear_outshaft_theta =
            foc->cfg.cali_cfg.u_cali_flag.bit.nonlinear_outshaft_theta;

    foc->lo.u_cali_flag.bit.resistance = foc->cfg.cali_cfg.u_cali_flag.bit.resistance;
    foc->lo.u_cali_flag.bit.inductance = foc->cfg.cali_cfg.u_cali_flag.bit.inductance;
}

static void
foc_update_fdb_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, out);

    /* 位置反馈: 全闭环用出轴编码器实测角度, 半闭环用电机轴换算的估计值 */
    /* 全闭环依赖出轴编码器实测值, 未使能出轴编码器时强制回退到半闭环估计值 */
    out->fdb_pvct.pos =
        (cfg->sensor_cfg.outshaft_theta_pos_loop_enable && cfg->sensor_cfg.outshaft_sensor_enable)
            ? in->rotor.outshaft_theta_total      // 全闭环: 出轴实测
            : in->rotor.outshaft_theta_total_est; // 半闭环: 电机轴换算
    out->fdb_pvct.vel =
        foc_vel_from_rads_rt(in->rotor.outshaft_omega_est, cfg->base_cfg.e_vel_unit);
    out->fdb_pvct.cur = in->stator.i_dq.q;

    /* 电磁力矩计算 */
    out->fdb_pvct.elec_tor = CPYSGN(poly_eval_rt(cfg->base_cfg.cur2tor,
                                                 ARRAY_LEN(cfg->base_cfg.cur2tor) - 1,
                                                 ABS(in->stator.i_dq.q)),
                                    in->stator.i_dq.q);

    out->fdb_pvct.load_tor =
        cfg->sensor_cfg.tor_sensor_enable ? in->load_tor : lo->luenberger.out.est_load_tor;
}

void
foc_set_pvct(struct foc *foc, struct foc_ref_pvct ref_pvct)
{
    DECL(foc, cfg, in, lo);

    switch (lo->e_mode) {
        case FOC_MODE_VF: {
            in->ref_pvct.vel = ref_pvct.vel * (float32_t)cfg->base_cfg.dir;
            break;
        }
        case FOC_MODE_IF: {
            in->ref_pvct.vel = ref_pvct.vel * (float32_t)cfg->base_cfg.dir;
            in->ref_pvct.cur = ref_pvct.cur * (float32_t)cfg->base_cfg.dir;
            break;
        }
        case FOC_MODE_VOL: {
            break;
        }
        case FOC_MODE_CUR: {
            in->ref_pvct.cur = ref_pvct.cur * (float32_t)cfg->base_cfg.dir;
            break;
        }
        case FOC_MODE_TOR: {
            in->ref_pvct.tor = ref_pvct.tor * (float32_t)cfg->base_cfg.dir;
            break;
        }
        case FOC_MODE_VEL: {
            in->ref_pvct.vel     = ref_pvct.vel * (float32_t)cfg->base_cfg.dir;
            in->ref_pvct.ffd_cur = ref_pvct.ffd_cur * (float32_t)cfg->base_cfg.dir;
            in->ref_pvct.ffd_tor = ref_pvct.ffd_tor * (float32_t)cfg->base_cfg.dir;
            break;
        }
        case FOC_MODE_POS:
        case FOC_MODE_PD: {
            in->ref_pvct.pos     = ref_pvct.pos * (float32_t)cfg->base_cfg.dir;
            in->ref_pvct.ffd_vel = ref_pvct.ffd_vel * (float32_t)cfg->base_cfg.dir;
            in->ref_pvct.ffd_cur = ref_pvct.ffd_cur * (float32_t)cfg->base_cfg.dir;
            in->ref_pvct.ffd_tor = ref_pvct.ffd_tor * (float32_t)cfg->base_cfg.dir;
            break;
        }
        case FOC_MODE_RATCHET: {
            in->ref_pvct.ffd_cur = ref_pvct.ffd_cur * (float32_t)cfg->base_cfg.dir;
            in->ref_pvct.ffd_tor = ref_pvct.ffd_tor * (float32_t)cfg->base_cfg.dir;
            break;
        }
        default:
            break;
    }
}

struct foc_fdb_pvct
foc_get_fdb_pvct(struct foc *foc)
{
    DECL(foc, cfg, in, lo, out);

    struct foc_fdb_pvct fdb_pvct = {0};
    fdb_pvct.pos                 = out->fdb_pvct.pos * (float32_t)cfg->base_cfg.dir;
    fdb_pvct.vel                 = out->fdb_pvct.vel * (float32_t)cfg->base_cfg.dir;
    fdb_pvct.cur                 = out->fdb_pvct.cur * (float32_t)cfg->base_cfg.dir;
    fdb_pvct.elec_tor            = out->fdb_pvct.elec_tor * (float32_t)cfg->base_cfg.dir;
    fdb_pvct.load_tor = cfg->sensor_cfg.tor_sensor_enable ? in->load_tor : out->fdb_pvct.load_tor;
    return fdb_pvct;
}

struct foc_ref_pvct
foc_get_ref_pvct(struct foc *foc)
{
    DECL(foc, cfg, in, lo);

    struct foc_ref_pvct ref_pvct = {0};
    ref_pvct.pos                 = in->ref_pvct.pos * (float32_t)cfg->base_cfg.dir;
    ref_pvct.vel                 = in->ref_pvct.vel * (float32_t)cfg->base_cfg.dir;
    ref_pvct.cur                 = in->ref_pvct.cur * (float32_t)cfg->base_cfg.dir;
    ref_pvct.tor                 = in->ref_pvct.tor * (float32_t)cfg->base_cfg.dir;
    ref_pvct.ffd_vel             = in->ref_pvct.ffd_vel * (float32_t)cfg->base_cfg.dir;
    ref_pvct.ffd_cur             = in->ref_pvct.ffd_cur * (float32_t)cfg->base_cfg.dir;
    ref_pvct.ffd_tor             = in->ref_pvct.ffd_tor * (float32_t)cfg->base_cfg.dir;
    return ref_pvct;
}

/**
 * @brief 获取 ADC 采样值
 *
 * @param foc FOC 结构体
 * @return    void
 */
static void
foc_get_adc_value_rt(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo);

    /* 高频回调仅更新电流、电压采样, 保留温度任务写入的低速 ADC 数据 */
    cfg->func_cfg.f_update_adc(foc);

    /* 相电流 */
    UVW_MUL(in->stator.f32_i_uvw_raw, in->adc.raw.i_uvw, cfg->base_cfg.periph.adc2cur);

    /* 端电压原始换算必须先于偏置校准,确保校准使用本周期 ADC 样本. */
    if (cfg->sensor_cfg.terminal_volt_sample_enable)
        UVW_MUL(in->stator.f32_v_uvw_raw, in->adc.raw.v_uvw, cfg->base_cfg.periph.adc2volt);

    if (in->stator.is_init)
        UVW_SUB_VEC(in->stator.f32_i_uvw, in->stator.f32_i_uvw_raw, in->stator.cur_offset);
    else
        foc_stator_init(foc);

    switch (cfg->sensor_cfg.e_loss_phase_cur) {
        case FOC_PHASE_U: {
            in->stator.f32_i_uvw.u = -(in->stator.f32_i_uvw.v + in->stator.f32_i_uvw.w);
            break;
        }
        case FOC_PHASE_V: {
            in->stator.f32_i_uvw.v = -(in->stator.f32_i_uvw.u + in->stator.f32_i_uvw.w);
            break;
        }
        case FOC_PHASE_W: {
            in->stator.f32_i_uvw.w = -(in->stator.f32_i_uvw.u + in->stator.f32_i_uvw.v);
            break;
        }
        default:
            break;
    }

    if (cfg->sensor_cfg.terminal_volt_sample_enable) {
        /* 端电压 */
        UVW_SUB_VEC(in->stator.f32_v_uvw, in->stator.f32_v_uvw_raw, in->stator.volt_offset);

        /* 线电压 */
        in->stator.f32_line_v_uvw.u = in->stator.f32_v_uvw.u - in->stator.f32_v_uvw.v;
        in->stator.f32_line_v_uvw.v = in->stator.f32_v_uvw.v - in->stator.f32_v_uvw.w;
        in->stator.f32_line_v_uvw.w = in->stator.f32_v_uvw.w - in->stator.f32_v_uvw.u;

        in->stator.v_n =
            (in->stator.f32_v_uvw.u + in->stator.f32_v_uvw.v + in->stator.f32_v_uvw.w) * DIV_1_BY_3;
        UVW_SUB(in->stator.f32_phase_v_uvw, in->stator.f32_v_uvw, in->stator.v_n);
    }

    /* 母线电压 */
    in->v_bus = (float32_t)in->adc.raw.v_bus * cfg->base_cfg.periph.adc2vbus;
}

static inline uint8_t
foc_update_motor_sensor_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, tmp);
    uint8_t sensor_triggered = false;

    if (++tmp->freq_div_cnt.motor_sensor >= cfg->ctl_cfg.freq_div.motor_sensor) {
        cfg->func_cfg.f_trigger_motor_theta();
        tmp->freq_div_cnt.motor_sensor = 0;
        sensor_triggered               = true;
    }

    const float32_t theta_raw = cfg->func_cfg.f_get_motor_theta();
    in->rotor.motor_theta_raw =
        f32_is_finite_rt(theta_raw) ? theta_raw : f32_finite_or_rt(in->rotor.motor_theta_raw, 0.0F);

    RENAME(&lo->omega_pll, omega_pll)
    pll_exec_theta_in_rt(omega_pll, in->rotor.motor_theta_raw);
    if (!f32_is_finite_rt(omega_pll->out.lpf_omega)) {
        RESET(&lo->omega_pll, in, out, lo);
        pll_init(&lo->omega_pll, cfg->obs_cfg.omega_pll);
    }

    in->rotor.motor_omega = f32_finite_or_rt(
        TOGGLE_OMEGA(in->rotor.sensor.motor_sensor_dir, omega_pll->out.lpf_omega), 0.0F);
    in->rotor.motor_theta_comp = tmp->motor_theta_comp_gain * in->rotor.motor_omega;
    in->rotor.motor_theta =
        f32_finite_or_rt(TOGGLE_THETA(in->rotor.sensor.motor_sensor_dir,
                                      in->rotor.motor_theta_raw + in->rotor.motor_theta_comp),
                         in->rotor.motor_theta_raw);

    CYCLE_CNT(in->rotor.motor_cycle_cnt, in->rotor.motor_theta, in->rotor.motor_theta_prev);
    in->rotor.motor_theta_total = (float32_t)in->rotor.motor_cycle_cnt * TAU +
                                  in->rotor.motor_theta - in->rotor.motor_theta_total_wrap_offset;

    return sensor_triggered;
}

static inline void
foc_update_motor_theta_rt(struct foc *foc, const uint8_t motor_npp_valid)
{
    DECL(foc, cfg, in, tmp);

    switch (cfg->sensor_cfg.e_motor_sensor) {
        case FOC_SENSOR_ELEC:
            in->rotor.motor_theta_elec = in->rotor.motor_theta;
            in->rotor.motor_omega_elec = in->rotor.motor_omega;
            if (motor_npp_valid) {
                WARP_TAU(in->rotor.motor_theta_mech,
                         in->rotor.motor_theta_elec * tmp->inv_motor_npp);
                in->rotor.motor_theta_mech_total = in->rotor.motor_theta_total * tmp->inv_motor_npp;
                in->rotor.motor_omega_mech       = in->rotor.motor_omega_elec * tmp->inv_motor_npp;
            } else {
                in->rotor.motor_theta_mech       = 0.0F;
                in->rotor.motor_theta_mech_total = 0.0F;
                in->rotor.motor_omega_mech       = 0.0F;
            }
            break;

        case FOC_SENSOR_MECH:
            in->rotor.motor_theta_mech       = in->rotor.motor_theta;
            in->rotor.motor_theta_mech_total = in->rotor.motor_theta_total;
            in->rotor.motor_omega_mech       = in->rotor.motor_omega;
            WARP_TAU(in->rotor.motor_theta_elec,
                     MECH2ELEC(in->rotor.motor_theta_mech, cfg->base_cfg.motor.npp));
            in->rotor.motor_omega_elec =
                MECH2ELEC(in->rotor.motor_omega_mech, cfg->base_cfg.motor.npp);
            break;

        default:
            break;
    }
}

static inline void
foc_update_mech_theta_rt(struct foc *foc, const uint8_t motor_npp_valid)
{
    DECL(foc, cfg, in, lo, tmp);

    if (lo->e_elec_theta == FOC_ELEC_THETA_SENSOR) {
        in->rotor.mech_theta_total = in->rotor.motor_theta_mech_total;
        in->rotor.mech_theta       = in->rotor.motor_theta_mech;
        in->rotor.mech_omega       = in->rotor.motor_omega_mech;
    } else if (motor_npp_valid) {
        in->rotor.mech_theta_total = in->rotor.elec_theta_total * tmp->inv_motor_npp;
        WARP_TAU(in->rotor.mech_theta, in->rotor.mech_theta_total);
        in->rotor.mech_omega = in->rotor.elec_omega * tmp->inv_motor_npp;
    } else {
        in->rotor.mech_theta_total = 0.0F;
        in->rotor.mech_theta       = 0.0F;
        in->rotor.mech_omega       = 0.0F;
    }

    if (!tmp->mech_acc_rls_valid) {
        tmp->prev_mech_omega    = in->rotor.mech_omega;
        tmp->mech_acc_rls_valid = true;
        in->rotor.mech_acc      = 0.0F;
    } else {
        const float32_t raw_acc = (in->rotor.mech_omega - tmp->prev_mech_omega) *
                                  (float32_t)cfg->base_cfg.periph.pwm_freq;
        const float32_t rls_x = 1.0F;
        tmp->prev_mech_omega  = in->rotor.mech_omega;
        rls_exec_in(&lo->mech_acc_rls, raw_acc, &rls_x);
        in->rotor.mech_acc = f32_finite_or_rt(lo->mech_acc_rls.out.w[0], 0.0F);
    }
}

static inline void
foc_update_outshaft_sensor_rt(struct foc   *foc,
                              const uint8_t motor_sensor_triggered,
                              const uint8_t outshaft_ratio_valid)
{
    DECL(foc, cfg, in, lo, tmp);

    if (!cfg->sensor_cfg.outshaft_sensor_enable)
        return;

    if (++tmp->freq_div_cnt.outshaft_sensor >= cfg->ctl_cfg.freq_div.outshaft_sensor) {
        const uint8_t shared_trigger =
            motor_sensor_triggered &&
            cfg->func_cfg.f_trigger_outshaft_theta == cfg->func_cfg.f_trigger_motor_theta;
        if (!shared_trigger)
            cfg->func_cfg.f_trigger_outshaft_theta();
        tmp->freq_div_cnt.outshaft_sensor = 0;
    }

    const float32_t theta_raw    = cfg->func_cfg.f_get_outshaft_theta();
    in->rotor.outshaft_theta_raw = f32_is_finite_rt(theta_raw)
                                       ? theta_raw
                                       : f32_finite_or_rt(in->rotor.outshaft_theta_raw, 0.0F);

    in->rotor.outshaft_theta_comp = 0.0F;
    if (cfg->sensor_cfg.outshaft_sensor_comp_enable && in->rotor.is_init) {
        in->rotor.outshaft_theta_comp =
            TOGGLE_THETA(in->rotor.sensor.outshaft_sensor_dir,
                         lut_idx((float32_t *)&lo->store.table.outshaft_theta,
                                 in->rotor.outshaft_theta_raw,
                                 ARRAY_LEN(lo->store.table.outshaft_theta)));
        in->rotor.outshaft_theta_comp = f32_finite_or_rt(in->rotor.outshaft_theta_comp, 0.0F);
    }

    const float32_t theta =
        TOGGLE_THETA(in->rotor.sensor.outshaft_sensor_dir,
                     in->rotor.outshaft_theta_raw + in->rotor.outshaft_theta_comp);
    WARP_TAU(in->rotor.outshaft_theta, f32_finite_or_rt(theta, in->rotor.outshaft_theta_raw));

    in->rotor.outshaft_omega =
        outshaft_ratio_valid ? in->rotor.mech_omega * tmp->inv_outshaft_ratio : 0.0F;
    CYCLE_CNT(
        in->rotor.outshaft_cycle_cnt, in->rotor.outshaft_theta, in->rotor.outshaft_theta_prev);
    in->rotor.outshaft_theta_total =
        (float32_t)in->rotor.outshaft_cycle_cnt * TAU + in->rotor.outshaft_theta -
        lo->store.sensor.outshaft_theta_offset - in->rotor.outshaft_theta_total_wrap_offset;
}

static inline void
foc_update_outshaft_est_rt(struct foc *foc, const uint8_t outshaft_ratio_valid)
{
    DECL(foc, cfg, in, tmp);

    in->rotor.outshaft_theta_total_est =
        outshaft_ratio_valid
            ? (in->rotor.mech_theta_total - in->rotor.motor_theta_start) * tmp->inv_outshaft_ratio +
                  in->rotor.outshaft_theta_start
            : 0.0F;
    in->rotor.outshaft_omega_est =
        outshaft_ratio_valid ? in->rotor.mech_omega * tmp->inv_outshaft_ratio : 0.0F;

    const float32_t theta_step =
        in->rotor.outshaft_theta_total_est - tmp->prev_outshaft_theta_total_est;
    if (tmp->outshaft_theta_est_valid && f32_is_finite_rt(theta_step) && ABS(theta_step) <= PI)
        in->rotor.outshaft_theta_est = wrap_pi_once_rt(in->rotor.outshaft_theta_est + theta_step);
    else
        WARP_PI(in->rotor.outshaft_theta_est, in->rotor.outshaft_theta_total_est);
    tmp->prev_outshaft_theta_total_est = in->rotor.outshaft_theta_total_est;
    tmp->outshaft_theta_est_valid      = true;

    if (cfg->sensor_cfg.outshaft_sensor_enable) {
        in->rotor.outshaft_theta_err =
            wrap_pi_once_rt(in->rotor.outshaft_theta_est - in->rotor.outshaft_theta);
        in->rotor.outshaft_theta_err_pp = ABS(in->rotor.outshaft_theta_err);
    } else {
        in->rotor.outshaft_theta_err    = 0.0F;
        in->rotor.outshaft_theta_err_pp = 0.0F;
    }
}

/**
 * @brief 更新电机轴和出轴角度反馈
 */
static void
foc_get_theta_rt(struct foc *foc)
{
    DECL(foc, tmp);
    const uint8_t motor_npp_valid      = tmp->inv_motor_npp != 0.0F;
    const uint8_t outshaft_ratio_valid = tmp->inv_outshaft_ratio != 0.0F;

    const uint8_t motor_sensor_triggered = foc_update_motor_sensor_rt(foc);
    foc_update_motor_theta_rt(foc, motor_npp_valid);
    foc_update_mech_theta_rt(foc, motor_npp_valid);
    foc_update_outshaft_sensor_rt(foc, motor_sensor_triggered, outshaft_ratio_valid);
    foc_update_outshaft_est_rt(foc, outshaft_ratio_valid);
    foc_rotor_init(foc);
}

static void
foc_get_tor_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, tmp);

    if (!cfg->sensor_cfg.tor_sensor_enable) {
        in->load_tor = 0.0F;
        return;
    }

    if (++tmp->freq_div_cnt.tor_sensor >= cfg->ctl_cfg.freq_div.tor_sensor)
        tmp->freq_div_cnt.tor_sensor = 0;

    in->load_tor = cfg->func_cfg.f_get_tor() - lo->store.sensor.tor_offset;
}

/**
 * @brief FOC 结构体参数初始化
 *
 * @param foc FOC 结构体
 * @return    void
 */
static void
foc_init_param(struct foc *foc)
{
    DECL(foc, cfg, in, lo, tmp);

    memset(&lo->identify, 0, sizeof(lo->identify));
    CFG_INIT(&lo->omega_pll, cfg->obs_cfg.omega_pll);
    CFG_INIT(&lo->luenberger, cfg->obs_cfg.luenberger);
    CFG_INIT(&lo->hfi, cfg->obs_cfg.hfi);
    CFG_INIT(&lo->smo, cfg->obs_cfg.smo);
    CFG_INIT(&lo->flux, cfg->obs_cfg.flux);

    /* 电流采样转换系数 */
    cfg->base_cfg.periph.adc2cur =
        2.0F * cfg->base_cfg.periph.cur_max / (float32_t)cfg->base_cfg.periph.adc_full_cnt;

    /* PWM 周期计算 */
    cfg->base_cfg.periph.pwm_full_cnt =
        cfg->base_cfg.periph.timer_freq / cfg->base_cfg.periph.pwm_freq;

    /* 电流环 PI 参数 */
    CUR_KP(cfg->ctl_cfg.id_pi.kp, cfg->ctl_cfg.cur_wc, cfg->base_cfg.motor.ld);
    CUR_KI(cfg->ctl_cfg.id_pi.ki, cfg->ctl_cfg.cur_wc, cfg->base_cfg.motor.rs);

    CUR_KP(cfg->ctl_cfg.iq_pi.kp, cfg->ctl_cfg.cur_wc, cfg->base_cfg.motor.lq);
    CUR_KI(cfg->ctl_cfg.iq_pi.ki, cfg->ctl_cfg.cur_wc, cfg->base_cfg.motor.rs);

    const float32_t cur_limit = MIN(cfg->base_cfg.motor.cur_peak, cfg->base_cfg.periph.cur_max);
    const float32_t vel_limit = cfg->base_cfg.motor.vel_peak;
    const float32_t tor_limit = cfg->base_cfg.motor.tor_peak * cfg->base_cfg.reducer.outshaft_ratio;

    /* 力矩环的 PI 修正量可独立于总电流限幅配置, 未配置时沿用总电流限幅. */
    const float32_t tor_pid_out_max = cfg->ctl_cfg.tor_pi.pid_out_max > 0.0F
                                          ? MIN(cfg->ctl_cfg.tor_pi.pid_out_max, cur_limit)
                                          : cur_limit;
    const float32_t tor_pid_out_min = cfg->ctl_cfg.tor_pi.pid_out_min < 0.0F
                                          ? MAX(cfg->ctl_cfg.tor_pi.pid_out_min, -cur_limit)
                                          : -tor_pid_out_max;
    const float32_t tor_ki_out_max  = cfg->ctl_cfg.tor_pi.ki_out_max > 0.0F
                                          ? MIN(cfg->ctl_cfg.tor_pi.ki_out_max, tor_pid_out_max)
                                          : tor_pid_out_max;
    const float32_t tor_ki_out_min  = cfg->ctl_cfg.tor_pi.ki_out_min < 0.0F
                                          ? MAX(cfg->ctl_cfg.tor_pi.ki_out_min, tor_pid_out_min)
                                          : -tor_ki_out_max;

    /* 电流环 PI/ADRC 控制器初始化.ADRC 参数直接使用 ctl_cfg 中的配置. */
    const float32_t cur_fs = (float32_t)cfg->base_cfg.periph.pwm_freq / cfg->ctl_cfg.freq_div.cur;
    FS_INIT(cur_fs, &cfg->ctl_cfg.id_pi, &cfg->ctl_cfg.iq_pi);
    tmp->inv_motor_npp =
        cfg->base_cfg.motor.npp != 0 ? 1.0F / (float32_t)cfg->base_cfg.motor.npp : 0.0F;
    tmp->inv_outshaft_ratio = f32_is_finite_rt(cfg->base_cfg.reducer.outshaft_ratio) &&
                                      cfg->base_cfg.reducer.outshaft_ratio != 0.0F
                                  ? 1.0F / cfg->base_cfg.reducer.outshaft_ratio
                                  : 0.0F;
    tmp->motor_theta_comp_gain =
        cfg->ctl_cfg.iq_pi.fs != 0.0F
            ? cfg->sensor_cfg.motor_theta_delay_comp_cycle / cfg->ctl_cfg.iq_pi.fs
            : 0.0F;
    tmp->pwm_theta_comp_gain =
        cfg->ctl_cfg.iq_pi.fs != 0.0F
            ? cfg->sensor_cfg.pwm_elec_theta_delay_comp_cycle / cfg->ctl_cfg.iq_pi.fs
            : 0.0F;

    pid_init(&lo->id_pi, cfg->ctl_cfg.id_pi);
    pid_init(&lo->iq_pi, cfg->ctl_cfg.iq_pi);
    adrc_init(&lo->id_adrc, cfg->ctl_cfg.id_adrc);
    adrc_init(&lo->iq_adrc, cfg->ctl_cfg.iq_adrc);

    /* 力矩环 PI 控制器初始化 */
    FS_INIT((float32_t)cfg->base_cfg.periph.pwm_freq / cfg->ctl_cfg.freq_div.tor,
            &cfg->ctl_cfg.tor_pi);
    pid_init(&lo->tor_pi, cfg->ctl_cfg.tor_pi);
    pid_set_out_limit(&lo->tor_pi,
                      cur_limit,
                      tor_pid_out_max,
                      tor_ki_out_max,
                      -cur_limit,
                      tor_pid_out_min,
                      tor_ki_out_min);

    /* 弱磁环 PI 控制器初始化 */
    FS_INIT((float32_t)cfg->base_cfg.periph.pwm_freq / cfg->ctl_cfg.freq_div.flux_week,
            &cfg->ctl_cfg.flux_week_pi);
    pid_init(&lo->flux_week_pi, cfg->ctl_cfg.flux_week_pi);

    /* 速度环 PI/ADRC 控制器初始化.ADRC 参数直接使用 ctl_cfg 中的配置. */
    cfg->ctl_cfg.vel_pi.ref_rate_max = cfg->base_cfg.acc_max;
    FS_INIT((float32_t)cfg->base_cfg.periph.pwm_freq / cfg->ctl_cfg.freq_div.vel,
            &cfg->ctl_cfg.vel_pi);
    pid_init(&lo->vel_pi, cfg->ctl_cfg.vel_pi);
    pid_set_out_limit(
        &lo->vel_pi, cur_limit, cur_limit, cur_limit, -cur_limit, -cur_limit, -cur_limit);

    cfg->ctl_cfg.vel_adrc.ref_rate_max = ABS(cfg->base_cfg.acc_max);
    cfg->ctl_cfg.vel_adrc.d_comp_max   = ABS(cfg->ctl_cfg.vel_adrc.d_comp_gain) * cur_limit;
    cfg->ctl_cfg.vel_adrc.out_rate_max = ABS(cfg->ctl_cfg.vel_adrc.out_rate_gain) * cur_limit;
    adrc_init(&lo->vel_adrc, cfg->ctl_cfg.vel_adrc);
    adrc_set_out_limit(&lo->vel_adrc, cur_limit, -cur_limit);

    /* 开环 I/F、V/F 使用各自的斜坡状态, 不复用速度 PI 内部变量 */
    lo->if_ctl.vel_acc_max = ABS(cfg->base_cfg.acc_max);
    lo->vf_ctl.vel_acc_max = ABS(cfg->base_cfg.acc_max);

    /* 位置环 P 控制器初始化 */
    FS_INIT((float32_t)cfg->base_cfg.periph.pwm_freq / cfg->ctl_cfg.freq_div.pos,
            &cfg->ctl_cfg.pos_p);
    pid_init(&lo->pos_p, cfg->ctl_cfg.pos_p);
    pid_set_out_limit(
        &lo->pos_p, vel_limit, vel_limit, vel_limit, -vel_limit, -vel_limit, -vel_limit);

    /* PD 环 PD 控制器初始化 */
    FS_INIT((float32_t)cfg->base_cfg.periph.pwm_freq / cfg->ctl_cfg.freq_div.pd,
            &cfg->ctl_cfg.pos_vel_pd);
    pid_init(&lo->pos_vel_pd, cfg->ctl_cfg.pos_vel_pd);
    pid_set_out_limit(
        &lo->pos_vel_pd, tor_limit, tor_limit, tor_limit, -tor_limit, -tor_limit, -tor_limit);

    /* 电机参数设置 */
    cfg->obs_cfg.luenberger.motor = cfg->obs_cfg.smo.motor = cfg->obs_cfg.flux.motor =
        &cfg->base_cfg.motor;

    FS_INIT(cfg->ctl_cfg.iq_pi.fs,
            &cfg->obs_cfg.omega_pll,
            &cfg->obs_cfg.luenberger,
            &cfg->obs_cfg.smo,
            &cfg->obs_cfg.flux,
            &cfg->obs_cfg.hfi);

    pll_init(&lo->omega_pll, cfg->obs_cfg.omega_pll);
    rls_init(&lo->mech_acc_rls, cfg->obs_cfg.mech_acc_rls);
    tmp->prev_mech_omega    = 0.0F;
    tmp->mech_acc_rls_valid = false;
    luenberger_init(&lo->luenberger, cfg->obs_cfg.luenberger);
    smo_init(&lo->smo, cfg->obs_cfg.smo);
    flux_init(&lo->flux, cfg->obs_cfg.flux);
    hfi_init(&lo->hfi, cfg->obs_cfg.hfi);
    lo->e_obs_switch = cfg->obs_cfg.u_obs_flag.bit.hfi ? FOC_OBS_SWITCH_HFI : FOC_OBS_SWITCH_FLUX;
    lo->hfi_weight   = cfg->obs_cfg.u_obs_flag.bit.hfi ? 1.0F : 0.0F;
    lo->flux_weight  = cfg->obs_cfg.u_obs_flag.bit.hfi ? 0.0F : 1.0F;
    lo->flux_theta_offset    = 0.0F;
    lo->obs_switch_theta_err = 0.0F;
    lo->obs_switch_ready_cnt = 0U;
    lo->flux_theta_aligned   = false;
    lo->hfi_injection_enable = cfg->obs_cfg.u_obs_flag.bit.hfi;
    rls_init(&lo->rls, cfg->obs_cfg.rls);
}

/**
 * @brief 获取电机轴/出轴编码器上电初始位置
 *
 * @param foc FOC 结构体
 * @return    void
 */
static void
foc_rotor_init(struct foc *foc)
{
    DECL(foc, cfg, in, lo);

    if (in->rotor.is_init)
        return;
    else {
        /* 使用强拖或无感 */
        if (lo->e_elec_theta != FOC_ELEC_THETA_SENSOR) {
            in->rotor.is_init = true;
            return;
        }
    }

    /* 未启用出轴编码器 */
    if (!cfg->sensor_cfg.outshaft_sensor_enable && lo->motor_theta_sensor_comm.rx_tick != 0) {
        WARP_PI(in->rotor.motor_theta_start, in->rotor.motor_theta_total);
        in->rotor.motor_theta_total_wrap_offset =
            (in->rotor.motor_theta_total > PI || in->rotor.motor_theta_total < -PI) ? TAU : 0.0F;
        in->rotor.is_init = true;
        return;
    }

    /* 启用出轴编码器 */
    if (cfg->sensor_cfg.outshaft_sensor_enable && lo->motor_theta_sensor_comm.rx_tick != 0 &&
        lo->outshaft_theta_sensor_comm.rx_tick != 0) {
        WARP_PI(in->rotor.motor_theta_start, in->rotor.motor_theta_total);
        in->rotor.motor_theta_total_wrap_offset =
            (in->rotor.motor_theta_total > PI || in->rotor.motor_theta_total < -PI) ? TAU : 0.0F;

        WARP_PI(in->rotor.outshaft_theta_start, in->rotor.outshaft_theta_total);
        in->rotor.outshaft_theta_total_wrap_offset =
            (in->rotor.outshaft_theta_total > PI || in->rotor.outshaft_theta_total < -PI) ? TAU
                                                                                          : 0.0F;
        in->rotor.is_init = true;
        return;
    }
}

/**
 * @brief 三相电流/电压偏置校准
 *
 * @param foc FOC 结构体
 * @return    int 状态码
 */
static int
foc_stator_init(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo, tmp);

    if (in->stator.is_init)
        return 0;

    /* 三相下管同时导通,将 U/V/W 端子钳位到同一电位后校准 ADC 偏置. */
    cfg->func_cfg.f_set_pwm_status(PWM_CH_H, false);
    cfg->func_cfg.f_set_pwm_duty((struct u32_uvw){0}, cfg->base_cfg.periph.pwm_full_cnt);
    cfg->func_cfg.f_set_pwm_status(PWM_CH_L, true);

    /* 丢弃钳位刚建立时的瞬态样本. */
    if (++tmp->offset_cali_settle_cnt < MS2CNT(OFFSET_CALI_SETTLE_MS, cfg->ctl_cfg.iq_pi.fs))
        return -MEBUSY;

    UVW_ADD_VEC(tmp->cur_offset_sum, tmp->cur_offset_sum, in->stator.f32_i_uvw_raw);
    if (cfg->sensor_cfg.terminal_volt_sample_enable)
        UVW_ADD_VEC(tmp->volt_offset_sum, tmp->volt_offset_sum, in->stator.f32_v_uvw_raw);

    if (++tmp->cur_offset_cali_cnt >= MS2CNT(CUR_OFFSET_SAMPLE_MS, cfg->ctl_cfg.iq_pi.fs)) {
        cfg->func_cfg.f_set_pwm_status(PWM_CH_L, false);
        UVW_DIV(in->stator.cur_offset, tmp->cur_offset_sum, tmp->cur_offset_cali_cnt);

        if (cfg->sensor_cfg.terminal_volt_sample_enable) {
            struct f32_uvw volt_avg;
            UVW_DIV(volt_avg, tmp->volt_offset_sum, tmp->cur_offset_cali_cnt);
            const float32_t volt_common = (volt_avg.u + volt_avg.v + volt_avg.w) / 3.0F;
            UVW_SUB(in->stator.volt_offset, volt_avg, volt_common);
        }

        in->stator.is_init          = true;
        tmp->cur_offset_cali_cnt    = 0;
        tmp->offset_cali_settle_cnt = 0;
        memset(&tmp->cur_offset_sum, 0, sizeof(tmp->cur_offset_sum));
        memset(&tmp->volt_offset_sum, 0, sizeof(tmp->volt_offset_sum));
        return 0;
    }
    return -MEBUSY;
}
