#ifndef SCH_H
#define SCH_H

#include "list.h"
#include "rbtree.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

#ifndef SCH_TASK_MAX
#define SCH_TASK_MAX (16)
#endif

#define SCH_PRIORITY_MAX (32)

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef void (*sch_cb_f)(void *arg);

typedef union sch_algo_ctx {
    struct {
        size_t prev_idx;
    } fcfs;
    struct {
        struct rb_root rb_root;
    } cfs;
    struct {
        struct rb_root   timer_root;
        struct list_head ready_queues[SCH_PRIORITY_MAX];
        uint32_t         ready_bitmap;
    } fixed_priority;
} sch_algo_ctx_u;

/*
 *             +-----------+
 *             |   休眠    |
 *             +-----------+
 *                   ^
 *                   |
 *                   | 唤醒
 *                   |
 *     +----------+  |  +----------+
 *     |   停止   |<--->|   运行   |
 *     +----------+     +----------+
 *           \              |
 *            \             |
 *             \            v
 *              \---->  +--------+
 *                      |  结束  |
 *                      +--------+
 *
 */
enum sch_task_state {
    SCH_TASK_STATE_RUN,   // 就绪或运行
    SCH_TASK_STATE_SLEEP, // 阻塞
    SCH_TASK_STATE_STOP,  // 手动挂起
    SCH_TASK_STATE_DEAD,  // 已结束
};

struct sch_task_cfg {
    size_t              id;            // 任务 ID
    uint32_t            priority;      // 任务优先级, 数值越小优先级越高
    float32_t           exec_freq;     // 执行频率
    size_t              exec_cnt_max;  // 最多执行次数
    enum sch_task_state e_init_state;  // 初始任务状态
    size_t              init_delay_us; // 初始延时
    uint32_t            budget_us;     // 单次执行预算, 0 表示不检查超时
    sch_cb_f
        f_init; // 初始化函数, 新建任务 (初始状态不为 DEAD) 或从 DEAD 状态变为其他状态时调用一次
    sch_cb_f f_exec;   // 执行函数, 周期调用
    sch_cb_f f_deinit; // 清理函数, 任务从其他状态变为 DEAD 时调用一次
    void    *arg;      // 回调参数
};

struct sch_task_status {
    size_t              exec_cnt;
    uint32_t            elapsed_us;
    uint32_t            elapsed_us_max;
    uint32_t            overrun_cnt;
    enum sch_task_state e_state;
    enum sch_task_state e_prev_state;
    size_t              next_exec_ts;
};

struct sch_task {
    struct sch_task_cfg    cfg;
    struct sch_task_status status;
    struct rb_node         rb_node;
    struct list_head       ready_node;
    uint8_t                in_time_queue;
    uint8_t                in_ready_queue;
};

struct sch;
typedef uint64_t (*sch_get_ts_f)(void);
typedef struct sch_task *(*sch_get_task_f)(struct sch *sch);
typedef void (*sch_insert_task_f)(struct sch *sch, struct sch_task *task);
typedef void (*sch_remove_task_f)(struct sch *sch, struct sch_task *task);

enum sch_type {
    SCH_TYPE_FCFS,
    SCH_TYPE_CFS,
    SCH_TYPE_FIXED_PRIORITY,
};

struct sch_cfg {
    uint8_t       cpu_id;
    enum sch_type e_type;
    sch_get_ts_f  f_get_ts;
};

struct sch_lo {
    uint32_t        elapsed_us_max;
    size_t          curr_ts;
    size_t          ntasks;
    struct sch_task tasks[SCH_TASK_MAX];
    sch_algo_ctx_u  algo_ctx;
    sch_get_task_f  f_get_task;
};

struct sch_tmp {
    uint32_t elapsed_us, prev_elapsed_us;
};

struct sch {
    struct sch_cfg cfg;
    struct sch_lo  lo;
    struct sch_tmp tmp;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

int sch_init(struct sch *sch, struct sch_cfg sch_cfg);
int sch_run(struct sch *sch);
int sch_exec(struct sch *sch);

int sch_add_task(struct sch *sch, struct sch_task_cfg task_cfg);
int sch_reinit_tasks(struct sch *sch); // 重新调用所有已注册任务的 f_init (热重载后同步各任务子模块)
int sch_set_task_freq(struct sch *sch, size_t task_id, size_t exec_freq);
int sch_set_task_state(struct sch *sch, const size_t id, const enum sch_task_state e_state);

#ifdef __cplusplus
}
#endif

#endif // !SCH_H
