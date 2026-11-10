// Координатор изменяет модель атомарно относительно каждого запроса.
#include "crossroad.h"
#include <inttypes.h>
#include <string.h>

void ModelInit(Model *m, const Config *cfg, Output *out) {
  *m = (Model){.cfg = *cfg, .out = out, .tick = (uint64_t)cfg->tickMs * NS};
}
void ModelStop(Model *m, unsigned reason, unsigned number) {
  if (m->stop)
    return;
  m->stop = reason;
  m->signo = number;
  m->status = reason == SIGNAL ? 128 + number : reason == ERROR ? 1 : reason == UNREACHABLE ? 3 : 0;
  Log(m, "Завершение: причина=%u сигнал=%u код=%u", reason, number, m->status);
}
static unsigned Population(unsigned mask) {
  unsigned n = 0;
  for (; mask; mask &= mask - 1)
    ++n;
  return n;
}
static void Signals(Model *m) {
  Log(m, "Сигналы: маска=%u", m->signals);
}
bool ModelApply(Model *m, const Packet *q, uint64_t now) {
  if (m->stop)
    return false;
  if (now < m->now) {
    ModelStop(m, ERROR, 0);
    return false;
  }
  uint64_t boundary = m->cfg.ticks * m->tick;
  m->now = boundary && now > boundary ? boundary : now;
  if (boundary && now > boundary) {
    ModelStop(m, PERIOD, 0);
    return false;
  }
  unsigned op = (unsigned)q->w[OP], role = (unsigned)q->w[ROLE];
  if (op == INTERRUPT) {
    ModelStop(m, SIGNAL, (unsigned)q->w[8]);
    return true;
  }
  if (op == CONTROL) {
    unsigned phase = (unsigned)q->w[9];
    if (phase >= m->cfg.phaseCount || q->w[10] != m->generation)
      return false;
    uint64_t elapsed = m->now - m->since;
    if (q->w[8] == BEGIN && (m->state == INITIAL ||
        (m->state == CLEARANCE && elapsed >= (uint64_t)m->cfg.clearance * m->tick))) {
      m->phase = phase;
      m->next = (phase + 1) % m->cfg.phaseCount;
      m->state = GREEN;
      m->since = m->now;
      m->signals = m->cfg.phases[phase].mask;
      ++m->generation;
      Log(m, "Начало фазы: имя=%s поколение=%" PRIu64, m->cfg.phases[phase].name, m->generation);
      Signals(m);
      return true;
    }
    if (phase != m->phase)
      return false;
    if (q->w[8] == CLOSE_ENTRY && m->state == GREEN &&
        elapsed >= (uint64_t)m->cfg.minGreen * m->tick) {
      m->state = DRAIN;
      Log(m, "Въезд закрыт: фаза=%s", m->cfg.phases[phase].name);
      return true;
    }
    if (q->w[8] == BEGIN_CLEARANCE && m->state == DRAIN && !m->active) {
      m->state = CLEARANCE;
      m->signals = 0;
      m->since = m->now;
      Log(m, "Интервал: длительность=%u", m->cfg.clearance);
      Signals(m);
      return true;
    }
    return false;
  }
  if (op == ARRIVE || op == ENTER || op == LEAVE) {
    unsigned stream = op == ARRIVE ? role - 1 : role - 1 - m->cfg.streamCount;
    if (stream >= m->cfg.streamCount)
      return false;
    Flow *f = &m->flows[stream];
    const Stream *s = &m->cfg.streams[stream];
    unsigned bit = 1u << stream;
    if (op == ARRIVE) {
      if ((boundary && now >= boundary) || m->nextId == UINT64_MAX)
        return false;
      Person person = {.id = ++m->nextId, .arrived = now, .emergency = q->w[9] != 0};
      ++f->arrived;
      Log(m, "Прибытие: поток=%s id=%" PRIu64 " экстренный=%u такт_генерации=%" PRIu64,
          s->name, person.id, (unsigned)person.emergency, q->w[8]);
      if (f->count == s->capacity) {
        ++f->refused;
        Log(m, "Отказ: поток=%s id=%" PRIu64, s->name, person.id);
      } else {
        f->queue[(f->head + f->count) % MAX_QUEUE] = person;
        ++f->count;
        if (f->count > f->peak)
          f->peak = f->count;
        Log(m, "Очередь: поток=%s id=%" PRIu64 " размер=%u", s->name, person.id, f->count);
        if (s->kind == PEDESTRIAN)
          Log(m, "Запрос пешехода: поток=%s id=%" PRIu64, s->name, person.id);
      }
      return true;
    }
    if (op == ENTER) {
      if (!f->count || f->active.id || q->w[8] != f->queue[f->head].id ||
          m->state != GREEN || !(m->signals & bit) || (m->cfg.conflicts[stream] & m->active) ||
          now - m->since >= (uint64_t)m->cfg.maxGreen * m->tick ||
          (boundary && now >= boundary) ||
          (m->cfg.target && m->passed + Population(m->active) >= m->cfg.target))
        return false;
      f->active = f->queue[f->head];
      f->head = (f->head + 1) % MAX_QUEUE;
      --f->count;
      f->leaveAt = now + (uint64_t)s->crossingTicks * m->tick;
      m->active |= bit;
      Log(m, "Разрешение: поток=%s id=%" PRIu64 " очередь=%u срок=%" PRIu64,
          s->name, f->active.id, f->count, f->leaveAt);
      return true;
    }
    if (!f->active.id || q->w[8] != f->active.id || now < f->leaveAt)
      return false;
    uint64_t wait = (f->leaveAt - (uint64_t)s->crossingTicks * m->tick - f->active.arrived) / m->tick;
    ++f->passed;
    ++m->passed;
    f->waitSum += wait;
    if (wait > f->waitMax)
      f->waitMax = wait;
    Log(m, "Выход: поток=%s id=%" PRIu64 " ожидание=%" PRIu64, s->name, f->active.id, wait);
    f->active.id = 0;
    m->active &= ~bit;
    if (m->cfg.target && m->passed == m->cfg.target)
      ModelStop(m, TARGET, 0);
    return true;
  }
  if (now >= m->display) {
    Log(m, "Состояние: фаза=%u состояние=%u активные=%u прошло=%" PRIu64,
        m->phase, m->state, m->active, m->passed);
    m->display = now + (m->cfg.delayMs ? (uint64_t)m->cfg.delayMs * NS : m->tick);
  }
  return true;
}
void ModelSnapshot(const Model *m, Packet *p) {
  p->w[NOW] = m->now;
  p->w[TICK] = m->tick;
  p->w[PHASE] = m->phase;
  p->w[GENERATION] = m->generation;
  p->w[STATE] = m->state;
  p->w[SIGNALS] = m->signals;
  p->w[ACTIVE] = m->active;
  p->w[PASSED] = m->passed;
  p->w[STOP] = m->stop;
  p->w[STATUS] = m->status;
  p->w[SIGNO] = m->signo;
  p->w[SINCE] = m->since;
  p->w[NEXT] = m->next;
  p->w[COUNT] = m->cfg.streamCount;
  for (unsigned i = 0; i < m->cfg.streamCount; ++i) {
    const Flow *f = &m->flows[i];
    uint64_t *v = &p->w[FLOWS + VIEW_SIZE * i];
    v[QUEUED] = f->count;
    v[HEAD_ID] = f->count ? f->queue[f->head].id : 0;
    v[ARRIVED_AT] = f->count ? f->queue[f->head].arrived : 0;
    for (unsigned j = 0; j < f->count; ++j) {
      Person person = f->queue[(f->head + j) % MAX_QUEUE];
      if (person.emergency) {
        v[EMERGENCY_ID] = person.id;
        break;
      }
    }
    v[ACTIVE_ID] = f->active.id;
    v[LEAVE_AT] = f->leaveAt;
  }
}
void ModelReport(Model *m, unsigned processes) {
  uint64_t arrived = 0, refused = 0, queued = 0, wait = 0, maximum = 0;
  unsigned peak = 0;
  for (unsigned i = 0; i < m->cfg.streamCount; ++i) {
    Flow *f = &m->flows[i];
    Log(m, "Итог потока: поток=%s прибыло=%" PRIu64 " прошло=%" PRIu64
        " отказов=%" PRIu64 " ожидают=%u в_зоне=%u", m->cfg.streams[i].name,
        f->arrived, f->passed, f->refused, f->count, (unsigned)(f->active.id != 0));
    arrived += f->arrived;
    refused += f->refused;
    queued += f->count;
    wait += f->waitSum;
    if (f->waitMax > maximum)
      maximum = f->waitMax;
    if (f->peak > peak)
      peak = f->peak;
  }
  Log(m, "ИТОГО: прибыло=%" PRIu64 " прошло=%" PRIu64 " отказов=%" PRIu64
      " ожидают=%" PRIu64 " в_зоне=%u ожидание=%" PRIu64 " среднее=%.2f максимум=%" PRIu64
      " пик=%u процессов=%u", arrived, m->passed, refused, queued, Population(m->active),
      wait, m->passed ? (double)wait / (double)m->passed : 0.0, maximum, peak, processes);
}
