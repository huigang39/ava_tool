#ifdef __linux__
#define _GNU_SOURCE
#endif

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <stdio.h>

#include "sch.h"

/* -------------------------------------------------------------------------- */
/*                                  工具函数                                  */
/* -------------------------------------------------------------------------- */

#ifdef __linux__
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
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        int ret = pthread_setaffinity_np(thread_tid, sizeof(cpu_set_t), &cpuset);
        if (ret)
                printf("[SCHED]set thread affinity failed, errcode: %d\n", ret);

        printf("[SCHED]bind thread to CPU %d success\n", cpu_id);
}
#elif defined(_WIN32)
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
        const DWORD_PTR mask = 1 << cpu_id;
        const DWORD_PTR ret  = SetThreadAffinityMask(thread_handle, mask);
        if (!ret)
                printf("[SCHED]set thread affinity failed, errcode: %lu\n", GetLastError());

        printf("[SCHED]bind thread to CPU %d success\n", cpu_id);
}
#endif

static void
sch_thread_init(void *arg, const int cpu_id)
{
#ifdef __linux__
        pthread_t sched_tid;
        int       ret = pthread_create(&sched_tid, NULL, sch_thread_exec, arg);
        if (ret != 0) {
                printf("[SCHED]create thread failed, errcode: %d\n", ret);
                return;
        }
        sch_bind_thread_to_cpu(sched_tid, cpu_id);
#elif defined(_WIN32)
        DWORD  thread_id;
        HANDLE sched_tid = CreateThread(NULL,            // 默认安全属性
                                        0,               // 默认堆栈大小
                                        sch_thread_exec, // 线程函数
                                        arg,             // 传递给线程函数的参数
                                        0,               // 默认创建标志
                                        &thread_id       // 用于接收线程ID
        );
        if (sched_tid == NULL) {
                printf("[SCHED]create thread failed, errcode: %lu\n", GetLastError());
                return;
        }
        sch_bind_thread_to_cpu(sched_tid, cpu_id);
#endif
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

u64
sch_hz2tick(sch_t *sched, const f32 hz)
{
        DECL(sched, cfg);

        switch (cfg->e_tick) {
                case SCHED_TICK_US:
                        return HZ2US(hz);
                case SCHED_TICK_MS:
                        return HZ2MS(hz);
                default:
                        return 0;
        }
}

int
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

void
sch_cfs_insert_task(sch_t *sched, sch_task_t *task)
{
        DECL(sched, lo);

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

void
sch_cfs_remove_task(sch_t *sched, sch_task_t *task)
{
        DECL(sched, lo);

        rb_root_t *rb_root = &lo->algo_ctx.cfs.rb_root;
        if (task->rb_node.rb_parent_color) {
                rb_erase(&task->rb_node, rb_root);
                memset(&task->rb_node, 0, sizeof(rb_node_t));
        }
}

sch_task_t *
sch_cfs_get_task(sch_t *sched)
{
        DECL(sched, lo);

        const rb_root_t *rb_root = &lo->algo_ctx.cfs.rb_root;
        rb_node_t       *rb_node = rb_first(rb_root);
        if (!rb_node)
                return NULL;

        return CONTAINER_OF(rb_node, sch_task_t, rb_node);
}

sch_task_t *
sch_fcfs_get_task(sch_t *sched)
{
        DECL(sched, lo);

        usize prev_idx = lo->algo_ctx.fcfs.prev_idx;
        for (usize i = 0; i < lo->task_num; ++i) {
                const usize idx = (prev_idx + i) % lo->task_num;
                sch_task_t *t   = &lo->tasks[idx];
                if (t->status.e_state == SCH_TASK_STATE_RUNNING) {
                        lo->algo_ctx.fcfs.prev_idx = idx + 1;
                        return t;
                }
        }
        return NULL;
}

int
sch_add_task(sch_t *sched, const sch_task_cfg_t task_cfg)
{
        DECL(sched, cfg, lo);

        sch_task_t *task          = &lo->tasks[lo->task_num];
        task->cfg                 = task_cfg;
        task->status.e_state      = SCH_TASK_STATE_RUNNING;
        task->status.create_ts    = cfg->f_get_ts();
        task->status.next_exec_ts = task->status.create_ts + task->cfg.delay_tick;

        lo->task_num++;
        if (sched->cfg.e_type == SCHED_TYPE_CFS)
                lo->f_insert_task(sched, task);

        return 0;
}

int
sch_init(sch_t *sched, const sch_cfg_t sched_cfg)
{
        DECL(sched, cfg, lo);
        CFG_INIT(sched, sched_cfg);

        switch (cfg->e_type) {
                case SCHED_TYPE_FCFS: {
                        lo->f_get_task    = sch_fcfs_get_task;
                        lo->f_insert_task = NULL;
                        lo->f_remove_task = NULL;
                        break;
                }
                case SCHED_TYPE_CFS: {
                        lo->f_get_task    = sch_cfs_get_task;
                        lo->f_insert_task = sch_cfs_insert_task;
                        lo->f_remove_task = sch_cfs_remove_task;
                        break;
                }
                default:
                        return -MEINVAL;
        }

        // only run on Linux/Windows
        sch_thread_init(sched, cfg->cpu_id);
        return 0;
}

int
sch_exec(sch_t *sched)
{
        DECL(sched, cfg, lo);

        lo->curr_ts = cfg->f_get_ts();

        sch_task_t *task = lo->f_get_task(sched);
        if (!task || !task->cfg.f_cb)
                return -MEINVAL;

        if (lo->curr_ts - task->status.create_ts < task->cfg.delay_tick)
                return 0;

        if (lo->curr_ts < task->status.next_exec_ts)
                return 0;

        if (sched->cfg.e_type == SCHED_TYPE_CFS)
                sch_cfs_remove_task(sched, task);

        const u64 start_ts = lo->curr_ts;
        task->cfg.f_cb(task->cfg.arg);
        const u64 end_ts = cfg->f_get_ts();

        task->status.exec_cnt++;
        task->status.elapsed_us = (f32)(end_ts - start_ts);

        if (task->cfg.exec_cnt_max == 0 || task->status.exec_cnt < task->cfg.exec_cnt_max) {
                task->status.next_exec_ts = end_ts + sch_hz2tick(sched, (f32)task->cfg.exec_freq);
                if (sched->cfg.e_type == SCHED_TYPE_CFS)
                        sch_cfs_insert_task(sched, task);
        } else
                task->status.e_state = SCH_TASK_STATE_DEAD;

        return 0;
}
