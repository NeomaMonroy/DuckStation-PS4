#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <mutex>
#include <string>

namespace {
std::mutex cwd_lock;
char cwd[1024] = "/";

const char* absolute(const char* path, std::string& buf) {
    if (path == nullptr || path[0] == '/' || path[0] == '\0')
        return path;
    while (path[0] == '.' && path[1] == '/')
        path += 2;
    {
        std::lock_guard lock(cwd_lock);
        buf = cwd;
    }
    if (path[0] == '.' && path[1] == '\0')
        return buf.c_str();
    if (buf.empty() || buf.back() != '/')
        buf += '/';
    buf += path;
    return buf.c_str();
}
}

extern "C" {
int __real_chdir(const char* path);
int __real_lstat(const char* path, struct stat* st);
int __real_mkdir(const char* path, mode_t mode);
int __real_rmdir(const char* path);
int __real_unlink(const char* path);
int __real_rename(const char* from, const char* to);
int __real_access(const char* path, int mode);
DIR* __real_opendir(const char* path);
int __real_chmod(const char* path, mode_t mode);
int __real_remove(const char* path);
int __real_truncate(const char* path, off_t length);
int __real_utimes(const char* path, const struct timeval times[2]);

int __wrap_chdir(const char* path) {
    std::string buf;
    const char* full = absolute(path, buf);
    struct stat st;
    if (full == nullptr || __real_lstat(full, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    __real_chdir(full);
    std::lock_guard lock(cwd_lock);
    snprintf(cwd, sizeof(cwd), "%s", full);
    return 0;
}

char* __wrap_getcwd(char* out, size_t size) {
    std::lock_guard lock(cwd_lock);
    const size_t length = strlen(cwd);
    if (out == nullptr) {

        char* copy = static_cast<char*>(malloc(length + 1));
        if (copy)
            memcpy(copy, cwd, length + 1);
        return copy;
    }
    if (size <= length) {
        errno = ERANGE;
        return nullptr;
    }
    memcpy(out, cwd, length + 1);
    return out;
}

#define ONE_PATH(ret, name, sig, call)                    \
    ret __wrap_##name sig {                               \
        std::string buf;                                  \
        const char* full = absolute(path, buf);           \
        return __real_##name call;                        \
    }
ONE_PATH(int, lstat, (const char* path, struct stat* st), (full, st))
ONE_PATH(int, mkdir, (const char* path, mode_t mode), (full, mode))
ONE_PATH(int, rmdir, (const char* path), (full))
ONE_PATH(int, unlink, (const char* path), (full))
ONE_PATH(int, access, (const char* path, int mode), (full, mode))
ONE_PATH(DIR*, opendir, (const char* path), (full))
ONE_PATH(int, chmod, (const char* path, mode_t mode), (full, mode))
ONE_PATH(int, remove, (const char* path), (full))
ONE_PATH(int, truncate, (const char* path, off_t length), (full, length))
ONE_PATH(int, utimes, (const char* path, const struct timeval times[2]), (full, times))
#undef ONE_PATH

int __wrap_rename(const char* from, const char* to) {
    std::string a, b;
    return __real_rename(absolute(from, a), absolute(to, b));
}

const char* ps4_absolute_path(const char* path, char* out, size_t size) {
    std::string buf;
    const char* full = absolute(path, buf);
    if (full != buf.c_str())
        return full;
    snprintf(out, size, "%s", buf.c_str());
    return out;
}
}
