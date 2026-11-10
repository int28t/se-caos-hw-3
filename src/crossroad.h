#ifndef CROSSROAD_H
#define CROSSROAD_H
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>

#define MAX_STREAMS 16
#define MAX_PHASES 16
#define MAX_QUEUE 64
#define NAME_SIZE 32
#define TEXT_SIZE 16384
#define NS UINT64_C(1000000)
#define HEALTH (UINT64_C(5000) * NS)
#define MAX_ROLES (2 * MAX_STREAMS + 1)

typedef enum Kind { CAR, PEDESTRIAN } Kind;
typedef enum Direction { NORTH, EAST, SOUTH, WEST } Direction;
typedef enum Maneuver { STRAIGHT, RIGHT, LEFT, WALK } Maneuver;
typedef enum Strategy { FIXED, ADAPTIVE } Strategy;
typedef struct Stream {
  char name[NAME_SIZE];
  Kind kind;
  Direction direction;
  Maneuver maneuver;
  unsigned capacity, crossingTicks, rate;
} Stream;
typedef struct Phase { char name[NAME_SIZE]; uint32_t mask; } Phase;
typedef struct Config {
  Stream streams[MAX_STREAMS];
  Phase phases[MAX_PHASES];
  uint32_t conflicts[MAX_STREAMS];
  unsigned streamCount, phaseCount, minGreen, maxGreen, clearance;
  unsigned emergencyRate, tickMs, delayMs, seed;
  uint64_t ticks, target;
  Strategy strategy;
  bool priority;
} Config;
// Локальный протокол одной сборки Linux: передаются слова, а не padding структур.
typedef struct Packet { uint64_t w[128]; } Packet;
enum { MAGIC, VERSION, SID, ROLE, SEQUENCE, OP, RESULT, ORIGIN,
       NOW, TICK, PHASE, GENERATION, STATE, SIGNALS, ACTIVE, PASSED,
       STOP, STATUS, SIGNO, SINCE, NEXT, COUNT, FLOWS = 24 };
enum { QUEUED, HEAD_ID, ARRIVED_AT, EMERGENCY_ID, ACTIVE_ID, LEAVE_AT, VIEW_SIZE };
enum { HELLO = 1, SNAPSHOT, ARRIVE, CONTROL, ENTER, LEAVE, INTERRUPT, BYE };
enum { INITIAL, GREEN, DRAIN, CLEARANCE };
enum { BEGIN = 1, CLOSE_ENTRY, BEGIN_CLEARANCE };
enum { RUNNING, PERIOD, TARGET, SIGNAL, ERROR, UNREACHABLE };
_Static_assert(FLOWS + VIEW_SIZE * MAX_STREAMS <= 128, "snapshot capacity");
typedef struct Output { int fd, consoleFlags; bool failed, consoleBad, logBad; } Output;
typedef struct Person { uint64_t id, arrived; bool emergency; } Person;
typedef struct Flow {
  Person queue[MAX_QUEUE], active;
  unsigned head, count, peak;
  uint64_t leaveAt, arrived, passed, refused, waitSum, waitMax;
} Flow;
typedef struct Model {
  Config cfg;
  Flow flows[MAX_STREAMS];
  Output *out;
  uint64_t now, tick, nextId, event, passed, since, generation, display;
  unsigned state, phase, next, signals, active, stop, status, signo;
} Model;
typedef struct RoleState { unsigned index; uint64_t nextTick; } RoleState;
extern volatile sig_atomic_t interrupted;
uint64_t ClockNow(void);
void Pause(void);
int ConfigOpen(Config *cfg, const char *path, uint64_t *hash);
bool OutputOpen(Output *out, const char *path, int input);
void Log(Model *m, const char *format, ...);
void OutputClose(Output *out);
void ModelInit(Model *m, const Config *cfg, Output *out);
void ModelStop(Model *m, unsigned reason, unsigned number);
bool ModelApply(Model *m, const Packet *q, uint64_t now);
void ModelSnapshot(const Model *m, Packet *p);
void ModelReport(Model *m, unsigned processes);
void RoleDecide(const Config *c, const Packet *p, RoleState *role, uint64_t now, Packet *q);
int Run(const Config *c, uint64_t hash, Output *out, const char *dir, unsigned backend, bool independent);
int Worker(const Config *c, uint64_t hash, const char *dir, unsigned backend, unsigned role, void *inherited);
#endif
