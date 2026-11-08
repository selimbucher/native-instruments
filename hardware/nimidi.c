/*
 * Relays the virtual MIDI ports of ni-wine's drivers (nimk3.sys: Maschine MK3
 * Ctrl MIDI) to ni-maschine, which has the ALSA port (maschine/README.md).
 *
 * While at least one port exists, it runs the Unix path in
 * HKLM\Software\ni-wine\ni-maschine (written by ni-wine) with
 * `--attach N`, which connects to 127.0.0.1:N and hands the connection to
 * the running ni-maschine. Wine starts Unix programs detached, without a process handle,
 * so the connection tells whether ni-maschine is there. Wine can't give a
 * service's Unix child pipes as stdio (named pipes have no Unix fds, children
 * of console-less processes get /dev/null) and its winsock has no AF_UNIX.
 * Only a peer of the same Unix user is accepted, checked in /proc/net/tcp.
 * Frames: nimidi.h.
 *
 * Messages go to OutputDebugString (WINEDEBUG=warn+seh).
 */

#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <shlwapi.h>
#include <initguid.h>
#include "nimidi.h"

/* mingw-w64 before 12 has the types but not these */
CMAPI CONFIGRET WINAPI CM_Register_Notification(PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK, PHCMNOTIFICATION);
CMAPI CONFIGRET WINAPI CM_Unregister_Notification(HCMNOTIFICATION);

#define MAX_PORTS 16
#define BUF_SIZE 0x10004 /* the largest frame */
#define RETRY_MS 1000
#define MAX_RETRY_MS 60000
#define CONNECT_MS 10000
#define WAIT_MS 30000

struct port
{
    WCHAR *path;
    HANDLE file;           /* overlapped */
    HANDLE thread;         /* reads */
    char name[80];
    BOOL dead;             /* the reader stopped */
};

static SERVICE_STATUS_HANDLE status_handle;
static HANDLE stop_event, change_event;
/* frames name port i by its index */
static struct port ports[MAX_PORTS];
static CRITICAL_SECTION ports_cs;
/* the connection to ni-maschine, INVALID_SOCKET while there is none */
static SOCKET maschine = INVALID_SOCKET;
static CRITICAL_SECTION send_cs;

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

static void log_line(const WCHAR *fmt, ...)
{
    WCHAR buf[512];
    va_list args;

    va_start(args, fmt);
    wvsprintfW(buf, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
}

static void set_state(DWORD state)
{
    SERVICE_STATUS status = {0};

    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(status_handle, &status);
}

static void CALLBACK status_reached(void *param)
{
}

/* A process that registers for device events before PlugPlay listens never
 * gets any in Wine: its notify thread fails to connect and exits. */
static void wait_for_plugplay(void)
{
    /* static: the notify thread may still write it after a timeout */
    static SERVICE_NOTIFYW notify = {.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE, .pfnNotifyCallback = status_reached};
    SC_HANDLE manager, service;

    if (!(manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT))) return;
    if ((service = OpenServiceW(manager, L"PlugPlay", SERVICE_QUERY_STATUS)))
    {
        if (!NotifyServiceStatusChangeW(service, SERVICE_NOTIFY_RUNNING, &notify)) SleepEx(WAIT_MS, TRUE);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
}

static DWORD CALLBACK interface_changed(HCMNOTIFICATION notify, void *context, CM_NOTIFY_ACTION action,
        CM_NOTIFY_EVENT_DATA *data, DWORD size)
{
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
        SetEvent(change_event);
    return ERROR_SUCCESS;
}

/* the interfaces present now, a multi-string the caller frees */
static WCHAR *present_interfaces(void)
{
    ULONG len = 0;
    WCHAR *list;

    if (CM_Get_Device_Interface_List_SizeW(&len, (GUID *)&GUID_DEVINTERFACE_NIWINE_MIDI, NULL,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) || !len)
        return NULL;
    if (!(list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (len + 1) * sizeof(WCHAR)))) return NULL;
    if (CM_Get_Device_Interface_ListW((GUID *)&GUID_DEVINTERFACE_NIWINE_MIDI, NULL, list, len,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT))
        list[0] = 0;
    return list;
}

static BOOL in_list(const WCHAR *list, const WCHAR *path)
{
    const WCHAR *p;

    for (p = list; p && *p; p += lstrlenW(p) + 1)
        if (!lstrcmpiW(p, path)) return TRUE;
    return FALSE;
}

static BOOL send_all(SOCKET s, const BYTE *data, DWORD len)
{
    int n;

    for (; len; data += n, len -= n)
        if ((n = send(s, (const char *)data, len, 0)) <= 0) return FALSE;
    return TRUE;
}

/* with send_cs held */
static void send_frame(BYTE type, BYTE id, const void *data, DWORD len)
{
    BYTE header[NIMIDI_HEADER] = {type, id, len & 0xff, len >> 8};

    if (maschine == INVALID_SOCKET) return;
    if (!send_all(maschine, header, sizeof(header)) || (len && !send_all(maschine, data, len)))
        shutdown(maschine, SD_BOTH); /* maschine_thread notices */
}

/* waits for an overlapped request on file, FALSE if it failed or the service stops */
static BOOL finish(HANDLE file, OVERLAPPED *ov, BOOL ok, DWORD *len)
{
    HANDLE handles[2] = {ov->hEvent, stop_event};

    if (!ok && GetLastError() != ERROR_IO_PENDING) return FALSE;
    if (!ok && WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0)
    {
        CancelIoEx(file, ov);
        GetOverlappedResult(file, ov, len, TRUE);
        return FALSE;
    }
    return GetOverlappedResult(file, ov, len, FALSE);
}

/* driver -> ni-maschine */
static DWORD WINAPI read_thread(void *arg)
{
    struct port *port = arg;
    BYTE id = port - ports, *buf;
    OVERLAPPED ov = {0};
    DWORD len, error = 0;
    WCHAR *list;
    BOOL ok;

    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if ((buf = HeapAlloc(GetProcessHeap(), 0, 0xffff)) && ov.hEvent)
    {
        for (;;)
        {
            ResetEvent(ov.hEvent);
            ok = DeviceIoControl(port->file, IOCTL_NIWINE_MIDI_READ, NULL, 0, buf, 0xffff, &len, &ov);
            if (!finish(port->file, &ov, ok, &len))
            {
                error = GetLastError();
                break;
            }
            EnterCriticalSection(&send_cs);
            send_frame(NIMIDI_DATA, id, buf, len);
            LeaveCriticalSection(&send_cs);
        }
    }
    /* a port that is still there gets opened again, not right away */
    if ((list = present_interfaces()) && in_list(list, port->path) && WaitForSingleObject(stop_event, 0))
    {
        log_line(L"nimidi: reading %s failed, error %u\n", port->path, error);
        WaitForSingleObject(stop_event, RETRY_MS);
    }
    HeapFree(GetProcessHeap(), 0, list);
    HeapFree(GetProcessHeap(), 0, buf);
    if (ov.hEvent) CloseHandle(ov.hEvent);
    port->dead = TRUE;
    SetEvent(change_event);
    return 0;
}

/* with ports_cs held */
static void remove_port(struct port *port)
{
    CancelIoEx(port->file, NULL);
    WaitForSingleObject(port->thread, INFINITE);
    CloseHandle(port->thread);
    CloseHandle(port->file);
    log_line(L"nimidi: removed %s\n", port->path);
    HeapFree(GetProcessHeap(), 0, port->path);
    memset(port, 0, sizeof(*port));
    EnterCriticalSection(&send_cs);
    send_frame(NIMIDI_PORT_REMOVE, port - ports, NULL, 0);
    LeaveCriticalSection(&send_cs);
}

/* "<driver's name>", then "<name> 2" and so on */
static void name_port(struct port *port, const char *base)
{
    unsigned int n, i;

    for (n = 1; ; n++)
    {
        if (n == 1) lstrcpynA(port->name, base, sizeof(port->name) - 4);
        else wsprintfA(port->name + lstrlenA(base), " %u", n);
        for (i = 0; i < MAX_PORTS; i++)
            if (&ports[i] != port && ports[i].path && !lstrcmpA(ports[i].name, port->name)) break;
        if (i == MAX_PORTS) return;
    }
}

/* the driver's name of a port */
static BOOL query_name(HANDLE file, char *name, DWORD size)
{
    OVERLAPPED ov = {0};
    DWORD len;
    BOOL ok;

    if (!(ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) return FALSE;
    ok = DeviceIoControl(file, IOCTL_NIWINE_MIDI_NAME, NULL, 0, name, size - 1, &len, &ov);
    ok = finish(file, &ov, ok, &len);
    CloseHandle(ov.hEvent);
    if (ok) name[len] = 0;
    return ok;
}

/* with ports_cs held */
static void add_port(const WCHAR *path)
{
    struct port *port = NULL;
    char base[64];
    unsigned int i;
    HANDLE file;

    for (i = 0; i < MAX_PORTS && !port; i++)
        if (!ports[i].path) port = &ports[i];
    if (!port) return;

    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (!query_name(file, base, sizeof(base)))
    {
        CloseHandle(file);
        return;
    }

    if (!(port->path = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(path) + 1) * sizeof(WCHAR))))
    {
        CloseHandle(file);
        return;
    }
    lstrcpyW(port->path, path);
    port->file = file;
    name_port(port, base);
    EnterCriticalSection(&send_cs);
    send_frame(NIMIDI_PORT_ADD, port - ports, port->name, lstrlenA(port->name));
    LeaveCriticalSection(&send_cs);
    if (!(port->thread = CreateThread(NULL, 0, read_thread, port, 0, NULL))) port->dead = TRUE;
    log_line(L"nimidi: added %s\n", path);
}

/* the ports follow the present interfaces; returns how many there are */
static unsigned int sync_ports(void)
{
    unsigned int i, count = 0;
    WCHAR *list = present_interfaces(), *p;

    EnterCriticalSection(&ports_cs);
    for (i = 0; i < MAX_PORTS; i++)
        if (ports[i].path && (ports[i].dead || !in_list(list, ports[i].path))) remove_port(&ports[i]);
    for (p = list; p && *p; p += lstrlenW(p) + 1)
    {
        for (i = 0; i < MAX_PORTS; i++)
            if (ports[i].path && !lstrcmpiW(ports[i].path, p)) break;
        if (i == MAX_PORTS) add_port(p);
    }
    for (i = 0; i < MAX_PORTS; i++)
        if (ports[i].path) count++;
    LeaveCriticalSection(&ports_cs);
    HeapFree(GetProcessHeap(), 0, list);
    return count;
}

/* ni-maschine -> driver */
static DWORD WINAPI maschine_thread(void *arg)
{
    SOCKET s = (SOCKET)(ULONG_PTR)arg;
    DWORD have = 0, done, size, len;
    OVERLAPPED ov = {0};
    BYTE *buf, *f;
    HANDLE file;
    int got;
    BOOL ok;

    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!(buf = HeapAlloc(GetProcessHeap(), 0, BUF_SIZE)) || !ov.hEvent) goto done;
    while ((got = recv(s, (char *)buf + have, BUF_SIZE - have, 0)) > 0)
    {
        have += got;
        for (done = 0; have - done >= NIMIDI_HEADER; done += NIMIDI_HEADER + size)
        {
            f = buf + done;
            size = f[2] | f[3] << 8;
            if (NIMIDI_HEADER + size > have - done) break;
            if (f[0] != NIMIDI_DATA || !size || f[1] >= MAX_PORTS) continue;
            EnterCriticalSection(&ports_cs);
            if ((file = ports[f[1]].path ? ports[f[1]].file : NULL))
            {
                ResetEvent(ov.hEvent);
                ok = DeviceIoControl(file, IOCTL_NIWINE_MIDI_WRITE, f + NIMIDI_HEADER, size, NULL, 0, &len, &ov);
                finish(file, &ov, ok, &len);
            }
            LeaveCriticalSection(&ports_cs);
        }
        have -= done;
        memcpy(buf, buf + done, have);
    }
done:
    HeapFree(GetProcessHeap(), 0, buf);
    if (ov.hEvent) CloseHandle(ov.hEvent);
    return 0;
}

/* uid column of the /proc/net/tcp line for local -> remote, both on 127.0.0.1 */
static BOOL tcp_uid(const char *table, USHORT local, USHORT remote, DWORD *uid)
{
    char want[32];
    const char *p, *q, *hit;
    int field;

    wsprintfA(want, "0100007F:%04X 0100007F:%04X", local, remote);
    for (p = table; *p; p = q + (*q != 0))
    {
        if (!(q = StrChrA(p, '\n'))) q = p + lstrlenA(p);
        if (!(hit = StrStrA(p, want)) || hit > q) continue;
        /* fields: sl local rem st queues tr retrnsmt uid */
        for (field = 0; field < 7 && p < q; field++)
        {
            while (p < q && *p == ' ') p++;
            while (p < q && *p != ' ') p++;
        }
        while (p < q && *p == ' ') p++;
        for (*uid = 0; p < q && *p >= '0' && *p <= '9'; p++) *uid = *uid * 10 + (*p - '0');
        return TRUE;
    }
    return FALSE;
}

/* the peer of a 127.0.0.1 connection belongs to our Unix user */
static BOOL same_user(SOCKET s)
{
    struct sockaddr_in ours, peer;
    int len = sizeof(ours);
    DWORD our_uid, peer_uid, size = 0, got;
    HANDLE file;
    char *table;
    BOOL ret = FALSE;

    if (getsockname(s, (struct sockaddr *)&ours, &len)) return FALSE;
    len = sizeof(peer);
    if (getpeername(s, (struct sockaddr *)&peer, &len)) return FALSE;
    file = CreateFileW(L"\\\\?\\unix\\proc\\net\\tcp", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if ((table = HeapAlloc(GetProcessHeap(), 0, 1 << 20)))
    {
        while (size < (1 << 20) - 1 && ReadFile(file, table + size, (1 << 20) - 1 - size, &got, NULL) && got)
            size += got;
        table[size] = 0;
        ret = tcp_uid(table, ntohs(ours.sin_port), ntohs(peer.sin_port), &our_uid)
                && tcp_uid(table, ntohs(peer.sin_port), ntohs(ours.sin_port), &peer_uid)
                && our_uid == peer_uid;
        HeapFree(GetProcessHeap(), 0, table);
    }
    CloseHandle(file);
    return ret;
}

/* the command line of ni-maschine --attach, from the Unix path ni-wine stored */
static BOOL maschine_command(WCHAR *cmd, DWORD size, USHORT tcp_port)
{
    WCHAR *(CDECL *to_dos)(const char *);
    char unix_path[1024];
    DWORD len = sizeof(unix_path);
    WCHAR *dos;
    BOOL fits;

    if (RegGetValueA(HKEY_LOCAL_MACHINE, "Software\\ni-wine", "ni-maschine", RRF_RT_REG_SZ, NULL, unix_path, &len))
    {
        log_line(L"nimidi: HKLM\\Software\\ni-wine\\ni-maschine is not set\n");
        return FALSE;
    }
    to_dos = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
    if (!to_dos || !(dos = to_dos(unix_path))) return FALSE;
    if ((fits = lstrlenW(dos) + 40 < (int)size)) wsprintfW(cmd, L"\"%s\" --attach %u", dos, tcp_port);
    HeapFree(GetProcessHeap(), 0, dos);
    return fits;
}

/* starts ni-maschine --attach and waits for its connection */
static SOCKET start_maschine(void)
{
    struct sockaddr_in addr = {.sin_family = AF_INET};
    STARTUPINFOW si = {.cb = sizeof(si)};
    SOCKET listener, s = INVALID_SOCKET;
    int len = sizeof(addr);
    PROCESS_INFORMATION pi;
    ULONGLONG start;
    struct timeval tv;
    WCHAR cmd[1200];
    fd_set fds;

    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if ((listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET) return INVALID_SOCKET;
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 4)
            || getsockname(listener, (struct sockaddr *)&addr, &len)
            || !maschine_command(cmd, ARRAYSIZE(cmd), ntohs(addr.sin_port)))
    {
        closesocket(listener);
        return INVALID_SOCKET;
    }
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        log_line(L"nimidi: starting %s failed, error %u\n", cmd, GetLastError());
        closesocket(listener);
        return INVALID_SOCKET;
    }
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);

    for (start = GetTickCount64(); s == INVALID_SOCKET && GetTickCount64() - start < CONNECT_MS;)
    {
        FD_ZERO(&fds);
        FD_SET(listener, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (select(0, &fds, NULL, NULL, &tv) <= 0) continue;
        if ((s = accept(listener, NULL, NULL)) == INVALID_SOCKET) continue;
        if (same_user(s)) break;
        log_line(L"nimidi: rejected a connection from another user\n");
        closesocket(s);
        s = INVALID_SOCKET;
    }
    closesocket(listener);
    if (s == INVALID_SOCKET) log_line(L"nimidi: %s did not connect\n", cmd);
    return s;
}

struct maschine_link
{
    SOCKET socket;
    HANDLE thread;
};

static BOOL connect_maschine(struct maschine_link *link)
{
    unsigned int i;
    SOCKET s;

    if ((s = start_maschine()) == INVALID_SOCKET) return FALSE;
    link->socket = s;
    /* the ports first, data for them only after */
    EnterCriticalSection(&ports_cs);
    EnterCriticalSection(&send_cs);
    maschine = s;
    for (i = 0; i < MAX_PORTS; i++)
        if (ports[i].path) send_frame(NIMIDI_PORT_ADD, i, ports[i].name, lstrlenA(ports[i].name));
    LeaveCriticalSection(&send_cs);
    LeaveCriticalSection(&ports_cs);
    if (!(link->thread = CreateThread(NULL, 0, maschine_thread, (void *)(ULONG_PTR)s, 0, NULL)))
        shutdown(s, SD_BOTH);
    log_line(L"nimidi: ni-maschine connected\n");
    return TRUE;
}

/* ni-maschine takes the MK3 back when the connection closes */
static void disconnect_maschine(struct maschine_link *link)
{
    EnterCriticalSection(&send_cs);
    maschine = INVALID_SOCKET;
    LeaveCriticalSection(&send_cs);
    if (link->socket != INVALID_SOCKET) shutdown(link->socket, SD_BOTH);
    if (link->thread)
    {
        WaitForSingleObject(link->thread, INFINITE);
        CloseHandle(link->thread);
    }
    if (link->socket != INVALID_SOCKET) closesocket(link->socket);
    link->socket = INVALID_SOCKET;
    link->thread = NULL;
}

static DWORD WINAPI handler(DWORD control, DWORD type, void *data, void *context)
{
    switch (control)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        set_state(SERVICE_STOP_PENDING);
        SetEvent(stop_event);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI service_main(DWORD argc, WCHAR **argv)
{
    struct maschine_link link = {INVALID_SOCKET, NULL};
    CM_NOTIFY_FILTER filter = {0};
    HCMNOTIFICATION notify = NULL;
    ULONGLONG retry_at = 0, retry_ms = RETRY_MS;
    unsigned int i, count;
    WSADATA wsa;
    DWORD timeout;
    HANDLE handles[3];

    if (!(status_handle = RegisterServiceCtrlHandlerExW(L"nimidi", handler, NULL))) return;
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    change_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!stop_event || !change_event || WSAStartup(MAKEWORD(2, 2), &wsa))
    {
        set_state(SERVICE_STOPPED);
        return;
    }
    InitializeCriticalSection(&ports_cs);
    InitializeCriticalSection(&send_cs);
    set_state(SERVICE_RUNNING);

    wait_for_plugplay();
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_NIWINE_MIDI;
    if (CM_Register_Notification(&filter, NULL, interface_changed, &notify))
        log_line(L"nimidi: CM_Register_Notification failed\n");

    for (;;)
    {
        count = sync_ports();
        if (link.thread && WaitForSingleObject(link.thread, 0) == WAIT_OBJECT_0)
        {
            log_line(L"nimidi: ni-maschine went away\n");
            disconnect_maschine(&link);
            retry_at = GetTickCount64() + retry_ms;
            retry_ms = min(retry_ms * 2, MAX_RETRY_MS);
        }
        if (!count && link.socket != INVALID_SOCKET) disconnect_maschine(&link);
        if (count && link.socket == INVALID_SOCKET && GetTickCount64() >= retry_at)
        {
            if (connect_maschine(&link)) retry_ms = RETRY_MS;
            else
            {
                retry_at = GetTickCount64() + retry_ms;
                retry_ms = min(retry_ms * 2, MAX_RETRY_MS);
            }
        }

        handles[0] = stop_event;
        handles[1] = change_event;
        handles[2] = link.thread;
        timeout = INFINITE;
        if (count && link.socket == INVALID_SOCKET) timeout = (DWORD)(retry_at - min(retry_at, GetTickCount64()));
        if (WaitForMultipleObjects(link.thread ? 3 : 2, handles, FALSE, timeout) == WAIT_OBJECT_0) break;
    }

    if (notify) CM_Unregister_Notification(notify);
    disconnect_maschine(&link);
    EnterCriticalSection(&ports_cs);
    for (i = 0; i < MAX_PORTS; i++)
        if (ports[i].path) remove_port(&ports[i]);
    LeaveCriticalSection(&ports_cs);
    set_state(SERVICE_STOPPED);
}

void __stdcall entry(void)
{
    SERVICE_TABLE_ENTRYW table[] = {{(WCHAR *)L"nimidi", service_main}, {NULL, NULL}};

    StartServiceCtrlDispatcherW(table);
    ExitProcess(0);
}
