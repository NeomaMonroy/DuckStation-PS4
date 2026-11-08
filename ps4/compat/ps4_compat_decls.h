#pragma once
#include <pthread.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int pthread_getname_np(pthread_t thread, char *name, size_t size);
#ifdef __cplusplus
}
#endif
