#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "qemu/log.h"
#include "qemu.h"

#include "pth/pth.h"
#include "fibers.h"
#include "src/fibers-futex.h"
#include "src/fibers-utils.h"

#define FUTEX_BITSET_MATCH_ANY 0xffffffff

// TODO: Check types, are you sure on the size?
// TODO: Check the timeout relative/absolute time
typedef struct fiber_futex
{
    pth_t pth_thread;
    int *uaddr;
    pth_cond_t cond;
    uint32_t bitset;
    QLIST_ENTRY(fiber_futex)
    entry;
} fiber_futex;

QLIST_HEAD(fiber_futex_list, fiber_futex)
futex_list;

pth_mutex_t futex_mutex;

void fiber_futex_init(void)
{
    QLIST_INIT(&futex_list);
    pth_mutex_init(&futex_mutex);
}

void fiber_clean_futex(void)
{
    pth_mutex_acquire(&futex_mutex, FALSE, NULL);
    fiber_futex *futex = NULL, *tmp = NULL;
    QLIST_FOREACH_SAFE(futex, &futex_list, entry, tmp)
    {
        QLIST_REMOVE(futex, entry);
        free(futex);
    }
    QLIST_INIT(&futex_list);
    pth_mutex_release(&futex_mutex);
}

static inline bool match_futex(fiber_futex *futex, int *uaddr, uint32_t bitset)
{
    return futex && futex->uaddr == uaddr && (bitset == FUTEX_BITSET_MATCH_ANY || futex->bitset == bitset);
}

static int fiber_futex_wait(int *uaddr, int val, const struct timespec *pts, uint32_t bitset)
{
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val)
        return -TARGET_EAGAIN;

    pth_mutex_acquire(&futex_mutex, FALSE, NULL);

    fiber_futex *futex = malloc(sizeof(fiber_futex));
    memset(futex, 0, sizeof(fiber_futex));
    futex->pth_thread = pth_self();
    futex->uaddr = uaddr;
    futex->bitset = bitset;

    pth_cond_init(&futex->cond);

    QLIST_INSERT_HEAD(&futex_list, futex, entry);

    pth_event_t timeout = NULL;
    if (pts != NULL)
    {
        timeout = pth_event(PTH_EVENT_TIME, pth_timeout(pts->tv_sec, pts->tv_nsec / 1000));
    }
    pth_cond_await(&futex->cond, &futex_mutex, timeout);

    QLIST_REMOVE(futex, entry);
    free(futex);

    pth_mutex_release(&futex_mutex);
    return 0;
}

static int fiber_futex_wake(int *uaddr, int val, uint32_t bitset)
{
    if (!bitset)
        return -TARGET_EINVAL;

    pth_mutex_acquire(&futex_mutex, FALSE, NULL);
    int count = 0;
    fiber_futex *current = NULL;
    QLIST_FOREACH(current, &futex_list, entry)
    {
        if (!match_futex(current, uaddr, bitset))
            continue;
        count++;
        pth_cond_notify(&current->cond, TRUE);
    }
    pth_mutex_release(&futex_mutex);
    return count;
}

static int fiber_futex_requeue(int op, int *uaddr, uint32_t val, uint32_t val2, int *uaddr2, int val3)
{
    fiber_futex *current = NULL;
    if (op == FUTEX_CMP_REQUEUE && __atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val3)
        return -TARGET_EAGAIN;
    int count_ops = 0;
    int count_requeqe = 0;
    QLIST_FOREACH(current, &futex_list, entry)
    {
        if (!match_futex(current, uaddr, FUTEX_BITSET_MATCH_ANY))
            continue;
        if (count_ops < val)
        {
            count_ops++;
            pth_cond_notify(&current->cond, TRUE);
        }
        else if (val2 != -1 && count_requeqe < val2)
        {
            current->uaddr = uaddr2;
            count_requeqe++;
            if (op == FUTEX_CMP_REQUEUE)
                count_ops++;
        }
    }
    return count_ops;
}

static inline bool valid_timeout(const struct timespec *timeout) {
    if(timeout == NULL) return true;
    if(timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000) return false;
    return true;
}

DEFINE_FIBER_SYSCALL(int, futex, int *uaddr, int op, int val, const struct timespec *timeout, int *uaddr2, int val3)
{
    if(!valid_timeout(timeout)){
        return -TARGET_EINVAL;
    }
    int base_op = op & FUTEX_CMD_MASK;
    switch (base_op)
    {
    case FUTEX_WAIT:
        val3 = FUTEX_BITSET_MATCH_ANY;
        // fallthrough
    case FUTEX_WAIT_BITSET:
        FIBERS_LOG_DEBUG("futex wait_bitset uaddr: %p val: %d val3: %p\n", uaddr, val, val3);
        return fiber_futex_wait(uaddr, val, timeout, val3);
    case FUTEX_WAKE:
        val3 = FUTEX_BITSET_MATCH_ANY;
        // fallthrough
    case FUTEX_WAKE_BITSET:
        return fiber_futex_wake(uaddr, val, val3);
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
        /* in case of FUTEX_CMP_REQUEUE timeout is interpreted as a counter
        of waiters that must be requeued. In the libc documetation the timeout's 
        type should be uint32_t*/
        FIBERS_LOG_DEBUG("futex FUTEX_CMP_REQUEUE uaddr: %p val: %d uaddr2: %p val3: %p\n", uaddr, val, uaddr2, val3);
        return fiber_futex_requeue(op, uaddr, val, (uint64_t) timeout, uaddr2, val3);
    case FUTEX_WAIT_REQUEUE_PI:
    case FUTEX_LOCK_PI:
    case FUTEX_LOCK_PI2:
    case FUTEX_TRYLOCK_PI:
    case FUTEX_UNLOCK_PI:
    case FUTEX_FD:
    case FUTEX_CMP_REQUEUE_PI:
    case FUTEX_WAKE_OP:
        exit(-1);
        break;
    default:
        return -TARGET_ENOSYS;
    }
    return -TARGET_ENOSYS;
}