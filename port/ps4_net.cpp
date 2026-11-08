#include <netdb.h>
#include <stddef.h>

extern "C" {
int getaddrinfo(const char*, const char*, const struct addrinfo*, struct addrinfo** res) {
    if (res)
        *res = NULL;
    return EAI_FAIL;
}

void freeaddrinfo(struct addrinfo*) {}

const char* gai_strerror(int) {
    return "no network on the PS4 port";
}

struct hostent* gethostbyname(const char*) {
    return NULL;
}
}
