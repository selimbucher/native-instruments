/*
 * Installs a driver INF on every device with the given hardware id,
 * replacing the driver another INF bound to it before:
 *
 *   nidrvinst USB\VID_17CC&PID_1600&MI_05 C:\windows\inf\maschine-mk3.inf
 *
 * Wine installs a driver for a device only the first time it sees it, so a
 * device NI's own INF got first keeps NI's driver, which Wine can't load.
 * Exit code 0 on success, else the Win32 error.
 */

#include <windows.h>
#include <newdev.h>

void __stdcall entry(void)
{
    WCHAR **argv;
    BOOL reboot;
    int argc;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc != 3) ExitProcess(ERROR_INVALID_PARAMETER);
    if (!UpdateDriverForPlugAndPlayDevicesW(NULL, argv[1], argv[2], INSTALLFLAG_FORCE, &reboot))
        ExitProcess(GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
    ExitProcess(0);
}
