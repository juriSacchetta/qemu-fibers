#include "qemu/osdep.h"
#include "qemu.h"
#include "user-internals.h"
#include "qemu/guest-random.h"

#include "cpu_loop-common.h"
#include "qemu/queue.h"

#include "pth.h"
#include "fibers.h"

void force_sig_env(CPUArchState *env, int sig);


typedef struct {
    CPUArchState *env;
    int tid;
    abi_ulong child_tidptr;
    abi_ulong parent_tidptr;
    sigset_t sigmask;
} new_thread_info;

typedef struct qemu_fiber{
    CPUArchState *env;
    int fibers_tid;
    pth_t thread;
    QLIST_ENTRY(qemu_fiber) entry;
} qemu_fiber;
QLIST_HEAD(qemu_fiber_list, qemu_fiber) fiber_list_head;

typedef struct fibers_futex {
    qemu_fiber *fiber;
    int *futex_uaddr;
    uint32_t mask;
    bool waiting;
    QLIST_ENTRY(fibers_futex) entry;
} fibers_futex;
QLIST_HEAD(fiber_futex_list, fibers_futex) futex_list;

typedef struct qemu_fiber_fd {
    int fd;
    int original_flags;
    QLIST_ENTRY(qemu_fiber_fd) entry;
} qemu_fiber_fd;

#define BASE_FIBERS_TID 0x3fffffff
qemu_fiber *fibers = NULL;

int fibers_count = BASE_FIBERS_TID;

static qemu_fiber * search_fiber_by_fibers_tid(int fibers_tid) {
    qemu_fiber *current;
    QLIST_FOREACH(current, &fiber_list_head, entry) {
        if (current->fibers_tid == fibers_tid) return current;
    }
    return NULL;
}

static qemu_fiber * search_fiber_by_pth(pth_t thread) {
    qemu_fiber *current;
    QLIST_FOREACH(current, &fiber_list_head, entry) {
        if (current->thread == thread) return current;
    }
    return NULL;
}

void qemu_fibers_init(CPUArchState *env)
{
    QLIST_INIT(&fiber_list_head);
    QLIST_INIT(&futex_list);
    qemu_fiber * new = malloc(sizeof(qemu_fiber));
    memset(new, 0, sizeof(qemu_fiber));

    new->env = env;
    new->fibers_tid = 0;
    new->thread = pth_init();
    QLIST_INSERT_HEAD(&fiber_list_head, new, entry);
}

static void qemu_fibers_exec(void *fiber_instance)
{
    CPUState *cpu;
    TaskState *ts;
    
    qemu_fiber *current = ((qemu_fiber *) fiber_instance);
    cpu = env_cpu(current->env);
    ts = (TaskState *)cpu->opaque;
    task_settid(ts);
    thread_cpu = cpu;

    fprintf(stderr, "qemu_fibers: starting 0x%d\n", current->fibers_tid - BASE_FIBERS_TID);
    cpu_loop(current->env);
}

int qemu_fibers_spawn(void *info_void)
{
    //TODO: Creare una interfacia unica per new_thread_info
    new_thread_info *info = (new_thread_info*)info_void;
    CPUArchState *env;
    CPUState *cpu;

    int new_tid = fibers_count++;
    env = info->env;
    cpu = env_cpu(env);
    info->tid = new_tid; // sys_gettid();
    if (info->child_tidptr)
        put_user_u32(info->tid, info->child_tidptr);
    if (info->parent_tidptr)
        put_user_u32(info->tid, info->parent_tidptr);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    qemu_fiber *new = malloc(sizeof(qemu_fiber));
    memset(new, 0, sizeof(qemu_fiber));

    QLIST_INSERT_HEAD(&fiber_list_head, new, entry);
    
    new->env = env;
    new->fibers_tid = new_tid;
    fprintf(stderr, "qemu_fibers: spawning 0x%d\n", new_tid - BASE_FIBERS_TID);
    new->thread = pth_spawn(PTH_ATTR_DEFAULT, (void* (*) (void*))qemu_fibers_exec, (void *) new);
    return new_tid;
}

/*
Thread Control
*/

static fibers_futex * create_futex(int *uaddr, uint32_t mask) {
    fibers_futex *new = malloc(sizeof(fibers_futex));
    memset(new, 0, sizeof(fibers_futex));
    new->fiber = search_fiber_by_pth(pth_self());
    new->futex_uaddr = uaddr;
    new->waiting = true;
    new->mask = mask;
    QLIST_INSERT_HEAD(&futex_list, new, entry);
    return new;
}

static void destroy_futex(fibers_futex *futex) {
    QLIST_REMOVE(futex, entry);
    free(futex);
}

static int fibers_futex_wait(int* uaddr, int val, struct timespec *pts) {
    fibers_futex *futex = NULL;
    fprintf(stderr, "qemu_fiber: futex wait 0x%x %d\n", *uaddr, val);

    while(__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) == val) {
        if(!futex) {
            futex = create_futex(uaddr, 0);
        } else if(futex->waiting == false) {
            destroy_futex(futex);
            futex = NULL;
            return 0;
        }
        if (pts != NULL) {
            pth_wait(pth_event(PTH_EVENT_TIME, pts->tv_sec, pts->tv_nsec/1000));
        }
        pth_yield(NULL);
    }
    return 0;
}

static int fibers_futex_wait_bitset(int* uaddr, int val, struct timespec *pts, uint32_t mask) {
    fibers_futex *futex = NULL;

    fprintf(stderr, "qemu_fiber: futex wait_bitset 0x%x %d %x\n", *uaddr, val, mask);

    while(__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) == val) {
        if(!futex) {
            futex = create_futex(uaddr, mask);
        } else if(futex->waiting == false) {
            destroy_futex(futex);
            futex = NULL;
            return 0;
        }
        if (pts != NULL) {
            pth_wait(pth_event(PTH_EVENT_TIME, pts->tv_sec, pts->tv_nsec/1000));
            return 0;
        }
        pth_yield(NULL);
    }
    return 0;
}

static int fibers_futex_wake(int* uaddr, int val) {

    fprintf(stderr, "qemu_fiber: futex wake %p %d\n", uaddr, val);
    
    int count = 0;
    fibers_futex *current;
    QLIST_FOREACH(current, &futex_list, entry) {
        current->waiting = false;
        if(count == val) goto exit;
        else count++;
    }
    exit:
    pth_yield(NULL);
    return count;
}

static int fibers_futex_wake_bitset(int* uaddr, int val, uint32_t mask) {
    fprintf(stderr, "qemu_fiber: futex wake_bitset %p %d %d\n", uaddr, val, mask);
    int count = 0;
    fibers_futex *current;
    QLIST_FOREACH(current, &futex_list, entry) {
        if((current->mask & mask) == 0) continue;
        current->waiting = false;
        if(count == val) goto exit;
        else count++;
    }
    exit: 
    pth_yield(NULL);
    return count;
}


// static int fibers_futex_requeue(CPUState *cpu, int base_op, int val, int val3,
//                     target_ulong uaddr, 
//                     target_ulong uaddr2, target_ulong timeout) {
    // uint32_t val2 = (uint32_t)timeout;
    // int* to_check = (int*)g2h(cpu, uaddr);
    // int expected = tswap32(val3);
    // if (base_op == FUTEX_CMP_REQUEUE &&
    //     __atomic_load_n(to_check, __ATOMIC_ACQUIRE) != expected)
    //     return -EAGAIN;
    // int i, j, k;
    // for (i = 0, j = 0, k = 0; i < fibers_count; ++i) {
    //     if (fibers[i].is_zombie) continue;
    //     if (fibers[i].waiting_futex && fibers[i].futex_addr == uaddr) {
    //         if (j < val) {
    //             ++j;
    //             fibers[i].waiting_futex = 0;
    //         } else if (k < val2) {
    //             ++k;
    //             fibers[i].futex_addr = uaddr2;
    //         }
    //     }
    // }
    // if (val) qemu_fibers_switch();
    //return j+k;
//     return 0;
// }

int fibers_do_futex(int *uaddr, int op, int val, const struct timespec *timeout, int *uaddr2, int val3) {
    switch (op) {
        case FUTEX_WAIT:
            return fibers_futex_wait(uaddr, val, (struct timespec *)timeout);
        case FUTEX_WAIT_BITSET:
            return fibers_futex_wait_bitset(uaddr, val, (struct timespec *)timeout, val3);
        case FUTEX_WAIT_REQUEUE_PI:
        case FUTEX_LOCK_PI:
        case FUTEX_LOCK_PI2:
        case FUTEX_WAKE:
            return fibers_futex_wake(uaddr, val);
        case FUTEX_WAKE_BITSET:
            return fibers_futex_wake_bitset(uaddr, val, val3);
        case FUTEX_TRYLOCK_PI:
        case FUTEX_UNLOCK_PI:
        case FUTEX_FD:
        case FUTEX_CMP_REQUEUE:
        case FUTEX_CMP_REQUEUE_PI:
        case FUTEX_REQUEUE:
        case FUTEX_WAKE_OP:
            break;
        default:
            return -TARGET_ENOSYS;
    }
    return -TARGET_ENOSYS;
}

/*
## Syscall replacements
*/

void fibers_syscall_exit(void *value) {
    pth_exit(value);
}

//TODO: check parametes 
int fibers_syscall_tkill(abi_long tid, abi_long sig) {
    if (tid > fibers_count) exit(-1); //TODO: Improve this error managing
    qemu_fiber *current = search_fiber_by_fibers_tid(tid);
    if (current == NULL) return -ESRCH;
    force_sig_env(current->env, sig);
    return 0;
}
//TODO: check parametes 
int fibers_syscall_tgkill(abi_long arg1, abi_long arg2, abi_long arg3) {
    if (arg2 > fibers_count) exit(-1); //TODO: Improve this error managing
    qemu_fiber *current = search_fiber_by_fibers_tid(arg2);
    if(current == NULL) return -ESRCH;
    force_sig_env(current->env, arg3);
    return 0;
}

int fibers_syscall_gettid(void) {
    pth_t me = pth_self();
    qemu_fiber *current = search_fiber_by_pth(me);
    if (current == NULL) exit(-1); //TODO: Logic error
    return current->fibers_tid;
}

int fibers_syscall_nanosleep(struct timespec *ts){
    fprintf(stderr, "qemu_fiber: nanosleep %ld %ld\n", ts->tv_sec, ts->tv_nsec/1000);
    return pth_nanosleep(ts, NULL); //TODO: Check this NULL
}

int fibers_syscall_clock_nanosleep(clockid_t clock_id, struct timespec *ts){
    fprintf(stderr, "qemu_fiber: clock_nanosleep %ld %ld\n", ts->tv_sec, ts->tv_nsec/1000);
    return pth_nanosleep(ts, NULL); //TODO: Check this NULL
}


/*##########
# I/O OPERATIONS
###########*/
ssize_t fibers_read(int fd, void *buf, size_t nbytes) {
    return pth_read(fd, buf, nbytes);
}

ssize_t fibers_write(int fd, const void *buf, size_t nbytes) {
    return pth_write(fd, buf, nbytes);
}