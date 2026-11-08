#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <atomic>
#include <sys/types.h>

extern "C" {
int32_t sceKernelDebugOutText(int32_t channel, const char* fmt, ...);
int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void* addr, size_t len);
int32_t sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags, const char* name);
int32_t sceKernelMapNamedSystemFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags,
                                              const char* name);
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type,
                                      off_t* physOut);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys,
                                 size_t align);

typedef void* mspace;
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
void* mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void* mem);
void* mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
void* mspace_realloc(mspace msp, void* mem, size_t newsize);
void* mspace_memalign(mspace msp, size_t alignment, size_t bytes);
size_t mspace_usable_size(const void* mem);
size_t mspace_footprint(mspace msp);
size_t mspace_max_footprint(mspace msp);
void* memcpy(void* dst, const void* src, size_t n);

__attribute__((weak)) void ps4_boot_trace(const char* stage);
int ps4_heap_ok(void* msp);

extern char __text_start[];
}

namespace {

const size_t MB = 1024 * 1024;
const size_t PAGE = 16 * 1024;

const size_t RESERVE_FOR_SYSTEM = 320 * MB;
const int32_t PROT_CPU_RW = 0x3;
const int32_t MAP_FIXED_FLAG = 0x10;

mspace g_heap = nullptr;
const char* g_heapPool = "none";
uintptr_t g_heapBase = 0;
size_t g_heapSize = 0;

__attribute__((format(printf, 1, 2))) void heapLog(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    sceKernelDebugOutText(0, line);
    if (ps4_boot_trace)
        ps4_boot_trace(line);
}

unsigned long long elf(const void* address) {
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                           reinterpret_cast<uintptr_t>(__text_start));
}

void logStack() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(__text_start);
    const auto* stack = static_cast<const uintptr_t*>(__builtin_frame_address(0));
    int found = 0;
    for (int i = 0; i < 512 && found < 16; i++) {
        if (stack[i] >= base && stack[i] < base + 0x1000000) {
            heapLog("[v3k] heap:   stack elf 0x%llx\n", static_cast<unsigned long long>(stack[i] - base));
            found++;
        }
    }
}

mspace createMspace(const char* pool, void* base, size_t size) {
    mspace msp = create_mspace_with_base(base, size, 1);
    if (!msp) {
        heapLog("[v3k] heap: create_mspace_with_base on %zu MiB of %s failed\n", size / MB, pool);
        return nullptr;
    }
    void* test = mspace_malloc(msp, 4096);
    if (!test) {
        heapLog("[v3k] heap: %zu MiB of %s at %p: self-test allocation failed\n", size / MB, pool, base);
        return nullptr;
    }
    mspace_free(msp, test);
    g_heapPool = pool;
    g_heapBase = reinterpret_cast<uintptr_t>(base);
    g_heapSize = size;
    heapLog("[v3k] heap: dlmalloc on %zu MiB of %s at %p\n", size / MB, pool, base);
    return msp;
}

void* reserveAwayFromKernel(size_t size) {
    const uintptr_t hints[] = {0x1000000000ull, 0x800000000ull, 0x600000000ull};
    for (uintptr_t hint : hints) {
        void* address = reinterpret_cast<void*>(hint);
        if (sceKernelReserveVirtualRange(&address, size, 0, PAGE) == 0) {
            if (reinterpret_cast<uintptr_t>(address) >= 0x400000000ull)
                return address;
            heapLog("[v3k] heap: reservation hint %p gave %p, trying another\n", reinterpret_cast<void*>(hint),
                    address);
        }
    }
    return nullptr;
}

mspace mapAndCreate(bool systemPool, size_t size) {

    void* guard = reserveAwayFromKernel(size + PAGE);
    if (!guard)
        return nullptr;
    void* base = static_cast<char*>(guard) + PAGE;
    int32_t ret = systemPool
                      ? sceKernelMapNamedSystemFlexibleMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, "v3k heap")
                      : sceKernelMapNamedFlexibleMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, "v3k heap");
    if (ret != 0) {
        heapLog("[v3k] heap: mapping %zu MiB of %s flexible memory failed (0x%x)\n", size / MB,
                systemPool ? "system" : "regular", ret);
        return nullptr;
    }
    return createMspace(systemPool ? "system flexible memory" : "flexible memory", base, size);
}

mspace createDirectHeap(size_t size) {
    off_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 2 * MB, 0, &phys) != 0)
        return nullptr;
    void* base = nullptr;
    if (sceKernelMapDirectMemory(&base, size, PROT_CPU_RW, 0, phys, 2 * MB) != 0)
        return nullptr;
    return createMspace("direct memory", base, size);
}

mspace createHeap() {
    size_t available = 0;
    int32_t ret = sceKernelAvailableFlexibleMemorySize(&available);
    heapLog("[v3k] heap: flexible memory available %zu MiB (0x%x)\n", available / MB, ret);

    void* attr[2] = {nullptr, nullptr};
    if (pthread_mutexattr_init(reinterpret_cast<pthread_mutexattr_t*>(attr)) == 0) {
        heapLog("[v3k] heap: kernel pthread objects live around %p\n", attr[0]);
        pthread_mutexattr_destroy(reinterpret_cast<pthread_mutexattr_t*>(attr));
    }

    const size_t systemSizes[] = {256 * MB, 192 * MB, 128 * MB};
    for (size_t size : systemSizes) {
        if (mspace msp = mapAndCreate(true, size))
            return msp;
    }
    size_t size = 0;
    if (ret == 0 && available > RESERVE_FOR_SYSTEM + 32 * MB)
        size = (available - RESERVE_FOR_SYSTEM) & ~(PAGE - 1);
    for (; size >= 32 * MB; size = (size / 2) & ~(PAGE - 1)) {
        if (mspace msp = mapAndCreate(false, size))
            return msp;
    }
    const size_t directSizes[] = {256 * MB, 128 * MB};
    for (size_t direct : directSizes) {
        if (mspace msp = createDirectHeap(direct))
            return msp;
    }
    heapLog("[v3k] heap: could not create a heap, out of memory\n");
    return nullptr;
}

bool g_creatingHeap = false;

inline mspace getHeap() {

    if (g_heap == nullptr && !g_creatingHeap) {
        g_creatingHeap = true;
        g_heap = createHeap();
        g_creatingHeap = false;
    }
    return g_heap;
}

mspace g_overflow = nullptr;
uintptr_t g_overflowBase = 0;
size_t g_overflowSize = 0;
bool g_overflowTried = false;

mspace overflowHeap() {
    if (mspace msp = __atomic_load_n(&g_overflow, __ATOMIC_ACQUIRE))
        return msp;
    if (__atomic_exchange_n(&g_overflowTried, true, __ATOMIC_ACQ_REL))
        return __atomic_load_n(&g_overflow, __ATOMIC_ACQUIRE);
    const size_t sizes[] = {512 * MB, 384 * MB, 256 * MB, 128 * MB};
    for (size_t size : sizes) {
        off_t phys = 0;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 2 * MB, 0, &phys) != 0)
            continue;

        void* base = reinterpret_cast<void*>(0x1800000000ull);
        const int32_t ret = sceKernelMapDirectMemory(&base, size, PROT_CPU_RW, 0, phys, 2 * MB);
        if (ret != 0 || reinterpret_cast<uintptr_t>(base) < 0x400000000ull) {
            heapLog("[v3k] heap: overflow heap: mapping %zu MiB of direct memory failed (0x%x, %p)\n",
                    size / MB, ret, base);
            continue;
        }
        mspace msp = create_mspace_with_base(base, size, 1);
        if (!msp)
            continue;
        g_overflowBase = reinterpret_cast<uintptr_t>(base);
        g_overflowSize = size;
        __atomic_store_n(&g_overflow, msp, __ATOMIC_RELEASE);
        heapLog("[v3k] heap: main heap full: overflow heap of %zu MiB of direct memory at %p\n", size / MB,
                base);
        return msp;
    }
    heapLog("[v3k] heap: main heap full and no direct memory for an overflow heap\n");
    return nullptr;
}

bool ownsPointer(const void* p) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    return (address >= g_heapBase && address < g_heapBase + g_heapSize) ||
           (address >= g_overflowBase && address < g_overflowBase + g_overflowSize);
}

__attribute__((tls_model("initial-exec"))) thread_local const char* t_op = "?";
__attribute__((tls_model("initial-exec"))) thread_local const void* t_caller = nullptr;
int g_errors = 0;

constexpr size_t ARENA_SIZE = 8 * MB;
constexpr int MAX_ARENAS = 12;
__attribute__((tls_model("initial-exec"))) thread_local mspace t_arena = nullptr;
std::atomic<int> g_arenaCount{0};

mspace g_freeArenas[MAX_ARENAS];
int g_freeArenaCount = 0;
std::atomic_flag g_freeArenaLock = ATOMIC_FLAG_INIT;
pthread_key_t g_arenaKey;
std::atomic<bool> g_arenaKeyReady{false};

void lockFreeArenas() {
    while (g_freeArenaLock.test_and_set(std::memory_order_acquire))
        sched_yield();
}
void unlockFreeArenas() { g_freeArenaLock.clear(std::memory_order_release); }

void releaseArena(void* arena) {
    if (!arena)
        return;
    lockFreeArenas();
    if (g_freeArenaCount < MAX_ARENAS)
        g_freeArenas[g_freeArenaCount++] = static_cast<mspace>(arena);
    unlockFreeArenas();
}

unsigned long long g_ops = 0;
const char* g_prevOp = "none";
const void* g_prevCaller = nullptr;
bool g_heapBroken = false;

void checkHeap(const char* op, const void* caller) {
    g_ops++;
    if (g_heap && !g_heapBroken && !ps4_heap_ok(g_heap)) {
        g_heapBroken = true;
        heapLog("[v3k] heap: CONTROL BLOCK OVERWRITTEN, noticed at operation #%llu (%s from elf 0x%llx); "
                "previous operation %s from elf 0x%llx\n",
                g_ops, op, elf(caller), g_prevOp, elf(g_prevCaller));
        const auto* words = reinterpret_cast<const unsigned long long*>(g_heapBase);
        for (int i = 0; i < 16; i += 4)
            heapLog("[v3k] heap:   +0x%02x: %016llx %016llx %016llx %016llx\n", i * 8, words[i], words[i + 1],
                    words[i + 2], words[i + 3]);
        logStack();
    }
    g_prevOp = op;
    g_prevCaller = caller;
}

void reportFailure(const char* what, size_t size) {
    if (g_errors++ < 8) {
        heapLog("[v3k] heap: %s(%zu) failed, caller elf 0x%llx\n", what, size, elf(t_caller));
        logStack();
    }
}

struct ThreadDtor {
    void (*dtor)(void*);
    void* obj;
    ThreadDtor* next;
};
pthread_key_t g_threadDtorKey;
pthread_once_t g_threadDtorOnce = PTHREAD_ONCE_INIT;

void runThreadDtors(void* p) {
    auto* head = static_cast<ThreadDtor*>(p);
    while (head) {
        ThreadDtor* next = head->next;
        head->dtor(head->obj);
        free(head);
        head = next;
    }
}

void createThreadDtorKey() { pthread_key_create(&g_threadDtorKey, runThreadDtors); }

}

extern "C" {

void ps4_heap_error(void* msp, void* chunk, int corruption) {
    heapLog("[v3k] heap: %s during %s from elf 0x%llx (chunk %p)\n",
            corruption ? "CORRUPTION DETECTED" : "invalid pointer", t_op, elf(t_caller), chunk);
    logStack();
    if (corruption)
        abort();
}

void* __wrap_malloc(size_t size) {
    t_op = "malloc";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    void* p = t_arena ? mspace_malloc(t_arena, size) : nullptr;
    if (!p)
        p = mspace_malloc(getHeap(), size);
    if (!p)
        if (mspace overflow = overflowHeap())
            p = mspace_malloc(overflow, size);
    if (!p)
        reportFailure("malloc", size);
    return p;
}

void __wrap_free(void* ptr) {
    if (!ptr)
        return;
    t_op = "free";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    if (!ownsPointer(ptr)) {
        if (g_errors++ < 8) {
            heapLog("[v3k] heap: free(%p) of memory not from this heap, caller elf 0x%llx (ignored)\n", ptr,
                    elf(t_caller));
            logStack();
        }
        return;
    }
    mspace_free(getHeap(), ptr);
}

void* __wrap_calloc(size_t nelem, size_t size) {
    t_op = "calloc";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    void* p = t_arena ? mspace_calloc(t_arena, nelem, size) : nullptr;
    if (!p)
        p = mspace_calloc(getHeap(), nelem, size);
    if (!p)
        if (mspace overflow = overflowHeap())
            p = mspace_calloc(overflow, nelem, size);
    if (!p)
        reportFailure("calloc", nelem * size);
    return p;
}

void* __wrap_realloc(void* ptr, size_t size) {
    t_op = "realloc";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    if (ptr && !ownsPointer(ptr)) {
        heapLog("[v3k] heap: realloc(%p) of memory not from this heap, caller elf 0x%llx\n", ptr,
                elf(t_caller));
        logStack();
        return nullptr;
    }
    if (ptr && size == 0) {
        mspace_free(getHeap(), ptr);
        return nullptr;
    }

    void* p = ptr ? mspace_realloc(getHeap(), ptr, size) : nullptr;
    if (!p) {
        p = t_arena ? mspace_malloc(t_arena, size) : nullptr;
        if (!p)
            p = mspace_malloc(getHeap(), size);
        if (!p)
            if (mspace overflow = overflowHeap())
                p = mspace_malloc(overflow, size);
        if (p && ptr) {
            const size_t old_size = mspace_usable_size(ptr);
            memcpy(p, ptr, old_size < size ? old_size : size);
            mspace_free(getHeap(), ptr);
        }
    }
    if (!p)
        reportFailure("realloc", size);
    return p;
}

void* __wrap_memalign(size_t alignment, size_t size) {
    t_op = "memalign";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    void* p = t_arena ? mspace_memalign(t_arena, alignment, size) : nullptr;
    if (!p)
        p = mspace_memalign(getHeap(), alignment, size);
    if (!p)
        if (mspace overflow = overflowHeap())
            p = mspace_memalign(overflow, alignment, size);
    if (!p)
        reportFailure("memalign", size);
    return p;
}

void ps4_heap_private_arena(const char* name) {
    static const char* const busy[] = {"CPU thread", "CPU-GPU thread", "Video thread",
                                       "VK submission thread", "AsyncShaderCompiler Worker",
                                       "DVD thread", "Audio thread - PS4"};
    if (t_arena || !name || !getHeap())
        return;
    bool wanted = false;
    for (const char* b : busy)
        wanted |= strcmp(name, b) == 0;
    if (!wanted)
        return;
    if (!g_arenaKeyReady.load(std::memory_order_acquire)) {
        static std::atomic_flag s_creating = ATOMIC_FLAG_INIT;
        if (!s_creating.test_and_set()) {
            pthread_key_create(&g_arenaKey, releaseArena);
            g_arenaKeyReady.store(true, std::memory_order_release);
        }
        while (!g_arenaKeyReady.load(std::memory_order_acquire))
            sched_yield();
    }

    lockFreeArenas();
    if (g_freeArenaCount > 0)
        t_arena = g_freeArenas[--g_freeArenaCount];
    unlockFreeArenas();
    if (t_arena) {
        pthread_setspecific(g_arenaKey, t_arena);
        heapLog("[v3k] heap: reused arena for '%s'\n", name);
        return;
    }
    if (g_arenaCount.fetch_add(1) >= MAX_ARENAS)
        return;
    void* block = mspace_malloc(getHeap(), ARENA_SIZE);
    if (!block)
        return;
    t_arena = create_mspace_with_base(block, ARENA_SIZE, 1);
    if (t_arena)
        pthread_setspecific(g_arenaKey, t_arena);
    heapLog("[v3k] heap: private %zu MiB arena for '%s'%s\n", ARENA_SIZE / MB, name,
            t_arena ? "" : " FAILED");
}

void* __wrap___memalign(size_t alignment, size_t size) { return __wrap_memalign(alignment, size); }

int ps4_replace_malloc_init(void) {
    getHeap();
    return 0;
}
int ps4_replace_malloc_finalize(void) { return 0; }
void* ps4_replace_malloc(size_t size) { return __wrap_malloc(size); }
void ps4_replace_free(void* ptr) { __wrap_free(ptr); }
void* ps4_replace_calloc(size_t nelem, size_t size) { return __wrap_calloc(nelem, size); }
void* ps4_replace_realloc(void* ptr, size_t size) { return __wrap_realloc(ptr, size); }
void* ps4_replace_memalign(size_t boundary, size_t size) { return __wrap_memalign(boundary, size); }
void* ps4_replace_reallocalign(void* ptr, size_t size, size_t boundary) {
    if (!ptr)
        return __wrap_memalign(boundary, size);
    if (size == 0) {
        __wrap_free(ptr);
        return nullptr;
    }
    void* p = __wrap_memalign(boundary, size);
    if (!p)
        return nullptr;
    const size_t old_size = mspace_usable_size(ptr);
    memcpy(p, ptr, old_size < size ? old_size : size);
    __wrap_free(ptr);
    return p;
}
int ps4_replace_posix_memalign(void** out, size_t boundary, size_t size) {
    void* p = __wrap_memalign(boundary, size);
    if (!p)
        return 12;
    *out = p;
    return 0;
}

struct MallocManagedSize {
    uint16_t size;
    uint16_t version;
    uint32_t reserved;
    size_t max_system_size, current_system_size, max_inuse_size, current_inuse_size;
};
int ps4_replace_malloc_stats(void* out) {
    auto* stats = static_cast<MallocManagedSize*>(out);
    if (!stats)
        return 0x80020016;
    const size_t footprint = mspace_footprint(getHeap());
    stats->max_system_size = mspace_max_footprint(getHeap());
    stats->current_system_size = footprint;
    stats->max_inuse_size = footprint;
    stats->current_inuse_size = footprint;
    return 0;
}
int ps4_replace_malloc_stats_fast(void* out) { return ps4_replace_malloc_stats(out); }

size_t ps4_heap_footprint() {
    size_t bytes = mspace_footprint(getHeap());
    if (mspace overflow = __atomic_load_n(&g_overflow, __ATOMIC_ACQUIRE))
        bytes += mspace_footprint(overflow);
    return bytes;
}
size_t ps4_replace_malloc_usable_size(void* ptr) { return ptr ? mspace_usable_size(ptr) : 0; }

int ps4_jit_in_system_pool = 1;
unsigned long long ps4_fault_count = 0;

void* __real_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset);
void* __wrap_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    const bool anonymous = fd == -1 && (flags & MAP_ANON) && !(flags & MAP_FIXED);
    const bool allowed = !(prot & PROT_EXEC) || ps4_jit_in_system_pool;
    if (anonymous && allowed && len >= MB) {
        const size_t size = (len + PAGE - 1) & ~(PAGE - 1);
        void* base = reserveAwayFromKernel(size);
        if (base) {
            const int32_t ret = sceKernelMapNamedSystemFlexibleMemory(&base, size, prot & 0x7, MAP_FIXED_FLAG,
                                                                      "v3k mmap");
            if (ret == 0) {
                heapLog("[v3k] mmap: %zu KiB prot %d from system flexible memory at %p, caller elf 0x%llx\n",
                        size / 1024, prot, base, elf(__builtin_return_address(0)));
                return base;
            }
            heapLog("[v3k] mmap: %zu KiB prot %d not possible from system flexible memory (0x%x)\n",
                    size / 1024, prot, ret);
            sceKernelMunmap(base, size);
        }
    }
    void* result = __real_mmap(addr, len, prot, flags, fd, offset);
    if (anonymous && len >= MB) {
        size_t available = 0;
        sceKernelAvailableFlexibleMemorySize(&available);
        heapLog("[v3k] mmap: %zu KiB prot %d from regular flexible memory = %p, caller elf 0x%llx, "
                "%zu MiB flexible left\n",
                len / 1024, prot, result, elf(__builtin_return_address(0)), available / MB);
    }
    return result;
}

int __real_clock_gettime(clockid_t id, struct timespec* ts);
uint64_t sceKernelGetTscFrequency_ps4() __asm__("sceKernelGetTscFrequency");
unsigned long long ps4_clock_reads;

int __wrap_clock_gettime(clockid_t id, struct timespec* ts) {
    if (id != CLOCK_MONOTONIC)
        return __real_clock_gettime(id, ts);
    struct Anchor {
        uint64_t tsc = 0, ns = 0, mult = 0;
        bool ok = false;
    };
    static const Anchor anchor = [] {
        Anchor a;
        const uint64_t freq = static_cast<uint64_t>(sceKernelGetTscFrequency_ps4());
        struct timespec now;
        if (freq < 1000000 || __real_clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return a;
        a.tsc = __builtin_ia32_rdtsc();
        a.ns = static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
        a.mult = (1000000000ull << 32) / freq;
        a.ok = true;
        return a;
    }();
    if (!anchor.ok)
        return __real_clock_gettime(id, ts);
    ps4_clock_reads++;
    const uint64_t ticks = __builtin_ia32_rdtsc() - anchor.tsc;
    const uint64_t ns =
        anchor.ns + static_cast<uint64_t>((static_cast<unsigned __int128>(ticks) * anchor.mult) >> 32);
    ts->tv_sec = static_cast<time_t>(ns / 1000000000ull);
    ts->tv_nsec = static_cast<long>(ns % 1000000000ull);
    return 0;
}

int __wrap_pthread_once(pthread_once_t* once, void (*init)(void)) {
    auto* state = reinterpret_cast<int*>(once);
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 2)
        return 0;
    int expected = 0;
    if (__atomic_compare_exchange_n(state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init();
        __atomic_store_n(state, 2, __ATOMIC_RELEASE);
        return 0;
    }
    while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2)
        sched_yield();
    return 0;
}

extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);
extern "C" void* scePthreadSelf();
extern "C" void scePthreadSetaffinity();
extern "C" void scePthreadGetaffinity();
extern "C" void ps4_boot_trace(const char* stage);
extern "C" void ps4_watch_thread(const char* name);
extern "C" void ps4_install_signal_stack();

namespace {
constexpr uint64_t kAppCores = 0x7F;

constexpr size_t kMinThreadStack = 2 * 1024 * 1024;
using SetAffinityFn = int (*)(void*, uint64_t);
using GetAffinityFn = int (*)(void*, uint64_t*);

struct ThreadStart {
    void* (*start)(void*);
    void* arg;
};

}

extern "C" __attribute__((noinline, disable_tail_calls)) void* ps4_thread_entry(void* raw) {
    const ThreadStart start = *static_cast<ThreadStart*>(raw);
    free(raw);
    reinterpret_cast<SetAffinityFn>(&scePthreadSetaffinity)(scePthreadSelf(), kAppCores);
    ps4_install_signal_stack();

    ps4_watch_thread("(unnamed)");
    return start.start(start.arg);
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {

    pthread_attr_t local;
    bool ownAttr = false;
    size_t stack = 0;
    if (attr) {
        pthread_attr_getstacksize(attr, &stack);
        if (stack < kMinThreadStack)
            pthread_attr_setstacksize(const_cast<pthread_attr_t*>(attr), kMinThreadStack);
    } else if (pthread_attr_init(&local) == 0) {
        ownAttr = true;
        pthread_attr_getstacksize(&local, &stack);
        if (stack < kMinThreadStack)
            pthread_attr_setstacksize(&local, kMinThreadStack);
        attr = &local;
    }
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true)) {
        uint64_t mask = 0;
        reinterpret_cast<GetAffinityFn>(&scePthreadGetaffinity)(scePthreadSelf(), &mask);
        char line[160];
        snprintf(line, sizeof(line),
                 "threads: first creator's cores %#llx, new threads get %#llx; stack %zu KiB by default, at least %zu KiB",
                 static_cast<unsigned long long>(mask), static_cast<unsigned long long>(kAppCores), stack / 1024,
                 kMinThreadStack / 1024);
        ps4_boot_trace(line);
    }
    int result;
    auto* wrapped = static_cast<ThreadStart*>(malloc(sizeof(ThreadStart)));
    if (!wrapped) {
        result = __real_pthread_create(thread, attr, start, arg);
    } else {
        *wrapped = {start, arg};
        result = __real_pthread_create(thread, attr, ps4_thread_entry, wrapped);
        if (result != 0)
            free(wrapped);
    }
    if (ownAttr)
        pthread_attr_destroy(&local);
    return result;
}

int __cxa_thread_atexit_impl(void (*dtor)(void*), void* obj, void* ) {
    pthread_once(&g_threadDtorOnce, createThreadDtorKey);
    auto* entry = static_cast<ThreadDtor*>(malloc(sizeof(ThreadDtor)));
    if (!entry)
        return -1;

    entry->dtor = dtor;
    entry->obj = obj;
    entry->next = static_cast<ThreadDtor*>(pthread_getspecific(g_threadDtorKey));
    pthread_setspecific(g_threadDtorKey, entry);
    return 0;
}

const char* ps4rt_heap_source(void) {
    getHeap();
    return g_heapPool;
}
size_t ps4rt_heap_size(void) {
    getHeap();
    return g_heapSize;
}

}
