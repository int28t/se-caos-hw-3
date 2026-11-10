// Один цикл координатора и один цикл рабочих для всех вариантов IPC.
#include "ipc.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

volatile sig_atomic_t interrupted;
static void OnSignal(int number) { interrupted = number; }
static bool Signals(void) {
  struct sigaction action = {.sa_handler = OnSignal};
  if (sigemptyset(&action.sa_mask) < 0 || sigaction(SIGINT, &action, NULL) < 0 ||
      sigaction(SIGTERM, &action, NULL) < 0)
    return false;
  action.sa_handler = SIG_IGN;
  return sigaction(SIGPIPE, &action, NULL) == 0;
}
static bool SnapshotValid(const Config *c, const Packet *p) {
  if (p->w[COUNT] != c->streamCount || p->w[TICK] != (uint64_t)c->tickMs * NS ||
      p->w[PHASE] >= c->phaseCount || p->w[NEXT] >= c->phaseCount || p->w[STATE] > CLEARANCE ||
      p->w[STOP] > UNREACHABLE || p->w[STATUS] > 255 || p->w[SINCE] > p->w[NOW] ||
      (p->w[ACTIVE] | p->w[SIGNALS]) >= (UINT64_C(1) << c->streamCount))
    return false;
  for (unsigned i = 0; i < c->streamCount; ++i)
    if (p->w[FLOWS + VIEW_SIZE * i] > c->streams[i].capacity)
      return false;
  return true;
}
int Worker(const Config *c, uint64_t hash, const char *dir, unsigned backend, unsigned role, void *inherited) {
  if (!Signals())
    return 1;
  Ipc local, *ipc = inherited ? inherited : &local;
  uint64_t deadline = ClockNow() + HEALTH;
  if (!inherited)
    IpcInit(ipc, dir, backend, 2 * c->streamCount + 1, hash);
  else {
    ipc->owner = ipc->directoryMade = false;
  }
  bool attached = inherited ? IpcClaim(ipc, role, deadline) : IpcAttach(ipc, role, deadline);
  int status = 1;
  if (!attached)
    goto finish;
  dprintf(STDERR_FILENO, "Роль: index=%u pid=%ld ppid=%ld\n", role, (long)getpid(), (long)getppid());
  Packet q = {0}, r;
  q.w[OP] = HELLO;
  q.w[8] = hash;
  q.w[9] = (uint64_t)(unsigned)getpid();
  q.w[SEQUENCE] = 1;
  if (!IpcCall(ipc, &q, &r, deadline) || r.w[RESULT])
    goto finish;
  RoleState state = {.index = role};
  for (;;) {
    if (!SnapshotValid(c, &r))
      break;
    q = (Packet){.w = {[SEQUENCE] = q.w[SEQUENCE] + 1, [OP] = SNAPSHOT}};
    if (r.w[STOP]) {
      status = (int)r.w[STATUS];
      q.w[OP] = BYE;
      if (!IpcCall(ipc, &q, &r, ClockNow() + HEALTH))
        status = 1;
      break;
    }
    if (interrupted) {
      q.w[OP] = INTERRUPT;
      q.w[8] = (unsigned)interrupted;
    } else if (r.w[ORIGIN]) {
      uint64_t now = ClockNow();
      if (now < r.w[ORIGIN])
        break;
      RoleDecide(c, &r, &state, now - r.w[ORIGIN], &q);
    } else if (ClockNow() >= deadline)
      break;
    if (!IpcCall(ipc, &q, &r, r.w[ORIGIN] ? ClockNow() + HEALTH : deadline))
      break;
    Pause();
  }
finish:
  if (!IpcClose(ipc))
    status = 1;
  return interrupted && !attached ? 128 + interrupted : status;
}
int Run(const Config *c, uint64_t hash, Output *out, const char *dir, unsigned backend, bool independent) {
  Model model;
  ModelInit(&model, c, out);
  Ipc ipc;
  unsigned roles = 2 * c->streamCount + 1, children = 0, registered = 0;
  pid_t pids[MAX_ROLES] = {0};
  uint64_t sequence[MAX_ROLES] = {0}, seen[MAX_ROLES] = {0}, sampled[MAX_STREAMS] = {0};
  bool generated[MAX_STREAMS] = {0};
  uint64_t hello = 0, bye = 0, attached = 0, origin = 0, stopDeadline = 0;
  uint64_t startup = ClockNow() + HEALTH;
  bool ready = false, failed = false;
  IpcInit(&ipc, dir, backend, roles, hash);
  if (!Signals())
    return 1;
  if (interrupted) {
    ModelStop(&model, SIGNAL, (unsigned)interrupted);
    goto finish;
  }
  if (!IpcCreate(&ipc)) {
    perror("Создание IPC");
    ModelStop(&model, ERROR, 0);
    goto finish;
  }
  ready = true;
  dprintf(STDERR_FILENO, "Сессия: sid=%" PRIu64 " pid=%ld backend=%u ролей=%u\n",
          ipc.sid, (long)getpid(), backend, roles);
  if (!independent) {
    for (unsigned i = 0; i < roles; ++i) {
      pid_t pid = fork();
      if (!pid) {
        (void)close(out->fd);
        _exit(Worker(c, hash, dir, backend, i, &ipc));
      }
      if (pid < 0) {
        ModelStop(&model, ERROR, 0);
        break;
      }
      pids[children++] = pid;
    }
  }
  while (true) {
    uint64_t absolute = ClockNow();
    if (origin && !model.stop) {
      Packet observe = {.w = {[OP] = SNAPSHOT}};
      (void)ModelApply(&model, &observe, absolute - origin);
    }
    if (interrupted)
      ModelStop(&model, SIGNAL, (unsigned)interrupted);
    if (out->failed)
      ModelStop(&model, ERROR, 0);
    if (!model.stop && ((!origin && absolute >= startup)))
      ModelStop(&model, ERROR, 0);
    for (unsigned i = 0; i < roles; ++i)
      if (origin && !(bye & (UINT64_C(1) << i)) && absolute - seen[i] >= HEALTH)
        ModelStop(&model, ERROR, 0);
    for (unsigned i = 0; i < children; ++i) {
      int state;
      pid_t pid = pids[i] ? waitpid(pids[i], &state, WNOHANG) : 0;
      if (pid > 0) {
        pids[i] = 0;
        if (!(bye & (UINT64_C(1) << i)) || !WIFEXITED(state) || WEXITSTATUS(state) != (int)model.status) {
          failed = true;
          ModelStop(&model, ERROR, 0);
        }
      } else if (pid < 0) {
        failed = true;
        ModelStop(&model, ERROR, 0);
      }
    }
    if (model.stop && !stopDeadline) {
      stopDeadline = absolute + HEALTH;
      if (!IpcSeal(&ipc, &attached, stopDeadline)) {
        failed = true;
        break;
      }
    }
    if (model.stop && (((bye & attached) == attached) || absolute >= stopDeadline))
      break;
    Packet q;
    int result = IpcReceive(&ipc, &q, absolute + NS);
    if (result < 0) {
      ModelStop(&model, ERROR, 0);
      failed = true;
      break;
    }
    if (!result)
      continue;
    unsigned i = ipc.slot;
    uint64_t bit = UINT64_C(1) << i, op = q.w[OP];
    bool valid = q.w[MAGIC] == WIRE_MAGIC && q.w[VERSION] == 1 && q.w[SID] == ipc.sid &&
        q.w[SEQUENCE] == sequence[i] + 1 && op >= HELLO && op <= BYE &&
        (!(bye & bit)) && (op == HELLO ? !(hello & bit) && q.w[8] == hash : (hello & bit) != 0) &&
        (op != CONTROL || i == 0) && (op != ARRIVE || (i >= 1 && i <= c->streamCount)) &&
        ((op != ENTER && op != LEAVE) || i > c->streamCount) &&
        (op != INTERRUPT || (q.w[8] > 0 && q.w[8] < 128)) &&
        (op != CONTROL || (q.w[8] >= BEGIN && q.w[8] <= BEGIN_CLEARANCE && q.w[9] < c->phaseCount));
    if (!valid) {
      ModelStop(&model, ERROR, 0);
      failed = true;
      break;
    }
    sequence[i] = q.w[SEQUENCE];
    seen[i] = ClockNow();
    bool accepted = true;
    if (op == HELLO) {
      hello |= bit;
      ++registered;
      if (registered == roles && !model.stop) {
        origin = ClockNow();
        for (unsigned j = 0; j < roles; ++j)
          seen[j] = origin;
        bool possible = false;
        for (unsigned j = 0; j < c->streamCount; ++j)
          possible |= c->streams[j].rate != 0;
        if (c->target && !possible)
          ModelStop(&model, UNREACHABLE, 0);
      }
    } else if (op == BYE)
      accepted = model.stop != 0;
    else if (origin && !model.stop) {
      if (op == ARRIVE) {
        unsigned stream = i - 1;
        accepted = q.w[9] <= 1 && q.w[8] <= (ClockNow() - origin) / model.tick &&
            (!generated[stream] || q.w[8] > sampled[stream]);
        if (accepted) {
          sampled[stream] = q.w[8];
          generated[stream] = true;
        }
      }
      if (accepted)
        accepted = ModelApply(&model, &q, ClockNow() - origin);
    } else if (op == INTERRUPT)
      ModelStop(&model, SIGNAL, (unsigned)q.w[8]);
    Packet reply = {0};
    memcpy(reply.w, q.w, 8 * sizeof(uint64_t));
    reply.w[RESULT] = !accepted;
    reply.w[ORIGIN] = origin;
    ModelSnapshot(&model, &reply);
    uint64_t deadline = ClockNow() + HEALTH;
    if (stopDeadline && deadline > stopDeadline)
      deadline = stopDeadline;
    if (!IpcReply(&ipc, &reply, deadline)) {
      ModelStop(&model, ERROR, 0);
      failed = true;
      break;
    }
    if (op == BYE && accepted)
      bye |= bit;
  }
finish:
  for (unsigned i = 0; i < children; ++i) {
    if (!pids[i])
      continue;
    int state;
    uint64_t deadline = ClockNow() + NS * 100;
    pid_t done;
    do {
      done = waitpid(pids[i], &state, WNOHANG);
      if (!done)
        Pause();
    } while (!done && ClockNow() < deadline);
    if (!done) {
      (void)kill(pids[i], SIGKILL);
      do { done = waitpid(pids[i], &state, 0); } while (done < 0 && errno == EINTR);
    }
    if (done < 0 || !WIFEXITED(state) || WEXITSTATUS(state) != (int)model.status)
      failed = true;
  }
  if (!IpcClose(&ipc))
    failed = true;
  if (!model.stop)
    ModelStop(&model, ERROR, 0);
  ModelReport(&model, 1 + (independent ? registered : children));
  return failed || out->failed || !ready ? (model.stop == SIGNAL && !failed ? (int)model.status : 1) : (int)model.status;
}
