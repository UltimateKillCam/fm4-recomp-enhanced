/* FM4 native extension ABI. GPL-3.0-only. */
#ifndef FM4_MOD_H
#define FM4_MOD_H
#include <stdint.h>
#ifdef _WIN32
#define FM4_MOD_EXPORT __declspec(dllexport)
#define FM4_MOD_CALL __cdecl
#else
#define FM4_MOD_EXPORT
#define FM4_MOD_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define FM4_MOD_ABI 0x00010000u
#define FM4_MOD_OK 0
#define FM4_MOD_INVALID -1
#define FM4_MOD_UNAVAILABLE -2
#define FM4_MOD_RESTART 1
typedef void* Fm4ModHandle;
typedef void (FM4_MOD_CALL *Fm4ModAction)(void* state);
typedef struct Fm4ModHost {
  uint32_t bytes;
  uint32_t abi;
  Fm4ModHandle handle;
  const char* id;
  void (FM4_MOD_CALL *log)(Fm4ModHandle, uint32_t severity, const char* utf8);
  int32_t (FM4_MOD_CALL *read_memory)(Fm4ModHandle, uint32_t guest, void* bytes, uint32_t count);
  int32_t (FM4_MOD_CALL *write_memory)(Fm4ModHandle, uint32_t guest, const void* bytes, uint32_t count);
  int32_t (FM4_MOD_CALL *call)(Fm4ModHandle, const char* symbol, const uint32_t* args, uint32_t count, uint64_t* result);
  int32_t (FM4_MOD_CALL *setting_get)(Fm4ModHandle, const char* key, char* utf8, uint32_t capacity);
  int32_t (FM4_MOD_CALL *setting_set)(Fm4ModHandle, const char* key, const char* utf8);
  int32_t (FM4_MOD_CALL *hud)(Fm4ModHandle, uint32_t label, float x, float y, float height, const char* utf8);
  int32_t (FM4_MOD_CALL *action)(Fm4ModHandle, uint32_t action, const char* label, Fm4ModAction callback);
  /* Queue an existing manifest string binding for the next preparation. */
  int32_t (FM4_MOD_CALL *replace_string)(Fm4ModHandle, const char* table, uint32_t id, const char* utf8);
} Fm4ModHost;
typedef struct Fm4ModCallbacks {
  uint32_t bytes;
  uint32_t abi;
  void* state;
  void (FM4_MOD_CALL *unload)(void*);
  void (FM4_MOD_CALL *frame)(void*, uint64_t frame, double elapsed_seconds);
  void (FM4_MOD_CALL *update)(void*, uint64_t update, double delta_seconds);
} Fm4ModCallbacks;
/* A DLL exports this one entry; return zero only after filling callbacks. */
typedef int32_t (FM4_MOD_CALL *Fm4ModEntry)(const Fm4ModHost*, Fm4ModCallbacks*);
FM4_MOD_EXPORT int32_t FM4_MOD_CALL Fm4ModStart(const Fm4ModHost*, Fm4ModCallbacks*);
#ifdef __cplusplus
}
#endif
#endif
