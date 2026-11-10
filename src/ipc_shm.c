#include "ipc.h"
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

bool ShmOpen(Ipc *p, bool create) {
  char name[64];
  IpcName(p, 0, name);
  int fd = shm_open(name, O_RDWR | (create ? O_CREAT | O_EXCL : 0), 0600);
  if (fd < 0)
    return false;
  p->memoryMade = create;
  struct stat info;
  bool ok = create ? ftruncate(fd, (off_t)sizeof(Shared)) == 0 :
      fstat(fd, &info) == 0 && info.st_size == (off_t)sizeof(Shared);
  if (ok) {
    void *address = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ok = address != MAP_FAILED;
    if (ok) {
      p->shared = address;
      if (create)
        memset(p->shared, 0, sizeof(Shared));
    }
  }
  if (close(fd) < 0)
    ok = false;
  return ok;
}
