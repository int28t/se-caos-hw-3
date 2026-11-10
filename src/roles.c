#include "crossroad.h"

// SplitMix64 адресуется по номеру такта: пропуски не меняют будущие выборки.
static unsigned Random(const Config *c, unsigned stream, uint64_t tick, unsigned draw) {
  uint64_t x = c->seed + UINT64_C(0x9e3779b97f4a7c15) *
      ((uint64_t)stream * UINT64_C(0x100000000) + 2 * tick + draw + 1);
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return (unsigned)((x ^ (x >> 31)) % 1000);
}
static unsigned Choose(const Config *c, const Packet *p) {
  unsigned best = (unsigned)p->w[NEXT], bestLength = 0;
  uint64_t oldest = UINT64_MAX, emergency = UINT64_MAX;
  for (unsigned n = 0; n < c->phaseCount; ++n) {
    unsigned phase = ((unsigned)p->w[NEXT] + n) % c->phaseCount, length = 0;
    uint64_t first = UINT64_MAX, urgent = UINT64_MAX;
    for (unsigned i = 0; i < c->streamCount; ++i) {
      const uint64_t *v = &p->w[FLOWS + VIEW_SIZE * i];
      if (!(c->phases[phase].mask & (1u << i)) || !v[QUEUED])
        continue;
      length += (unsigned)v[QUEUED];
      if (v[ARRIVED_AT] < first)
        first = v[ARRIVED_AT];
      if (c->priority && v[EMERGENCY_ID] && v[EMERGENCY_ID] < urgent)
        urgent = v[EMERGENCY_ID];
    }
    if (urgent < emergency || (urgent == emergency && c->strategy == ADAPTIVE &&
        (first < oldest || (first == oldest && length > bestLength)))) {
      best = phase;
      emergency = urgent;
      oldest = first;
      bestLength = length;
    }
  }
  return best;
}
void RoleDecide(const Config *c, const Packet *p, RoleState *r, uint64_t now, Packet *q) {
  q->w[OP] = SNAPSHOT;
  if (!r->index) {
    unsigned state = (unsigned)p->w[STATE], phase = (unsigned)p->w[PHASE];
    uint64_t elapsed = now - p->w[SINCE], tick = p->w[TICK];
    unsigned chosen = Choose(c, p), action = 0;
    bool urgent = false;
    for (unsigned i = 0; i < c->streamCount; ++i)
      urgent |= c->priority && (c->phases[chosen].mask & (1u << i)) &&
                p->w[FLOWS + VIEW_SIZE * i + EMERGENCY_ID] != 0;
    if (state == INITIAL || (state == CLEARANCE && elapsed >= (uint64_t)c->clearance * tick)) {
      action = BEGIN;
      phase = chosen;
    } else if (state == DRAIN && !p->w[ACTIVE])
      action = BEGIN_CLEARANCE;
    else if (state == GREEN && (elapsed >= (uint64_t)c->maxGreen * tick ||
        (elapsed >= (uint64_t)c->minGreen * tick && chosen != phase &&
         (c->strategy == ADAPTIVE || urgent))))
      action = CLOSE_ENTRY;
    if (action) {
      q->w[OP] = CONTROL;
      q->w[8] = action;
      q->w[9] = phase;
      q->w[10] = p->w[GENERATION];
    }
  } else if (r->index <= c->streamCount) {
    unsigned i = r->index - 1;
    uint64_t tick = now / p->w[TICK];
    if (tick < r->nextTick || (c->ticks && tick >= c->ticks))
      return;
    r->nextTick = tick + 1;
    if (Random(c, i, tick, 0) < c->streams[i].rate) {
      q->w[OP] = ARRIVE;
      q->w[8] = tick;
      q->w[9] = c->streams[i].kind == CAR && Random(c, i, tick, 1) < c->emergencyRate;
    }
  } else {
    unsigned i = r->index - c->streamCount - 1;
    const uint64_t *v = &p->w[FLOWS + VIEW_SIZE * i];
    if (v[ACTIVE_ID]) {
      if (now >= v[LEAVE_AT]) {
        q->w[OP] = LEAVE;
        q->w[8] = v[ACTIVE_ID];
      }
    } else if (v[HEAD_ID] && p->w[STATE] == GREEN && (p->w[SIGNALS] & (1u << i)) &&
               !(c->conflicts[i] & p->w[ACTIVE])) {
      q->w[OP] = ENTER;
      q->w[8] = v[HEAD_ID];
    }
  }
}
