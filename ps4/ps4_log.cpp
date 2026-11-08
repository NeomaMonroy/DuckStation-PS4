#include "ps4_log.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <unistd.h>

extern "C" const char *ps4_data_dir();

namespace {
std::mutex log_mutex;
const auto start = std::chrono::steady_clock::now();
}

void ps4_log_init() {
    const std::string dir = ps4_data_dir();
    const std::string path = dir + "/ps4.log";
    std::rename(path.c_str(), (dir + "/ps4.prev.log").c_str());

    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
}

void ps4_log_line(const char *level, const std::string &text) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "[%9.3f] %s ", seconds, level);
    const std::string line = prefix + text + "\n";
    const std::lock_guard lock(log_mutex);
    std::fwrite(line.data(), 1, line.size(), stdout);
}
