#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BACKEND
#define BACKEND NAMED
#endif
#ifndef ROLE_KIND
#define ROLE_KIND 0
#endif
int main(int argc, char **argv) {
  if (argc < 2 || !strcmp(argv[1], "--help")) {
    puts("Вариант 39. crossroad_* CONFIG [LOG] [--session DIR]\n"
         "Роли: CONFIG --session DIR --transport shm|fifo [--stream NAME]");
    return argc == 2 ? 0 : 2;
  }
  const char *log = "crossroad.log", *directory = NULL, *stream = NULL;
  unsigned backend = ROLE_KIND ? SHM : BACKEND;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--session") && i + 1 < argc)
      directory = argv[++i];
    else if (ROLE_KIND > 1 && !strcmp(argv[i], "--stream") && i + 1 < argc)
      stream = argv[++i];
    else if (ROLE_KIND && !strcmp(argv[i], "--transport") && i + 1 < argc) {
      const char *name = argv[++i];
      if (strcmp(name, "shm") && strcmp(name, "fifo"))
        return 2;
      backend = !strcmp(name, "fifo") ? FIFO : SHM;
    } else if (i == 2 && argv[i][0] != '-' && !ROLE_KIND)
      log = argv[i];
    else
      return 2;
  }
  char automatic[64];
  if (ROLE_KIND && !directory)
    return 2;
  if (!directory) {
    (void)snprintf(automatic, sizeof(automatic), ".crossroad-%ld", (long)getpid());
    directory = automatic;
  }
  if (strlen(directory) >= PATH_SIZE)
    return 2;
  Config cfg;
  uint64_t hash;
  int fd = ConfigOpen(&cfg, argv[1], &hash);
  if (fd < 0)
    return 2;
  if (ROLE_KIND) {
    (void)close(fd);
    unsigned role = 0;
    if (ROLE_KIND != 1) {
      if (!stream)
        return 2;
      unsigned i = 0;
      while (i < cfg.streamCount && strcmp(stream, cfg.streams[i].name))
        ++i;
      if (i == cfg.streamCount)
        return 2;
      role = 1 + i + (ROLE_KIND == 3 ? cfg.streamCount : 0);
    }
    return Worker(&cfg, hash, directory, backend, role, NULL);
  }
  Output out;
  bool opened = OutputOpen(&out, log, fd);
  (void)close(fd);
  if (!opened) {
    perror("Открытие журнала");
    return 1;
  }
  int status = Run(&cfg, hash, &out, directory, backend, backend >= SHM);
  OutputClose(&out);
  return out.failed ? 1 : status;
}
