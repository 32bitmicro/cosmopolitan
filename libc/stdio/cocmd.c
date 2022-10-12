/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2022 Justine Alexandra Roberts Tunney                              │
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
#include "libc/errno.h"
#include "libc/fmt/itoa.h"
#include "libc/macros.internal.h"
#include "libc/mem/mem.h"
#include "libc/runtime/runtime.h"
#include "libc/stdio/stdio.h"
#include "libc/str/errfun.h"
#include "libc/str/str.h"
#include "libc/sysv/consts/o.h"

/**
 * @fileoverview Cosmopolitan Command Interpreter
 *
 * This is a lightweight command interpreter for GNU Make. It has just
 * enough shell script language support to support our build config.
 */

#define STATE_SHELL 0
#define STATE_STR   1
#define STATE_QUO   2

static char *p;
static char *q;
static size_t n;
static char *cmd;
static char *args[8192];
static const char *prog;
static char argbuf[ARG_MAX];
static bool unsupported[256];

static wontreturn void Wexit(int rc, const char *s, ...) {
  va_list va;
  va_start(va, s);
  do {
    write(2, s, strlen(s));
  } while ((s = va_arg(va, const char *)));
  va_end(va);
  exit(rc);
}

static wontreturn void UnsupportedSyntax(unsigned char c) {
  char cbuf[2];
  char ibuf[13];
  cbuf[0] = c;
  cbuf[1] = 0;
  FormatOctal32(ibuf, c, true);
  Wexit(4, prog, ": unsupported shell syntax '", cbuf, "' (", ibuf, "): ", cmd,
        "\n", 0);
}

static wontreturn void SysExit(int rc, const char *call, const char *thing) {
  int err;
  char ibuf[12];
  const char *estr;
  err = errno;
  FormatInt32(ibuf, err);
  estr = strerdoc(err);
  if (!estr) estr = "EUNKNOWN";
  Wexit(rc, thing, ": ", call, "() failed: ", estr, " (", ibuf, ")\n", 0);
}

static void Open(const char *path, int fd, int flags) {
  const char *err;
  close(fd);
  if (open(path, flags, 0644) == -1) {
    SysExit(7, "open", path);
  }
}

static wontreturn void Exec(void) {
  const char *s;
  if (!n) {
    Wexit(5, prog, ": error: too few args\n", 0);
  }
  execvp(args[0], args);
  SysExit(127, "execve", args[0]);
}

static void Pipe(void) {
  int pid, pfds[2];
  if (pipe2(pfds, O_CLOEXEC)) {
    SysExit(8, "pipe2", prog);
  }
  if ((pid = vfork()) == -1) {
    SysExit(9, "vfork", prog);
  }
  if (!pid) {
    dup2(pfds[1], 1);
    Exec();
  }
  dup2(pfds[0], 0);
  n = 0;
}

static char *Tokenize(void) {
  char *r;
  int c, t;
  while (*p == ' ' || *p == '\t' || *p == '\n' ||
         (p[0] == '\\' && p[1] == '\n')) {
    ++p;
  }
  if (!*p) return 0;
  t = STATE_SHELL;
  for (r = q;; ++p) {
    switch (t) {

      case STATE_SHELL:
        if (unsupported[*p & 255]) {
          UnsupportedSyntax(*p);
        }
        if (!*p || *p == ' ' || *p == '\t') {
          *q++ = 0;
          return r;
        } else if (*p == '"') {
          t = STATE_QUO;
        } else if (*p == '\'') {
          t = STATE_STR;
        } else if (*p == '\\') {
          if (!p[1]) UnsupportedSyntax(*p);
          *q++ = *++p;
        } else if (*p == '|') {
          if (q > r) {
            *q = 0;
            return r;
          } else {
            Pipe();
            ++p;
          }
        } else {
          *q++ = *p;
        }
        break;

      case STATE_STR:
        if (!*p) {
          Wexit(6, "cmd: error: unterminated single string\n", 0);
        }
        if (*p == '\'') {
          t = STATE_SHELL;
        } else {
          *q++ = *p;
        }
        break;

      case STATE_QUO:
        if (!*p) {
          Wexit(6, "cmd: error: unterminated quoted string\n", 0);
        }
        if (*p == '"') {
          t = STATE_SHELL;
        } else if (p[0] == '\\') {
          switch ((c = *++p)) {
            case 0:
              UnsupportedSyntax('\\');
            case '\n':
              break;
            case '$':
            case '`':
            case '"':
              *q++ = c;
              break;
            default:
              *q++ = '\\';
              *q++ = c;
              break;
          }
        } else {
          *q++ = *p;
        }
        break;

      default:
        unreachable;
    }
  }
}

static const char *GetRedirectArg(const char *prog, const char *arg, int n) {
  if (arg[n]) {
    return arg + n;
  } else if ((arg = Tokenize())) {
    return arg;
  } else {
    Wexit(14, prog, ": error: redirect missing path\n", 0);
  }
}

int cocmd(int argc, char *argv[]) {
  char *arg;
  size_t i, j;
  prog = argc > 0 ? argv[0] : "cocmd.com";

  for (i = 1; i < 32; ++i) {
    unsupported[i] = true;
  }
  unsupported['\t'] = false;
  unsupported[0177] = true;
  unsupported['~'] = true;
  unsupported['`'] = true;
  unsupported['#'] = true;
  unsupported['*'] = true;
  unsupported['('] = true;
  unsupported[')'] = true;
  unsupported['['] = true;
  unsupported[']'] = true;
  unsupported['{'] = true;
  unsupported['}'] = true;
  unsupported[';'] = true;
  unsupported['?'] = true;
  unsupported['!'] = true;

  if (argc != 3) {
    Wexit(10, prog, ": error: wrong number of args\n", 0);
  }

  if (strcmp(argv[1], "-c")) {
    Wexit(11, prog, ": error: argv[1] should -c\n", 0);
  }

  p = cmd = argv[2];
  if (strlen(cmd) >= ARG_MAX) {
    Wexit(12, prog, ": error: cmd too long: ", cmd, "\n", 0);
  }

  n = 0;
  q = argbuf;
  while ((arg = Tokenize())) {
    if (n + 1 < ARRAYLEN(args)) {
      if (isdigit(arg[0]) && arg[1] == '>' && arg[2] == '&' &&
          isdigit(arg[3])) {
        dup2(arg[3] - '0', arg[0] - '0');
      } else if (arg[0] == '>' && arg[1] == '&' && isdigit(arg[2])) {
        dup2(arg[2] - '0', 1);
      } else if (isdigit(arg[0]) && arg[1] == '>' && arg[2] == '>') {
        Open(GetRedirectArg(prog, arg, 3), arg[0] - '0',
             O_WRONLY | O_CREAT | O_APPEND);
      } else if (arg[0] == '>' && arg[1] == '>') {
        Open(GetRedirectArg(prog, arg, 2), 1, O_WRONLY | O_CREAT | O_APPEND);
      } else if (isdigit(arg[0]) && arg[1] == '>') {
        Open(GetRedirectArg(prog, arg, 2), arg[0] - '0',
             O_WRONLY | O_CREAT | O_TRUNC);
      } else if (arg[0] == '>') {
        Open(GetRedirectArg(prog, arg, 1), 1, O_WRONLY | O_CREAT | O_TRUNC);
      } else if (arg[0] == '<') {
        Open(GetRedirectArg(prog, arg, 1), 0, O_RDONLY);
      } else {
        args[n++] = arg;
        args[n] = 0;
      }
    } else {
      Wexit(13, prog, ": error: too many args\n", 0);
    }
  }

  Exec();
}
