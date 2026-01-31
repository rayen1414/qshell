#include "win_utils.hpp"

// MUST include windows.h BEFORE commdlg.h, and NOT define NOGDI
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

std::string OpenFilePicker(bool exeOnly) {
    char szFile[MAX_PATH] = {0};
    
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    
    if (exeOnly) {
        ofn.lpstrFilter = "Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
        ofn.lpstrTitle = "Select Game Executable";
    } else {
        ofn.lpstrFilter = "Images (*.png;*.jpg;*.jpeg;*.gif;*.bmp)\0*.png;*.jpg;*.jpeg;*.gif;*.bmp\0All Files (*.*)\0*.*\0";
        ofn.lpstrTitle = "Select Image";
    }
    
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    
    if (GetOpenFileNameA(&ofn)) {
        return std::string(szFile);
    }
    return "";
}

void LaunchGame(const std::string& exePath) {
    // Check if it's a URL protocol
    if (exePath.find("://") != std::string::npos) {
        ShellExecuteA(NULL, "open", exePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    
    // Get directory from path
    std::string dir = exePath;
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) {
        dir = dir.substr(0, pos);
    }
    
    ShellExecuteA(NULL, "open", exePath.c_str(), NULL, dir.c_str(), SW_SHOWNORMAL);
}

void OpenURL(const std::string& url) {
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

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