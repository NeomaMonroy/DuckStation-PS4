#include "ps4_compat_decls.h"
#include <dlfcn.h>
#include <execinfo.h>

#include <cerrno>
#include <cstdint>
#include <unwind.h>

#define WEAK __attribute__((weak))

extern "C" void* ps4_thread_entry(void* raw);

extern "C" int ps4_backtrace(void** frames, int max)
{
  struct Walk
  {
    void** frames;
    int max;
    int count;
  } walk{frames, max, 0};
  _Unwind_Backtrace(
    [](_Unwind_Context* context, void* arg) -> _Unwind_Reason_Code {
      Walk& w = *static_cast<Walk*>(arg);
      if (w.count >= w.max)
        return _URC_END_OF_STACK;
      w.frames[w.count++] = reinterpret_cast<void*>(_Unwind_GetIP(context));
      if (_Unwind_GetRegionStart(context) == reinterpret_cast<uintptr_t>(&ps4_thread_entry))
        return _URC_END_OF_STACK;
      return _URC_NO_REASON;
    },
    &walk);
  return walk.count;
}

extern "C" {
WEAK void* dlopen(const char*, int)
{
  return nullptr;
}
WEAK void* dlsym(void*, const char*)
{
  return nullptr;
}
WEAK int dlclose(void*)
{
  return 0;
}
WEAK char* dlerror(void)
{
  static char message[] = "no shared libraries on the PS4";
  return message;
}
WEAK int dladdr(const void*, Dl_info* info)
{
  if (info != nullptr)
    *info = Dl_info{};
  return 0;
}
WEAK int backtrace(void** frames, int max)
{
  return ps4_backtrace(frames, max);
}
WEAK char** backtrace_symbols(void* const*, int)
{
  return nullptr;
}
WEAK void backtrace_symbols_fd(void* const*, int, int)
{
}
WEAK int pthread_getname_np(pthread_t, char* name, size_t size)
{
  if (name != nullptr && size > 0)
    name[0] = '\0';
  return 0;
}
WEAK int execv(const char*, char* const*)
{
  errno = ENOSYS;
  return -1;
}
}
