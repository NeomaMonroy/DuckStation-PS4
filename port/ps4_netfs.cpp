#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

extern "C" {
ssize_t __real_read(int fd, void* buffer, size_t size);
ssize_t __real__read(int fd, void* buffer, size_t size);
ssize_t __real_readv(int fd, const struct iovec* iov, int count);
ssize_t __real__readv(int fd, const struct iovec* iov, int count);
ssize_t __real_pread(int fd, void* buffer, size_t size, off_t offset);
off_t __real_lseek(int fd, off_t offset, int whence);
int __real_fstat(int fd, struct stat* st);
int __real_stat(const char* path, struct stat* st);
int __real_getdents(int fd, char* buffer, size_t size);

void ps4_netfs_mount(const char*, const char*, const char*, const char*) {}
int ps4_netfs_check(const char*, char* message, size_t size) {
    if (message && size)
        message[0] = '\0';
    return -1;
}
void ps4_netfs_forget() {}
int ps4_netfs_is_path(const char*) { return 0; }
int ps4_netfs_open(const char*, int) { return -1; }
int ps4_netfs_close(int) { return 0; }

void ps4_vm_prepare(const void* address, size_t size);
static void prepare_iov(const struct iovec* iov, int count) {
    for (int i = 0; iov && i < count; i++)
        ps4_vm_prepare(iov[i].iov_base, iov[i].iov_len);
}

ssize_t __wrap_read(int fd, void* buffer, size_t size) {
    ps4_vm_prepare(buffer, size);
    return __real_read(fd, buffer, size);
}
ssize_t __wrap__read(int fd, void* buffer, size_t size) {
    ps4_vm_prepare(buffer, size);
    return __real__read(fd, buffer, size);
}
ssize_t __wrap_readv(int fd, const struct iovec* iov, int count) {
    prepare_iov(iov, count);
    return __real_readv(fd, iov, count);
}
ssize_t __wrap__readv(int fd, const struct iovec* iov, int count) {
    prepare_iov(iov, count);
    return __real__readv(fd, iov, count);
}
ssize_t __wrap_pread(int fd, void* buffer, size_t size, off_t offset) {
    ps4_vm_prepare(buffer, size);
    return __real_pread(fd, buffer, size, offset);
}
off_t __wrap_lseek(int fd, off_t offset, int whence) { return __real_lseek(fd, offset, whence); }
int __wrap_fstat(int fd, struct stat* st) { return __real_fstat(fd, st); }

const char* ps4_absolute_path(const char* path, char* out, size_t size);
int __wrap_stat(const char* path, struct stat* st) {
    char full[1024];
    return __real_stat(ps4_absolute_path(path, full, sizeof(full)), st);
}
int __wrap_getdents(int fd, char* buffer, size_t size) { return __real_getdents(fd, buffer, size); }
}
