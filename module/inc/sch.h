#ifndef SCH_H
#define SCH_H

#include "rbtree.h"
#include "timeops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

#ifndef SCH_TASK_MAX
#define SCH_TASK_MAX (8)
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef void (*sch_cb_f)(void *arg);

typedef union sch_algo_ctx {
        struct {
                usize prev_idx;
        } fcfs;
        struct {
                rb_root_t rb_root;
        } cfs;
} sch_algo_ctx_u;

/*
 *             +-----------+
 *             |  SLEEPING |
 *             +-----------+
 *                   ^
 *                   |
 *                   | wakeup
 *                   |
 *     +----------+  |  +----------+
 *     |  STOPPED |<--->| RUNNING  |
 *     +----------+     +----------+
 *           \              |
 *            \             |
 *             \            v
 *              \---->  +--------+
 *                      |  DEAD  |
 *                      +--------+
 *
 */
typedef enum sch_task_state {
        SCH_TASK_STATE_RUNNING,  // 就绪或运行
        SCH_TASK_STATE_SLEEPING, // 阻塞
        SCH_TASK_STATE_STOPPED,  // 手动挂起
        SCH_TASK_STATE_DEAD,     // 已结束
} sch_task_state_e;

typedef struct sch_task_cfg {
        usize    id;           // 任务 ID
        u32      priority;     // 任务优先级, 数值越小优先级越高
        usize    exec_freq;    // 执行频率
        usize    exec_cnt_max; // 最多执行次数
        usize    delay_tick;   // 初始延时
        sch_cb_f f_cb;         // 回调函数
        void    *arg;          // 回调参数
} sch_task_cfg_t;

typedef struct sch_task_status {
        sch_task_state_e e_state;
        usize            exec_cnt;
        f32              elapsed_us;
        usize            create_ts;
        usize            next_exec_ts;
} sch_task_status_t;

typedef struct sch_task {
        sch_task_cfg_t    cfg;
        sch_task_status_t status;
        rb_node_t         rb_node;
} sch_task_t;

struct sch;
typedef u64 (*sch_get_ts_f)(void);
typedef sch_task_t *(*sch_get_task_f)(struct sch *sched);
typedef void (*sch_insert_task_f)(struct sch *sched, sch_task_t *task);
typedef void (*sch_remove_task_f)(struct sch *sched, sch_task_t *task);

typedef enum sch_type {
        SCHED_TYPE_FCFS,
        SCHED_TYPE_CFS,
} sch_type_e;

typedef enum sch_tick {
        SCHED_TICK_US,
        SCHED_TICK_MS,
} sch_tick_e;

typedef struct sch_cfg {
        u8           cpu_id;
        sch_type_e   e_type;
        sch_tick_e   e_tick;
        sch_get_ts_f f_get_ts;
} sch_cfg_t;

typedef struct sch_lo {
        f32               elapsed_us;
        usize             curr_ts;
        usize             task_num;
        sch_task_t        tasks[SCH_TASK_MAX];
        sch_algo_ctx_u    algo_ctx;
        sch_get_task_f    f_get_task;
        sch_insert_task_f f_insert_task;
        sch_remove_task_f f_remove_task;
} sch_lo_t;

typedef struct sch {
        sch_cfg_t cfg;
        sch_lo_t  lo;
} sch_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

u64         sch_hz2tick(sch_t *sched, f32 hz);
int         sch_cfs_task_cmp(const sch_task_t *a, const sch_task_t *b);
void        sch_cfs_insert_task(sch_t *sched, sch_task_t *task);
void        sch_cfs_remove_task(sch_t *sched, sch_task_t *task);
sch_task_t *sch_cfs_get_task(sch_t *sched);
sch_task_t *sch_fcfs_get_task(sch_t *sched);
int         sch_add_task(sch_t *sched, sch_task_cfg_t task_cfg);
int         sch_init(sch_t *sched, sch_cfg_t sched_cfg);
int         sch_exec(sch_t *sched);

#ifdef __cplusplus
}
#endif

#endif // !SCH_H
