#include "ps4_log.h"

#include "common/ps4_host.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int32_t k_allocate_direct(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t* phys) __asm__("sceKernelAllocateDirectMemory");
int32_t k_release_direct(off_t start, size_t len) __asm__("sceKernelReleaseDirectMemory");
size_t k_direct_size() __asm__("sceKernelGetDirectMemorySize");
int32_t k_map_direct(void** addr, size_t len, int32_t prot, int32_t flags, off_t start, size_t align) __asm__("sceKernelMapDirectMemory");
int32_t k_reserve(void** addr, size_t len, int32_t flags, size_t align) __asm__("sceKernelReserveVirtualRange");
int32_t k_munmap(void* addr, size_t len) __asm__("sceKernelMunmap");
int32_t k_mprotect(const void* addr, size_t len, int32_t prot) __asm__("sceKernelMprotect");
int32_t k_map_flexible(void** addr, size_t len, int32_t prot, int32_t flags, const char* name) __asm__("sceKernelMapNamedFlexibleMemory");
int32_t k_map_system_flexible(void** addr, size_t len, int32_t prot, int32_t flags, const char* name) __asm__("sceKernelMapNamedSystemFlexibleMemory");
int32_t k_jit_shm(const char* name, size_t len, int32_t prot, int32_t* fd) __asm__("sceKernelJitCreateSharedMemory");
int32_t k_close(int32_t fd) __asm__("sceKernelClose");
int32_t k_enable_aliasing() __asm__("sceKernelEnableDmemAliasing");
int32_t k_enable_aliasing2() __asm__("sceKernelEnableDmemAliasing2");
int32_t k_load_exec(const char* path, char* const* argv) __asm__("sceSystemServiceLoadExec");
int32_t k_send_notification(int32_t device, void* request, size_t size, int32_t blocking) __asm__("sceKernelSendNotificationRequest");
int32_t k_user_init(void* params) __asm__("sceUserServiceInitialize");
void* __real_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset);
const char* ps4_data_dir();
void ps4_boot_trace(const char* stage);
void ps4_watch_thread(const char* name);
void ps4_dump_thread_stacks(const char* path, const char* reason);
void ps4_hot_profiler_set_enabled(int on);
size_t ps4_heap_footprint();
void* k_pthread_self() __asm__("scePthreadSelf");
int32_t k_pthread_set_prio(void* thread, int prio) __asm__("scePthreadSetprio");
}

namespace {

constexpr size_t PAGE = 0x4000;
constexpr size_t RESERVE_ALIGN = 0x10000;
constexpr int32_t MAP_FIXED_FLAG = 0x10;
constexpr int32_t PROT_RW = 0x3;
constexpr int32_t PROT_RWX = 0x7;

size_t AlignUp(size_t value, size_t align)
{
  return (value + align - 1) & ~(align - 1);
}

struct SharedMemory
{
  off_t phys;
  size_t size;
};

std::once_flag s_aliasing_once;

void* ReserveAway(size_t size)
{
  for (uintptr_t hint : {0x1000000000ull, 0x800000000ull, 0x600000000ull})
  {
    void* addr = reinterpret_cast<void*>(hint);
    if (k_reserve(&addr, size, 0, RESERVE_ALIGN) == 0)
    {
      if (reinterpret_cast<uintptr_t>(addr) >= 0x400000000ull)
        return addr;
      k_munmap(addr, size);
    }
  }
  return nullptr;
}

const char* const EXEC_METHOD_NAMES[] = {"mmap", "mmap+mprotect", "flexible memory", "system flexible memory",
                                         "JIT shared memory"};
int s_exec_method = -2;
bool s_jit_unavailable = false;
std::mutex s_exec_mutex;

void* MapExec(int method, void* at, size_t len)
{
  const int fixed = at ? MAP_FIXED : 0;
  switch (method)
  {
    case 0:
    case 1:
    {
      void* p = __real_mmap(at, len, method == 0 ? PROT_RWX : PROT_RW, MAP_ANON | MAP_PRIVATE | fixed, -1, 0);
      if (p == MAP_FAILED)
        return nullptr;
      if (method == 1 && k_mprotect(p, len, PROT_RWX) != 0)
      {
        k_munmap(p, len);
        return nullptr;
      }
      return p;
    }
    case 2:
    case 3:
    {
      void* base = at ? at : ReserveAway(len);
      if (!base)
        return nullptr;
      const int32_t ret = (method == 2) ? k_map_flexible(&base, len, PROT_RWX, MAP_FIXED_FLAG, "duckstation jit") :
                                          k_map_system_flexible(&base, len, PROT_RWX, MAP_FIXED_FLAG, "duckstation jit");
      if (ret != 0)
      {
        if (!at)
          k_munmap(base, len);
        return nullptr;
      }
      return base;
    }
    case 4:
    {
      int32_t shm = -1;
      if (k_jit_shm(nullptr, len, PROT_RWX, &shm) != 0)
        return nullptr;
      void* p = __real_mmap(at, len, PROT_RWX, MAP_SHARED | fixed, shm, 0);
      k_close(shm);
      return (p == MAP_FAILED) ? nullptr : p;
    }
  }
  return nullptr;
}

sigjmp_buf s_exec_jump;

void ExecFault(int)
{
  siglongjmp(s_exec_jump, 1);
}

bool Runs(void* code)
{
  struct sigaction fault = {}, old_segv = {}, old_bus = {};
  fault.sa_handler = ExecFault;
  sigemptyset(&fault.sa_mask);
  sigaction(SIGSEGV, &fault, &old_segv);
  sigaction(SIGBUS, &fault, &old_bus);
  volatile int result = 0;
  if (sigsetjmp(s_exec_jump, 1) == 0)
    result = reinterpret_cast<int (*)()>(code)();
  sigaction(SIGSEGV, &old_segv, nullptr);
  sigaction(SIGBUS, &old_bus, nullptr);
  return result == 0x1234;
}

int ProbeExec()
{
  static const unsigned char test_code[] = {0xB8, 0x34, 0x12, 0x00, 0x00, 0xC3};
  for (int method = 0; method < 5; method++)
  {
    void* page = MapExec(method, nullptr, PAGE);
    if (!page)
    {
      LOG_INFO("exec: {} refused", EXEC_METHOD_NAMES[method]);
      continue;
    }
    std::memcpy(page, test_code, sizeof(test_code));
    const bool ok = Runs(page);
    k_munmap(page, PAGE);
    if (ok)
    {
      LOG_INFO("exec: executable memory from {}", EXEC_METHOD_NAMES[method]);
      return method;
    }
    LOG_INFO("exec: {} maps, but code there faults", EXEC_METHOD_NAMES[method]);
  }
  LOG_WARN("exec: no executable memory, the recompiler is off");
  return -1;
}

int ExecMethod()
{
  std::lock_guard lock(s_exec_mutex);
  if (s_exec_method == -2)
    s_exec_method = ProbeExec();
  return s_exec_method;
}

}

extern "C" int ps4_host_protect(void* addr, size_t size, int prot)
{
  return (k_mprotect(addr, size, prot & 0x7) == 0) ? 0 : -1;
}

extern "C" void* ps4_host_shm_create(size_t size)
{
  std::call_once(s_aliasing_once, []() {
    const int32_t r1 = k_enable_aliasing();
    const int32_t r2 = (r1 == 0) ? 0 : k_enable_aliasing2();
    LOG_INFO("direct memory aliasing: {:#x}, aliasing2: {:#x}", static_cast<uint32_t>(r1), static_cast<uint32_t>(r2));
  });

  const size_t aligned = AlignUp(size, PAGE);
  off_t phys = 0;
  const int32_t ret = k_allocate_direct(0, static_cast<off_t>(k_direct_size()), aligned, RESERVE_ALIGN, 0, &phys);
  if (ret != 0)
  {
    LOG_ERROR("shared memory: allocating {} KiB of direct memory failed: {:#x}", aligned / 1024,
              static_cast<uint32_t>(ret));
    return nullptr;
  }

  void* scratch = nullptr;
  if (k_map_direct(&scratch, aligned, PROT_RW, 0, phys, PAGE) == 0 && scratch)
  {
    std::memset(scratch, 0, aligned);
    k_munmap(scratch, aligned);
  }

  SharedMemory* shm = new SharedMemory{phys, aligned};
  LOG_INFO("shared memory: {} KiB of direct memory", aligned / 1024);
  return shm;
}

extern "C" void ps4_host_shm_destroy(void* handle)
{
  SharedMemory* shm = static_cast<SharedMemory*>(handle);
  if (!shm)
    return;
  k_release_direct(shm->phys, shm->size);
  delete shm;
}

extern "C" void* ps4_host_shm_map(void* handle, size_t offset, void* addr, size_t size, int prot)
{
  SharedMemory* shm = static_cast<SharedMemory*>(handle);
  if (!shm || offset + size > shm->size)
    return nullptr;

  void* at = addr;
  const int32_t ret = k_map_direct(&at, AlignUp(size, PAGE), prot & 0x7, addr ? MAP_FIXED_FLAG : 0,
                                   shm->phys + static_cast<off_t>(offset), PAGE);
  if (ret != 0 || (addr && at != addr))
  {
    LOG_ERROR("shared memory: mapping {} KiB at {} failed: {:#x}", size / 1024, addr, static_cast<uint32_t>(ret));
    return nullptr;
  }
  return at;
}

extern "C" int ps4_host_shm_unmap(void* addr, size_t size)
{
  return (k_munmap(addr, AlignUp(size, PAGE)) == 0) ? 0 : -1;
}

extern "C" void* ps4_host_reserve(size_t size)
{
  return ReserveAway(AlignUp(size, RESERVE_ALIGN));
}

extern "C" void ps4_host_release(void* addr, size_t size)
{
  k_munmap(addr, AlignUp(size, PAGE));
}

extern "C" int ps4_host_rereserve(void* addr, size_t size)
{
  const size_t len = AlignUp(size, PAGE);
  k_munmap(addr, len);
  void* at = addr;
  return (k_reserve(&at, len, MAP_FIXED_FLAG, PAGE) == 0 && at == addr) ? 0 : -1;
}

extern "C" void* ps4_host_jit_alloc(size_t size)
{
  const int method = ExecMethod();
  if (method < 0)
    return nullptr;

  const size_t len = AlignUp(size, RESERVE_ALIGN);
  const uintptr_t code = reinterpret_cast<uintptr_t>(&ps4_host_jit_alloc);
  const uintptr_t step = 0x10000000;
  for (int i = 1; i <= 6; i++)
  {
    const uintptr_t want = (code & ~(step - 1)) + static_cast<uintptr_t>(i) * step;
    void* addr = reinterpret_cast<void*>(want);
    if (k_reserve(&addr, len, 0, RESERVE_ALIGN) != 0)
      continue;
    if (addr != reinterpret_cast<void*>(want))
    {
      k_munmap(addr, len);
      continue;
    }
    void* p = MapExec(method, addr, len);
    if (p == addr)
    {
      LOG_INFO("jit: {} MiB from {} at {} (code at {:#x})", len >> 20, EXEC_METHOD_NAMES[method], p, code);
      return p;
    }
    if (p)
      k_munmap(p, len);
    k_munmap(addr, len);
  }

  void* p = MapExec(method, nullptr, len);
  if (p)
    LOG_WARN("jit: {} MiB from {} at {}, far from the code at {:#x}", len >> 20, EXEC_METHOD_NAMES[method], p, code);
  else
    LOG_ERROR("jit: mapping {} MiB of executable memory failed", len >> 20);
  return p;
}

extern "C" void ps4_host_jit_free(void* addr, size_t size)
{
  if (addr)
    k_munmap(addr, AlignUp(size, RESERVE_ALIGN));
}

extern "C" int ps4_host_jit_available(void)
{
  return (!s_jit_unavailable && ExecMethod() >= 0) ? 1 : 0;
}

extern "C" void ps4_host_set_jit_unavailable(void)
{
  s_jit_unavailable = true;
}

extern "C" const char* ps4_host_data_dir(void)
{
  return ps4_data_dir();
}

extern "C" const char* ps4_host_app_dir(void)
{
  return "/app0";
}

extern "C" void ps4_host_notify(const char* text)
{
  struct
  {
    int32_t type;
    int32_t req_id;
    int32_t priority;
    int32_t msg_id;
    int32_t target_id;
    int32_t user_id;
    int32_t unk1;
    int32_t unk2;
    int32_t app_id;
    int32_t error_num;
    int32_t unk3;
    char use_icon_image_uri;
    char message[1024];
    char icon_uri[1024];
    char unk[1024];
  } request = {};
  request.target_id = -1;
  std::strncpy(request.message, text, sizeof(request.message) - 1);
  k_send_notification(0, &request, sizeof(request), 0);
}

extern "C" void ps4_host_exit(int code)
{
  LOG_INFO("exit {}: back to the home screen", code);
  std::fflush(stdout);
  std::fflush(stderr);
  k_load_exec("exit", nullptr);
  for (;;)
    usleep(100000);
}

extern "C" void ps4_vm_prepare(const void* address, size_t size)
{
}

namespace {

std::atomic<uint64_t> s_frames{0};
std::once_flag s_monitor_once;

uint64_t ReadClock(clockid_t clock)
{
  timespec ts = {};
  if (clock_gettime(clock, &ts) != 0)
    return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

bool FileExists(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

void Monitor()
{
  ps4_watch_thread("monitor");
  k_pthread_set_prio(k_pthread_self(), 720);
  const std::string dir = ps4_data_dir();
  const std::string freeze = dir + "/freeze.log";
  std::rename(freeze.c_str(), (dir + "/freeze.prev.log").c_str());
  const bool perf_log = !FileExists(dir + "/noperflog");
  constexpr clockid_t PROCESS_CPU_CLOCK = 15;

  uint64_t last_frames = s_frames.load();
  uint64_t window_frames = last_frames;
  uint64_t window_wall = ReadClock(CLOCK_MONOTONIC);
  uint64_t window_cpu = ReadClock(PROCESS_CPU_CLOCK);
  int still = 0;
  bool reported = false;
  for (unsigned tick = 1;; tick++)
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const uint64_t frames = s_frames.load();
    still = (frames == last_frames) ? still + 1 : 0;
    last_frames = frames;
    if (still >= 10 && !reported)
    {
      reported = true;
      LOG_WARN("no frame for {} s: thread stacks in freeze.log", still);
      ps4_dump_thread_stacks(freeze.c_str(), "freeze: no frame presented for 10 s (first look)");
      std::this_thread::sleep_for(std::chrono::seconds(3));
      ps4_dump_thread_stacks(freeze.c_str(), "freeze: second look, 3 s later");
    }
    else if (still == 0)
    {
      reported = false;
    }

    if (perf_log && tick % 5 == 0)
    {
      const uint64_t wall = ReadClock(CLOCK_MONOTONIC);
      const uint64_t cpu = ReadClock(PROCESS_CPU_CLOCK);
      const double seconds = (wall - window_wall) / 1e9;
      if (seconds > 0.0)
      {
        LOG_INFO("perf: {:.1f} frames/s, cpu {:.2f} of 7 cores, heap {} MiB", (frames - window_frames) / seconds,
                 cpu && window_cpu ? (cpu - window_cpu) / 1e9 / seconds : 0.0, ps4_heap_footprint() >> 20);
      }
      window_frames = frames;
      window_wall = wall;
      window_cpu = cpu;
    }
  }
}

}

extern "C" void ps4_host_frame(void)
{
  s_frames.fetch_add(1, std::memory_order_relaxed);
  std::call_once(s_monitor_once, []() {
    std::thread(Monitor).detach();
    if (FileExists(std::string(ps4_data_dir()) + "/profile"))
    {
      LOG_INFO("profiler on: profile.log every 10 s");
      ps4_hot_profiler_set_enabled(1);
    }
  });
}

extern "C" void ps4_host_thread_named(const char* name)
{
  ps4_watch_thread(name);
  int prio = 0;
  if (std::strcmp(name, "CPU Thread") == 0)
    prio = 660;
  else if (std::strcmp(name, "Audio Output") == 0)
    prio = 640;
  else if (std::strcmp(name, "ImGuiFullscreen Texture Loader") == 0 || std::strcmp(name, "Async Op") == 0)
    prio = 750;
  if (prio)
    k_pthread_set_prio(k_pthread_self(), prio);
}

__attribute__((constructor(200))) static void ps4_host_early_init()
{
  const char* dir = ps4_data_dir();
  ps4_log_init();
  for (const char* sub : {"/bios", "/games", "/memcards", "/savestates", "/cache"})
    mkdir((std::string(dir) + sub).c_str(), 0777);
  k_user_init(nullptr);
  char version[16] = "?";
  if (FILE* file = std::fopen("/app0/version.txt", "r"))
  {
    if (std::fgets(version, sizeof(version), file))
      version[std::strcspn(version, "\r\n")] = '\0';
    std::fclose(file);
  }
  LOG_INFO("DuckStation for PS4 v{}, data in {}", version, dir);
  ps4_boot_trace("main");
  ExecMethod();
}

extern "C" void ps4_crash_guest_info()
{
}

extern "C" unsigned long long ZSTD_trace_compress_begin(const void* cctx)
{
  return 0;
}

extern "C" void ZSTD_trace_compress_end(unsigned long long ctx, const void* trace)
{
}

extern "C" unsigned long long ZSTD_trace_decompress_begin(const void* dctx)
{
  return 0;
}

extern "C" void ZSTD_trace_decompress_end(unsigned long long ctx, const void* trace)
{
}
