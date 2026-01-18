#include "macrodef.h"
#include "mathdef.h"

#include "foc.h"

extern void foc_disable(foc_t *foc);
extern void foc_enable(foc_t *foc);

int  foc_cali(foc_t *foc);
f32  foc_nonlinear_lookup(f32 theta, const f16 *table, usize table_size);
void foc_store_by_cali_bit(foc_t *foc);
void foc_load_by_cali_bit(foc_t *foc);

static foc_cali_state_e foc_get_next_cali_state(foc_t *foc);

static int foc_offset_adc_cali(foc_t *foc);
static int foc_offset_theta_cali(foc_t *foc);

static int foc_nonlinear_theta_cali(foc_t *foc);
static int foc_nonlinear_outshaft_theta_cali(foc_t *foc);

static foc_cali_state_e
foc_get_next_cali_state(foc_t *foc)
{
        DECL(foc, lo);

        if (lo->cali_flag.bits.offset_adc)
                return FOC_OFFSET_CALI_ADC_INIT;
        if (lo->cali_flag.bits.offset_theta)
                return FOC_OFFSET_CALI_THETA_INIT;
        if (lo->cali_flag.bits.offset_outshaft_theta)
                return FOC_OFFSET_CALI_OUTSHAFT_THETA_INIT;
        if (lo->cali_flag.bits.nonlinear_theta)
                return FOC_NONLINEAR_CALI_THETA_INIT;
        if (lo->cali_flag.bits.nonlinear_outshaft_theta)
                return FOC_NONLINEAR_CALI_OUTSHAFT_THETA_INIT;

        return FOC_CALI_FINISH;
}

void
foc_store_by_cali_bit(foc_t *foc)
{
        DECL(foc, cfg, lo);

        if (lo->store.info.cali_flag.bits.offset_adc || lo->store.info.cali_flag.bits.offset_theta ||
            lo->store.info.cali_flag.bits.offset_outshaft_theta)
                cfg->func_cfg.f_store((void *)(usize)cfg->base_cfg.store.offset, &lo->store.offset, sizeof(lo->store.offset));

        if (lo->store.info.cali_flag.bits.nonlinear_theta || lo->store.info.cali_flag.bits.nonlinear_outshaft_theta)
                cfg->func_cfg.f_store(
                    (void *)(usize)cfg->base_cfg.store.nonlinear, &lo->store.nonlinear, sizeof(lo->store.nonlinear));

        cfg->func_cfg.f_store((void *)(usize)cfg->base_cfg.store.info, &lo->store.info, sizeof(lo->store.info));
}

void
foc_load_by_cali_bit(foc_t *foc)
{
        DECL(foc, cfg, lo);

        lo->u_error.bits.null_ptr = !RUN_FUNC_PTR(
            cfg->func_cfg.f_load, &lo->store.info, (void *)(usize)cfg->base_cfg.store.info, sizeof(lo->store.info));

        if (lo->store.info.cali_flag.bits.offset_adc || lo->store.info.cali_flag.bits.offset_theta ||
            lo->store.info.cali_flag.bits.offset_outshaft_theta)
                cfg->func_cfg.f_load(&lo->store.offset, (void *)(usize)cfg->base_cfg.store.offset, sizeof(lo->store.offset));

        if (lo->store.info.cali_flag.bits.nonlinear_theta || lo->store.info.cali_flag.bits.nonlinear_outshaft_theta)
                cfg->func_cfg.f_load(
                    &lo->store.nonlinear, (void *)(usize)cfg->base_cfg.store.nonlinear, sizeof(lo->store.nonlinear));
}

int
foc_offset_adc_cali(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        switch (lo->e_cali_state) {
                case FOC_OFFSET_CALI_ADC_INIT: {
                        memset(&lo->store.offset.adc, 0, sizeof(lo->store.offset.adc));
                        lo->e_cali_state = FOC_OFFSET_CALI_ADC_SAMPING;
                        break;
                }
                case FOC_OFFSET_CALI_ADC_SAMPING: {
                        cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, 1);
                        in->adc_raw = cfg->func_cfg.f_get_adc();
                        UVW_ADD_VEC(lo->store.offset.adc.i32_i_uvw, lo->store.offset.adc.i32_i_uvw, in->adc_raw.i32_i_uvw);
                        if (++lo->cali_cnt.offset_adc >= cfg->cali_cfg.cnt.offset_adc) {
                                lo->store.offset.adc.i32_i_uvw.u /= (i32)lo->cali_cnt.offset_adc;
                                lo->store.offset.adc.i32_i_uvw.v /= (i32)lo->cali_cnt.offset_adc;
                                lo->store.offset.adc.i32_i_uvw.w /= (i32)lo->cali_cnt.offset_adc;
                                lo->e_cali_state                  = FOC_OFFSET_CALI_ADC_FINISH;
                        }
                        break;
                }
                case FOC_OFFSET_CALI_ADC_FINISH: {
                        cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, 0);
                        return 0;
                }
                default:
                        break;
        }
        return -MEBUSY;
}

int
foc_offset_theta_cali(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        switch (lo->e_cali_state) {
                case FOC_OFFSET_CALI_THETA_INIT: {
                        lo->store.offset.theta = 0.0f;
                        lo->ref_i_dq.d         = cfg->cali_cfg.force_id;
                        in->rotor.force_omega  = cfg->cali_cfg.force_omega;
                        lo->e_mode             = FOC_MODE_CUR;
                        lo->e_theta            = FOC_THETA_FORCE;
                        lo->e_cali_state       = FOC_OFFSET_CALI_THETA_CW;
                        break;
                }
                case FOC_OFFSET_CALI_THETA_CW: {
                        foc_enable(foc);

                        if (in->rotor.force_theta >= TAU) {
                                in->rotor.force_theta = TAU;

                                LOWPASS(lo->store.offset.theta,
                                        in->rotor.elec_theta,
                                        cfg->cali_cfg.offset_theta_lpf_wc,
                                        cfg->base_cfg.exec_freq);

                                if (++lo->cali_cnt.offset_theta_sample >= cfg->cali_cfg.cnt.offset_theta_sample) {
                                        lo->cali_cnt.offset_theta_sample = 0;
                                        if (++lo->cali_cnt.offset_theta_cycle >= cfg->base_cfg.motor.npp)
                                                lo->e_cali_state = FOC_OFFSET_CALI_THETA_CCW;
                                        else
                                                in->rotor.force_theta = 0.0f;
                                }
                        } else
                                in->rotor.force_theta += in->rotor.force_omega / cfg->base_cfg.exec_freq;

                        break;
                }
                case FOC_OFFSET_CALI_THETA_CCW: {
                        foc_enable(foc);

                        if (in->rotor.force_theta <= 0.0f) {
                                in->rotor.force_theta = 0.0f;

                                LOWPASS(lo->store.offset.theta,
                                        in->rotor.elec_theta,
                                        cfg->cali_cfg.offset_theta_lpf_wc,
                                        cfg->base_cfg.exec_freq);

                                if (++lo->cali_cnt.offset_theta_sample >= cfg->cali_cfg.cnt.offset_theta_sample) {
                                        lo->cali_cnt.offset_theta_sample = 0;
                                        if (++lo->cali_cnt.offset_theta_cycle >= cfg->base_cfg.motor.npp * 2)
                                                lo->e_cali_state = FOC_OFFSET_CALI_THETA_FINISH;
                                        else
                                                in->rotor.force_theta = TAU;
                                }
                        } else
                                in->rotor.force_theta -= in->rotor.force_omega / cfg->base_cfg.exec_freq;

                        break;
                }
                case FOC_OFFSET_CALI_THETA_FINISH: {
                        foc_disable(foc);

                        lo->ref_i_dq.d               = 0.0f;
                        in->rotor.force_theta        = 0.0f;
                        in->rotor.force_omega        = 0.0f;
                        in->rotor.theta              = 0.0f;
                        in->rotor.omega              = 0.0f;
                        in->rotor.theta_cycle_cnt    = 0;
                        in->rotor.outshaft_cycle_cnt = 0;

                        memset(&lo->id_pid.lo, 0, sizeof(lo->id_pid.lo));
                        memset(&lo->iq_pid.lo, 0, sizeof(lo->iq_pid.lo));
                        RESET(&lo->id_pid, out);
                        RESET(&lo->iq_pid, out);
                        RESET(foc, out);

                        lo->e_mode  = FOC_MODE_NONE;
                        lo->e_theta = FOC_THETA_SENSOR;
                        lo->e_state = FOC_STATE_READY;
                        return 0;
                }
                default:
                        break;
        }
        return -MEBUSY;
}

f32
foc_nonlinear_lookup(f32 theta, const f16 *table, usize table_size)
{
        // 将角度归一化到 [0, 1)
        f32 normalized = theta / TAU;

        // 计算浮点索引
        f32 idx_float = normalized * (f32)table_size;

        // 获取整数索引
        u32 idx = (u32)idx_float;

        // 边界处理
        if (idx >= table_size - 1)
                return table[table_size - 1];

        // 计算插值权重
        f32 weight = idx_float - (f32)idx;

        // 线性插值
        return table[idx] * (1.0f - weight) + table[idx + 1] * weight;
}

int
foc_nonlinear_theta_cali(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        switch (lo->e_cali_state) {
                case FOC_NONLINEAR_CALI_THETA_INIT: {
                        break;
                }
                case FOC_NONLINEAR_CALI_THETA_CW: {
                        foc_enable(foc);
                        break;
                }
                case FOC_NONLINEAR_CALI_THETA_CCW: {
                        foc_enable(foc);
                        break;
                }
                case FOC_NONLINEAR_CALI_THETA_FINISH: {
                        foc_disable(foc);
                        return 0;
                }
                default:
                        break;
        }
        return -MEBUSY;
}

int
foc_nonlinear_outshaft_theta_cali(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        // 计算数组索引
        f32 theta_normalized = in->rotor.outshaft_theta_raw / TAU;
        u32 idx              = (u32)(theta_normalized * FOC_NONLINEAR_OUTSHAFT_THETA_POINT_NUM);

        // 边界检查
        if (idx >= FOC_NONLINEAR_OUTSHAFT_THETA_POINT_NUM)
                idx = FOC_NONLINEAR_OUTSHAFT_THETA_POINT_NUM - 1;

        switch (lo->e_cali_state) {
                case FOC_NONLINEAR_CALI_OUTSHAFT_THETA_INIT: {
                        memset(lo->store.nonlinear.outshaft_theta, 0, sizeof(lo->store.nonlinear.outshaft_theta));
                        lo->ref_pvct.vel = cfg->cali_cfg.nonlinear_outshaft_theta_vel;
                        lo->e_mode = lo->e_prev_mode = FOC_MODE_VEL;
                        lo->e_theta                  = FOC_THETA_SENSOR;
                        lo->e_cali_state             = FOC_NONLINEAR_CALI_OUTSHAFT_THETA_CW;
                        break;
                }
                case FOC_NONLINEAR_CALI_OUTSHAFT_THETA_CW: {
                        foc_enable(foc);

                        LOWPASS(lo->store.nonlinear.outshaft_theta[idx],
                                in->rotor.outshaft_theta_err,
                                cfg->cali_cfg.nonlinear_outshaft_theta_lpf_wc,
                                cfg->base_cfg.exec_freq);

                        if (in->rotor.outshaft_cycle_cnt == cfg->cali_cfg.nonlinear_outshaft_theta_cycle) {
                                lo->ref_pvct.vel *= FOC_DIR_REVERSE;
                                lo->e_cali_state  = FOC_NONLINEAR_CALI_OUTSHAFT_THETA_CCW;
                        }
                        break;
                }
                case FOC_NONLINEAR_CALI_OUTSHAFT_THETA_CCW: {
                        foc_enable(foc);

                        LOWPASS(lo->store.nonlinear.outshaft_theta[idx],
                                in->rotor.outshaft_theta_err,
                                cfg->cali_cfg.nonlinear_outshaft_theta_lpf_wc,
                                cfg->base_cfg.exec_freq);

                        if (in->rotor.outshaft_cycle_cnt == 0)
                                lo->e_cali_state = FOC_NONLINEAR_CALI_OUTSHAFT_THETA_FINISH;

                        break;
                }
                case FOC_NONLINEAR_CALI_OUTSHAFT_THETA_FINISH: {
                        foc_disable(foc);

                        memset(&lo->ref_pvct, 0, sizeof(lo->ref_pvct));
                        lo->e_mode  = FOC_MODE_NONE;
                        lo->e_theta = FOC_THETA_SENSOR;
                        lo->e_state = FOC_STATE_READY;
                        return 0;
                }
                default:
                        break;
        }
        return -MEBUSY;
}

int
foc_cali(foc_t *foc)
{
        DECL(foc, cfg, lo);

        if (lo->e_state == lo->e_prev_state)
                return -MEXIST;

        lo->store.info.crc = 0;
        lo->store.info.ver = 0x01;

        int ret;

        /* -------------------------------------------------------------------------- */
        /*                                 OFFSET 校准                                */
        /* -------------------------------------------------------------------------- */

        // 如果正在 ADC 校准中, 继续执行
        if (IS_IN_RANGE(lo->e_cali_state, FOC_OFFSET_CALI_ADC_INIT, FOC_OFFSET_CALI_ADC_FINISH)) {
                ret = foc_offset_adc_cali(foc);
                if (ret < 0)
                        return ret;

                if (ret == 0) {
                        lo->store.info.cali_flag.bits.offset_adc = 1;
                        lo->cali_flag.bits.offset_adc            = 0;
                        foc_store_by_cali_bit(foc);
                        lo->e_cali_state = foc_get_next_cali_state(foc);
                        return (lo->e_cali_state == FOC_CALI_FINISH) ? 0 : -MEBUSY;
                }
                return ret;
        }

        // 如果正在 THETA 校准中, 继续执行
        if (IS_IN_RANGE(lo->e_cali_state, FOC_OFFSET_CALI_THETA_INIT, FOC_OFFSET_CALI_THETA_FINISH)) {
                ret = foc_offset_theta_cali(foc);
                if (ret < 0)
                        return ret;

                if (ret == 0) {
                        lo->store.info.cali_flag.bits.offset_theta = 1;
                        lo->cali_flag.bits.offset_theta            = 0;
                        foc_store_by_cali_bit(foc);
                        lo->e_cali_state = foc_get_next_cali_state(foc);
                        return (lo->e_cali_state == FOC_CALI_FINISH) ? 0 : -MEBUSY;
                }
                return ret;
        }

        // 如果正在 OUTSHAFT_THETA 校准中, 继续执行
        if (IS_IN_RANGE(lo->e_cali_state, FOC_OFFSET_CALI_OUTSHAFT_THETA_INIT, FOC_OFFSET_CALI_OUTSHAFT_THETA_FINISH)) {
                lo->store.info.cali_flag.bits.offset_outshaft_theta = 1;
                lo->cali_flag.bits.offset_outshaft_theta            = 0;
                lo->store.offset.outshaft_theta =
                    TOGGLE_THETA(cfg->sensor_cfg.outshaft_theta_dir, cfg->func_cfg.f_get_outshaft_theta());
                foc_store_by_cali_bit(foc);
                lo->e_cali_state = foc_get_next_cali_state(foc);
                return (lo->e_cali_state == FOC_CALI_FINISH) ? 0 : -MEBUSY;
        }

        /* -------------------------------------------------------------------------- */
        /*                               NONLINEAR 校准                               */
        /* -------------------------------------------------------------------------- */

        // 如果正在 THETA 非线性校准中, 继续执行
        if (IS_IN_RANGE(lo->e_cali_state, FOC_NONLINEAR_CALI_THETA_INIT, FOC_NONLINEAR_CALI_THETA_FINISH)) {
                ret = foc_nonlinear_theta_cali(foc);
                if (ret < 0)
                        return ret;

                if (ret == 0) {
                        lo->store.info.cali_flag.bits.nonlinear_theta = 1;
                        lo->cali_flag.bits.nonlinear_theta            = 0;
                        foc_store_by_cali_bit(foc);
                        lo->e_cali_state = foc_get_next_cali_state(foc);
                        return (lo->e_cali_state == FOC_CALI_FINISH) ? 0 : -MEBUSY;
                }
                return ret;
        }

        // 如果正在 OUTSHAFT_THETA 非线性校准中, 继续执行
        if (IS_IN_RANGE(lo->e_cali_state, FOC_NONLINEAR_CALI_OUTSHAFT_THETA_INIT, FOC_NONLINEAR_CALI_OUTSHAFT_THETA_FINISH)) {
                ret = foc_nonlinear_outshaft_theta_cali(foc);
                if (ret < 0)
                        return ret;

                if (ret == 0) {
                        lo->store.info.cali_flag.bits.nonlinear_outshaft_theta = 1;
                        lo->cali_flag.bits.nonlinear_outshaft_theta            = 0;
                        foc_store_by_cali_bit(foc);
                        lo->e_cali_state = foc_get_next_cali_state(foc);
                        return (lo->e_cali_state == FOC_CALI_FINISH) ? 0 : -MEBUSY;
                }
                return ret;
        }

        // 没有正在进行的校准, 根据 cali_bitset 启动新的校准
        lo->e_cali_state = foc_get_next_cali_state(foc);
        if (lo->e_cali_state == FOC_CALI_FINISH) {
                lo->e_prev_state = lo->e_state;
                return 0;
        }
        return -MEBUSY;
}
