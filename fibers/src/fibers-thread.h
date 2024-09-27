#include "fibers-types.h"
#include "qemu/osdep.h"

extern int fiber_count;

void fiber_thread_init(CPUArchState *cpu);
void fiber_thread_clear_all(void);
qemu_fiber * fiber_thread_by_pth(pth_t thread);
qemu_fiber * fiber_thread_by_tid(int fiber_tid);