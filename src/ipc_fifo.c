#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

bool FifoOpen(Ipc *p, bool create, uint64_t deadline) {
  char path[PATH_SIZE + 16];
  for (unsigned i = create ? 0 : p->slot; i < (create ? p->roles : p->slot + 1); ++i) {
    for (unsigned j = 0; j < 2; ++j) {
      (void)snprintf(path, sizeof(path), "%s/%c%u", p->directory, j ? 'r' : 'q', i);
      if (create) {
        if (mkfifo(path, 0600) < 0)
          return false;
        p->fifoMade[i][j] = true;
      }
      if ((create && !j) || (!create && j)) {
        p->input[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (p->input[i] < 0)
          return false;
      }
    }
    if (!create) {
      (void)snprintf(path, sizeof(path), "%s/q%u", p->directory, i);
      do {
        p->output[i] = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (p->output[i] >= 0)
          break;
        if (errno != ENXIO || ClockNow() >= deadline)
          return false;
        Pause();
      } while (true);
    }
  }
  return true;
}
bool FifoWrite(int fd, const Packet *p, uint64_t deadline) {
  long limit = fpathconf(fd, _PC_PIPE_BUF);
  if (limit < _POSIX_PIPE_BUF)
    return false;
  size_t used = 0;
  while (used < sizeof(*p) && ClockNow() < deadline) {
    size_t length = sizeof(*p) - used;
    if (length > (size_t)limit)
      length = (size_t)limit;
    ssize_t n = write(fd, (const char *)p + used, length);
    if (n > 0)
      used += (size_t)n;
    else if (n < 0 && (errno == EAGAIN || errno == EINTR))
      Pause();
    else
      return false;
  }
  return used == sizeof(*p);
}
int FifoRead(int fd, Packet *p, size_t *used, uint64_t deadline) {
  do {
    ssize_t n = read(fd, (char *)p + *used, sizeof(*p) - *used);
    if (n > 0)
      *used += (size_t)n;
    else if (n < 0 && errno == EINTR)
      continue;
    else
      return n < 0 && errno == EAGAIN ? 0 : -1;
    if (*used == sizeof(*p))
      return 1;
  } while (ClockNow() < deadline);
  return 0;
}
