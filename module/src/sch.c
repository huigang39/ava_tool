#include "platdef.h"

#if OS(LINUX)
#define _GNU_SOURCE
#endif

#if OS(POSIX)
#include <pthread.h>
#endif
#if OS(LINUX)
#include <sched.h>
#endif
#if OS(MAC)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif
#if OS(WIN)
#include <windows.h>
#endif

#include <stdio.h>

#include "errdef.h"
#include "mathdef.h"
#include "printops.h"
#include "sch.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

#if OS(POSIX)
static void *
sch_thread_exec(void *arg)
{
        struct sch *t = (struct sch *)arg;
        for (;;)
                sch_exec(t);

        return NULL;
}

static void
sch_bind_thread_to_cpu(pthread_t thread_tid, const int cpu_id)
{
#if OS(LINUX)
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        CPU_SET(cpu_id, &cpu_set);
        int ret = pthread_setaffinity_np(thread_tid, sizeof(cpu_set_t), &cpu_set);
        if (ret)
                print_error(FALSE, "[SCH] set thread affinity failed, errcode: %d", ret);

        print_error(FALSE, "[SCH] bind thread to CPU %d success", cpu_id);
#elif OS(MAC)
        /* macOS pthreads don't support hard affinity; thread_policy provides hints. */
        thread_affinity_policy_data_t policy      = {.affinity_tag = cpu_id + 1};
        const mach_port_t             mach_thread = pthread_mach_thread_np(thread_tid);
        const kern_return_t           ret =
            thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT);
        if (ret != KERN_SUCCESS)
                print_error(FALSE, "[SCH] set thread affinity hint failed, errcode: %d", ret);
        else
                print_error(FALSE, "[SCH] set thread affinity hint to tag %d", cpu_id + 1);
#else
        ARG_UNUSED(thread_tid);
        ARG_UNUSED(cpu_id);
#endif
}
#elif OS(WIN)
static DWORD WINAPI
sch_thread_exec(LPVOID arg)
{
        struct sch *t = (struct sch *)arg;
        for (;;)
                sch_exec(t);

        return 0;
}

static void
sch_bind_thread_to_cpu(HANDLE thread_handle, const int cpu_id)
{
        const DWORD_PTR mask = (DWORD_PTR)1 << cpu_id;
        const DWORD_PTR ret  = SetThreadAffinityMask(thread_handle, mask);
        if (!ret)
                print_error(FALSE, "[SCH] set thread affinity failed, errcode: %lu", GetLastError());

        print_error(FALSE, "[SCH] bind thread to CPU %d success", cpu_id);
}
#endif

static void
sch_thread_init(void *arg, const int cpu_id)
{
        ARG_UNUSED(arg);
        ARG_UNUSED(cpu_id);

#if OS(POSIX)
        pthread_t sch_tid;
        int       ret = pthread_create(&sch_tid, NULL, sch_thread_exec, arg);
        if (ret != 0) {
                print_error(FALSE, "[SCH] create thread failed, errcode: %d", ret);
                return;
        }
        sch_bind_thread_to_cpu(sch_tid, cpu_id);
#elif OS(WIN)
        DWORD  thread_id;
        HANDLE sch_tid = CreateThread(NULL,            // 默认安全属性
                                      0,               // 默认堆栈大小
                                      sch_thread_exec, // 线程函数
                                      arg,             // 传递给线程函数的参数
                                      0,               // 默认创建标志
                                      &thread_id       // 用于接收线程ID
        );
        if (sch_tid == NULL) {
                print_error(FALSE, "[SCH] create thread failed, errcode: %lu", GetLastError());
                return;
        }
        sch_bind_thread_to_cpu(sch_tid, cpu_id);
#endif
}

static int
sch_cfs_task_cmp(const sch_task_t *a, const sch_task_t *b)
{
        if (a->status.next_exec_ts < b->status.next_exec_ts)
                return -1;
        if (a->status.next_exec_ts > b->status.next_exec_ts)
                return 1;
        if (a->cfg.priority < b->cfg.priority)
                return -1;
        if (a->cfg.priority > b->cfg.priority)
                return 1;

        return 0;
}

static void
sch_cfs_insert_task(sch_t *sch, sch_task_t *task)
{
        DECL(sch, lo);

        rb_root_t  *rb_root     = &lo->algo_ctx.cfs.rb_root;
        rb_node_t **new_rb_node = &rb_root->rb_node;
        rb_node_t  *rb_parent   = NULL;

        if (task->cfg.id >= SCH_TASK_MAX)
                return;

        while (*new_rb_node) {
                const sch_task_t *curr = CONTAINER_OF(*new_rb_node, sch_task_t, rb_node);
                const int         cmp  = sch_cfs_task_cmp(task, curr);
                rb_parent              = *new_rb_node;
                new_rb_node            = (cmp < 0) ? &(*new_rb_node)->rb_left : &(*new_rb_node)->rb_right;
        }
        rb_link_node(&task->rb_node, rb_parent, new_rb_node);
        rb_insert_color(&task->rb_node, rb_root);
}

static void
sch_cfs_remove_task(sch_t *sch, sch_task_t *task)
{
        DECL(sch, lo);

        rb_root_t *rb_root = &lo->algo_ctx.cfs.rb_root;
        if (task->rb_node.rb_parent_color) {
                rb_erase(&task->rb_node, rb_root);
                memset(&task->rb_node, 0, sizeof(rb_node_t));
        }
}

static sch_task_t *
sch_cfs_get_task(sch_t *sch)
{
        DECL(sch, lo);

        const rb_root_t *rb_root = &lo->algo_ctx.cfs.rb_root;
        rb_node_t       *rb_node = rb_first(rb_root);
        if (!rb_node)
                return NULL;

        return CONTAINER_OF(rb_node, sch_task_t, rb_node);
}

static sch_task_t *
sch_fcfs_get_task(sch_t *sch)
{
        DECL(sch, lo);

        const usize prev_idx = lo->algo_ctx.fcfs.prev_idx;
        for (usize i = 0; i < lo->ntasks; ++i) {
                const usize idx = (prev_idx + i) % lo->ntasks;
                sch_task_t *t   = &lo->tasks[idx];
                if (t->status.e_state == SCH_TASK_STATE_RUN) {
                        lo->algo_ctx.fcfs.prev_idx = idx + 1;
                        return t;
                }
        }
        return NULL;
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

int
sch_init(sch_t *sch, const sch_cfg_t sch_cfg)
{
        DECL(sch, cfg, lo);
        CFG_INIT(sch, sch_cfg);

        switch (cfg->e_type) {
                case SCH_TYPE_FCFS: {
                        lo->f_get_task = sch_fcfs_get_task;
                        break;
                }
                case SCH_TYPE_CFS: {
                        lo->f_get_task = sch_cfs_get_task;
                        break;
                }
                default:
                        return -MEINVAL;
        }

        return 0;
}

int
sch_run(sch_t *sch)
{
        DECL(sch, cfg);
        sch_thread_init(sch, cfg->cpu_id);
        return 0;
}

int
sch_exec(sch_t *sch)
{
        DECL(sch, cfg, lo, tmp);

        lo->curr_ts = cfg->f_get_ts();

        for (;;) {
                sch_task_t *task = lo->f_get_task(sch);
                if (!task)
                        return 0;
                if (!task->cfg.f_exec)
                        return -MEINVAL;

                if (lo->curr_ts < task->status.next_exec_ts)
                        return 0;

                if (sch->cfg.e_type == SCH_TYPE_CFS)
                        sch_cfs_remove_task(sch, task);

                const u64 start_ts = lo->curr_ts;
                task->cfg.f_exec(task->cfg.arg);
                const u64 end_ts = cfg->f_get_ts();

                task->status.exec_cnt++;
                task->status.elapsed_us = (u32)(end_ts - start_ts);

                if (task->cfg.exec_cnt_max == 0 || task->status.exec_cnt < task->cfg.exec_cnt_max) {
                        task->status.next_exec_ts = end_ts + (usize)HZ2US(task->cfg.exec_freq);
                        if (sch->cfg.e_type == SCH_TYPE_CFS)
                                sch_cfs_insert_task(sch, task);
                } else {
                        task->status.e_state = SCH_TASK_STATE_DEAD;
                        if (task->cfg.f_deinit)
                                task->cfg.f_deinit(task->cfg.arg);
                }

                tmp->elapsed_us      = (i64)(end_ts - lo->curr_ts) > 0 ? (u32)(end_ts - lo->curr_ts) : 0;
                lo->elapsed_us_max   = MAX(lo->elapsed_us_max, tmp->elapsed_us);
                tmp->prev_elapsed_us = tmp->elapsed_us;
                lo->curr_ts          = end_ts;
        }
}

int
sch_add_task(sch_t *sch, const sch_task_cfg_t task_cfg)
{
        DECL(sch, cfg, lo);

        if (lo->ntasks >= SCH_TASK_MAX)
                return -MEINVAL;

        sch_task_t *task          = &lo->tasks[lo->ntasks];
        task->cfg                 = task_cfg;
        task->status.e_state      = task_cfg.e_init_state;
        task->status.next_exec_ts = cfg->f_get_ts() + task->cfg.init_delay_us;

        if (task->cfg.f_init)
                task->cfg.f_init(task->cfg.arg);

        if (sch->cfg.e_type == SCH_TYPE_CFS)
                sch_cfs_insert_task(sch, task);

        lo->ntasks++;
        return 0;
}

int
sch_reinit_tasks(sch_t *sch)
{
        DECL(sch, cfg, lo);

        for (usize i = 0; i < lo->ntasks; i++) {
                sch_task_t *task = &lo->tasks[i];
                if (task->cfg.f_init)
                        task->cfg.f_init(task->cfg.arg);

                task->status.next_exec_ts = cfg->f_get_ts() + task->cfg.init_delay_us;
        }
        return 0;
}

int
sch_set_task_freq(sch_t *sch, const usize id, const usize exec_freq)
{
        DECL(sch, cfg, lo);

        for (usize i = 0; i < lo->ntasks; i++) {
                sch_task_t *task = &lo->tasks[i];
                if (task->cfg.id == id) {
                        task->cfg.exec_freq       = (f32)exec_freq;
                        task->status.next_exec_ts = cfg->f_get_ts() + (usize)HZ2US(exec_freq);
                        return 0;
                }
        }
        return -MEINVAL;
}

int
sch_set_task_state(sch_t *sch, const usize id, const sch_task_state_e e_state)
{
        DECL(sch, cfg, lo);

        for (usize i = 0; i < lo->ntasks; i++) {
                sch_task_t *task = &lo->tasks[i];
                if (task->cfg.id == id) {
                        if (task->status.e_state == e_state)
                                return -MEXIST;

                        if (e_state == SCH_TASK_STATE_DEAD) {
                                task->status.exec_cnt = 0;
                                if (task->cfg.f_deinit)
                                        task->cfg.f_deinit(task->cfg.arg);
                        }

                        if (task->status.e_state == SCH_TASK_STATE_DEAD) {
                                if (task->cfg.f_init)
                                        task->cfg.f_init(task->cfg.arg);
                        }

                        task->status.next_exec_ts = cfg->f_get_ts() + task->cfg.init_delay_us;
                        task->status.e_prev_state = task->status.e_state;
                        task->status.e_state      = e_state;
                        return 0;
                }
        }
        return -MEINVAL;
}
