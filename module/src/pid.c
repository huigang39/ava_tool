#include "pid.h"
#include "mathdef.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
pid_init(pid_ctl_t *pid, const pid_cfg_t pid_cfg)
{
        DECL(pid, cfg, lo);
        CFG_INIT(pid, pid_cfg);

        cfg->ref_rate_max  = ABS(cfg->ref_rate_max);
        lo->ref_change_max = cfg->ref_rate_max / cfg->fs;
}

void
pid_parallel_exec(pid_ctl_t *pid)
{
        DECL(pid, cfg, in, out, lo);
        CFG_CHECK(pid, pid_init);

        // 误差
        lo->err = in->ref - in->fdb;

        // 比例
        lo->kp_out = cfg->kp * lo->err;

        // 积分
        INTEGRATOR(lo->ki_out, lo->err, cfg->ki, cfg->fs);

        // 积分限幅
        CLAMP(lo->ki_out, cfg->ki_out_min, cfg->ki_out_max);

        // 微分
        DERIVATIVE(lo->kd_out, lo->err, lo->prev_err, cfg->kd, cfg->fs);

        // 输出
        out->u_raw = out->u = lo->kp_out + lo->ki_out + lo->kd_out + in->ffd;

        // 输出限幅
        CLAMP(out->u, cfg->out_min, cfg->out_max);
}

void
pid_serial_exec(pid_ctl_t *pid)
{
        DECL(pid, cfg, in, out, lo);
        CFG_CHECK(pid, pid_init);

        // 误差
        lo->err = in->ref - in->fdb;

        // 比例
        lo->kp_out = cfg->kp * lo->err;

        // 积分
        INTEGRATOR(lo->ki_out, lo->err + lo->kp_out, cfg->ki, cfg->fs);

        // 积分限幅
        CLAMP(lo->ki_out, cfg->ki_out_min, cfg->ki_out_max);

        // 微分
        DERIVATIVE(lo->kd_out, lo->err + lo->ki_out, lo->prev_err, cfg->kd, cfg->fs);

        // 输出
        out->u_raw = out->u = lo->kp_out + lo->ki_out + lo->kd_out + in->ffd;

        // 输出限幅
        CLAMP(out->u, cfg->out_min, cfg->out_max);
}

void
pid_parallel_exec_in(pid_ctl_t *pid, const f32 ref, const f32 fdb, const f32 ffd)
{
        DECL(pid, cfg, in, lo);
        CFG_CHECK(pid, pid_init);

        lo->ref_limited = ref;
        if (cfg->ref_rate_max > 0.0f) {
                lo->ref_change = ref - lo->prev_ref;
                if (lo->ref_change > lo->ref_change_max)
                        lo->ref_limited = lo->prev_ref + lo->ref_change_max;
                else if (lo->ref_change < -lo->ref_change_max)
                        lo->ref_limited = lo->prev_ref - lo->ref_change_max;
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
        CFG_CHECK(pid, pid_init);

        lo->ref_limited = ref;
        if (cfg->ref_rate_max > 0.0f) {
                lo->ref_change = ref - lo->prev_ref;
                if (lo->ref_change > lo->ref_change_max)
                        lo->ref_limited = lo->prev_ref + lo->ref_change_max;
                else if (lo->ref_change < -lo->ref_change_max)
                        lo->ref_limited = lo->prev_ref - lo->ref_change_max;
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
        DECL(pid, cfg, in, lo, out);
        CFG_CHECK(pid, pid_init);

        in->ref = ref_pos;
        in->fdb = fdb_pos;
        in->ffd = ffd_tor;

        out->u = cfg->kp * (in->ref - in->fdb) + cfg->kd * (ref_vel - fdb_vel) + in->ffd;
        CLAMP(out->u, -cfg->out_max, cfg->out_max);
}
