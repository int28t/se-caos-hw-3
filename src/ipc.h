#ifndef IPC_H
#define IPC_H
#include "crossroad.h"
#include <semaphore.h>
#define SEM_COUNT (2 + 3 * MAX_ROLES)
#define PATH_SIZE 256
#define WIRE_MAGIC UINT64_C(0x435233)
enum { NAMED, UNNAMED, SHM, FIFO };
typedef struct Shared {
  sem_t sem[SEM_COUNT];
  Packet request[MAX_ROLES], reply[MAX_ROLES];
} Shared;
typedef struct Ipc {
  unsigned backend, roles, slot, cursor, initialized;
  uint64_t sid, hash;
  pid_t ownerPid;
  bool owner, directoryMade, memoryMade, semMade[SEM_COUNT];
  bool fifoMade[MAX_ROLES][2], pending[MAX_ROLES];
  int input[MAX_ROLES], output[MAX_ROLES];
  size_t used[MAX_ROLES];
  Packet frame[MAX_ROLES];
  Shared *shared;
  sem_t *sem[SEM_COUNT];
  char directory[PATH_SIZE];
} Ipc;
void IpcInit(Ipc *p, const char *directory, unsigned backend, unsigned roles, uint64_t hash);
void IpcName(const Ipc *p, unsigned object, char *name);
bool IpcCreate(Ipc *p);
bool IpcAttach(Ipc *p, unsigned slot, uint64_t deadline);
bool IpcClaim(Ipc *p, unsigned slot, uint64_t deadline);
bool IpcSeal(Ipc *p, uint64_t *attached, uint64_t deadline);
bool IpcWait(sem_t *sem, uint64_t deadline);
bool IpcCall(Ipc *p, Packet *q, Packet *r, uint64_t deadline);
int IpcReceive(Ipc *p, Packet *q, uint64_t deadline);
bool IpcReply(Ipc *p, const Packet *r, uint64_t deadline);
bool IpcClose(Ipc *p);
bool ShmOpen(Ipc *p, bool create);
bool FifoOpen(Ipc *p, bool create, uint64_t deadline);
bool FifoWrite(int fd, const Packet *p, uint64_t deadline);
int FifoRead(int fd, Packet *p, size_t *used, uint64_t deadline);
#endif
