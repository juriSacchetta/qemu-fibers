#include "qemu/queue.h"
#include "src/fibers-types.h"
#include "src/fibers-thread.h"
#include "fibers.h"

QLIST_HEAD(qemu_fiber_list, qemu_fiber) fiber_list_head;
int fiber_count = BASE_FIBERS_TID;

void fiber_thread_init(CPUArchState *cpu) {
    QLIST_INIT(&fiber_list_head);
    fiber_register(pth_init(env_cpu(cpu)), cpu);
}

int fiber_register(pth_t thread, CPUArchState *cpu) {
    qemu_fiber *new = malloc(sizeof(qemu_fiber));
    memset(new, 0, sizeof(qemu_fiber));
    new->env = cpu;
    new->thread = thread;
    new->fiber_tid = ++fiber_count;
    QLIST_INSERT_HEAD(&fiber_list_head, new, entry);
    return new->fiber_tid;
}

bool fiber_unregister(pth_t thread) {
    qemu_fiber *current;
    bool found = false;
    QLIST_FOREACH(current, &fiber_list_head, entry) {
        if (current->thread == thread) {
            QLIST_REMOVE(current, entry);
            free(current);
            found = true;
            break;
        }
    }
    return found;
}

void fiber_thread_clear_all(void) {
    qemu_fiber *current;
    QLIST_FOREACH(current, &fiber_list_head, entry) {
        if(current->thread == pth_self()) continue;
        QLIST_REMOVE(current, entry);
        free(current);
    }
    int count = 0;
    QLIST_FOREACH(current, &fiber_list_head, entry) {
        count++;
    }
    assert(count == 1);
}

qemu_fiber * fiber_thread_by_pth(pth_t thread) {
    qemu_fiber *current;
    QLIST_FOREACH(current, &fiber_list_head, entry) {
        if (current->thread == thread) return current;
    }
    return NULL;
}

qemu_fiber * fiber_thread_by_tid(int fiber_tid) {
    qemu_fiber *current;
    QLIST_FOREACH(current, &fiber_list_head, entry) {
        if (current->fiber_tid == fiber_tid) return current;
    }
    return NULL;
}