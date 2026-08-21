#include "motor_control/controller/pid.h"
#include "mathdef.h"

void
pid_init(struct pid_ctl *pid, const struct pid_cfg pid_cfg)
{
    DECL(pid, cfg, lo);
    CFG_INIT(pid, pid_cfg);

    cfg->ref_rate_max   = ABS(cfg->ref_rate_max);
    cfg->ref_change_max = cfg->ref_rate_max / cfg->fs;
}

void
pid_parallel_exec_rt(struct pid_ctl *pid)
{
    DECL(pid, cfg, in, out, lo);
    lo->err    = in->ref - in->fdb;
    lo->kp_out = cfg->kp * lo->err;
    INTEGRATOR(lo->ki_out, lo->err, cfg->ki, cfg->fs);
    CLAMP(lo->ki_out, cfg->ki_out_min, cfg->ki_out_max);
    DERIVATIVE(lo->kd_out, lo->err, lo->prev_err, cfg->kd, cfg->fs);

    out->pid_raw = out->pid = lo->kp_out + lo->ki_out + lo->kd_out;
    CLAMP(out->pid, cfg->pid_out_min, cfg->pid_out_max);

    out->u_raw = out->u = out->pid + in->ffd;
    CLAMP(out->u, cfg->out_min, cfg->out_max);
}

void
pid_serial_exec_rt(struct pid_ctl *pid)
{
    DECL(pid, cfg, in, out, lo);
    lo->err    = in->ref - in->fdb;
    lo->kp_out = cfg->kp * lo->err;
    INTEGRATOR(lo->ki_out, lo->err + lo->kp_out, cfg->ki, cfg->fs);
    CLAMP(lo->ki_out, cfg->ki_out_min, cfg->ki_out_max);
    DERIVATIVE(lo->kd_out, lo->err + lo->ki_out, lo->prev_err, cfg->kd, cfg->fs);

    out->pid_raw = out->pid = lo->kp_out + lo->ki_out + lo->kd_out;
    CLAMP(out->pid, cfg->pid_out_min, cfg->pid_out_max);

    out->u_raw = out->u = out->pid + in->ffd;
    CLAMP(out->u, cfg->out_min, cfg->out_max);
}

void
pid_parallel_exec_in_rt(struct pid_ctl *pid,
                        const float32_t ref,
                        const float32_t fdb,
                        const float32_t ffd)
{
    DECL(pid, cfg, in, lo);
    lo->ref_limited = ref;
    if (cfg->ref_rate_max > 0.0F) {
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
    pid_parallel_exec_rt(pid);
}

void
pid_serial_exec_in_rt(struct pid_ctl *pid,
                      const float32_t ref,
                      const float32_t fdb,
                      const float32_t ffd)
{
    DECL(pid, cfg, in, lo);
    lo->ref_limited = ref;
    if (cfg->ref_rate_max > 0.0F) {
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
    pid_serial_exec_rt(pid);
}

void
pd_parallel_exec_in_rt(struct pid_ctl *pid,
                       const float32_t ref_pos,
                       const float32_t fdb_pos,
                       const float32_t ref_vel,
                       const float32_t fdb_vel,
                       const float32_t ffd_tor)
{
    DECL(pid, cfg, in, lo, out);
    in->ref = ref_pos;
    in->fdb = fdb_pos;
    in->ffd = ffd_tor;

    out->pid_raw = out->pid = cfg->kp * (in->ref - in->fdb) + cfg->kd * (ref_vel - fdb_vel);
    CLAMP(out->pid, cfg->pid_out_min, cfg->pid_out_max);

    out->u_raw = out->u = out->pid + in->ffd;
    CLAMP(out->u, cfg->out_min, cfg->out_max);
}

void
pid_set_out_limit(struct pid_ctl *pid,
                  const float32_t out_max,
                  const float32_t pid_out_max,
                  const float32_t ki_out_max,
                  const float32_t out_min,
                  const float32_t pid_out_min,
                  const float32_t ki_out_min)
{
    DECL(pid, cfg);

    cfg->out_max     = out_max;
    cfg->pid_out_max = pid_out_max;
    cfg->ki_out_max  = ki_out_max;
    cfg->out_min     = out_min;
    cfg->pid_out_min = pid_out_min;
    cfg->ki_out_min  = ki_out_min;
}
