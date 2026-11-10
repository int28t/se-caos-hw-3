#include "crossroad.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

uint64_t ClockNow(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) < 0)
    _exit(1);
  return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
void Pause(void) {
  struct timespec t = {.tv_nsec = 1000000};
  (void)nanosleep(&t, NULL);
}
static bool Write(int fd, const char *s, size_t n) {
  while (n) {
    ssize_t done = write(fd, s, n);
    if (done < 0 && errno == EINTR)
      continue;
    if (done <= 0)
      return false;
    s += (size_t)done;
    n -= (size_t)done;
  }
  return true;
}
bool OutputOpen(Output *out, const char *path, int input) {
  *out = (Output){.fd = open(path, O_WRONLY | O_CREAT | O_NONBLOCK | O_CLOEXEC, 0600)};
  if (out->fd < 0)
    return false;
  struct stat a, b;
  if (fstat(input, &a) < 0 || fstat(out->fd, &b) < 0 ||
      (a.st_dev == b.st_dev && a.st_ino == b.st_ino) ||
      (S_ISREG(b.st_mode) && ftruncate(out->fd, 0) < 0)) {
    (void)close(out->fd);
    out->fd = -1;
    return false;
  }
  out->consoleFlags = fcntl(STDOUT_FILENO, F_GETFL);
  if (out->consoleFlags < 0 || fcntl(STDOUT_FILENO, F_SETFL, out->consoleFlags | O_NONBLOCK) < 0)
    out->failed = out->consoleBad = true;
  return true;
}
void Log(Model *m, const char *format, ...) {
  char line[1024];
  int prefix = snprintf(line, sizeof(line), "[t=%" PRIu64 "] ", m->now / m->tick);
  va_list args;
  va_start(args, format);
  int n = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, args);
  va_end(args);
  if (n < 0 || (size_t)n >= sizeof(line) - (size_t)prefix) {
    m->out->failed = true;
    return;
  }
  size_t used = (size_t)(prefix + n);
  n = snprintf(line + used, sizeof(line) - used, " событие=%" PRIu64 " время_нс=%" PRIu64 "\n", ++m->event, m->now);
  if (n < 0 || (size_t)n >= sizeof(line) - used) {
    m->out->failed = true;
    return;
  }
  used += (size_t)n;
  Output *o = m->out;
  if (!o->consoleBad)
    o->consoleBad = !Write(STDOUT_FILENO, line, used);
  if (!o->logBad)
    o->logBad = !Write(o->fd, line, used);
  o->failed |= o->consoleBad || o->logBad;
}
void OutputClose(Output *out) {
  if (out->consoleFlags >= 0 && fcntl(STDOUT_FILENO, F_SETFL, out->consoleFlags) < 0)
    out->failed = true;
  if (out->fd >= 0 && close(out->fd) < 0)
    out->failed = true;
}
