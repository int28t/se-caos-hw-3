// Конфигурация полностью проверяется до создания IPC и открытия журнала.
#include "crossroad.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static bool Number(const char *s, uint64_t limit, uint64_t *out) {
  if (!*s)
    return false;
  uint64_t n = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || n > limit / 10 ||
        (n == limit / 10 && (unsigned)(*s - '0') > limit % 10))
      return false;
    n = n * 10 + (unsigned)(*s - '0');
  }
  *out = n;
  return true;
}
static bool Name(const char *s) {
  size_t n = strlen(s);
  if (!n || n >= NAME_SIZE)
    return false;
  for (; *s; ++s)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
          (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
      return false;
  return true;
}
static int Index(const char *s, const char *const *names, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    if (!strcmp(s, names[i]))
      return (int)i;
  return -1;
}
static int StreamIndex(const Config *c, const char *s) {
  for (unsigned i = 0; i < c->streamCount; ++i)
    if (!strcmp(s, c->streams[i].name))
      return (int)i;
  return -1;
}
static bool Line(Config *c, char **t, unsigned count) {
  uint64_t a, b, n;
  if (!strcmp(t[0], "stream") && count == 8) {
    const char *const kinds[] = {"car", "pedestrian"};
    const char *const directions[] = {"north", "east", "south", "west"};
    const char *const maneuvers[] = {"straight", "right", "left", "walk"};
    int kind = Index(t[2], kinds, 2), dir = Index(t[3], directions, 4);
    int turn = Index(t[4], maneuvers, 4);
    if (c->streamCount == MAX_STREAMS || !Name(t[1]) || StreamIndex(c, t[1]) >= 0 ||
        kind < 0 || dir < 0 || turn < 0 || (kind == PEDESTRIAN) != (turn == WALK) ||
        !Number(t[5], MAX_QUEUE, &a) || !a || !Number(t[6], 1000, &b) || !b ||
        !Number(t[7], 1000, &n))
      return false;
    unsigned i = c->streamCount++;
    c->streams[i] = (Stream){.kind = (Kind)kind, .direction = (Direction)dir,
        .maneuver = (Maneuver)turn, .capacity = (unsigned)a,
        .crossingTicks = (unsigned)b, .rate = (unsigned)n};
    strcpy(c->streams[i].name, t[1]);
    c->conflicts[i] = UINT32_MAX;
    return true;
  }
  if (!strcmp(t[0], "phase") && count >= 3) {
    if (c->phaseCount == MAX_PHASES || !Name(t[1]))
      return false;
    for (unsigned i = 0; i < c->phaseCount; ++i)
      if (!strcmp(c->phases[i].name, t[1]))
        return false;
    Phase p = {0};
    strcpy(p.name, t[1]);
    for (unsigned i = 2; i < count; ++i) {
      int j = StreamIndex(c, t[i]);
      if (j < 0 || (p.mask & (1u << (unsigned)j)))
        return false;
      p.mask |= 1u << (unsigned)j;
    }
    c->phases[c->phaseCount++] = p;
    return true;
  }
  if (!strcmp(t[0], "conflict") && count == 4) {
    int i = StreamIndex(c, t[1]), j = StreamIndex(c, t[2]);
    if (i < 0 || j < 0 || !Number(t[3], 1, &n) || (i == j && !n))
      return false;
    if (n) {
      c->conflicts[i] |= 1u << (unsigned)j;
      c->conflicts[j] |= 1u << (unsigned)i;
    } else {
      c->conflicts[i] &= ~(1u << (unsigned)j);
      c->conflicts[j] &= ~(1u << (unsigned)i);
    }
    return true;
  }
  if (count != 2)
    return false;
  if (!strcmp(t[0], "strategy")) {
    if (strcmp(t[1], "fixed") && strcmp(t[1], "adaptive"))
      return false;
    c->strategy = !strcmp(t[1], "fixed") ? FIXED : ADAPTIVE;
    return true;
  }
  if (!Number(t[1], !strcmp(t[0], "seed") ? UINT32_MAX : 1000000000, &n))
    return false;
  if (!strcmp(t[0], "ticks") || !strcmp(t[0], "target")) {
    *(!strcmp(t[0], "ticks") ? &c->ticks : &c->target) = n;
    return true;
  }
  struct { const char *name; unsigned *value, min, max; } fields[] = {
      {"seed", &c->seed, 0, UINT32_MAX}, {"min_green", &c->minGreen, 1, 10000},
      {"max_green", &c->maxGreen, 1, 10000}, {"clearance", &c->clearance, 1, 10000},
      {"emergency_rate", &c->emergencyRate, 0, 1000}, {"tick_ms", &c->tickMs, 1, 60000},
      {"delay_ms", &c->delayMs, 0, 60000}};
  if (!strcmp(t[0], "priority") && n <= 1) {
    c->priority = n != 0;
    return true;
  }
  for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); ++i)
    if (!strcmp(t[0], fields[i].name) && n >= fields[i].min && n <= fields[i].max) {
      *fields[i].value = (unsigned)n;
      return true;
    }
  return false;
}
int ConfigOpen(Config *c, const char *path, uint64_t *hash) {
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
    return -1;
  struct stat st;
  char text[TEXT_SIZE + 1];
  size_t used = 0;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
    goto fail;
  for (;;) {
    ssize_t n = read(fd, text + used, sizeof(text) - used);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 || used == sizeof(text))
      goto fail;
    if (!n)
      break;
    if (memchr(text + used, 0, (size_t)n))
      goto fail;
    used += (size_t)n;
  }
  text[used] = 0;
  *hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < used; ++i)
    *hash = (*hash ^ (unsigned char)text[i]) * UINT64_C(1099511628211);
  *c = (Config){.ticks = 120, .seed = 39, .minGreen = 4, .maxGreen = 10,
      .clearance = 2, .emergencyRate = 20, .tickMs = 1, .priority = true, .strategy = ADAPTIVE};
  char *save, *line = strtok_r(text, "\n", &save);
  while (line) {
    char *comment = strchr(line, '#'), *tokens[MAX_STREAMS + 2], *word;
    if (comment)
      *comment = 0;
    unsigned count = 0;
    for (char *t = strtok_r(line, " \r\t", &word); t; t = strtok_r(NULL, " \r\t", &word)) {
      if (count == MAX_STREAMS + 2)
        goto fail;
      tokens[count++] = t;
    }
    if (count && !Line(c, tokens, count))
      goto fail;
    line = strtok_r(NULL, "\n", &save);
  }
  if (!c->streamCount || !c->phaseCount || c->minGreen > c->maxGreen ||
      c->ticks > UINT64_MAX / ((uint64_t)c->tickMs * NS))
    goto fail;
  uint32_t covered = 0;
  for (unsigned p = 0; p < c->phaseCount; ++p) {
    uint32_t mask = c->phases[p].mask;
    covered |= mask;
    for (unsigned i = 0; i < c->streamCount; ++i)
      if ((mask & (1u << i)) && (c->conflicts[i] & mask & ~(1u << i)))
        goto fail;
  }
  for (unsigned i = 0; i < c->streamCount; ++i)
    if (!(covered & (1u << i)) || c->streams[i].crossingTicks > c->maxGreen)
      goto fail;
  return fd;
fail:
  dprintf(STDERR_FILENO, "Некорректный вход: %s\n", path);
  (void)close(fd);
  return -1;
}
