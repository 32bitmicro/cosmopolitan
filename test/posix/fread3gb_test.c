#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SIZE ((size_t)2 * 1024 * 1024 * 1024 + 13)

char path[PATH_MAX];

void teardown(void) {
  unlink(path);
}

int setup(void) {
  int fd;
  struct stat st;
  const char *tmpdir;
  if (!stat("/dev/shm", &st))
    strlcpy(path, "/dev/shm", sizeof(path));
  else if ((tmpdir = getenv("TMPDIR")))
    strlcpy(path, tmpdir, sizeof(path));
  else
    strlcpy(path, "/tmp", sizeof(path));
  strlcat(path, "/fread3gb.XXXXXX", sizeof(path));
  if ((fd = mkstemp(path)) == -1)
    return 1;
  if (ftruncate(fd, SIZE))
    return 2;
  if (pwrite(fd, "a", 1, 0) != 1)
    return 3;
  if (pwrite(fd, "z", 1, SIZE - 1) != 1)
    return 4;
  if (close(fd))
    return 5;
  return 0;
}

int test(void) {
  FILE *f;
  char *buf;
  size_t rc;
  if (!(f = fopen(path, "r")))
    return 6;
  if (!(buf = malloc(SIZE)))
    return 7;
  if ((rc = fread(buf, SIZE, 1, f)) != 1) {
    fprintf(stderr, "tell = %zu\n", ftello(f));
    fprintf(stderr, "rc = %zu\n", rc);
    perror("fread");
    return 8;
  }
  if (buf[0] != 'a')
    return 9;
  if (buf[SIZE - 1] != 'z')
    return 10;
  if (fclose(f))
    return 11;
  return 0;
}

int main(int argc, char *argv[]) {
  int rc;
  if ((rc = setup())) {
    perror(path);
    teardown();
    return rc;
  }
  rc = test();
  teardown();
  return rc;
}
