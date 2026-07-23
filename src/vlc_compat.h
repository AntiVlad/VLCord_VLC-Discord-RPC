#ifndef VLC_COMPAT_H
#define VLC_COMPAT_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#ifndef MODULE_STRING
#define MODULE_STRING "discord_rpc_plugin"
#endif

#ifndef N_
#define N_(str) str
#endif

#ifndef _
#define _(str) str
#endif

#ifndef static_assert
#define static_assert(expr, msg)
#endif

struct pollfd;
static inline int poll(struct pollfd *fds, unsigned int nfds, int timeout) {
    return WSAPoll((LPWSAPOLLFD)fds, (ULONG)nfds, timeout);
}

// Safely free strings allocated by VLC (msvcrt.dll heap)
static inline void vlc_free_string(void *ptr)
{
    if (!ptr) return;
    typedef void (__cdecl *free_fn_t)(void *);
    static free_fn_t msvcrt_free = NULL;
    if (!msvcrt_free) {
        HMODULE hMsvcrt = GetModuleHandleA("msvcrt.dll");
        if (!hMsvcrt) hMsvcrt = LoadLibraryA("msvcrt.dll");
        if (hMsvcrt) msvcrt_free = (free_fn_t)GetProcAddress(hMsvcrt, "free");
    }
    if (msvcrt_free) {
        msvcrt_free(ptr);
    }
}

#endif // VLC_COMPAT_H
