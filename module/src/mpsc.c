#include "mpsc.h"

void
mpsc_init(struct mpsc *mpsc)
{
    ATOMIC_STORE_EXPLICIT(&mpsc->stub.next, NULL, ATOMIC_RELAXED);
    ATOMIC_STORE_EXPLICIT(&mpsc->head, &mpsc->stub, ATOMIC_RELEASE);
    mpsc->tail = &mpsc->stub;
}

void
mpsc_push(struct mpsc *mpsc, struct mpsc_node *node)
{
    ATOMIC_STORE_EXPLICIT(&node->next, NULL, ATOMIC_RELAXED);

    struct mpsc_node *prev = ATOMIC_EXCHANGE_EXPLICIT(&mpsc->head, node, ATOMIC_RELEASE);

    // 生产者在交换和链接之间被抢占时, 该节点暂时对消费者不可见.
    ATOMIC_STORE_EXPLICIT(&prev->next, node, ATOMIC_RELEASE);
}

struct mpsc_node *
mpsc_pop(struct mpsc *mpsc)
{
    struct mpsc_node *tail = mpsc->tail;
    struct mpsc_node *next = ATOMIC_LOAD_EXPLICIT(&tail->next, ATOMIC_ACQUIRE);

    if (tail == &mpsc->stub) {
        if (next == NULL)
            return NULL;

        mpsc->tail = next;
        tail       = next;
        next       = ATOMIC_LOAD_EXPLICIT(&tail->next, ATOMIC_ACQUIRE);
    }

    if (next != NULL) {
        mpsc->tail = next;
        return tail;
    }

    // head != tail 表示生产者已交换 head, 但尚未链接 next.
    struct mpsc_node *cur_head = ATOMIC_LOAD_EXPLICIT(&mpsc->head, ATOMIC_ACQUIRE);
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
