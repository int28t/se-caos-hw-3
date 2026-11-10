#include "crossroad.h"
#include <assert.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
int main(int argc, char **argv) {
  assert(argc == 2);
  Config c;
  uint64_t hash;
  int fd = ConfigOpen(&c, argv[1], &hash);
  assert(fd >= 0 && c.streamCount == 2 && hash);
  close(fd);
  c.priority = true;
  Packet snapshot = {.w = {[TICK] = 100, [STATE] = GREEN, [PHASE] = 0,
      [NEXT] = 1, [COUNT] = 2}}, command = {0};
  RoleState controller = {0};
  RoleDecide(&c, &snapshot, &controller, c.minGreen * 100, &command);
  assert(command.w[OP] == SNAPSHOT);
  Output out = {.fd = open("/dev/null", O_WRONLY)};
  assert(out.fd >= 0);
  c.target = 1;
  Model model;
  ModelInit(&model, &c, &out);
  Packet q = {.w = {[OP] = ARRIVE, [ROLE] = 1}};
  assert(ModelApply(&model, &q, 0));
  q = (Packet){.w = {[OP] = CONTROL, [8] = BEGIN}};
  assert(ModelApply(&model, &q, 0));
  q = (Packet){.w = {[OP] = ENTER, [ROLE] = 3, [8] = 1}};
  assert(ModelApply(&model, &q, 0));
  q.w[OP] = LEAVE;
  assert(!ModelApply(&model, &q, model.tick - 1));
  assert(ModelApply(&model, &q, model.tick));
  assert(model.stop == TARGET && model.passed == 1);
  uint64_t count = model.flows[0].arrived;
  q = (Packet){.w = {[OP] = ARRIVE, [ROLE] = 1}};
  assert(!ModelApply(&model, &q, 2 * model.tick));
  assert(model.flows[0].arrived == count);
  c.target = 0;
  ModelInit(&model, &c, &out);
  q = (Packet){.w = {[OP] = ARRIVE, [ROLE] = 1}};
  assert(ModelApply(&model, &q, 0));
  q = (Packet){.w = {[OP] = CONTROL, [8] = BEGIN}};
  assert(ModelApply(&model, &q, 0));
  q = (Packet){.w = {[OP] = ENTER, [ROLE] = 3, [8] = 1}};
  assert(ModelApply(&model, &q, 0));
  q = (Packet){.w = {[OP] = CONTROL, [8] = CLOSE_ENTRY, [10] = 1}};
  uint64_t end = c.minGreen * model.tick;
  assert(ModelApply(&model, &q, end) && model.state == DRAIN);
  q.w[8] = BEGIN_CLEARANCE;
  assert(!ModelApply(&model, &q, end));
  q = (Packet){.w = {[OP] = LEAVE, [ROLE] = 3, [8] = 99}};
  assert(!ModelApply(&model, &q, end));
  q.w[8] = 1;
  assert(ModelApply(&model, &q, end));
  q = (Packet){.w = {[OP] = CONTROL, [8] = BEGIN_CLEARANCE, [10] = 1}};
  assert(ModelApply(&model, &q, end));
  q.w[8] = BEGIN;
  q.w[9] = 1;
  assert(!ModelApply(&model, &q, end + c.clearance * model.tick - 1));
  assert(ModelApply(&model, &q, end + c.clearance * model.tick));
  int bad = open("overflow.conf", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  assert(bad >= 0);
  assert(dprintf(bad, "ticks 307445735\ntick_ms 60000\nstream A car north straight 1 1 0\nphase a A\n") > 0);
  close(bad);
  Config invalid;
  assert(ConfigOpen(&invalid, "overflow.conf", &hash) == -1);
  unlink("overflow.conf");
  close(out.fd);
  return 0;
}
