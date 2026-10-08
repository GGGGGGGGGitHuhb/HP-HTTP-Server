#define _GNU_SOURCE
#include "ClientObserver.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}

int main(int argc, char **argv) {
  require(argc == 2, "expected admitted output directory");
  char path[4096];
  require(snprintf(path, sizeof(path), "%s/client-unit-control.bin", argv[1]) < (int)sizeof(path), "path too long");
  HpS4Control initial = {0};
  memcpy(initial.magic, "S4CTRL02", 8);
  initial.version = 2; initial.bytes = sizeof(initial); initial.connections = 2;
  initial.detailed = 1;
  FILE *file = fopen(path, "wb");
  require(file && fwrite(&initial, sizeof(initial), 1, file) == 1 && fclose(file) == 0, "control fixture write failed");
  require(setenv("HP_S4_CONTROL", path, 1) == 0 && setenv("HP_S4_OUTPUT", argv[1], 1) == 0, "fixture environment failed");
  unsetenv("HP_S4_MARKER_FD");
  initializeClientObserver(2, 2);
  registerClientThread(0); registerClientThread(1);
  HpS4Control *control = clientControl();
  control->selectedCount = 1; control->selectedClient[0] = 0;
  __atomic_store_n(&control->go, 1U, __ATOMIC_RELEASE);
  errno = EAGAIN;
  appendClientEvent(0, 1, 1, 1, 64, 0, 0);
  require(errno == EAGAIN, "unselected append changed errno");
  for (unsigned int index = 0; index <= HP_S4_BUFFER_BYTES / sizeof(HpS4Event); ++index)
    appendClientEvent(0, 0, 1, 1, 64, 0, 0);
  require(errno == EAGAIN, "append changed errno");
  require(__atomic_load_n(&control->abortRun, __ATOMIC_ACQUIRE) == 1, "overflow did not stop producer");
  stopClientThread(0); stopClientThread(1); exportClientObserver();
  require(snprintf(path, sizeof(path), "%s/client-worker-0.events.bin", argv[1]) < (int)sizeof(path), "path too long");
  file = fopen(path, "rb"); HpS4Header header;
  require(file && fread(&header, sizeof(header), 1, file) == 1 && fclose(file) == 0, "header read failed");
  require(header.flags == 3 && header.count == HP_S4_BUFFER_BYTES / sizeof(HpS4Event), "overflow corrupted header/count");
  puts("client observer cases=identity/filter/errno/overflow/export status=valid");
  return 0;
}
