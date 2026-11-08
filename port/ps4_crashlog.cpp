#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cxxabi.h>
#include <exception>
#include <typeinfo>

extern "C" char __text_start[];
extern "C" __attribute__((weak)) void ps4_crash_guest_info();

struct BsdSignalStack {
    void* sp;
    size_t size;
    int flags;
};
extern "C" int ps4_sigaltstack(const BsdSignalStack* stack, BsdSignalStack* old) __asm__("sigaltstack");

extern "C" void ps4_install_signal_stack() {
    constexpr size_t kSignalStackSize = 64 * 1024;
    void* stack = malloc(kSignalStackSize);
    if (!stack)
        return;
    const BsdSignalStack signalStack{stack, kSignalStackSize, 0};
    if (ps4_sigaltstack(&signalStack, nullptr) != 0)
        free(stack);
}
extern "C" void ps4_dump_thread_stacks(const char* path, const char* reason);

extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

namespace {
constexpr const char* kDataDir = "/data/DuckStation";
std::atomic<const char*> g_dataDir{nullptr};
int g_dataMoved = 0;
char g_crashLogPath[64];
char g_bootTracePath[64];
char g_bootTracePrevPath[64];
}

extern "C" const char* ps4_data_dir() {
    if (const char* dir = g_dataDir.load(std::memory_order_acquire))
        return dir;
    const char* dir = kDataDir;
    mkdir(dir, 0777);
    snprintf(g_crashLogPath, sizeof(g_crashLogPath), "%s/crash.log", dir);
    snprintf(g_bootTracePath, sizeof(g_bootTracePath), "%s/boot-trace.log", dir);
    snprintf(g_bootTracePrevPath, sizeof(g_bootTracePrevPath), "%s/boot-trace.prev.log", dir);
    g_dataDir.store(dir, std::memory_order_release);
    return dir;
}

extern "C" int ps4_data_dir_moved() {
    ps4_data_dir();
    return g_dataMoved;
}

extern "C" const char* ps4_crash_log_path() {
    ps4_data_dir();
    return g_crashLogPath;
}

namespace {

constexpr int kSigIll = 4, kSigAbrt = 6, kSigFpe = 8, kSigBus = 10, kSigSegv = 11;
constexpr int kSaSiginfo = 0x40, kSaOnstack = 0x1;
constexpr int kMcontextOffset = 0x40;

constexpr const char* kRegNames[] = {"rdi", "rsi", "rdx", "rcx", "r8",  "r9",  "rax", "rbx",
                                     "rbp", "r10", "r11", "r12", "r13", "r14", "r15"};
constexpr int kRipIndex = 20, kRspIndex = 23;
constexpr uintptr_t kTextWindow = 0x40000000;

int g_traceFd = -1;
int g_probePipe[2] = {-1, -1};

bool readable(const void* address) {
    if (g_probePipe[0] < 0)
        return false;
    if (write(g_probePipe[1], address, 8) != 8)
        return false;
    char drain[8];
    read(g_probePipe[0], drain, sizeof(drain));
    return true;
}

int traceFd() {
    if (g_traceFd < 0) {
        ps4_data_dir();
        rename(g_bootTracePath, g_bootTracePrevPath);
        g_traceFd = open(g_bootTracePath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    return g_traceFd;
}

void writeAll(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

__attribute__((format(printf, 2, 3))) void writeFormat(int fd, const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    writeAll(fd, line);
}

bool isCode(uint64_t address) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(__text_start);
    return address >= base && address < base + kTextWindow;
}

uint64_t toElf(uint64_t address) { return address - reinterpret_cast<uintptr_t>(__text_start); }

void setHandler(int sig, void (*handler)(int, siginfo_t*, void*), int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));

    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, nullptr);
}

void crashHandler(int sig, siginfo_t* info, void* context) {
    static volatile int s_depth = 0;

    if (s_depth++ > 0)
        _exit(128 + sig);

    const int fd = open(ps4_crash_log_path(), O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        const auto* regs = reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        const uint64_t rip = regs[kRipIndex];
        const uint64_t rsp = regs[kRspIndex];
        const uint64_t address = *reinterpret_cast<const uint64_t*>(reinterpret_cast<char*>(info) + 24);
        writeFormat(fd, "\n=== signal %d (code %d), fault address 0x%llx\n", sig,
                    *reinterpret_cast<const int*>(reinterpret_cast<char*>(info) + 8),
                    static_cast<unsigned long long>(address));
        writeFormat(fd, "rip 0x%llx%s elf 0x%llx, rsp 0x%llx, text at %p\n",
                    static_cast<unsigned long long>(rip), isCode(rip) ? "" : " (not eboot code)",
                    static_cast<unsigned long long>(toElf(rip)), static_cast<unsigned long long>(rsp),
                    static_cast<void*>(__text_start));
        for (int i = 0; i < 15; i++)
            writeFormat(fd, "%s 0x%llx%s", kRegNames[i], static_cast<unsigned long long>(regs[1 + i]),
                        i % 4 == 3 ? "\n" : "  ");
        if (address < rsp && rsp - address < 0x10000)
            writeAll(fd, "\nthe fault is just below the stack pointer: this thread ran out of stack\n");
        writeAll(fd, "\ncode addresses on the stack (elf):\n");
        const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
        int found = 0;
        for (int i = 0; i < 2048 && found < 48; i++) {

            if ((i == 0 || ((reinterpret_cast<uintptr_t>(&stack[i]) & 0xfff) == 0)) && !readable(&stack[i]))
                break;
            if (isCode(stack[i])) {
                writeFormat(fd, "  [rsp+0x%x] 0x%llx\n", i * 8, static_cast<unsigned long long>(toElf(stack[i])));
                found++;
            }
        }
        close(fd);
    }
    if (traceFd() >= 0)
        writeAll(traceFd(), "crashed (see crash.log)\n");

    pthread_t timer;
    __real_pthread_create(&timer, nullptr, [](void*) -> void* {
        sleep(15);
        _exit(126);
    }, nullptr);

    ps4_dump_thread_stacks(ps4_crash_log_path(), "every thread at the crash");

    if (ps4_crash_guest_info) {
        ps4_crash_guest_info();
        if (traceFd() >= 0)
            writeAll(traceFd(), "guest state written to crash.log\n");
    }

    _exit(128 + sig);
}

[[noreturn]] void terminateHandler() {
    char message[512] = "std::terminate called";
    if (const std::type_info* type = abi::__cxa_current_exception_type()) {
        int status = 0;
        char* name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
        const char* what = "";
        try {
            if (std::exception_ptr current = std::current_exception())
                std::rethrow_exception(current);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        snprintf(message, sizeof(message), "uncaught exception %s: %s", name ? name : type->name(), what);
    }
    const int fd = open(ps4_crash_log_path(), O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        writeFormat(fd, "\n=== %s\n", message);
        close(fd);
    }
    if (traceFd() >= 0)
        writeFormat(traceFd(), "%s\n", message);
    abort();
}

__attribute__((constructor(101))) void earlyInit() {
    if (traceFd() >= 0)
        writeFormat(traceFd(), "static init (text at %p)\n", static_cast<void*>(__text_start));
    std::set_terminate(terminateHandler);
    if (pipe(g_probePipe) == 0) {
        fcntl(g_probePipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_probePipe[1], F_SETFL, O_NONBLOCK);
    }
    ps4_install_signal_stack();
    const int signals[] = {kSigIll, kSigAbrt, kSigFpe, kSigBus, kSigSegv};
    for (int sig : signals)
        setHandler(sig, crashHandler, kSaSiginfo | kSaOnstack);
}

}

extern "C" void ps4_boot_trace(const char* stage);
extern "C" __attribute__((noinline)) void ps4_trace_callers(const char* what) {

    const uint64_t* stack;
    asm volatile("mov %%rsp, %0" : "=r"(stack));
    char line[320];
    int len = snprintf(line, sizeof(line), "%s: callers", what);
    int found = 0;
    for (int i = 0; i < 512 && found < 10 && len < 280; i++) {
        if (isCode(stack[i])) {
            len += snprintf(line + len, sizeof(line) - len, " 0x%llx",
                            static_cast<unsigned long long>(toElf(stack[i])));
            found++;
        }
    }
    ps4_boot_trace(line);
}

extern "C" void ps4_boot_trace(const char* stage) {
    const int fd = traceFd();
    if (fd < 0)
        return;
    static timespec s_start;
    static bool s_started = false;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!s_started) {
        s_start = now;
        s_started = true;
    }
    const double seconds = static_cast<double>(now.tv_sec - s_start.tv_sec) +
                           static_cast<double>(now.tv_nsec - s_start.tv_nsec) / 1e9;

    char line[2048];
    const size_t length = strlen(stage);
    const bool newline = length == 0 || stage[length - 1] != '\n';
    const int written =
        snprintf(line, sizeof(line), "[%8.3f] %s%s", seconds, stage, newline ? "\n" : "");
    if (written >= static_cast<int>(sizeof(line)))
        line[sizeof(line) - 2] = '\n';
    writeAll(fd, line);
}

extern "C" int __real_open(const char* path, int flags, ...);
extern "C" int __real_close(int fd);
extern "C" int ps4_netfs_is_path(const char* path);
extern "C" int ps4_netfs_open(const char* path, int flags);
extern "C" int ps4_netfs_close(int fd);
extern "C" void ps4_fileat_closed(int fd);
extern "C" void ps4_fileat_opened(int fd, const char* path);

extern "C" __attribute__((noinline)) int __wrap_open(const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = va_arg(args, int);
    va_end(args);

    char absolute[1024];
    if (path != nullptr && path[0] != '/' && path[0] != '\0' && getcwd(absolute, sizeof(absolute)) != nullptr) {
        size_t length = strlen(absolute);
        const char* rest = (path[0] == '.' && path[1] == '/') ? path + 2 : path;
        if (length > 0 && absolute[length - 1] != '/' && length + 1 < sizeof(absolute))
            absolute[length++] = '/';
        if (length + strlen(rest) < sizeof(absolute)) {
            strcpy(absolute + length, rest);
            path = absolute;
        }
    }
    const int fd = ps4_netfs_is_path(path) ? ps4_netfs_open(path, flags) : __real_open(path, flags, mode);

    if (fd >= 0 && (flags & O_DIRECTORY))
        ps4_fileat_opened(fd, path);
    return fd;
}

extern "C" int __wrap_close(int fd) {

    ps4_fileat_closed(fd);
    if (ps4_netfs_close(fd))
        return 0;
    return __real_close(fd);
}
