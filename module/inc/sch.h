#ifndef SCH_H
#define SCH_H

#include "rbtree.h"

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
        SCH_TASK_STATE_RUN,   // 就绪或运行
        SCH_TASK_STATE_SLEEP, // 阻塞
        SCH_TASK_STATE_STOP,  // 手动挂起
        SCH_TASK_STATE_DEAD,  // 已结束
} sch_task_state_e;

typedef struct sch_task_cfg {
        usize            id;            // 任务 ID
        u32              priority;      // 任务优先级, 数值越小优先级越高
        f32              exec_freq;     // 执行频率
        usize            exec_cnt_max;  // 最多执行次数
        sch_task_state_e e_init_state;  // 初始任务状态
        usize            init_delay_us; // 初始延时
        sch_cb_f         f_init;        // 初始化函数, 新建任务 (初始状态不为 DEAD) 或从 DEAD 状态变为其他状态时调用一次
        sch_cb_f         f_exec;        // 执行函数, 周期调用
        sch_cb_f         f_deinit;      // 清理函数, 任务从其他状态变为 DEAD 时调用一次
        void            *arg;           // 回调参数
} sch_task_cfg_t;

typedef struct sch_task_status {
        usize            exec_cnt;
        u32              elapsed_us;
        sch_task_state_e e_state;
        sch_task_state_e e_prev_state;
        usize            next_exec_ts;
} sch_task_status_t;

typedef struct sch_task {
        sch_task_cfg_t    cfg;
        sch_task_status_t status;
        rb_node_t         rb_node;
} sch_task_t;

struct sch;
typedef u64 (*sch_get_ts_f)(void);
typedef sch_task_t *(*sch_get_task_f)(struct sch *sch);
typedef void (*sch_insert_task_f)(struct sch *sch, sch_task_t *task);
typedef void (*sch_remove_task_f)(struct sch *sch, sch_task_t *task);

typedef enum sch_type {
        SCH_TYPE_FCFS,
        SCH_TYPE_CFS,
} sch_type_e;

typedef struct sch_cfg {
        u8           cpu_id;
        sch_type_e   e_type;
        sch_get_ts_f f_get_ts;
} sch_cfg_t;

typedef struct sch_lo {
        u32            elapsed_us_max;
        usize          curr_ts;
        usize          ntasks;
        sch_task_t     tasks[SCH_TASK_MAX];
        sch_algo_ctx_u algo_ctx;
        sch_get_task_f f_get_task;
} sch_lo_t;

typedef struct sch_tmp {
        u32 elapsed_us, prev_elapsed_us;
} sch_tmp_t;

typedef struct sch {
        sch_cfg_t cfg;
        sch_lo_t  lo;
        sch_tmp_t tmp;
} sch_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

int sch_init(sch_t *sch, sch_cfg_t sch_cfg);
int sch_run(sch_t *sch);
int sch_exec(sch_t *sch);

int sch_add_task(sch_t *sch, sch_task_cfg_t task_cfg);
int sch_reinit_tasks(sch_t *sch); // 重新调用所有已注册任务的 f_init (热重载后同步各任务子模块)
int sch_set_task_freq(sch_t *sch, usize task_id, usize exec_freq);
int sch_set_task_state(sch_t *sch, const usize id, const sch_task_state_e e_state);

#ifdef __cplusplus
}
#endif

#endif // !SCH_H
