#include "mpsc.h"

void
mpsc_init(mpsc_t *mpsc)
{
        ATOMIC_STORE_EXPLICIT(&mpsc->stub.next, NULL, ATOMIC_RELAXED);
        ATOMIC_STORE_EXPLICIT(&mpsc->head, &mpsc->stub, ATOMIC_RELEASE);
        mpsc->tail = &mpsc->stub;
}

void
mpsc_push(mpsc_t *mpsc, mpsc_node_t *node)
{
        // 1. 初始化新节点的 next
        ATOMIC_STORE_EXPLICIT(&node->next, NULL, ATOMIC_RELAXED);

        // 2. 原子地交换头指针，硬件串行化多个生产者
        mpsc_node_t *prev = ATOMIC_EXCHANGE_EXPLICIT(&mpsc->head, node, ATOMIC_RELEASE);

        // 3. 把旧头节点的 next 链到自己。step2/step3 之间被打断时，
        //    本节点对消费者暂时不可见（lock-free，非 wait-free）。
        ATOMIC_STORE_EXPLICIT(&prev->next, node, ATOMIC_RELEASE);
}

mpsc_node_t *
mpsc_pop(mpsc_t *mpsc)
{
        mpsc_node_t *tail = mpsc->tail;
        mpsc_node_t *next = ATOMIC_LOAD_EXPLICIT(&tail->next, ATOMIC_ACQUIRE);

        // 若当前 tail 是哨兵，先跳过它，把真正的数据节点作为待返回 tail
        if (tail == &mpsc->stub) {
                if (next == NULL)
                        return NULL; // 队列为空

                mpsc->tail = next;
                tail       = next;
                next       = ATOMIC_LOAD_EXPLICIT(&tail->next, ATOMIC_ACQUIRE);
        }

        if (next != NULL) {
                mpsc->tail = next;
                return tail;
        }

        // tail 是当前队尾。若 head 已超过 tail，说明有生产者卡在 step2/step3 之间，
        // 让调用方稍后重试即可；否则把哨兵重新链到队尾，便于本次返回 tail。
        mpsc_node_t *cur_head = ATOMIC_LOAD_EXPLICIT(&mpsc->head, ATOMIC_ACQUIRE);
        if (tail != cur_head)
                return NULL;

        mpsc_push(mpsc, &mpsc->stub);

        next = ATOMIC_LOAD_EXPLICIT(&tail->next, ATOMIC_ACQUIRE);
        if (next != NULL) {
                mpsc->tail = next;
                return tail;
        }

        return NULL;
}
