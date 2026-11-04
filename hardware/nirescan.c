/*
 * Restarts NI's hardware services when NI devices arrive.
 *
 * NIHostIntegrationAgent and NIHardwareService enumerate devices when they
 * start and learn about later ones only from SERVICE_CONTROL_DEVICEEVENT,
 * which Wine doesn't send to services. Wine also starts them before winebus
 * and wineusb have created any device. On a Wine that sends the event this
 * service does nothing.
 *
 * Maschine doesn't reconnect to a restarted NIHostIntegrationAgent, it has to
 * be restarted after a controller is plugged in while it runs. Removals don't
 * restart anything.
 *
 * Messages go to OutputDebugString (WINEDEBUG=warn+seh).
 */

#include <windows.h>
#include <dbt.h>
#include <cfgmgr32.h>
#include <shlwapi.h>
#include <initguid.h>

/* mingw-w64 before 12 has the types but not these */
CMAPI CONFIGRET WINAPI CM_Register_Notification(PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK, PHCMNOTIFICATION);
CMAPI CONFIGRET WINAPI CM_Unregister_Notification(HCMNOTIFICATION);

DEFINE_GUID(GUID_DEVINTERFACE_HID, 0x4d1e55b2, 0xf16f, 0x11cf, 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);
DEFINE_GUID(GUID_DEVINTERFACE_MK3, 0x64352aa7, 0x9dfd, 0x4fa8, 0xb6, 0xf0, 0x47, 0x4a, 0xf2, 0x3a, 0x78, 0xe9);

/* the classes NI's services open, HID only with NI's vendor id */
static const GUID *ni_classes[] = {&GUID_DEVINTERFACE_HID, &GUID_DEVINTERFACE_MK3};

static const WCHAR *ni_services[] = {L"NIHardwareService", L"NIHostIntegrationAgent"};

/* a burst of interface arrivals (one controller has several) gets one restart */
#define SETTLE_MS 1000
#define WAIT_MS 30000

static SERVICE_STATUS_HANDLE status_handle;
static HANDLE stop_event, settle_timer;
static LONG service_events;

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

/* IsEqualGUID needs memcmp */
static BOOL guid_equal(const GUID *a, const GUID *b)
{
    const ULONG *x = (const ULONG *)a, *y = (const ULONG *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

static BOOL is_ni_interface(const GUID *class, const WCHAR *path)
{
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(ni_classes); i++)
        if (guid_equal(class, ni_classes[i])) return StrStrIW(path, L"VID_17CC") != NULL;
    return FALSE;
}

static BOOL ni_devices_present(void)
{
    unsigned int i;
    BOOL found = FALSE;

    for (i = 0; i < ARRAYSIZE(ni_classes) && !found; i++)
    {
        ULONG len = 0;
        WCHAR *list, *p;

        if (CM_Get_Device_Interface_List_SizeW(&len, (GUID *)ni_classes[i], NULL,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) || len <= 1)
            continue;
        if (!(list = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR)))) continue;
        if (!CM_Get_Device_Interface_ListW((GUID *)ni_classes[i], NULL, list, len,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT))
            for (p = list; *p && !found; p += lstrlenW(p) + 1)
                found = is_ni_interface(ni_classes[i], p);
        HeapFree(GetProcessHeap(), 0, list);
    }
    return found;
}

static void settle_later(void)
{
    LARGE_INTEGER due;

    due.QuadPart = -10000LL * SETTLE_MS;
    SetWaitableTimer(settle_timer, &due, 0, NULL, NULL, FALSE);
}

static void CALLBACK status_reached(void *param)
{
}

static BOOL wait_for_state(SC_HANDLE service, DWORD state, DWORD mask)
{
    /* static: the notify thread may still write it after a timeout */
    static SERVICE_NOTIFYW notify = {.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE, .pfnNotifyCallback = status_reached};
    SERVICE_STATUS status;

    if (!NotifyServiceStatusChangeW(service, mask, &notify)) SleepEx(WAIT_MS, TRUE);
    return QueryServiceStatus(service, &status) && status.dwCurrentState == state;
}

/* A service that registers for device events before PlugPlay listens never
 * gets any in Wine: the process's notify thread fails to connect and exits. */
static void wait_for_plugplay(SC_HANDLE manager)
{
    SC_HANDLE service;

    if (!(service = OpenServiceW(manager, L"PlugPlay", SERVICE_QUERY_STATUS))) return;
    if (!wait_for_state(service, SERVICE_RUNNING, SERVICE_NOTIFY_RUNNING))
        log_line(L"nirescan: PlugPlay did not start\n");
    CloseServiceHandle(service);
}

static void restart_service(SC_HANDLE manager, const WCHAR *name)
{
    QUERY_SERVICE_CONFIGW *config;
    SERVICE_STATUS status;
    SC_HANDLE service;
    BOOL start;
    DWORD size;

    if (!(service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG
            | SERVICE_STOP | SERVICE_START)))
        return;

    QueryServiceConfigW(service, NULL, 0, &size);
    if ((config = HeapAlloc(GetProcessHeap(), 0, size)))
    {
        start = QueryServiceConfigW(service, config, size, &size) && config->dwStartType == SERVICE_AUTO_START;
        HeapFree(GetProcessHeap(), 0, config);
    }
    else start = FALSE;

    if (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED)
    {
        start = TRUE;
        /* a service still starting doesn't accept STOP yet */
        if (status.dwCurrentState == SERVICE_START_PENDING)
            wait_for_state(service, SERVICE_RUNNING, SERVICE_NOTIFY_RUNNING);
        if (status.dwCurrentState != SERVICE_STOP_PENDING) ControlService(service, SERVICE_CONTROL_STOP, &status);
        if (!wait_for_state(service, SERVICE_STOPPED, SERVICE_NOTIFY_STOPPED))
        {
            log_line(L"nirescan: %s did not stop\n", name);
            start = FALSE;
        }
    }
    if (start && !StartServiceW(service, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        log_line(L"nirescan: starting %s failed, error %u\n", name, GetLastError());
    else if (start)
        log_line(L"nirescan: restarted %s\n", name);
    CloseServiceHandle(service);
}

static DWORD CALLBACK device_changed(HCMNOTIFICATION notify, void *context, CM_NOTIFY_ACTION action,
        CM_NOTIFY_EVENT_DATA *data, DWORD size)
{
    if (action != CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL && action != CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
        return ERROR_SUCCESS;
    if (!is_ni_interface(&data->u.DeviceInterface.ClassGuid, data->u.DeviceInterface.SymbolicLink))
        return ERROR_SUCCESS;
    log_line(L"nirescan: %s %s\n", action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ? L"arrival" : L"removal",
             data->u.DeviceInterface.SymbolicLink);
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) settle_later();
    return ERROR_SUCCESS;
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
    case SERVICE_CONTROL_DEVICEEVENT:
        InterlockedExchange(&service_events, 1);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI service_main(DWORD argc, WCHAR **argv)
{
    DEV_BROADCAST_DEVICEINTERFACE_W probe = {0};
    CM_NOTIFY_FILTER filter = {0};
    HCMNOTIFICATION cm_notify = NULL;
    HDEVNOTIFY probe_notify;
    HANDLE handles[2];
    SC_HANDLE manager;
    unsigned int i;

    if (!(status_handle = RegisterServiceCtrlHandlerExW(L"nirescan", handler, NULL))) return;
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    settle_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (!(manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT)) || !stop_event || !settle_timer)
    {
        set_state(SERVICE_STOPPED);
        return;
    }
    set_state(SERVICE_RUNNING);

    wait_for_plugplay(manager);

    /* Registered first, so on a Wine that sends service device events the
     * handler sees each event before device_changed() does. */
    probe.dbcc_size = sizeof(probe);
    probe.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    probe_notify = RegisterDeviceNotificationW(status_handle, &probe,
            DEVICE_NOTIFY_SERVICE_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);

    filter.cbSize = sizeof(filter);
    filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    if (CM_Register_Notification(&filter, NULL, device_changed, &cm_notify))
        log_line(L"nirescan: CM_Register_Notification failed\n");

    /* what came up before the registration above */
    if (ni_devices_present()) settle_later();

    handles[0] = stop_event;
    handles[1] = settle_timer;
    for (;;)
    {
        DWORD ret = WaitForMultipleObjectsEx(2, handles, FALSE, INFINITE, TRUE);

        /* alertable for the APCs of timed out status waits */
        if (ret == WAIT_IO_COMPLETION) continue;
        if (ret != WAIT_OBJECT_0 + 1) break;
        if (service_events)
        {
            log_line(L"nirescan: Wine sends service device events, nothing to do\n");
            continue;
        }
        for (i = 0; i < ARRAYSIZE(ni_services); i++) restart_service(manager, ni_services[i]);
    }

    if (cm_notify) CM_Unregister_Notification(cm_notify);
    if (probe_notify) UnregisterDeviceNotification(probe_notify);
    CloseServiceHandle(manager);
    set_state(SERVICE_STOPPED);
}

void __stdcall entry(void)
{
    SERVICE_TABLE_ENTRYW table[] = {{(WCHAR *)L"nirescan", service_main}, {NULL, NULL}};

    StartServiceCtrlDispatcherW(table);
    ExitProcess(0);
}
