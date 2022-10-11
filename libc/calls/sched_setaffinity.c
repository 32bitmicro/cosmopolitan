/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2020 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "libc/calls/calls.h"
#include "libc/calls/internal.h"
#include "libc/calls/sched-sysv.internal.h"
#include "libc/calls/struct/cpuset.h"
#include "libc/calls/syscall_support-nt.internal.h"
#include "libc/dce.h"
#include "libc/intrin/strace.internal.h"
#include "libc/nt/enum/processaccess.h"
#include "libc/nt/process.h"
#include "libc/nt/runtime.h"
#include "libc/sysv/errfuns.h"

static dontinline textwindows int sys_sched_setaffinity_nt(
    int pid, uint64_t size, const cpu_set_t *bitset) {
  int rc;
  int64_t h, closeme = -1;

  if (!pid || pid == getpid()) {
    h = GetCurrentProcess();
  } else if (__isfdkind(pid, kFdProcess)) {
    h = g_fds.p[pid].handle;
  } else {
    h = OpenProcess(kNtProcessSetInformation | kNtProcessQueryInformation,
                    false, pid);
    if (!h) return __winerr();
    closeme = h;
  }

  if (SetProcessAffinityMask(h, bitset->__bits[0])) {
    rc = 0;
  } else {
    rc = __winerr();
  }

  if (closeme != -1) {
    CloseHandle(closeme);
  }

  return rc;
}

/**
 * Asks kernel to only schedule process on particular CPUs.
 *
 * @param pid is the process or process id (or 0 for caller)
 * @param size is bytes in bitset, which should be `sizeof(cpuset_t)`
 * @return 0 on success, or -1 w/ errno
 * @raise ENOSYS if not Linux, FreeBSD, NetBSD, or Windows
 * @see pthread_getaffinity_np() for threads
 */
int sched_setaffinity(int pid, size_t size, const cpu_set_t *bitset) {
  int rc;
  if (size != sizeof(cpu_set_t)) {
    rc = einval();
  } else if (IsWindows()) {
    rc = sys_sched_setaffinity_nt(pid, size, bitset);
  } else if (IsFreebsd()) {
    rc = sys_sched_setaffinity_freebsd(CPU_LEVEL_WHICH, CPU_WHICH_PID, pid, 32,
                                       bitset);
  } else if (IsNetbsd()) {
    rc = sys_sched_setaffinity_netbsd(P_ALL_LWPS, pid, 32, bitset);
  } else {
    rc = sys_sched_setaffinity(pid, size, bitset);
  }
  STRACE("sched_setaffinity(%d, %'zu, %p) → %d% m", pid, size, bitset, rc);
  return rc;
}
