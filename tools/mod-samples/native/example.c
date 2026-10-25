/* Independent native mod example. GPL-3.0-only. */
#include "../../mods/fm4_mod.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static const Fm4ModHost* host;
static int show = 1;
static uint64_t updates;
static void FM4_MOD_CALL toggle(void* state) {
  (void)state;
  show = !show;
  host->setting_set(host->handle, "visible", show ? "yes" : "no");
  host->log(host->handle, 1, "Example menu action completed");
}
static void FM4_MOD_CALL frame(void* state, uint64_t index, double seconds) {
  char text[128];
  (void)state; (void)seconds;
  if (show) snprintf(text, sizeof(text), "Example mod | frame %llu | updates %llu", (unsigned long long)index, (unsigned long long)updates);
  else text[0] = 0;
  host->hud(host->handle, 1, 64, 44, 20, text);
  if (index == 1) host->log(host->handle, 1, "Example frame callback reached");
  if (index == 2 && getenv("FM4_MOD_SAMPLE_CHECK")) {
    toggle(0); toggle(0);
    host->log(host->handle, 1, "Example settings/action callback check passed");
  }
}
static void FM4_MOD_CALL update(void* state, uint64_t index, double seconds) {
  unsigned char code[4]; uint64_t result;
  (void)state; (void)seconds; updates = index;
  if (index == 1) {
    int read = host->read_memory(host->handle, 0x826E5F88u, code, sizeof(code));
    int invalid = host->read_memory(host->handle, 0xfffffff0u, code, 32);
    int unknown = host->call(host->handle, "unknown", 0, 0, &result);
    if (read == FM4_MOD_OK && invalid == FM4_MOD_INVALID && unknown == FM4_MOD_INVALID)
      host->log(host->handle, 1, "Example update callback and memory/call checks passed");
  }
}
static void FM4_MOD_CALL unload(void* state) {
  (void)state;
  host->log(host->handle, 1, "Example plugin unloaded");
}
FM4_MOD_EXPORT int32_t FM4_MOD_CALL Fm4ModStart(const Fm4ModHost* api, Fm4ModCallbacks* callbacks) {
  char greeting[128];
  if (!api || !callbacks || api->abi != FM4_MOD_ABI || api->bytes < sizeof(Fm4ModHost) || callbacks->bytes < sizeof(Fm4ModCallbacks)) return FM4_MOD_INVALID;
  host = api;
  memset(callbacks, 0, sizeof(*callbacks));
  callbacks->bytes = sizeof(*callbacks); callbacks->abi = FM4_MOD_ABI;
  callbacks->frame = frame; callbacks->update = update; callbacks->unload = unload;
  if (host->setting_get(host->handle, "greeting", greeting, sizeof(greeting)) == FM4_MOD_OK) host->log(host->handle, 1, greeting);
  host->action(host->handle, 1, "Toggle example HUD", toggle);
  host->hud(host->handle, 1, 64, 44, 20, "Example mod loaded");
  host->log(host->handle, 1, "Example plugin loaded");
  return FM4_MOD_OK;
}
