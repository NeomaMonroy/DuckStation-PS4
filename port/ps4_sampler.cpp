#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>

extern "C" char __text_start[];
extern "C" int pthread_getthreadid_np(void);
extern "C" void ps4_boot_trace(const char* stage);
extern "C" const char* ps4_data_dir();

namespace {

int createDataFile(const char* name) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", ps4_data_dir(), name);
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
}

constexpr int kSigProf = 27;
constexpr int kSaSiginfo = 0x40, kSaRestart = 0x0002;
constexpr int kMcontextOffset = 0x40;
constexpr int kRipIndex = 20, kRspIndex = 23;

constexpr uint64_t kSystemLibs = 0x800000000ULL, kCallerTag = 1ULL << 62;
constexpr uintptr_t kTextWindow = 0x4000000;
constexpr int kMaxThreads = 4;
constexpr int kMaxSamples = 16384;

struct ThreadSamples {
    std::atomic<pthread_t> thread{};
    const char* name = nullptr;
    uint64_t samples[kMaxSamples];
    std::atomic<int> count{0};
};

ThreadSamples g_threads[kMaxThreads];
std::atomic<int> g_thread_count{0};

void profHandler(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    for (int i = 0; i < g_thread_count.load(std::memory_order_acquire); i++) {
        ThreadSamples& t = g_threads[i];
        if (t.thread.load(std::memory_order_relaxed) != self)
            continue;
        const auto* regs =
            reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        const int n = t.count.load(std::memory_order_relaxed);
        if (n < kMaxSamples) {
            uint64_t sample = regs[kRipIndex];

            if (sample >= kSystemLibs && sample < 2 * kSystemLibs)
                sample |= kCallerTag;
            t.samples[n] = sample;
            t.count.store(n + 1, std::memory_order_release);
        }
        return;
    }
}

void writeLine(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        const ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

void noteHotJit(uint64_t address, int count);

void report(int fd, ThreadSamples& t, double window_start) {

    static uint64_t s[kMaxSamples];
    const int n = std::min(t.count.load(std::memory_order_acquire), kMaxSamples);
    memcpy(s, t.samples, sizeof(uint64_t) * n);
    t.count.store(0, std::memory_order_release);
    if (n == 0)
        return;
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);

    for (int i = 0; i < n; i++) {
        if (s[i] & kCallerTag) {
            s[i] &= ~0xFULL;
            continue;
        }
        const bool eboot = s[i] >= text && s[i] < text + kTextWindow;
        s[i] = eboot ? ((s[i] - text) & ~0xFULL) : (s[i] | (1ULL << 63)) & ~0xFULL;
    }
    std::sort(s, s + n);
    struct Entry { uint64_t key; int count; };
    static Entry entries[kMaxSamples];
    int unique = 0, eboot_samples = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && s[j] == s[i])
            j++;
        entries[unique++] = {s[i], j - i};
        if (!(s[i] >> 62))
            eboot_samples += j - i;
        i = j;
    }
    std::sort(entries, entries + unique, [](const Entry& a, const Entry& b) { return a.count > b.count; });

    char line[256];
    snprintf(line, sizeof(line), "== %.0f s, %s: %d samples, %d in eboot code (%.0f%%)\n",
             window_start, t.name, n, eboot_samples, 100.0 * eboot_samples / n);
    writeLine(fd, line);

    for (int i = 0; i < unique && i < 1500 && entries[i].count >= 2; i++) {
        if (entries[i].key >> 63) {
            const uint64_t address = entries[i].key & ~(1ULL << 63);
            snprintf(line, sizeof(line), "  %5.1f%% jit 0x%llx\n", 100.0 * entries[i].count / n,
                     static_cast<unsigned long long>(address));
            noteHotJit(address, entries[i].count);
        }
        else if (entries[i].key & kCallerTag)
            snprintf(line, sizeof(line), "  %5.1f%% sys 0x%llx\n",
                     100.0 * entries[i].count / n,
                     static_cast<unsigned long long>(entries[i].key & ~kCallerTag));
        else
            snprintf(line, sizeof(line), "  %5.1f%% elf 0x%llx\n", 100.0 * entries[i].count / n,
                     static_cast<unsigned long long>(entries[i].key));
        writeLine(fd, line);
    }
}

constexpr int kMaxHotChunks = 96;
uint64_t g_hot_chunks[kMaxHotChunks];
int g_hot_chunk_count = 0;

void noteHotJit(uint64_t address, int count) {
    if (count < 3)
        return;
    const uint64_t chunk = address & ~0xFFFFULL;
    for (int i = 0; i < g_hot_chunk_count; i++) {
        if (g_hot_chunks[i] == chunk)
            return;
    }
    if (g_hot_chunk_count < kMaxHotChunks)
        g_hot_chunks[g_hot_chunk_count++] = chunk;
}

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

struct ModuleSegment {
    void* address;
    uint32_t size;
    int32_t prot;
};
struct ModuleInfo {
    size_t size;
    char name[256];
    ModuleSegment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
};
extern "C" int32_t sceKernelGetModuleList(int32_t* handles, size_t count, size_t* available);
extern "C" int32_t sceKernelGetModuleInfo(int32_t handle, ModuleInfo* info);

void writeModuleList(int fd) {
    int32_t handles[256];
    size_t count = 0;
    static size_t s_listed = 0;
    if (sceKernelGetModuleList(handles, 256, &count) != 0 || count == s_listed)
        return;
    s_listed = count;
    char line[512];
    for (size_t m = 0; m < count && m < 256; m++) {
        ModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(handles[m], &info) != 0)
            continue;
        int len = snprintf(line, sizeof(line), "module %s", info.name);
        for (uint32_t g = 0; g < info.segment_count && g < 4; g++)
            len += snprintf(line + len, sizeof(line) - len, " seg %p+0x%x/%d",
                            info.segments[g].address, info.segments[g].size, info.segments[g].prot);
        snprintf(line + len, sizeof(line) - len, "\n");
        writeLine(fd, line);
    }
}

}

extern "C" int32_t sceKernelQueryMemoryProtection(void* address, void** start, void** end,
                                                  uint32_t* protection);

extern "C" void* ps4_profile_guest_ram;
extern "C" uint32_t ps4_profile_guest_ram_size;
extern "C" {
void* ps4_profile_guest_ram = nullptr;
uint32_t ps4_profile_guest_ram_size = 0;
}

namespace {

bool isReadable(uint64_t address, uint64_t size) {
    void* start = nullptr;
    void* end = nullptr;
    uint32_t protection = 0;
    return sceKernelQueryMemoryProtection(reinterpret_cast<void*>(address), &start, &end,
                                          &protection) == 0 &&
           (protection & 1) && address + size <= reinterpret_cast<uintptr_t>(end);
}

void dumpJitCode() {
    const int fd = createDataFile("jitcode.bin");
    if (fd >= 0) {
        static uint8_t buffer[0x10000];
        for (int i = 0; i < g_hot_chunk_count; i++) {
            if (!isReadable(g_hot_chunks[i], sizeof(buffer)))
                continue;
            memcpy(buffer, reinterpret_cast<const void*>(g_hot_chunks[i]), sizeof(buffer));
            const uint64_t header[2] = {g_hot_chunks[i], sizeof(buffer)};
            write(fd, header, sizeof(header));
            write(fd, buffer, sizeof(buffer));
        }
        close(fd);
    }
    g_hot_chunk_count = 0;
    if (ps4_profile_guest_ram && ps4_profile_guest_ram_size) {
        const int ram = createDataFile("mem1.bin");
        if (ram >= 0) {
            static uint8_t copy[0x100000];
            for (uint32_t offset = 0; offset < ps4_profile_guest_ram_size; offset += sizeof(copy)) {
                const uint32_t size =
                    std::min<uint32_t>(sizeof(copy), ps4_profile_guest_ram_size - offset);
                memcpy(copy, static_cast<const uint8_t*>(ps4_profile_guest_ram) + offset, size);
                write(ram, copy, size);
            }
            close(ram);
        }
    }
}

std::atomic<bool> g_sampler_on{false};

void* samplerThread(void*) {
    const int fd = createDataFile("samples.log");
    if (fd < 0)
        return nullptr;
    writeModuleList(fd);
    const double start = now();
    double window_start = start, last_dump = start;
    const timespec period = {0, 1000000}, paused = {0, 100000000};
    for (;;) {

        if (!g_sampler_on.load(std::memory_order_relaxed)) {
            nanosleep(&paused, nullptr);
            window_start = now();
            continue;
        }
        nanosleep(&period, nullptr);
        const int threads = g_thread_count.load(std::memory_order_acquire);
        for (int i = 0; i < threads; i++)
            pthread_kill(g_threads[i].thread.load(std::memory_order_relaxed), kSigProf);
        if (now() - window_start >= 10.0) {
            writeModuleList(fd);
            for (int i = 0; i < threads; i++)
                report(fd, g_threads[i], window_start - start);
            window_start = now();
            if (window_start - last_dump >= 60.0) {
                dumpJitCode();
                last_dump = window_start = now();
                writeLine(fd, "(jitcode.bin and mem1.bin updated)\n");
            }
        }
    }
    return nullptr;
}

}

namespace {
constexpr int kSigDump = 30;
constexpr int kMaxWatched = 96, kMaxFrames = 48;

constexpr uintptr_t kForeignStackScan = 16 * 1024;

struct WatchedThread {
    std::atomic<pthread_t> thread{};
    char name[32] = {};

    uintptr_t stack_limit = 0;
    int tid = 0;
    uint64_t rip = 0;
    uint64_t frames[kMaxFrames];
    int frame_count = 0;

    bool in_umtx = false;
    uint64_t umtx_obj = 0, umtx_op = 0, umtx_val = 0, umtx_obj2 = 0;
    uint32_t umtx_words[4] = {}, umtx_words2[4] = {};
    std::atomic<int> done{0};
};
WatchedThread g_watched[kMaxWatched];
std::atomic<int> g_watched_count{0};

void dumpHandler(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
    for (int i = 0; i < g_watched_count.load(std::memory_order_acquire); i++) {
        WatchedThread& t = g_watched[i];
        if (t.thread.load(std::memory_order_relaxed) != self)
            continue;
        const auto* regs =
            reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        t.rip = regs[kRipIndex];

        t.in_umtx = false;
        if (t.rip >= kSystemLibs && t.rip < 2 * kSystemLibs) {
            const auto* code = reinterpret_cast<const uint8_t*>(t.rip - 12);
            static const uint8_t kUmtxStub[12] = {0x48, 0xc7, 0xc0, 0xc6, 0x01, 0x00,
                                                  0x00, 0x49, 0x89, 0xca, 0x0f, 0x05};
            if (memcmp(code, kUmtxStub, sizeof(kUmtxStub)) == 0) {

                t.in_umtx = true;
                t.umtx_obj = regs[1];
                t.umtx_op = regs[2];
                t.umtx_val = regs[3];
                t.umtx_obj2 = regs[10];
            }
        }

        const uint64_t rsp = regs[kRspIndex];
        const bool same_thread = pthread_getthreadid_np() == t.tid;
        const bool sane = t.stack_limit > rsp && t.stack_limit - rsp <= (1u << 20);
        const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
        uintptr_t limit_address = rsp;
        if (same_thread && sane) {
            limit_address = t.stack_limit;
        } else if (same_thread) {
            void* start = nullptr;
            void* end = nullptr;
            uint32_t protection = 0;
            if (sceKernelQueryMemoryProtection(reinterpret_cast<void*>(rsp), &start, &end,
                                               &protection) == 0 && (protection & 1))
                limit_address = std::min<uintptr_t>(rsp + kForeignStackScan,
                                                    reinterpret_cast<uintptr_t>(end));
        }
        const auto* limit = reinterpret_cast<const uint64_t*>(limit_address);
        int n = 0;
        for (const uint64_t* p = stack; p < limit && n < kMaxFrames; p++) {
            if (*p >= text && *p < text + kTextWindow)
                t.frames[n++] = *p - text;
        }
        t.frame_count = n;
        t.done.store(1, std::memory_order_release);
        return;
    }
}

void installDumpHandler() {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true))
        return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = dumpHandler;
    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = kSaSiginfo | kSaRestart;
    sigemptyset(&action.sa_mask);
    sigaction(kSigDump, &action, nullptr);
}
}

extern "C" void ps4_heap_private_arena(const char* name);

extern "C" void ps4_watch_thread(const char* name) {
    installDumpHandler();
    ps4_heap_private_arena(name);
    const pthread_t self = pthread_self();
    for (int i = 0; i < std::min(g_watched_count.load(), kMaxWatched); i++) {
        if (g_watched[i].thread.load() == self) {

            if (name)
                snprintf(g_watched[i].name, sizeof(g_watched[i].name), "%s", name);
            return;
        }
    }
    const int index = g_watched_count.fetch_add(1);
    if (index >= kMaxWatched)
        return;
    snprintf(g_watched[index].name, sizeof(g_watched[index].name), "%s", name ? name : "?");

    g_watched[index].stack_limit = reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) + 16;
    g_watched[index].tid = pthread_getthreadid_np();
    g_watched[index].thread.store(self);
}

extern "C" int32_t sceKernelQueryMemoryProtection(void* address, void** start, void** end,
                                                  uint32_t* protection);

static void readIfMapped(uint64_t address, uint32_t (&words)[4]) {
    memset(words, 0, sizeof(words));
    void* start = nullptr;
    void* end = nullptr;
    uint32_t protection = 0;
    if (address < 0x10000 ||
        sceKernelQueryMemoryProtection(reinterpret_cast<void*>(address), &start, &end,
                                       &protection) != 0 ||
        !(protection & 1) || address + 16 > reinterpret_cast<uintptr_t>(end))
        return;
    memcpy(words, reinterpret_cast<const void*>(address), sizeof(words));
}

extern "C" void ps4_dump_thread_stacks(const char* path, const char* reason) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;
    char line[256];
    snprintf(line, sizeof(line), "==== %s (eboot addresses relative to .text, symbolize with "
                                 "llvm-symbolizer --obj=<oelf>)\n", reason);
    writeLine(fd, line);
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
    const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
    for (int i = 0; i < count; i++) {
        WatchedThread& t = g_watched[i];
        t.done.store(0);
        if (pthread_kill(t.thread.load(), kSigDump) != 0) {
            snprintf(line, sizeof(line), "-- %s: signal failed (thread gone?)\n", t.name);
            writeLine(fd, line);
            continue;
        }
        const timespec pause = {0, 1000000};
        for (int w = 0; w < 200 && !t.done.load(std::memory_order_acquire); w++)
            nanosleep(&pause, nullptr);
        if (!t.done.load(std::memory_order_acquire)) {
            snprintf(line, sizeof(line), "-- %s: no answer (signals blocked?)\n", t.name);
            writeLine(fd, line);
            continue;
        }
        const bool in_eboot = t.rip >= text && t.rip < text + kTextWindow;
        snprintf(line, sizeof(line), "-- %s (thread id %d): rip %s0x%llx\n", t.name, t.tid,
                 in_eboot ? "elf " : "(system/JIT) ",
                 static_cast<unsigned long long>(in_eboot ? t.rip - text : t.rip));
        writeLine(fd, line);
        if (t.in_umtx) {
            readIfMapped(t.umtx_obj, t.umtx_words);
            readIfMapped(t.umtx_obj2, t.umtx_words2);
            snprintf(line, sizeof(line),
                     "   blocked in _umtx_op(obj %#llx, op %llu, val %#llx, obj2 %#llx); obj: %08x "
                     "%08x %08x %08x; obj2: %08x %08x %08x %08x\n",
                     static_cast<unsigned long long>(t.umtx_obj),
                     static_cast<unsigned long long>(t.umtx_op),
                     static_cast<unsigned long long>(t.umtx_val),
                     static_cast<unsigned long long>(t.umtx_obj2), t.umtx_words[0], t.umtx_words[1],
                     t.umtx_words[2], t.umtx_words[3], t.umtx_words2[0], t.umtx_words2[1],
                     t.umtx_words2[2], t.umtx_words2[3]);
            writeLine(fd, line);
        }
        writeLine(fd, "   stack:");
        for (int f = 0; f < t.frame_count; f++) {
            snprintf(line, sizeof(line), " 0x%llx", static_cast<unsigned long long>(t.frames[f]));
            writeLine(fd, line);
        }
        writeLine(fd, "\n");
    }
    close(fd);
}

namespace {
constexpr clockid_t kThreadCpuClock = 14;
std::atomic<uint64_t> g_thread_cpu_ns[2];
}

extern "C" void ps4_thread_cpu_sample(int slot) {
    timespec ts;
    if (slot < 0 || slot > 1 || clock_gettime(kThreadCpuClock, &ts) != 0)
        return;
    g_thread_cpu_ns[slot].store(static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                                    static_cast<uint64_t>(ts.tv_nsec),
                                std::memory_order_relaxed);
}

namespace {
std::atomic<long long> g_wait_ns[6];
}

extern "C" void ps4_wait_add(int slot, long long ns) {
    if (slot >= 0 && slot < 6 && ns > 0)
        g_wait_ns[slot].fetch_add(ns, std::memory_order_relaxed);
}

extern "C" long long ps4_wait_total(int slot) {
    return slot >= 0 && slot < 6 ? g_wait_ns[slot].load(std::memory_order_relaxed) : 0;
}

extern "C" double ps4_thread_cpu_seconds(int slot) {
    if (slot < 0 || slot > 1)
        return 0;
    return g_thread_cpu_ns[slot].load(std::memory_order_relaxed) / 1e9;
}

namespace {
std::atomic<uint64_t> g_watched_cpu_ns[kMaxWatched];
}

extern "C" void ps4_thread_cpu_tick() {
    static thread_local int t_slot = -2;
    static thread_local uint64_t t_last = 0;
    const uint64_t now = __builtin_ia32_rdtsc();
    if (now - t_last < 16000000)
        return;
    t_last = now;
    if (t_slot == -2) {
        t_slot = -1;
        const pthread_t self = pthread_self();
        for (int i = 0; i < std::min(g_watched_count.load(), kMaxWatched); i++) {
            if (g_watched[i].thread.load() == self)
                t_slot = i;
        }
    }
    timespec ts;
    if (t_slot >= 0 && clock_gettime(kThreadCpuClock, &ts) == 0) {
        g_watched_cpu_ns[t_slot].store(static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec,
                                       std::memory_order_relaxed);
    }
}

extern "C" void ps4_thread_busy_report(char* out, size_t size, int min_percent) {
    static uint64_t s_prev_cpu[kMaxWatched];
    static uint64_t s_prev_wall = 0;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t wall = static_cast<uint64_t>(now.tv_sec) * 1000000000ull + now.tv_nsec;
    const uint64_t elapsed = s_prev_wall ? wall - s_prev_wall : 0;
    s_prev_wall = wall;
    int percents[kMaxWatched];
    const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
    for (int i = 0; i < count; i++) {
        percents[i] = -1;
        uint64_t ns = g_watched_cpu_ns[i].load(std::memory_order_relaxed);
        if (ns == 0) {
            clockid_t clock;
            timespec cpu;
            if (pthread_getcpuclockid(g_watched[i].thread.load(), &clock) != 0 ||
                clock_gettime(clock, &cpu) != 0)
                continue;
            ns = static_cast<uint64_t>(cpu.tv_sec) * 1000000000ull + cpu.tv_nsec;
        }
        if (elapsed && s_prev_cpu[i] && ns >= s_prev_cpu[i])
            percents[i] = static_cast<int>((ns - s_prev_cpu[i]) * 100 / elapsed);
        s_prev_cpu[i] = ns;
    }
    size_t length = 0;
    out[0] = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < count; i++)
            if (percents[i] >= min_percent && (best < 0 || percents[i] > percents[best]))
                best = i;
        if (best < 0 || length + 40 >= size)
            break;
        length += snprintf(out + length, size - length, "%s%s %d%%", length ? ", " : "",
                           g_watched[best].name, percents[best]);
        percents[best] = -1;
    }
}

extern "C" void ps4_sampler_register_thread(const char* name) {
    ps4_watch_thread(name);

    for (int i = 0; i < g_thread_count.load(); i++) {
        if (g_threads[i].name && name && strcmp(g_threads[i].name, name) == 0) {
            g_threads[i].thread.store(pthread_self());
            return;
        }
    }
    const int index = g_thread_count.load();
    if (index >= kMaxThreads)
        return;
    g_threads[index].name = name;
    g_threads[index].thread.store(pthread_self());
    g_thread_count.store(index + 1, std::memory_order_release);
}

extern "C" void ps4_sampler_start() {
    static std::atomic<bool> s_started{false};
    g_sampler_on.store(true);
    if (s_started.exchange(true))
        return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = profHandler;
    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = kSaSiginfo | kSaRestart;
    sigemptyset(&action.sa_mask);
    sigaction(kSigProf, &action, nullptr);

    pthread_t thread;
    if (pthread_create(&thread, nullptr, samplerThread, nullptr) == 0)
        ps4_boot_trace("sampler: started, writing samples.log every 10 s");
}

extern "C" void ps4_sampler_set_enabled(int on) {
    if (on)
        ps4_sampler_start();
    else if (g_sampler_on.exchange(false))
        ps4_boot_trace("sampler: paused");
}

namespace {
std::atomic<bool> g_hot_on{false};
std::atomic<bool> g_hot_started{false};
constexpr int kHotMax = 1024;

struct HotThread {
    bool running = true;
    int looks = 0, busy = 0;
    uint64_t samples[kHotMax];
    uint64_t callers[kHotMax];
    int count = 0;
};
HotThread g_hot[kMaxWatched];

bool inSyscall(uint64_t rip) {
    if (rip < kSystemLibs || rip >= 2 * kSystemLibs)
        return false;
    const auto* code = reinterpret_cast<const uint8_t*>(rip - 2);
    return code[0] == 0x0f && code[1] == 0x05;
}

void hotReport(int fd, double at) {
    char line[256];
    snprintf(line, sizeof(line), "======== %.0f s\n", at);
    writeLine(fd, line);
    const int count = std::min(g_watched_count.load(), kMaxWatched);
    static uint64_t keys[kHotMax];
    struct Entry { uint64_t key; int n; };
    static Entry entries[kHotMax];
    for (int i = 0; i < count; i++) {
        HotThread& h = g_hot[i];
        if (h.looks == 0)
            continue;
        snprintf(line, sizeof(line), "-- %s: running in %d of %d looks, %d samples\n", g_watched[i].name,
                 h.busy, h.looks, h.count);
        writeLine(fd, line);

        for (int pass = 0; pass < 2 && h.count > 0; pass++) {
            int n = 0;
            for (int s = 0; s < h.count; s++) {
                if (pass == 0)
                    keys[n++] = h.samples[s];
                else if (h.samples[s] >> 63)
                    keys[n++] = h.callers[s];
            }
            if (n == 0)
                continue;
            std::sort(keys, keys + n);
            int unique = 0;
            for (int s = 0; s < n;) {
                int e = s;
                while (e < n && keys[e] == keys[s])
                    e++;
                entries[unique++] = {keys[s], e - s};
                s = e;
            }
            std::sort(entries, entries + unique, [](const Entry& a, const Entry& b) { return a.n > b.n; });
            writeLine(fd, pass == 0 ? "   where:\n" : "   system-library time, called from:\n");
            for (int e = 0; e < unique && e < 40; e++) {
                const uint64_t key = entries[e].key;
                if (pass == 0 && (key >> 63))
                    snprintf(line, sizeof(line), "   %5.1f%% sys 0x%llx\n", 100.0 * entries[e].n / h.count,
                             static_cast<unsigned long long>(key & ~(1ULL << 63)));
                else
                    snprintf(line, sizeof(line), "   %5.1f%% elf 0x%llx\n", 100.0 * entries[e].n / (pass == 0 ? h.count : n),
                             static_cast<unsigned long long>(key));
                writeLine(fd, line);
            }
        }
        h.looks = h.busy = h.count = 0;
    }
}

void* hotThread(void*) {
    ps4_watch_thread("hot profiler");
    const int fd = createDataFile("profile.log");
    if (fd < 0)
        return nullptr;
    writeLine(fd, "DuckStation hot-spot profile: every 10 s, the busiest code of each thread that ran.\n");
    writeModuleList(fd);
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
    const timespec period = {0, 20000000}, pause = {0, 200000}, idle = {0, 200000000};
    const double start = now();
    double window = start;
    for (unsigned round = 0;; round++) {
        if (!g_hot_on.load(std::memory_order_relaxed)) {
            nanosleep(&idle, nullptr);
            window = now();
            continue;
        }
        nanosleep(&period, nullptr);
        const pthread_t self = pthread_self();
        const int count = std::min(g_watched_count.load(std::memory_order_acquire), kMaxWatched);
        for (int i = 0; i < count; i++) {
            WatchedThread& t = g_watched[i];
            HotThread& h = g_hot[i];
            const pthread_t thread = t.thread.load();
            if (thread == self || (!h.running && round % 50 != static_cast<unsigned>(i) % 50))
                continue;
            t.done.store(0);
            if (pthread_kill(thread, kSigDump) != 0)
                continue;
            for (int w = 0; w < 25 && !t.done.load(std::memory_order_acquire); w++)
                nanosleep(&pause, nullptr);
            if (!t.done.load(std::memory_order_acquire))
                continue;
            const uint64_t rip = t.rip;
            h.looks++;
            h.running = !t.in_umtx && !inSyscall(rip);
            if (!h.running || h.count >= kHotMax)
                continue;
            h.busy++;
            const bool eboot = rip >= text && rip < text + kTextWindow;
            if (eboot) {
                h.samples[h.count] = (rip - text) & ~0xFULL;
                h.callers[h.count] = 0;
            } else {
                h.samples[h.count] = (rip & ~0xFULL) | (1ULL << 63);
                h.callers[h.count] = t.frame_count > 0 ? (t.frames[0] & ~0xFULL) : 0;
            }
            h.count++;
        }
        if (now() - window >= 10.0) {
            hotReport(fd, window - start);
            window = now();
        }
    }
    return nullptr;
}
}

extern "C" void ps4_hot_profiler_set_enabled(int on) {
    g_hot_on.store(on != 0);
    if (on && !g_hot_started.exchange(true)) {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, hotThread, nullptr) == 0)
            ps4_boot_trace("hot-spot profiler: started, writing profile.log every 10 s");
    }
}

extern "C" int ps4_hot_profiler_enabled() {
    return g_hot_on.load() ? 1 : 0;
}
