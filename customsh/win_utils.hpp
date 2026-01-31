#ifndef WIN_UTILS_HPP
#define WIN_UTILS_HPP

#include <string>

// File operations
bool DownloadFile(const std::string& url, const std::string& savePath);
std::string OpenFilePicker(bool exeOnly);

// Application launching
void LaunchGame(const std::string& exePath);
void OpenURL(const std::string& url);

// System settings
void OpenWiFiSettings();
void OpenBluetoothSettings();
void OpenSoundSettings();
void OpenDisplaySettings();
void OpenBatterySettings();
void OpenOnScreenKeyboard();
void OpenFileExplorer();
void OpenSystemSettings();

// System info
int GetBatteryLevel();
bool IsRunningOnBattery();

#endif