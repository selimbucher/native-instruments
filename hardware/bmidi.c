/*
 * Stand-in for Bome's bmidilib2.dll in the prefix. NI's services make their
 * virtual MIDI ports with it, the Maschine Mikro MK3's among them, and Bome's
 * bus driver doesn't load in Wine. The ports go to nimidi.exe over a pipe
 * (nimidi.h) and from there to ni-maschine. Only what NI's services import.
 *
 * Ports and connections live in the process. Until nimidi.exe is there (it
 * starts after NI's services) sends are dropped, and the ports are announced
 * when the pipe connects.
 *
 * No CRT, see Makefile.
 */

#include <windows.h>
#include "nimidi.h"

#define MAX_PORTS 16
#define RETRY_MS 1000

/* Bome's error codes */
#define BMIDI_OK 0
#define BMIDI_INVALID_PARAMETER 1
#define BMIDI_ALREADY_EXISTS 2
#define BMIDI_NO_PORT 3
#define BMIDI_INVALID_HANDLE 5
#define BMIDI_NO_RESOURCES 8

typedef void (CALLBACK *bmidi_callback)(void *handle, const BYTE *data, DWORD len, void *ctx);

struct port
{
    BOOL used;
    char company[64];
    char name[128];
    bmidi_callback callback;    /* while connected */
    void *ctx;
};

static struct port ports[MAX_PORTS];
/* ports, pipe and the frame being written */
static CRITICAL_SECTION lock;
static HANDLE pipe = INVALID_HANDLE_VALUE;
static HANDLE thread;
static BYTE out[NIMIDI_FRAME_MAX];

void *memset(void *dst, int c, size_t n)
{
    unsigned char *p = dst;
    while (n--) *p++ = c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

/* waits for an overlapped request on the pipe */
static BOOL finish(HANDLE file, OVERLAPPED *ov, BOOL ok, DWORD *len)
{
    if (!ok && GetLastError() != ERROR_IO_PENDING) return FALSE;
    return GetOverlappedResult(file, ov, len, TRUE);
}

/* with lock held; dropped while nimidi.exe isn't there */
static void write_frame(BYTE type, int port, const void *data, DWORD len)
{
    OVERLAPPED ov = {0};
    DWORD done;
    BOOL ok;

    if (pipe == INVALID_HANDLE_VALUE || len > NIMIDI_FRAME_MAX - NIMIDI_HEADER) return;
    out[0] = type;
    out[1] = port;
    out[2] = len & 0xff;
    out[3] = len >> 8;
    memcpy(out + NIMIDI_HEADER, data, len);
    if (!(ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) return;
    ok = WriteFile(pipe, out, NIMIDI_HEADER + len, &done, &ov);
    finish(pipe, &ov, ok, &done);
    CloseHandle(ov.hEvent);
}

static void announce(int port)
{
    write_frame(NIMIDI_PORT_ADD, port, ports[port].name, lstrlenA(ports[port].name));
}

/* MIDI from nimidi.exe to the port's callback */
static void deliver(const BYTE *frame, DWORD len)
{
    DWORD size = frame[2] | frame[3] << 8;
    bmidi_callback callback = NULL;
    struct port *port = NULL;
    void *ctx = NULL;

    if (len < NIMIDI_HEADER || NIMIDI_HEADER + size > len) return;
    if (frame[0] != NIMIDI_DATA || frame[1] >= MAX_PORTS || !size) return;
    EnterCriticalSection(&lock);
    if (ports[frame[1]].used && ports[frame[1]].callback)
    {
        port = &ports[frame[1]];
        callback = port->callback;
        ctx = port->ctx;
    }
    LeaveCriticalSection(&lock);
    /* the callback may send */
    if (callback) callback(port, frame + NIMIDI_HEADER, size, ctx);
}

/* connects to nimidi.exe, announces the ports and reads, again when the pipe breaks */
static DWORD WINAPI pipe_thread(void *arg)
{
    static BYTE frame[NIMIDI_FRAME_MAX];
    DWORD mode = PIPE_READMODE_MESSAGE, len;
    OVERLAPPED ov = {0};
    HANDLE h;
    int i;

    if (!(ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) return 0;
    for (;;)
    {
        h = CreateFileW(NIMIDI_PIPE, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE)
        {
            Sleep(RETRY_MS);
            continue;
        }
        SetNamedPipeHandleState(h, &mode, NULL, NULL);
        EnterCriticalSection(&lock);
        pipe = h;
        for (i = 0; i < MAX_PORTS; i++)
            if (ports[i].used) announce(i);
        LeaveCriticalSection(&lock);

        for (;;)
        {
            ResetEvent(ov.hEvent);
            if (!finish(h, &ov, ReadFile(h, frame, sizeof(frame), &len, &ov), &len)) break;
            deliver(frame, len);
        }

        EnterCriticalSection(&lock);
        pipe = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&lock);
        CloseHandle(h);
        Sleep(RETRY_MS);
    }
}

/* with lock held */
static int find_port(const char *company, const char *name)
{
    int i;

    for (i = 0; i < MAX_PORTS; i++)
        if (ports[i].used && !lstrcmpA(ports[i].company, company) && !lstrcmpA(ports[i].name, name)) return i;
    return -1;
}

/* with lock held */
static int free_port(void)
{
    int i;

    for (i = 0; i < MAX_PORTS; i++)
        if (!ports[i].used) return i;
    return -1;
}

static struct port *port_of(void *handle)
{
    struct port *port = handle;

    if (port < ports || port >= ports + MAX_PORTS || !port->used) return NULL;
    return port;
}

/* the library is the bus */
__declspec(dllexport) int BMIDI_IsBusInstalled(void)
{
    return 1;
}

__declspec(dllexport) DWORD BMIDI_GetVersion(void)
{
    return 0x02000000;
}

__declspec(dllexport) const char *BMIDI_GetErrorString(DWORD error)
{
    static const char *const strings[] =
    {
        "OK", "invalid parameter", "already exists", "port does not exist", "still connected",
        "invalid handle", "not open for output", "not open for input", "not enough system resources",
    };

    return error < ARRAYSIZE(strings) ? strings[error] : "unknown error";
}

__declspec(dllexport) int BMIDI_GetPortIndexByName(const char *company, const char *name)
{
    int index;

    if (!company || !name) return -1;
    EnterCriticalSection(&lock);
    index = find_port(company, name);
    LeaveCriticalSection(&lock);
    return index;
}

/* name2 is the port's input name for Bome's driver; NI passes the same twice */
__declspec(dllexport) int BMIDI_AddPort(const char *company, const char *name1, const char *name2, int *index)
{
    int i, ret = BMIDI_NO_RESOURCES;

    if (!company || !*company || !name1 || !*name1 || !index) return BMIDI_INVALID_PARAMETER;
    EnterCriticalSection(&lock);
    if (find_port(company, name1) >= 0) ret = BMIDI_ALREADY_EXISTS;
    else if ((i = free_port()) >= 0)
    {
        ports[i].used = TRUE;
        lstrcpynA(ports[i].company, company, sizeof(ports[i].company));
        lstrcpynA(ports[i].name, name1, sizeof(ports[i].name));
        announce(i);
        *index = i;
        ret = BMIDI_OK;
    }
    if (!thread) thread = CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL);
    LeaveCriticalSection(&lock);
    return ret;
}

__declspec(dllexport) int BMIDI_RemovePort(const char *company, int index)
{
    int ret = BMIDI_NO_PORT;

    EnterCriticalSection(&lock);
    if (index >= 0 && index < MAX_PORTS && ports[index].used)
    {
        write_frame(NIMIDI_PORT_REMOVE, index, NULL, 0);
        memset(&ports[index], 0, sizeof(ports[index]));
        ret = BMIDI_OK;
    }
    LeaveCriticalSection(&lock);
    return ret;
}

__declspec(dllexport) int BMIDI_Connect(const char *company, int index, DWORD dir, bmidi_callback callback,
                                        void *ctx, void **handle)
{
    int ret = BMIDI_NO_PORT;

    if (!handle) return BMIDI_INVALID_PARAMETER;
    EnterCriticalSection(&lock);
    if (index >= 0 && index < MAX_PORTS && ports[index].used)
    {
        ports[index].callback = callback;
        ports[index].ctx = ctx;
        *handle = &ports[index];
        ret = BMIDI_OK;
    }
    LeaveCriticalSection(&lock);
    return ret;
}

__declspec(dllexport) int BMIDI_Disconnect(void *handle, DWORD dir)
{
    struct port *port;
    int ret = BMIDI_INVALID_HANDLE;

    EnterCriticalSection(&lock);
    if ((port = port_of(handle)))
    {
        port->callback = NULL;
        port->ctx = NULL;
        ret = BMIDI_OK;
    }
    LeaveCriticalSection(&lock);
    return ret;
}

__declspec(dllexport) int BMIDI_Send(void *handle, const BYTE *data, DWORD len)
{
    struct port *port;
    int ret = BMIDI_INVALID_HANDLE;

    if (!len) return BMIDI_OK;
    EnterCriticalSection(&lock);
    if ((port = port_of(handle)))
    {
        write_frame(NIMIDI_DATA, port - ports, data, len);
        ret = BMIDI_OK;
    }
    LeaveCriticalSection(&lock);
    return ret;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        InitializeCriticalSection(&lock);
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
