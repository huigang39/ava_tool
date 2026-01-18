#include <stdint.h>
#include <stdio.h>

#include "module.h"

sched_t cfs;

u64 fake_time_us = 0;

u64
get_ts_us(void)
{
        return fake_time_us;
}

void
task_cb(void *arg)
{
        const char *name = arg;
        printf("[%.6llu] Task %s executed\n", fake_time_us, name);
}

int
main(void)
{
        const sched_cfg_t cfg_cfg = {
            .cpu_id   = 19,
            .e_type   = SCHED_TYPE_CFS,
            .e_tick   = SCHED_TICK_US,
            .f_get_ts = get_ts_us,
        };
        sched_init(&cfs, cfg_cfg);

        const sched_task_cfg_t tasks[] = {
            {
                .id           = 0,
                .priority     = 1,
                .exec_freq    = 1000,
                .exec_cnt_max = 3,
                .delay_tick   = 0,
                .f_cb         = task_cb,
                .arg          = "A",
            },
            {
                .id           = 1,
                .priority     = 1,
                .exec_freq    = 500,
                .exec_cnt_max = 5,
                .delay_tick   = 0,
                .f_cb         = task_cb,
                .arg          = "B",
            },
            {
                .id           = 2,
                .priority     = 1,
                .exec_freq    = 800,
                .exec_cnt_max = 2,
                .delay_tick   = 0,
                .f_cb         = task_cb,
                .arg          = "C",
            },
        };

        for (int i = 0; i < 3; i++)
                sched_add_task(&cfs, tasks[i]);

        for (int step = 0; step < 10; step++) {
                fake_time_us += 500;

                printf("\n=== CFS step %d ===\n", step);
                sched_exec(&cfs);
        }

        return 0;
}
