#include "pid.h"
#include "mathdef.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
pid_init(pid_ctl_t *pid, const pid_cfg_t pid_cfg)
{
        CFG_INIT(pid, pid_cfg);
        DECL(pid, cfg, lo);
        cfg->ref_change_max = cfg->ref_rate_max / cfg->fs;
}

void
pid_parallel_exec(pid_ctl_t *pid)
{
        DECL(pid, cfg, in, out, lo);

        // 误差
        lo->err = in->ref - in->fdb;

        // 比例
        lo->kp_out = cfg->kp * lo->err;

        // 积分
        INTEGRATOR(lo->ki_out, lo->err, cfg->ki, cfg->fs);

        // 积分限幅
        CLAMP(lo->ki_out, -cfg->ki_out_max, cfg->ki_out_max);

        // 微分
        DERIVATIVE(lo->kd_out, lo->err, lo->prev_err, cfg->kd, cfg->fs);

        // 输出
        out->u = lo->kp_out + lo->ki_out + lo->kd_out + in->ffd;

        // 输出限幅
        CLAMP(out->u, -cfg->out_max, cfg->out_max);
}

void
pid_serial_exec(pid_ctl_t *pid)
{
        DECL(pid, cfg, in, out, lo);

        // 误差
        lo->err = in->ref - in->fdb;

        // 比例
        lo->kp_out = cfg->kp * lo->err;

        // 积分
        INTEGRATOR(lo->ki_out, lo->err + lo->kp_out, cfg->ki, cfg->fs);

        // 积分限幅
        CLAMP(lo->ki_out, -cfg->ki_out_max, cfg->ki_out_max);

        // 微分
        DERIVATIVE(lo->kd_out, lo->err + lo->ki_out, lo->prev_err, cfg->kd, cfg->fs);

        // 输出
        out->u = lo->kp_out + lo->ki_out + lo->kd_out + in->ffd;

        // 输出限幅
        CLAMP(out->u, -cfg->out_max, cfg->out_max);
}

void
pid_parallel_exec_in(pid_ctl_t *pid, const f32 ref, const f32 fdb, const f32 ffd)
{
        DECL(pid, cfg, in, lo);

        lo->ref_limited = ref;
        if (cfg->ref_rate_max > 0.0f) {
                lo->ref_change = ref - lo->prev_ref;
                if (lo->ref_change > cfg->ref_change_max)
                        lo->ref_limited = lo->prev_ref + cfg->ref_change_max;
                else if (lo->ref_change < -cfg->ref_change_max)
                        lo->ref_limited = lo->prev_ref - cfg->ref_change_max;
        }
        lo->prev_ref = lo->ref_limited;

        in->ref = lo->ref_limited;
        in->fdb = fdb;
        in->ffd = ffd;
        pid_parallel_exec(pid);
}

void
pid_serial_exec_in(pid_ctl_t *pid, const f32 ref, const f32 fdb, const f32 ffd)
{
        DECL(pid, cfg, in, lo);

        lo->ref_limited = ref;
        if (cfg->ref_rate_max > 0.0f) {
                lo->ref_change = ref - lo->prev_ref;
                if (lo->ref_change > cfg->ref_change_max)
                        lo->ref_limited = lo->prev_ref + cfg->ref_change_max;
                else if (lo->ref_change < -cfg->ref_change_max)
                        lo->ref_limited = lo->prev_ref - cfg->ref_change_max;
        }
        lo->prev_ref = lo->ref_limited;

        in->ref = lo->ref_limited;
        in->fdb = fdb;
        in->ffd = ffd;
        pid_serial_exec(pid);
}

void
pd_parallel_exec_in(
    pid_ctl_t *pid, const f32 ref_pos, const f32 fdb_pos, const f32 ref_vel, const f32 fdb_vel, const f32 ffd_tor)
{
        DECL(pid, cfg, in, out);

        out->u = cfg->kp * (ref_pos - fdb_pos) + cfg->kd * (ref_vel - fdb_vel) + ffd_tor;
        CLAMP(out->u, -cfg->out_max, cfg->out_max);
}
