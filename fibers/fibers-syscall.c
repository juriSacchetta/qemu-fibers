#include <sys/prctl.h>

#include "qemu/osdep.h"
#include "qemu.h"
#include "user-internals.h"

#include "pth/pth.h"
#include "fibers.h"
#include "src/fibers-types.h"
#include "src/fibers-thread.h"
#include "src/fibers-utils.h"
//TODO: check this signature
extern void force_sig_env(CPUArchState *env, int sig);

DEFINE_FIBER_SYSCALL(int, accept4, int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    //TODO: check is is safe use pth_accept to emulate accept4
    FIBERS_LOG_DEBUG("accept4 sockfd: %d addr: %p addrlen: %d flags: %d\n", sockfd, addr, addrlen, flags);
    return pth_accept(sockfd, addr, addrlen);
}

DEFINE_FIBER_SYSCALL(int, clock_nanosleep, const clockid_t clock, int flags, const struct timespec * req, struct timespec * rem) {
    FIBERS_LOG_DEBUG("clock_nanosleep %ld %ld\n", ts->tv_sec, ts->tv_nsec/1000);
    return pth_nanosleep(req, NULL); //TODO: Check this NULL
}

DEFINE_FIBER_SYSCALL(int, connect, int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    FIBERS_LOG_DEBUG("connect sockfd: %d addr: %p addrlen: %d \n", sockfd, addr, addrlen);
    return pth_connect(sockfd, addr, addrlen);
}

DEFINE_FIBER_SYSCALL(int, gettid, void) {
    pth_t me = pth_self();
    qemu_fiber *current = fiber_thread_by_pth(me);
    assert(current != NULL);
    return current->fiber_tid;
}

DEFINE_FIBER_SYSCALL(int, nanosleep, const struct timespec *req, struct timespec *rem) {
    FIBERS_LOG_DEBUG("nanosleep %ld %ld\n", ts->tv_sec, ts->tv_nsec/1000);
    return pth_nanosleep(req, NULL);
}

DEFINE_FIBER_SYSCALL(int, ppoll, struct pollfd *fds, unsigned int nfds, const struct timespec *timeout_ts, const sigset_t *sigmask) {
    //TODO: check if is same use pth_poll to emulate ppoll
    FIBERS_LOG_DEBUG("ppoll pfd: %p nfds: %d timeout: %ld\n", pfd, nfds, timeout_ts->tv_nsec/1000);
    return pth_poll(fds, nfds, timeout_ts->tv_nsec/1000);
}

DEFINE_FIBER_SYSCALL(int, prctl, int option, abi_ulong arg2, abi_ulong arg3, abi_ulong arg4, abi_ulong arg5) {
    pth_attr_t  attr;
    switch (option) {
        case PR_SET_NAME:
            attr = pth_attr_of(pth_self());
            pth_attr_set(attr, PTH_ATTR_NAME, (char *)arg2);
            return 0;
        case PR_GET_NAME:
            attr = pth_attr_of(pth_self());
            pth_attr_get(attr, PTH_ATTR_NAME, (char *)arg2);
            FIBERS_LOG_DEBUG("PR_GET_NAME\n");
            return 0;
        default:
            qemu_log("prctl: unknown option %d\n", option);
            abort();
    }
}

DEFINE_FIBER_SYSCALL(ssize_t, pread64, int fd, void *buf, size_t nbytes, off_t offset) {
    //Use pth_pread to emulate pread64 seems to be safe
    FIBERS_LOG_DEBUG("pread fd: %d buf: %p count: %d offset: %d\n", fd, buf, count, offset);
    return pth_pread(fd, buf, nbytes, offset);
}

DEFINE_FIBER_SYSCALL(ssize_t, pwrite64, int fd, const void *buf, size_t nbytes, off_t offset) {
    //Use pth_pwrite to emulate pwrite64 seems to be safe
    FIBERS_LOG_DEBUG("pwrite fd: %d buf: %p count: %d offset: %d\n", fd, buf, count, offset);
    return pth_pwrite(fd, buf, nbytes, offset);
}

DEFINE_FIBER_SYSCALL(ssize_t, read, int fd, void *buf, size_t nbytes) {
    return pth_read(fd, buf, nbytes);
}

DEFINE_FIBER_SYSCALL(ssize_t, readv, int fd, const struct iovec *iov, int iovcnt) {
    FIBERS_LOG_DEBUG("readv fd: %d iov: %p iovcnt: %d\n", fd, iov, iovcnt);
    return pth_readv(fd, iov, iovcnt);
}

DEFINE_FIBER_SYSCALL(ssize_t, recvfrom, int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    FIBERS_LOG_DEBUG("recvfrom sockfd: %d buf: %p len: %d flags: %d src_addr: %p addrlen: %d\n", sockfd, buf, len, flags, src_addr, addrlen);
    return pth_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}

DEFINE_FIBER_SYSCALL(ssize_t, sendto, int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
    FIBERS_LOG_DEBUG("sendto sockfd: %d buf: %p len: %d flags: %d dest_addr: %p addrlen: %d\n", sockfd, buf, len, flags, dest_addr, addrlen);
    return pth_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

DEFINE_FIBER_SYSCALL(int, tkill, int tid, int sig) {
    assert(tid <= fiber_count);
    qemu_fiber *current = fiber_thread_by_tid(tid);
    if (current == NULL) return -TARGET_ESRCH;
    force_sig_env(current->env, sig);
    return 0;
}

DEFINE_FIBER_SYSCALL(int, tgkill, int arg1, int arg2, int arg3) {
    assert(arg2 > BASE_FIBERS_TID);
    qemu_fiber *current = fiber_thread_by_tid(arg2);
    if(current == NULL) return -TARGET_ESRCH;
    force_sig_env(current->env, arg3);
    return 0;
}

DEFINE_FIBER_SYSCALL(pid_t, wait4, pid_t pid, int *status, int options, struct rusage *rusage) {
    //TODO: check if is same use pth_waitpid to emulate wait4
    FIBERS_LOG_DEBUG("wait4 pid: %d status: %p options: %d rusage: %p\n", pid, status, options, rusage);
    return pth_waitpid(pid, status, options);
}

DEFINE_FIBER_SYSCALL(int, waitpid, pid_t pid, int *status, int options) {
    return pth_waitpid(pid, status, options);
}

DEFINE_FIBER_SYSCALL(ssize_t, write, int fd, const void *buf, size_t nbytes) {
    return pth_write(fd, buf, nbytes);
}

DEFINE_FIBER_SYSCALL(ssize_t, writev, int fd, const struct iovec *iov, int iovcnt) {
    FIBERS_LOG_DEBUG("writev fd: %d iov: %p iovcnt: %d\n", fd, iov, iovcnt);
    return pth_writev(fd, iov, iovcnt);
}