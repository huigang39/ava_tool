#include "mathdef.h"

#include "foc.h"

void foc_select_mode(foc_t *foc);
void foc_vol_ctl(foc_t *foc);
void foc_cur_ctl(foc_t *foc);
void foc_vel_ctl(foc_t *foc);
void foc_pos_ctl(foc_t *foc);
void foc_pd_ctl(foc_t *foc);

void
foc_select_mode(foc_t *foc)
{
        DECL(foc, lo);

        if (lo->e_mode == lo->e_prev_mode)
                return;

        memset(&lo->ref_pvct, 0, sizeof(lo->ref_pvct));

        switch (lo->e_mode) {
                case FOC_MODE_POS:
                case FOC_MODE_PD: {
                        lo->ref_pvct.pos = lo->fdb_pvct.pos;
                        break;
                }
                default:
                        break;
        }

        lo->e_prev_mode = lo->e_mode;
}

void
foc_vol_ctl(foc_t *foc)
{
        ARG_UNUSED(foc);
}

void
foc_cur_ctl(foc_t *foc)
{
        DECL(foc, cfg, in, out, lo);

        if (++lo->freq_div.cur < cfg->ctl_cfg.div.cur)
                return;

        lo->freq_div.cur = 0;

        lo->ref_i_dq.q = lo->ref_pvct.cur + lo->comp_i_dq.q;

        // q轴电流环
        RENAME(&lo->iq_pid, iq_pid);
        iq_pid->cfg.ki_out_max = iq_pid->cfg.out_max = in->v_bus / SQRT_3 * cfg->base_cfg.periph.f32_pwm_max;
        lo->ffd_v_dq.q                               = in->rotor.omega * cfg->base_cfg.motor.psi * 0.7f;
        pid_parallel_exec_in(iq_pid, lo->ref_i_dq.q, in->i_dq.q, lo->ffd_v_dq.q);
        out->v_dq.q = iq_pid->out.u;

        // d轴电流环
        RENAME(&lo->id_pid, id_pid);
        id_pid->cfg.ki_out_max = id_pid->cfg.out_max = in->v_bus / SQRT_3 * cfg->base_cfg.periph.f32_pwm_max;
        lo->ffd_v_dq.d                               = -in->rotor.omega * cfg->base_cfg.motor.lq * in->i_dq.q * 0.7f;
        pid_parallel_exec_in(id_pid, lo->ref_i_dq.d, in->i_dq.d, lo->ffd_v_dq.d);
        out->v_dq.d = id_pid->out.u;
}

void
foc_vel_ctl(foc_t *foc)
{
        DECL(foc, cfg, lo);

        if (++lo->freq_div.vel < cfg->ctl_cfg.div.vel)
                return;

        lo->freq_div.vel = 0;

        pid_parallel_exec_in(&lo->vel_pid, lo->ref_pvct.vel, lo->fdb_pvct.vel, lo->ref_pvct.ffd_cur);
        lo->ref_pvct.cur = lo->vel_pid.out.u;
}

void
foc_pos_ctl(foc_t *foc)
{
        DECL(foc, cfg, lo);

        if (++lo->freq_div.pos < cfg->ctl_cfg.div.pos)
                return;

        lo->freq_div.pos = 0;

        pid_parallel_exec_in(&lo->pos_pid, lo->ref_pvct.pos, lo->fdb_pvct.pos, lo->ref_pvct.ffd_vel);
        lo->ref_pvct.vel = lo->pos_pid.out.u;
}

void
foc_pd_ctl(foc_t *foc)
{
        DECL(foc, cfg, lo);

        if (++lo->freq_div.pd < cfg->ctl_cfg.div.pd)
                return;

        lo->freq_div.pd = 0;

        pd_parallel_exec_in(
            &lo->pd_pid, lo->ref_pvct.pos, lo->fdb_pvct.pos, lo->ref_pvct.vel, lo->fdb_pvct.vel, lo->ref_pvct.elec_tor);
        lo->ref_pvct.cur = lo->pd_pid.out.u;
}
