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
#include "libc/assert.h"
#include "libc/atomic.h"
#include "libc/calls/calls.h"
#include "libc/calls/state.internal.h"
#include "libc/calls/struct/sigaction.h"
#include "libc/calls/struct/sigset.h"
#include "libc/calls/struct/utsname.h"
#include "libc/calls/syscall-sysv.internal.h"
#include "libc/calls/ucontext.h"
#include "libc/dce.h"
#include "libc/errno.h"
#include "libc/fmt/itoa.h"
#include "libc/intrin/asan.internal.h"
#include "libc/intrin/atomic.h"
#include "libc/intrin/describebacktrace.internal.h"
#include "libc/intrin/describeflags.internal.h"
#include "libc/intrin/kprintf.h"
#include "libc/intrin/strace.internal.h"
#include "libc/intrin/weaken.h"
#include "libc/log/backtrace.internal.h"
#include "libc/log/gdb.h"
#include "libc/log/internal.h"
#include "libc/log/log.h"
#include "libc/macros.internal.h"
#include "libc/math.h"
#include "libc/nexgen32e/stackframe.h"
#include "libc/runtime/internal.h"
#include "libc/runtime/pc.internal.h"
#include "libc/runtime/runtime.h"
#include "libc/stdio/stdio.h"
#include "libc/str/str.h"
#include "libc/sysv/consts/auxv.h"
#include "libc/sysv/consts/sig.h"
#include "libc/thread/thread.h"
#include "libc/thread/tls.h"
#ifdef __x86_64__

__static_yoink("strerror_wr");  // for kprintf %m
__static_yoink("strsignal_r");  // for kprintf %G

static const char kGregOrder[17] forcealign(1) = {
    13, 11, 8, 14, 12, 9, 10, 15, 16, 0, 1, 2, 3, 4, 5, 6, 7,
};

static const char kGregNames[17][4] forcealign(1) = {
    "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15", "RDI",
    "RSI", "RBP", "RBX", "RDX", "RAX", "RCX", "RSP", "RIP",
};

static const char kCpuFlags[12] forcealign(1) = "CVPRAKZSTIDO";
static const char kFpuExceptions[6] forcealign(1) = "IDZOUP";

relegated static void ShowFunctionCalls(ucontext_t *ctx) {
  struct StackFrame *bp;
  struct StackFrame goodframe;
  if (!ctx->uc_mcontext.rip) {
    kprintf("%s is NULL can't show backtrace\n", "RIP");
  } else {
    goodframe.next = (struct StackFrame *)ctx->uc_mcontext.rbp;
    goodframe.addr = ctx->uc_mcontext.rip;
    bp = &goodframe;
    ShowBacktrace(2, bp);
  }
}

relegated static char *AddFlag(char *p, int b, const char *s) {
  if (b) {
    p = stpcpy(p, s);
  } else {
    *p = 0;
  }
  return p;
}

relegated static char *DescribeCpuFlags(char *p, int flags, int x87sw,
                                        int mxcsr) {
  unsigned i;
  for (i = 0; i < ARRAYLEN(kCpuFlags); ++i) {
    if (flags & 1) {
      *p++ = ' ';
      *p++ = kCpuFlags[i];
      *p++ = 'F';
    }
    flags >>= 1;
  }
  for (i = 0; i < ARRAYLEN(kFpuExceptions); ++i) {
    if ((x87sw | mxcsr) & (1 << i)) {
      *p++ = ' ';
      *p++ = kFpuExceptions[i];
      *p++ = 'E';
    }
  }
  p = AddFlag(p, x87sw & FPU_SF, " SF");
  p = AddFlag(p, x87sw & FPU_C0, " C0");
  p = AddFlag(p, x87sw & FPU_C1, " C1");
  p = AddFlag(p, x87sw & FPU_C2, " C2");
  p = AddFlag(p, x87sw & FPU_C3, " C3");
  return p;
}

static char *HexCpy(char p[hasatleast 17], uint64_t x, uint8_t k) {
  while (k > 0) *p++ = "0123456789abcdef"[(x >> (k -= 4)) & 15];
  *p = '\0';
  return p;
}

relegated static char *ShowGeneralRegisters(char *p, ucontext_t *ctx) {
  int64_t x;
  const char *s;
  size_t i, j, k;
  long double st;
  *p++ = '\n';
  for (i = 0, j = 0, k = 0; i < ARRAYLEN(kGregNames); ++i) {
    if (j > 0) *p++ = ' ';
    if (!(s = kGregNames[(unsigned)kGregOrder[i]])[2]) *p++ = ' ';
    p = stpcpy(p, s), *p++ = ' ';
    p = HexCpy(p, ctx->uc_mcontext.gregs[(unsigned)kGregOrder[i]], 64);
    if (++j == 3) {
      j = 0;
      if (ctx->uc_mcontext.fpregs) {
        memcpy(&st, (char *)&ctx->uc_mcontext.fpregs->st[k], sizeof(st));
        p = stpcpy(p, " ST(");
        p = FormatUint64(p, k++);
        p = stpcpy(p, ") ");
        if (signbit(st)) {
          st = -st;
          *p++ = '-';
        }
        if (isnan(st)) {
          p = stpcpy(p, "nan");
        } else if (isinf(st)) {
          p = stpcpy(p, "inf");
        } else {
          if (st > 999.999) st = 999.999;
          x = st * 1000;
          p = FormatUint64(p, x / 1000), *p++ = '.';
          p = FormatUint64(p, x % 1000);
        }
      }
      *p++ = '\n';
    }
  }
  DescribeCpuFlags(
      p, ctx->uc_mcontext.eflags,
      ctx->uc_mcontext.fpregs ? ctx->uc_mcontext.fpregs->swd : 0,
      ctx->uc_mcontext.fpregs ? ctx->uc_mcontext.fpregs->mxcsr : 0);
  *p++ = '\n';
  return p;
}

relegated static char *ShowSseRegisters(char *p, ucontext_t *ctx) {
  size_t i;
  if (ctx->uc_mcontext.fpregs) {
    *p++ = '\n';
    for (i = 0; i < 8; ++i) {
      *p++ = 'X';
      *p++ = 'M';
      *p++ = 'M';
      if (i >= 10) {
        *p++ = i / 10 + '0';
        *p++ = i % 10 + '0';
      } else {
        *p++ = i + '0';
        *p++ = ' ';
      }
      *p++ = ' ';
      p = HexCpy(p, ctx->uc_mcontext.fpregs->xmm[i + 0].u64[1], 64);
      p = HexCpy(p, ctx->uc_mcontext.fpregs->xmm[i + 0].u64[0], 64);
      p = stpcpy(p, " XMM");
      if (i + 8 >= 10) {
        *p++ = (i + 8) / 10 + '0';
        *p++ = (i + 8) % 10 + '0';
      } else {
        *p++ = (i + 8) + '0';
        *p++ = ' ';
      }
      *p++ = ' ';
      p = HexCpy(p, ctx->uc_mcontext.fpregs->xmm[i + 8].u64[1], 64);
      p = HexCpy(p, ctx->uc_mcontext.fpregs->xmm[i + 8].u64[0], 64);
      *p++ = '\n';
    }
  }
  return p;
}

void ShowCrashReportHook(int, int, int, struct siginfo *, ucontext_t *);

relegated void ShowCrashReport(int err, int sig, struct siginfo *si,
                               ucontext_t *ctx) {
  int i;
  char *p;
  char host[64];
  char buf[3000];
  struct utsname names;
  if (_weaken(ShowCrashReportHook)) {
    ShowCrashReportHook(2, err, sig, si, ctx);
  }
  names.sysname[0] = 0;
  names.release[0] = 0;
  names.version[0] = 0;
  names.nodename[0] = 0;
  stpcpy(host, "unknown");
  gethostname(host, sizeof(host));
  uname(&names);
  errno = err;
  // TODO(jart): Buffer the WHOLE crash report with backtrace for atomic write.
  p = buf;
  p += ksnprintf(
      p, 10000,
      "\n%serror%s: Uncaught %G (%s) on %s pid %d tid %d\n"
      "  %s\n"
      "  %s\n"
      "  %s %s %s %s\n",
      !__nocolor ? "\e[30;101m" : "", !__nocolor ? "\e[0m" : "", sig,
      (ctx &&
       (ctx->uc_mcontext.rsp >= GetStaticStackAddr(0) &&
        ctx->uc_mcontext.rsp <= GetStaticStackAddr(0) + getauxval(AT_PAGESZ)))
          ? "Stack Overflow"
          : DescribeSiCode(sig, si->si_code),
      host, getpid(), gettid(), program_invocation_name, strerror(err),
      names.sysname, names.version, names.nodename, names.release);
  if (ctx) {
    p = ShowGeneralRegisters(p, ctx);
    p = ShowSseRegisters(p, ctx);
    *p++ = '\n';
    klog(buf, p - buf);
    ShowFunctionCalls(ctx);
  } else {
    *p++ = '\n';
    klog(buf, p - buf);
  }
  kprintf("\n");
  if (!IsWindows()) __print_maps();
  /* PrintSystemMappings(2); */
  if (__argv) {
    for (i = 0; i < __argc; ++i) {
      if (!__argv[i]) continue;
      if (IsAsan() && !__asan_is_valid_str(__argv[i])) continue;
      kprintf("%s ", __argv[i]);
    }
  }
  kprintf("\n");
}

static relegated wontreturn void RaiseCrash(int sig) {
  sigset_t ss;
  sigfillset(&ss);
  sigdelset(&ss, sig);
  sigprocmask(SIG_SETMASK, &ss, 0);
  signal(sig, SIG_DFL);
  kill(getpid(), sig);
  _Exit(128 + sig);
}

relegated void __oncrash_amd64(int sig, struct siginfo *si, void *arg) {
  int gdbpid, err;
  ucontext_t *ctx = arg;

  // print vital error nubers reliably
  // the surface are of code this calls is small and audited
  kprintf(
      "\r\n\e[1;31m__oncrash %G %s pid %d tid %d rip %x bt %s\e[0m\n", sig,
      program_invocation_short_name, getpid(), sys_gettid(),
      ctx ? ctx->uc_mcontext.rip : 0,
      DescribeBacktrace(ctx ? (struct StackFrame *)ctx->uc_mcontext.rbp
                            : (struct StackFrame *)__builtin_frame_address(0)));

  // print friendlier detailed crash report less reliably
  // we're in a broken runtime state and so much can go wrong
  ftrace_enabled(-1);
  strace_enabled(-1);
  err = errno;
  if ((gdbpid = IsDebuggerPresent(true))) {
    DebugBreak();
  }
  if (!(gdbpid > 0 && (sig == SIGTRAP || sig == SIGQUIT))) {
    __restore_tty();
    ShowCrashReport(err, sig, si, ctx);
    RaiseCrash(sig);
  }
}

#endif /* __x86_64__ */
