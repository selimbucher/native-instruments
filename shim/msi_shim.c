/*
 * ni-wine msi shim: a forwarding msi.dll for the Kontakt installer.
 *
 * Kontakt's InstallAware installer opens its MSI as a database, rewrites the
 * component table at runtime, then calls MsiInstallProductA once.  Under
 * Wine that call never returns.  This DLL replaces msi.dll for that installer
 * process only (per-application DllOverride), forwards the other 294 exports
 * unchanged to Wine's real msi (kept next to it as msi_wine.dll) and handles
 * MsiInstallProduct itself: for a package named in the config file it runs
 * the hook (ni-wine, which lays the files out from the payload the installer
 * already extracted) and returns the hook's result; anything else goes
 * straight through.
 *
 * Tiny on purpose: no network, no crypto, kernel32 and user32 (wsprintfA)
 * only.  All policy (which package, which hook) is in msi_shim.cfg, written
 * by ni-wine.  `winedump -j export msi_shim32.dll` shows every export except
 * MsiInstallProductA/W as a five-byte jump.
 *
 * Config file (msi_shim.cfg, same directory as the DLL, one `key=value` per
 * line, CRLF or LF):
 *   divert=<substring>   package paths containing this are handled by ni-wine
 *                        (case-insensitive; may repeat)
 *   hook=<unix path>     script to run for a diverted package (receives the
 *                        package path as its only argument)
 *   result=<win path>    file the hook writes when done: first line "OK" or
 *                        an error message (anything else -> 1603)
 *   log=<win path>       append-only log of intercepts (optional)
 *   timeout=<seconds>    how long to wait for the hook (default 7200)
 */

#include <windows.h>

#include "exports.h"

#define ERROR_INSTALL_FAILURE_ 1603
#define MAX_DIVERT 8
#define CFG_MAX 8192

void *g_real[N_EXPORTS];

/* Lets ni-wine recognise the file without loading it (see msishim.py). */
const char g_ident[] = "ni-wine-msi-shim/1";

static HMODULE g_msi;
static char g_dir[MAX_PATH];
static char g_cfg[CFG_MAX];
static const char *g_divert[MAX_DIVERT];
static int g_ndivert;
static const char *g_hook;
static const char *g_result;
static const char *g_log;
static DWORD g_timeout = 7200;
static CRITICAL_SECTION g_lock;

/* --- freestanding helpers (no CRT) ---------------------------------------- */

/* gcc emits memset/memcpy calls even when freestanding; supply them. */
void *memset(void *dst, int value, size_t count)
{
    unsigned char *d = dst;
    while (count--) *d++ = (unsigned char)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (count--) *d++ = *s++;
    return dst;
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void scat(char *dst, int cap, const char *src)
{
    int n = slen(dst), i = 0;
    while (src[i] && n + 1 < cap) dst[n++] = src[i++];
    dst[n] = 0;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return 0;
    for (const char *p = hay; *p; p++) {
        int k = 0;
        while (needle[k] && lower(p[k]) == lower(needle[k])) k++;
        if (!needle[k]) return 1;
    }
    return 0;
}

static void logline(const char *a, const char *b)
{
    HANDLE h;
    SYSTEMTIME t;
    char line[1200];
    DWORD written;

    if (!g_log) return;
    EnterCriticalSection(&g_lock);
    h = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        GetLocalTime(&t);
        wsprintfA(line, "%04d-%02d-%02d %02d:%02d:%02d msi-shim: ",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        scat(line, sizeof line, a);
        if (b) { scat(line, sizeof line, " "); scat(line, sizeof line, b); }
        scat(line, sizeof line, "\r\n");
        WriteFile(h, line, slen(line), &written, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_lock);
}

/* --- configuration -------------------------------------------------------- */

static void load_config(void)
{
    char path[MAX_PATH];
    HANDLE h;
    DWORD got = 0;
    char *p;

    path[0] = 0;
    scat(path, sizeof path, g_dir);
    scat(path, sizeof path, "msi_shim.cfg");
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    ReadFile(h, g_cfg, CFG_MAX - 1, &got, NULL);
    CloseHandle(h);
    g_cfg[got] = 0;

    for (p = g_cfg; *p; ) {
        char *line = p, *eq, *end;
        while (*p && *p != '\n') p++;
        end = p;
        if (*p) *p++ = 0;
        while (end > line && (end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        if (!*line || *line == '#') continue;
        eq = line;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        *eq++ = 0;
        if (!lstrcmpA(line, "divert")) {
            if (g_ndivert < MAX_DIVERT && *eq) g_divert[g_ndivert++] = eq;
        } else if (!lstrcmpA(line, "hook")) {
            g_hook = eq;
        } else if (!lstrcmpA(line, "result")) {
            g_result = eq;
        } else if (!lstrcmpA(line, "log")) {
            g_log = eq;
        } else if (!lstrcmpA(line, "timeout")) {
            DWORD v = 0;
            while (*eq >= '0' && *eq <= '9') v = v * 10 + (*eq++ - '0');
            if (v) g_timeout = v;
        }
    }
}

static int is_diverted(const char *package)
{
    for (int i = 0; i < g_ndivert; i++)
        if (contains_ci(package, g_divert[i])) return 1;
    return 0;
}

/* --- the hook ------------------------------------------------------------- */

/* Run the hook with the package path and wait for its result file.
 * `start /unix` returns as soon as the script is spawned, so completion is
 * signalled by the result file the hook writes, not by the process. */
static UINT run_hook(const char *package)
{
    char cmd[2048];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE h;
    DWORD waited = 0, got = 0;
    char result[512];

    if (!g_hook || !g_result) {
        logline("no hook/result configured; failing the install for", package);
        return ERROR_INSTALL_FAILURE_;
    }
    DeleteFileA(g_result);

    cmd[0] = 0;
    scat(cmd, sizeof cmd, "start.exe /unix \"");
    scat(cmd, sizeof cmd, g_hook);
    scat(cmd, sizeof cmd, "\" \"");
    scat(cmd, sizeof cmd, package);
    scat(cmd, sizeof cmd, "\"");
    logline("running hook:", cmd);

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        logline("CreateProcess failed for the hook", NULL);
        return ERROR_INSTALL_FAILURE_;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    for (;;) {
        h = CreateFileA(g_result, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        if (waited >= g_timeout * 1000) {
            logline("hook timed out; failing the install for", package);
            return ERROR_INSTALL_FAILURE_;
        }
        Sleep(500);
        waited += 500;
    }
    ReadFile(h, result, sizeof result - 1, &got, NULL);
    CloseHandle(h);
    result[got] = 0;
    for (DWORD i = 0; i < got; i++)
        if (result[i] == '\r' || result[i] == '\n') { result[i] = 0; break; }
    logline("hook result:", result);
    return (result[0] == 'O' && result[1] == 'K' && (result[2] == 0 || result[2] == ' '))
        ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE_;
}

/* InstallAware ignores MsiInstallProduct's return value (its script carries
 * on and re-registers the product as installed), and the NTK daemon ignores
 * the installer's exit code and rescans the product's install record.  So
 * when the hook fails, ni-wine has removed that record and the installer
 * must not get the chance to write it back: end the process here.  The
 * outer setup wrapper cleans the temp files up on its own. */
static UINT fail_install(const char *package)
{
    logline("hook failed; ending the installer (exit 1603) for", package);
    ExitProcess(ERROR_INSTALL_FAILURE_);
}

/* --- intercepted exports -------------------------------------------------- */

typedef UINT (WINAPI *install_a_fn)(LPCSTR, LPCSTR);
typedef UINT (WINAPI *install_w_fn)(LPCWSTR, LPCWSTR);

UINT WINAPI MsiInstallProductA(LPCSTR package, LPCSTR command_line)
{
    if (package && is_diverted(package)) {
        logline("MsiInstallProductA diverted:", package);
        return run_hook(package) == ERROR_SUCCESS ? ERROR_SUCCESS : fail_install(package);
    }
    return ((install_a_fn)g_real[IDX_INSTALL_A])(package, command_line);
}

UINT WINAPI MsiInstallProductW(LPCWSTR package, LPCWSTR command_line)
{
    char narrow[MAX_PATH * 2];

    if (package) {
        int n = WideCharToMultiByte(CP_ACP, 0, package, -1, narrow, sizeof narrow, NULL, NULL);
        if (n > 0 && is_diverted(narrow)) {
            logline("MsiInstallProductW diverted:", narrow);
            return run_hook(narrow) == ERROR_SUCCESS ? ERROR_SUCCESS : fail_install(narrow);
        }
    }
    return ((install_w_fn)g_real[IDX_INSTALL_W])(package, command_line);
}

/* --- setup ---------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    char path[MAX_PATH];
    int n;

    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    InitializeCriticalSection(&g_lock);

    n = GetModuleFileNameA(instance, g_dir, sizeof g_dir);
    while (n > 0 && g_dir[n - 1] != '\\') n--;
    g_dir[n] = 0;

    path[0] = 0;
    scat(path, sizeof path, g_dir);
    scat(path, sizeof path, "msi_wine.dll");
    g_msi = LoadLibraryA(path);
    if (!g_msi) return FALSE;  /* refuse to load rather than run without a real msi */
    for (int i = 0; i < N_EXPORTS; i++)
        g_real[i] = (void *)GetProcAddress(g_msi, g_name[i]);
    if (!g_real[IDX_INSTALL_A] || !g_real[IDX_INSTALL_W]) return FALSE;

    load_config();
    return TRUE;
}
