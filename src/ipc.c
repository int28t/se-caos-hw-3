// Владение именами остаётся у создателя; рабочие закрывают только handles.
#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

void IpcInit(Ipc *p, const char *directory, unsigned backend, unsigned roles, uint64_t hash) {
  *p = (Ipc){.backend = backend, .roles = roles, .hash = hash};
  (void)snprintf(p->directory, sizeof(p->directory), "%s", directory);
  for (unsigned i = 0; i < MAX_ROLES; ++i)
    p->input[i] = p->output[i] = -1;
}
void IpcName(const Ipc *p, unsigned object, char *name) {
  (void)snprintf(name, 64, "/cr3-%016" PRIx64 "-%u", p->sid, object);
}
bool IpcWait(sem_t *sem, uint64_t deadline) {
  for (;;) {
    if (sem_trywait(sem) == 0)
      return true;
    if ((errno != EAGAIN && errno != EINTR) || ClockNow() >= deadline)
      return false;
    Pause();
  }
}
static bool Semaphores(Ipc *p, bool create) {
  for (unsigned i = 0; i < 2 + 3 * p->roles; ++i) {
    unsigned value = i == 0 || (i >= 2 && (i - 2) % 3 == 0);
    if (p->backend == UNNAMED) {
      p->sem[i] = &p->shared->sem[i];
      if (create) {
        #ifdef __APPLE__
        errno = ENOTSUP;
        return false;
#else
        if (sem_init(p->sem[i], 1, value) < 0)
          return false;
        ++p->initialized;
#endif
      }
    } else {
      char name[64];
      IpcName(p, i + 1, name);
      sem_t *s = create ? sem_open(name, O_CREAT | O_EXCL, 0600, value) : sem_open(name, 0);
      if (s == SEM_FAILED)
        return false;
      p->sem[i] = s;
      p->semMade[i] = create;
    }
  }
  return true;
}
bool IpcCreate(Ipc *p) {
  if (mkdir(p->directory, 0700) < 0)
    return false;
  p->owner = p->directoryMade = true;
  p->ownerPid = getpid();
  p->sid = ClockNow() ^ ((uint64_t)(unsigned)getpid() << 32);
  if (!(p->backend == FIFO ? FifoOpen(p, true, 0) : ShmOpen(p, true)) || !Semaphores(p, true))
    return false;
  char path[PATH_SIZE + 16], ready[PATH_SIZE + 16];
  (void)snprintf(path, sizeof(path), "%s/pending", p->directory);
  (void)snprintf(ready, sizeof(ready), "%s/ready", p->directory);
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    return false;
  bool ok = dprintf(fd, "1 %u %u %" PRIu64 " %ld %" PRIu64 "\n", p->backend,
                    p->roles, p->sid, (long)p->ownerPid, p->hash) > 0;
  if (close(fd) < 0)
    ok = false;
  return ok && rename(path, ready) == 0;
}
bool IpcClaim(Ipc *p, unsigned slot, uint64_t deadline) {
  p->slot = slot;
  if (slot >= p->roles || !IpcWait(p->sem[0], deadline))
    return false;
  bool closed = sem_trywait(p->sem[1]) == 0;
  bool ok = !closed && errno == EAGAIN && sem_trywait(p->sem[2 + 3 * slot]) == 0;
  if (closed && sem_post(p->sem[1]) < 0)
    ok = false;
  if (sem_post(p->sem[0]) < 0)
    ok = false;
  return ok;
}
bool IpcAttach(Ipc *p, unsigned slot, uint64_t deadline) {
  char path[PATH_SIZE + 16];
  (void)snprintf(path, sizeof(path), "%s/ready", p->directory);
  int fd;
  while ((fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)) < 0) {
    if (errno != ENOENT || interrupted || ClockNow() >= deadline)
      return false;
    Pause();
  }
  char text[160] = {0};
  ssize_t length = read(fd, text, sizeof(text) - 1);
  bool closed = close(fd) == 0;
  unsigned version, backend, roles;
  uint64_t sid, hash;
  long owner;
  if (length <= 0 || !closed || sscanf(text, "%u %u %u %" SCNu64 " %ld %" SCNu64,
      &version, &backend, &roles, &sid, &owner, &hash) != 6 || version != 1 ||
      backend != p->backend || roles != p->roles || hash != p->hash || !sid || owner <= 0)
    return false;
  p->sid = sid;
  p->ownerPid = (pid_t)owner;
  p->slot = slot;
  return (p->backend == FIFO ? FifoOpen(p, false, deadline) : ShmOpen(p, false)) &&
         Semaphores(p, false) && IpcClaim(p, slot, deadline);
}
bool IpcSeal(Ipc *p, uint64_t *attached, uint64_t deadline) {
  if (!IpcWait(p->sem[0], deadline))
    return false;
  bool ok = sem_post(p->sem[1]) == 0;
  *attached = 0;
  for (unsigned i = 0; i < p->roles; ++i) {
    if (sem_trywait(p->sem[2 + 3 * i]) == 0) {
      if (sem_post(p->sem[2 + 3 * i]) < 0)
        ok = false;
    } else if (errno == EAGAIN)
      *attached |= UINT64_C(1) << i;
    else
      ok = false;
  }
  return sem_post(p->sem[0]) == 0 && ok;
}
bool IpcCall(Ipc *p, Packet *q, Packet *r, uint64_t deadline) {
  unsigned i = p->slot;
  q->w[MAGIC] = WIRE_MAGIC;
  q->w[VERSION] = 1;
  q->w[SID] = p->sid;
  q->w[ROLE] = i;
  if (p->backend == FIFO) {
    if (!FifoWrite(p->output[i], q, deadline))
      return false;
  } else
    p->shared->request[i] = *q;
  if (sem_post(p->sem[3 + 3 * i]) < 0 || !IpcWait(p->sem[4 + 3 * i], deadline))
    return false;
  if (p->backend == FIFO) {
    size_t used = 0;
    while (FifoRead(p->input[i], r, &used, deadline) == 0) {
      if (ClockNow() >= deadline)
        return false;
      Pause();
    }
    if (used != sizeof(*r))
      return false;
  } else
    *r = p->shared->reply[i];
  return r->w[MAGIC] == WIRE_MAGIC && r->w[VERSION] == 1 && r->w[SID] == p->sid &&
         r->w[ROLE] == i && r->w[SEQUENCE] == q->w[SEQUENCE] && r->w[OP] == q->w[OP];
}
int IpcReceive(Ipc *p, Packet *q, uint64_t deadline) {
  do {
    for (unsigned n = 0; n < p->roles; ++n) {
      unsigned i = p->cursor++ % p->roles;
      if (!p->pending[i]) {
        if (sem_trywait(p->sem[3 + 3 * i]) < 0) {
          if (errno != EAGAIN && errno != EINTR)
            return -1;
          continue;
        }
        p->pending[i] = true;
      }
      if (p->backend == FIFO) {
        int result = FifoRead(p->input[i], &p->frame[i], &p->used[i], deadline);
        if (result < 0)
          return -1;
        if (!result)
          continue;
        *q = p->frame[i];
        p->used[i] = 0;
      } else
        *q = p->shared->request[i];
      p->pending[i] = false;
      p->slot = i;
      return q->w[ROLE] == i ? 1 : -1;
    }
    if (ClockNow() < deadline)
      Pause();
  } while (ClockNow() < deadline);
  return 0;
}
bool IpcReply(Ipc *p, const Packet *r, uint64_t deadline) {
  unsigned i = p->slot;
  if (p->backend == FIFO) {
    if (p->output[i] < 0) {
      char path[PATH_SIZE + 16];
      (void)snprintf(path, sizeof(path), "%s/r%u", p->directory, i);
      p->output[i] = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (p->output[i] < 0 || !FifoWrite(p->output[i], r, deadline))
      return false;
  } else
    p->shared->reply[i] = *r;
  return sem_post(p->sem[4 + 3 * i]) == 0;
}
bool IpcClose(Ipc *p) {
  bool ok = true;
  char name[64], path[PATH_SIZE + 16];
  for (unsigned i = 0; i < 2 + 3 * p->roles; ++i) {
    if (p->backend == UNNAMED) {
      #ifndef __APPLE__
      if (p->owner && i < p->initialized && sem_destroy(p->sem[i]) < 0)
        ok = false;
#endif
    } else {
      if (p->sem[i] && sem_close(p->sem[i]) < 0)
        ok = false;
      IpcName(p, i + 1, name);
      if (p->owner && p->semMade[i] && sem_unlink(name) < 0)
        ok = false;
    }
  }
  if (p->shared && munmap(p->shared, sizeof(Shared)) < 0)
    ok = false;
  IpcName(p, 0, name);
  if (p->owner && p->memoryMade && shm_unlink(name) < 0)
    ok = false;
  for (unsigned i = 0; i < p->roles; ++i) {
    if (p->input[i] >= 0 && close(p->input[i]) < 0)
      ok = false;
    if (p->output[i] >= 0 && close(p->output[i]) < 0)
      ok = false;
    for (unsigned j = 0; j < 2; ++j) {
      (void)snprintf(path, sizeof(path), "%s/%c%u", p->directory, j ? 'r' : 'q', i);
      if (p->owner && p->fifoMade[i][j] && unlink(path) < 0)
        ok = false;
    }
  }
  if (p->directoryMade) {
    const char *leaves[] = {"ready", "pending"};
    for (unsigned i = 0; i < 2; ++i) {
      (void)snprintf(path, sizeof(path), "%s/%s", p->directory, leaves[i]);
      if (unlink(path) < 0 && errno != ENOENT)
        ok = false;
    }
    if (rmdir(p->directory) < 0)
      ok = false;
  }
  return ok;
}
