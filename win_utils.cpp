#include "win_utils.hpp"

// MUST include windows.h BEFORE commdlg.h, and NOT define NOGDI
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <urlmon.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

bool DownloadFile(const std::string& url, const std::string& savePath) {
    HRESULT hr = URLDownloadToFileA(NULL, url.c_str(), savePath.c_str(), 0, NULL);
    return (hr == S_OK);
}

// NOTE: OpenFilePicker, LaunchGame, OpenURL are implemented in qshell.cpp
// These declarations are here for reference only

void OpenWiFiSettings() {
    ShellExecuteA(NULL, "open", "ms-settings:network-wifi", NULL, NULL, SW_SHOWNORMAL);
}

void OpenBluetoothSettings() {
    ShellExecuteA(NULL, "open", "ms-settings:bluetooth", NULL, NULL, SW_SHOWNORMAL);
}

void OpenSoundSettings() {
    ShellExecuteA(NULL, "open", "ms-settings:sound", NULL, NULL, SW_SHOWNORMAL);
}

void OpenDisplaySettings() {
    ShellExecuteA(NULL, "open", "ms-settings:display", NULL, NULL, SW_SHOWNORMAL);
}

void OpenBatterySettings() {
    ShellExecuteA(NULL, "open", "ms-settings:batterysaver", NULL, NULL, SW_SHOWNORMAL);
}

void OpenOnScreenKeyboard() {
    ShellExecuteA(NULL, "open", "osk.exe", NULL, NULL, SW_SHOWNORMAL);
}

void OpenFileExplorer() {
    ShellExecuteA(NULL, "open", "explorer.exe", NULL, NULL, SW_SHOWNORMAL);
}

void OpenSystemSettings() {
    ShellExecuteA(NULL, "open", "ms-settings:", NULL, NULL, SW_SHOWNORMAL);
}

int GetBatteryLevel() {
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        return (int)sps.BatteryLifePercent;
    }
    return 100;
}

bool IsRunningOnBattery() {
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        return sps.ACLineStatus == 0;
    }
    return false;
}