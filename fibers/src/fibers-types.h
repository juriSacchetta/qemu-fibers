#pragma once 

#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "../pth/pth.h"


typedef struct qemu_fiber{
    CPUArchState *env;
    int fiber_tid;
    pth_t thread;
    QLIST_ENTRY(qemu_fiber) entry;
} qemu_fiber;


