#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int backtrace(void **frames, int max);
char **backtrace_symbols(void *const *frames, int count);
void backtrace_symbols_fd(void *const *frames, int count, int fd);
#ifdef __cplusplus
}
#endif
