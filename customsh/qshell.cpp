// ============================================================================
// Q-SHELL LAUNCHER v2.5 - COMPLETE FIXED VERSION
// Features: Fixed Task Switcher, Global Hotkeys (Tab+O / Share+X), Pro Design
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// Handle Windows/Raylib conflicts
#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor
#define Rectangle Win32Rectangle
#define LoadImage Win32LoadImage
#define DrawText Win32DrawText
#define DrawTextEx Win32DrawTextEx
#define PlaySound Win32PlaySound

#include <windows.h>
#include <wingdi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <wininet.h>
#include <commdlg.h>
#include <urlmon.h>

// Windows API cursor function wrapper (must be before #undef)
inline int WindowsShowCursor(BOOL bShow) {
    typedef int (WINAPI *ShowCursorFn)(BOOL);
    static ShowCursorFn fn = (ShowCursorFn)GetProcAddress(GetModuleHandleA("user32.dll"), "ShowCursor");
    return fn ? fn(bShow) : 0;
}

#undef CloseWindow
#undef ShowCursor
#undef Rectangle
#undef LoadImage
#undef DrawText
#undef DrawTextEx
#undef PlaySound

#include "raylib.h"
#include "raymath.h"
#include "game_finder.hpp"
#include "system_control.hpp"

#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "comdlg32.lib")

namespace fs = std::filesystem;

// ============================================================================
// XINPUT DYNAMIC LOADING (Fixes missing xinput1_3.dll)
// ============================================================================

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, void*);
static PFN_XInputGetState g_XInputGetState = nullptr;
static HMODULE g_XInputLib = NULL;
static bool g_XInputLoaded = false;

#define XINPUT_GAMEPAD_DPAD_UP          0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN        0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT        0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT       0x0008
#define XINPUT_GAMEPAD_START            0x0010
#define XINPUT_GAMEPAD_BACK             0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB       0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB      0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER    0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER   0x0200
#define XINPUT_GAMEPAD_A                0x1000
#define XINPUT_GAMEPAD_B                0x2000
#define XINPUT_GAMEPAD_X                0x4000
#define XINPUT_GAMEPAD_Y                0x8000

struct XINPUT_GAMEPAD_STRUCT {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XINPUT_STATE_STRUCT {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD_STRUCT Gamepad;
};

void LoadXInput() {
    if (g_XInputLoaded) return;
    
    const char* dllNames[] = {
        "xinput1_4.dll",
        "xinput1_3.dll", 
        "xinput9_1_0.dll",
        "xinput1_2.dll",
        "xinput1_1.dll"
    };
    
    for (const char* dll : dllNames) {
        g_XInputLib = LoadLibraryA(dll);
        if (g_XInputLib) {
            g_XInputGetState = (PFN_XInputGetState)GetProcAddress(g_XInputLib, "XInputGetState");
            if (g_XInputGetState) {
                g_XInputLoaded = true;
                return;
            }
            FreeLibrary(g_XInputLib);
            g_XInputLib = NULL;
        }
    }
    g_XInputLoaded = true; // Mark as tried even if failed
}

DWORD SafeXInputGetState(DWORD dwUserIndex, XINPUT_STATE_STRUCT* pState) {
    if (!g_XInputGetState) return ERROR_DEVICE_NOT_CONNECTED;
    return g_XInputGetState(dwUserIndex, pState);
}

void UnloadXInput() {
    if (g_XInputLib) {
        FreeLibrary(g_XInputLib);
        g_XInputLib = NULL;
        g_XInputGetState = nullptr;
    }
}

// ============================================================================
// GLOBALS
// ============================================================================

bool g_isShellMode = false;
bool g_shouldRestart = false;
bool g_windowOnTop = true;
HWND g_mainWindow = NULL;
std::string g_exeDirectory = "";
std::mutex g_downloadMutex;

// Global Task Switcher Control
std::atomic<bool> g_taskSwitcherRequested(false);
std::atomic<bool> g_appRunning(true);
std::thread g_inputMonitorThread;
HHOOK g_keyboardHook = NULL;
std::mutex g_inputMutex;

// Debounce timing
DWORD g_lastTaskSwitchTime = 0;
const DWORD DEBOUNCE_TIME = 400;

// UI Mode
enum class UIMode { MAIN, TASK_SWITCHER, SHELL_MENU, POWER_MENU };
UIMode g_currentMode = UIMode::MAIN;
std::mutex g_modeMutex;

// Task Switcher State
std::vector<struct RunningTask> g_tasks;
int g_taskFocusIndex = 0;
float g_taskSwitcherSlideIn = 0.0f;
float g_taskSwitcherAnimTime = 0.0f;

// ============================================================================
// DEBUG LOGGING
// ============================================================================

void DebugLog(const std::string& message) {
    static bool firstLog = true;
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);

    std::string logPath = g_exeDirectory.empty() ? "qshell.log" : (g_exeDirectory + "\\qshell.log");
    std::ofstream log(logPath, firstLog ? std::ios::trunc : std::ios::app);
    firstLog = false;
    if (log.is_open()) {
        time_t now = time(nullptr);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
        log << "[" << timeStr << "] " << message << "\n";
        log.flush();
    }
}

// ============================================================================
// PATH MANAGEMENT
// ============================================================================

void SetWorkingDirectoryToExe() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string pathStr = exePath;
    size_t lastSlash = pathStr.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        g_exeDirectory = pathStr.substr(0, lastSlash);
        SetCurrentDirectoryA(g_exeDirectory.c_str());
    }
    SetEnvironmentVariableA("QSHELL_DIR", g_exeDirectory.c_str());
}

std::string GetFullPath(const std::string& relativePath) {
    if (relativePath.empty()) return "";
    if (relativePath.length() > 2 && relativePath[1] == ':') return relativePath;
    return g_exeDirectory + "\\" + relativePath;
}

// ============================================================================
// STRUCTS
// ============================================================================

struct UIGame {
    GameInfo info;
    Texture2D poster;
    bool hasPoster;
    float detailAlpha;
    float selectAnim;
};

struct UserProfile {
    std::string username = "Player";
    std::string avatarPath;
    Texture2D avatar = {0};
    bool hasAvatar = false;
};

struct RunningTask {
    std::string name;
    std::string windowTitle;
    HWND hwnd;
    DWORD processId;
    bool isQShell;
    HICON hIcon;
};

struct MediaApp {
    std::string name;
    std::string url;
    std::string imagePath;
    Texture2D texture = {0};
    bool hasTexture = false;
    Color accentColor;
};

struct ShareOption {
    std::string name;
    std::string description;
    Color accentColor;
};

enum class StartupChoice { NONE, NORMAL_APP, SHELL_MODE, EXIT_SHELL };
enum class ShellAction { NONE, EXPLORER, KEYBOARD, SETTINGS, TASKMGR, RESTART_SHELL, EXIT_SHELL, POWER };
enum class PowerChoice { NONE, RESTART, SHUTDOWN, SLEEP, CANCEL };

// ============================================================================
// TASK LIST MANAGEMENT
// ============================================================================

BOOL CALLBACK EnumWindowsForTasks(HWND hwnd, LPARAM lParam) {
    auto* tasks = reinterpret_cast<std::vector<RunningTask>*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowTextLengthA(hwnd) == 0) return TRUE;

    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

    char title[512];
    GetWindowTextA(hwnd, title, 512);
    std::string titleStr(title);

    // Filter out system windows and Q-Shell
    if (titleStr == "Program Manager" || 
        titleStr == "Windows Input Experience" ||
        titleStr.find("Q-Shell") != std::string::npos ||
        titleStr.empty()) return TRUE;

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    char processName[MAX_PATH] = "App";
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (hProcess) {
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameA(hProcess, 0, processName, &size)) {
            std::string fullPath = processName;
            size_t pos = fullPath.find_last_of("\\/");
            if (pos != std::string::npos) strcpy(processName, fullPath.substr(pos + 1).c_str());
        }
        CloseHandle(hProcess);
    }

    HICON hIcon = (HICON)SendMessage(hwnd, WM_GETICON, ICON_BIG, 0);
    if (!hIcon) hIcon = (HICON)SendMessage(hwnd, WM_GETICON, ICON_SMALL, 0);
    if (!hIcon) hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICON);

    RunningTask task;
    task.name = processName;
    task.windowTitle = titleStr;
    task.hwnd = hwnd;
    task.processId = processId;
    task.isQShell = false;
    task.hIcon = hIcon;

    tasks->push_back(task);
    return TRUE;
}

void RefreshTaskList() {
    g_tasks.clear();
    EnumWindows(EnumWindowsForTasks, reinterpret_cast<LPARAM>(&g_tasks));
    DebugLog("Refreshed task list: " + std::to_string(g_tasks.size()) + " tasks");
}

// ============================================================================
// GLOBAL INPUT MONITORING
// ============================================================================

static bool g_tabDown = false;
static bool g_oDown = false;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kbStruct = (KBDLLHOOKSTRUCT*)lParam;

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            if (kbStruct->vkCode == VK_TAB) g_tabDown = true;
            if (kbStruct->vkCode == 'O') g_oDown = true;
            
            // Tab + O = Task Switcher
            if (g_tabDown && g_oDown) {
                DWORD now = GetTickCount();
                if (now - g_lastTaskSwitchTime > DEBOUNCE_TIME) {
                    std::lock_guard<std::mutex> lock(g_modeMutex);
                    if (g_currentMode == UIMode::MAIN) {
                        g_taskSwitcherRequested = true;
                        g_lastTaskSwitchTime = now;
                        DebugLog("Global Hotkey: Tab+O pressed");
                    }
                }
            }
        }
        
        if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            if (kbStruct->vkCode == VK_TAB) g_tabDown = false;
            if (kbStruct->vkCode == 'O') g_oDown = false;
        }
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

void InputMonitorThread() {
    DebugLog("Input monitor thread started");
    
    g_keyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    if (g_keyboardHook) {
        DebugLog("Keyboard hook installed");
    } else {
        DebugLog("ERROR: Failed to install keyboard hook");
    }

    bool wasControllerPressed = false;
    
    while (g_appRunning) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        // XInput monitoring (if available)
        if (g_XInputGetState) {
            for (DWORD i = 0; i < 4; i++) {
                XINPUT_STATE_STRUCT state;
                ZeroMemory(&state, sizeof(XINPUT_STATE_STRUCT));
                
                if (SafeXInputGetState(i, &state) == ERROR_SUCCESS) {
                    WORD buttons = state.Gamepad.wButtons;
                    
                    // Share + X (View + X on Xbox controller) OR Start + Back
                    bool controllerPressed = ((buttons & XINPUT_GAMEPAD_BACK) && (buttons & XINPUT_GAMEPAD_X)) ||
                                             ((buttons & XINPUT_GAMEPAD_START) && (buttons & XINPUT_GAMEPAD_BACK));
                    
                    DWORD now = GetTickCount();
                    
                    if (controllerPressed && !wasControllerPressed) {
                        if (now - g_lastTaskSwitchTime > DEBOUNCE_TIME) {
                            std::lock_guard<std::mutex> lock(g_modeMutex);
                            if (g_currentMode == UIMode::MAIN) {
                                g_taskSwitcherRequested = true;
                                g_lastTaskSwitchTime = now;
                                DebugLog("Controller: Task Switcher triggered");
                            }
                        }
                    }
                    
                    wasControllerPressed = controllerPressed;
                    break;
                }
            }
        }
        
        Sleep(10);
    }

    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = NULL;
    }
    
    DebugLog("Input monitor thread stopped");
}

void StartInputMonitoring() {
    LoadXInput();
    g_appRunning = true;
    g_inputMonitorThread = std::thread(InputMonitorThread);
}

void StopInputMonitoring() {
    g_appRunning = false;
    if (g_inputMonitorThread.joinable()) {
        g_inputMonitorThread.join();
    }
    UnloadXInput();
}

// ============================================================================
// INPUT ADAPTER
// ============================================================================

class InputAdapter {
public:
    int gamepadID = -1;
    float stickTimer = 0.0f;
    const float STICK_DELAY = 0.18f;
    const float DEADZONE = 0.5f;

    void Update() {
        if (stickTimer > 0) stickTimer -= GetFrameTime();
        
        gamepadID = -1;
        for (int i = 0; i < 4; i++) {
            if (IsGamepadAvailable(i)) {
                gamepadID = i;
                break;
            }
        }
    }

    bool IsMoveDown() {
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) return true;
        if (gamepadID >= 0) {
            float axisY = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_Y);
            if (axisY > DEADZONE && stickTimer <= 0) { stickTimer = STICK_DELAY; return true; }
        }
        return false;
    }

    bool IsMoveUp() {
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_LEFT_FACE_UP)) return true;
        if (gamepadID >= 0) {
            float axisY = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_Y);
            if (axisY < -DEADZONE && stickTimer <= 0) { stickTimer = STICK_DELAY; return true; }
        }
        return false;
    }

    bool IsMoveLeft() {
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_LEFT_FACE_LEFT)) return true;
        if (gamepadID >= 0) {
            float axisX = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_X);
            if (axisX < -DEADZONE && stickTimer <= 0) { stickTimer = STICK_DELAY; return true; }
        }
        return false;
    }

    bool IsMoveRight() {
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) return true;
        if (gamepadID >= 0) {
            float axisX = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_X);
            if (axisX > DEADZONE && stickTimer <= 0) { stickTimer = STICK_DELAY; return true; }
        }
        return false;
    }

    bool IsConfirm() {
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) return true;
        return false;
    }

    bool IsBack() {
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_ESCAPE)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)) return true;
        return false;
    }

    bool IsChangeArt() {
        if (IsKeyPressed(KEY_Y)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_UP)) return true;
        return false;
    }

    bool IsDeleteDown() {
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_H)) return true;
        if (gamepadID >= 0 && IsGamepadButtonDown(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)) return true;
        return false;
    }

    bool IsDeleteReleased() {
        if (IsKeyReleased(KEY_X) || IsKeyReleased(KEY_H)) return true;
        if (gamepadID >= 0 && IsGamepadButtonReleased(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)) return true;
        return false;
    }

    bool IsDeletePressed() {
        if (IsKeyPressed(KEY_X)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)) return true;
        return false;
    }

    bool IsLB() {
        if (IsKeyPressed(KEY_Q) || IsKeyPressed(KEY_PAGE_UP)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_LEFT_TRIGGER_1)) return true;
        return false;
    }

    bool IsRB() {
        if (IsKeyPressed(KEY_E) || IsKeyPressed(KEY_PAGE_DOWN)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) return true;
        return false;
    }

    bool IsMenu() {
        if (IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_F1)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_MIDDLE_RIGHT)) return true;
        return false;
    }

    bool IsView() {
        if (IsKeyPressed(KEY_F2)) return true;
        if (gamepadID >= 0 && IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_MIDDLE_LEFT)) return true;
        return false;
    }

    bool IsBackgroundKey() { return IsKeyPressed(KEY_B); }
};

// ============================================================================
// WINDOW MANAGEMENT
// ============================================================================

void BringWindowToFront(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    
    DWORD currentThread = GetCurrentThreadId();
    DWORD targetThread = GetWindowThreadProcessId(hwnd, NULL);
    AttachThreadInput(currentThread, targetThread, TRUE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
    SetActiveWindow(hwnd);
    AttachThreadInput(currentThread, targetThread, FALSE);
}

void MakeQShellNotTopmost() {
    if (g_mainWindow) {
        SetWindowPos(g_mainWindow, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        g_windowOnTop = false;
    }
}

void MakeQShellTopmost() {
    if (g_mainWindow && g_isShellMode) {
        SetWindowPos(g_mainWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(g_mainWindow);
        g_windowOnTop = true;
    }
}

void SwitchToTask(int index) {
    if (index < 0 || index >= (int)g_tasks.size()) return;
    HWND hwnd = g_tasks[index].hwnd;
    if (!IsWindow(hwnd)) return;
    
    DebugLog("Switching to task: " + g_tasks[index].windowTitle);
    
    if (g_isShellMode) {
        MakeQShellNotTopmost();
    } else if (g_mainWindow) {
        ShowWindow(g_mainWindow, SW_MINIMIZE);
    }
    
    Sleep(100);
    BringWindowToFront(hwnd);
}

void LaunchGame(const std::string& exePath) {
    DebugLog("Launching: " + exePath);
    MakeQShellNotTopmost();

    if (exePath.find("://") != std::string::npos) {
        ShellExecuteA(NULL, "open", exePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else {
        std::string dir = exePath;
        size_t pos = dir.find_last_of("\\/");
        if (pos != std::string::npos) dir = dir.substr(0, pos);
        
        SHELLEXECUTEINFOA sei = {0};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = "open";
        sei.lpFile = exePath.c_str();
        sei.lpDirectory = dir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        
        if (ShellExecuteExA(&sei)) {
            Sleep(500);
            if (sei.hProcess) CloseHandle(sei.hProcess);
        }
    }
}

void OpenURL(const std::string& url) {
    DebugLog("Opening URL: " + url);
    MakeQShellNotTopmost();
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
    Sleep(300);
}

// ============================================================================
// FILE UTILITIES
// ============================================================================

std::string OpenFilePicker(bool exeOnly) {
    char szFile[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_mainWindow;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = exeOnly ? "Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0" 
                              : "Images (*.png;*.jpg;*.jpeg;*.gif;*.bmp)\0*.png;*.jpg;*.jpeg;*.gif;*.bmp\0All Files (*.*)\0*.*\0";
    ofn.lpstrTitle = exeOnly ? "Select Game Executable" : "Select Image";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return std::string(szFile);
    return "";
}

void DownloadFileAsync(const std::string& url, const std::string& dest) {
    std::thread([url, dest]() {
        URLDownloadToFileA(NULL, url.c_str(), dest.c_str(), 0, NULL);
    }).detach();
}

// ============================================================================
// CRASH RECOVERY
// ============================================================================

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exInfo) {
    DebugLog("!!! CRASH - Launching explorer !!!");
    StopInputMonitoring();
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    char cmd[] = "explorer.exe";
    CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);
    return EXCEPTION_EXECUTE_HANDLER;
}

void CreateEmergencyRestoreBatch() {
    std::string backupDir = GetFullPath("backup");
    fs::create_directories(backupDir);
    std::string batPath = backupDir + "\\EMERGENCY_RESTORE.bat";
    std::ofstream bat(batPath);
    bat << "@echo off\n";
    bat << "color 0C\n";
    bat << "title Q-SHELL EMERGENCY RESTORE\n";
    bat << "echo.\n";
    bat << "echo  Q-SHELL EMERGENCY RESTORE\n";
    bat << "echo.\n";
    bat << "pause > nul\n";
    bat << "reg delete \"HKCU\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v Shell /f 2>nul\n";
    bat << "reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v Shell /t REG_SZ /d \"explorer.exe\" /f 2>nul\n";
    bat << "start explorer.exe\n";
    bat << "echo RESTORE COMPLETE!\n";
    bat << "pause\n";
    bat.close();
    DebugLog("Created emergency restore: " + batPath);
}

// ============================================================================
// PROFILE MANAGEMENT
// ============================================================================

void SaveProfile(const std::vector<UIGame>& library, const std::string& bgPath, const UserProfile& profile) {
    std::string profileDir = GetFullPath("profile");
    fs::create_directories(profileDir);
    
    std::ofstream cfg(profileDir + "\\config.txt");
    if (cfg.is_open()) {
        cfg << bgPath << "\n" << profile.username << "\n" << profile.avatarPath << "\n";
    }
    cfg.close();

    std::ofstream libFile(profileDir + "\\library.txt");
    if (libFile.is_open()) {
        for (const auto& g : library) {
            libFile << g.info.name << "|" << g.info.exePath << "|" << g.info.platform << "|" << g.info.appId << "\n";
        }
    }
    libFile.close();
}

void LoadProfile(std::string& bgPath, UserProfile& profile) {
    std::string configPath = GetFullPath("profile\\config.txt");
    if (fs::exists(configPath)) {
        std::ifstream cfg(configPath);
        if (cfg.is_open()) {
            std::getline(cfg, bgPath);
            std::getline(cfg, profile.username);
            std::getline(cfg, profile.avatarPath);
        }
        cfg.close();
    }
    if (profile.username.empty()) profile.username = "Player";
}

void RefreshLibrary(std::vector<UIGame>& library, const std::string& bgPath, const UserProfile& profile) {
    std::vector<GameInfo> scanned = GetInstalledGames();
    bool foundNew = false;
    for (auto& s : scanned) {
        bool exists = false;
        for (const auto& lib : library) {
            if (lib.info.exePath == s.exePath) { exists = true; break; }
        }
        if (!exists) {
            library.push_back({ s, {0}, false, 0.0f, 0.0f });
            foundNew = true;
        }
    }
    if (foundNew) SaveProfile(library, bgPath, profile);
    DebugLog("Library refreshed: " + std::to_string(library.size()) + " games");
}

void PhysicallyUninstall(UIGame& game) {
    if (game.info.platform == "Steam" && !game.info.appId.empty()) {
        std::string cmd = "steam://uninstall/" + game.info.appId;
        ShellExecuteA(NULL, "open", cmd.c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else {
        try {
            fs::path p(game.info.exePath);
            if (fs::exists(p)) fs::remove_all(p.parent_path());
        } catch (...) {}
    }
}

void LoadMediaAppTextures(std::vector<MediaApp>& apps) {
    for (auto& app : apps) {
        if (!app.imagePath.empty() && !app.hasTexture) {
            std::string fullPath = GetFullPath("profile\\" + app.imagePath);
            if (!fs::exists(fullPath)) fullPath = GetFullPath("img\\" + app.imagePath);
            if (fs::exists(fullPath)) {
                app.texture = LoadTexture(fullPath.c_str());
                app.hasTexture = (app.texture.id > 0);
            }
        }
    }
}

// ============================================================================
// VIDEO PLAYBACK FOR BOOT SCREEN (MCI-based)
// ============================================================================

bool PlayBootVideoMCI(const std::string& videoPath, int screenWidth, int screenHeight) {
    std::string fullPath = GetFullPath(videoPath);
    if (!fs::exists(fullPath)) {
        DebugLog("Boot video not found: " + fullPath);
        return false;
    }
    
    DebugLog("Playing boot video (MCI): " + fullPath);
    
    // Create fullscreen window
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "QShellMCIVideo";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    
    HWND videoWnd = CreateWindowExA(
        WS_EX_TOPMOST,
        "QShellMCIVideo",
        "",
        WS_POPUP,
        0, 0, screenWidth, screenHeight,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );
    
    ShowWindow(videoWnd, SW_SHOW);
    UpdateWindow(videoWnd);
    SetForegroundWindow(videoWnd);
    
    // Hide cursor using Win32 API
    while (WindowsShowCursor(FALSE) >= 0);
    
    // Open video with MCI
    std::string openCmd = "open \"" + fullPath + "\" type mpegvideo alias bootvid";
    MCIERROR err = mciSendStringA(openCmd.c_str(), NULL, 0, NULL);
    
    if (err != 0) {
        // Try without type specification
        openCmd = "open \"" + fullPath + "\" alias bootvid";
        err = mciSendStringA(openCmd.c_str(), NULL, 0, NULL);
    }
    
    if (err == 0) {
        // Set window
        char buf[512];
        snprintf(buf, sizeof(buf), "window bootvid handle %lld", (long long)(intptr_t)videoWnd);
        mciSendStringA(buf, NULL, 0, NULL);
        
        // Set display area
        snprintf(buf, sizeof(buf), "put bootvid window at 0 0 %d %d", screenWidth, screenHeight);
        mciSendStringA(buf, NULL, 0, NULL);
        
        // Get video length
        char lenBuf[64] = {0};
        mciSendStringA("status bootvid length", lenBuf, sizeof(lenBuf), NULL);
        DWORD videoLength = atoi(lenBuf);
        if (videoLength == 0) videoLength = 10000; // Default 10 seconds
        
        // Play video
        mciSendStringA("play bootvid", NULL, 0, NULL);
        
        // Wait for completion or skip
        DWORD startTime = GetTickCount();
        bool done = false;
        
        while (!done) {
            // Check if video finished
            char statusBuf[64] = {0};
            mciSendStringA("status bootvid mode", statusBuf, sizeof(statusBuf), NULL);
            if (strcmp(statusBuf, "stopped") == 0) {
                done = true;
                break;
            }
            
            // Timeout check
            if (GetTickCount() - startTime > videoLength + 2000) {
                done = true;
                break;
            }
            
            // Check for skip input
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_KEYDOWN) {
                    if (msg.wParam == VK_RETURN || msg.wParam == VK_SPACE || 
                        msg.wParam == VK_ESCAPE) {
                        done = true;
                        break;
                    }
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            
            // Gamepad skip
            if (g_XInputGetState) {
                for (DWORD i = 0; i < 4; i++) {
                    XINPUT_STATE_STRUCT state;
                    ZeroMemory(&state, sizeof(state));
                    if (SafeXInputGetState(i, &state) == ERROR_SUCCESS) {
                        if (state.Gamepad.wButtons & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) {
                            done = true;
                            break;
                        }
                    }
                }
            }
            
            Sleep(16);
        }
        
        // Stop and close
        mciSendStringA("stop bootvid", NULL, 0, NULL);
        mciSendStringA("close bootvid", NULL, 0, NULL);
    } else {
        char errBuf[256];
        mciGetErrorStringA(err, errBuf, sizeof(errBuf));
        DebugLog("MCI Error: " + std::string(errBuf));
    }
    
    // Cleanup
    DestroyWindow(videoWnd);
    UnregisterClassA("QShellMCIVideo", GetModuleHandle(NULL));
    
    // Show cursor again
    while (WindowsShowCursor(TRUE) < 0);
    
    DebugLog("Boot video playback finished");
    return (err == 0);
}

// ============================================================================
// BOOT SCREEN
// ============================================================================

void ShowBootScreen() {
    DebugLog("ShowBootScreen starting...");

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    if (screenWidth <= 0) screenWidth = 1920;
    if (screenHeight <= 0) screenHeight = 1080;

    // Try to play video first
    const char* videoPaths[] = {
        "profile\\intro\\boot.mp4",
        "profile\\intro\\intro.mp4",
        "profile\\intro\\boot.avi",
        "profile\\intro\\intro.avi",
        "profile\\intro\\boot.wmv",
        "profile\\intro\\intro.wmv"
    };

    for (const char* path : videoPaths) {
        std::string fullPath = GetFullPath(path);
        DebugLog("Checking for video: " + fullPath);
        if (fs::exists(fullPath)) {
            DebugLog("Found video file: " + fullPath);
            
            // Make sure XInput is loaded for gamepad skip
            LoadXInput();
            
            if (PlayBootVideoMCI(path, screenWidth, screenHeight)) {
                DebugLog("Video played successfully");
                return;
            }
            
            DebugLog("Failed to play video, trying next...");
        }
    }

    DebugLog("No video found, showing logo animation");

    // Fall back to logo/text animation
    SetConfigFlags(FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TOPMOST | FLAG_VSYNC_HINT);
    InitWindow(screenWidth, screenHeight, "Q-Shell Boot");
    SetWindowPosition(0, 0);
    SetTargetFPS(60);
    HideCursor();

    HWND bootHwnd = (HWND)GetWindowHandle();
    if (bootHwnd) {
        SetWindowPos(bootHwnd, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_SHOWWINDOW);
        SetForegroundWindow(bootHwnd);
    }

    // Try to load logo image
    Texture2D logoTex = {0};
    bool hasLogo = false;
    const char* imgPaths[] = { 
        "profile\\intro\\logo.png", 
        "profile\\intro\\intro.png", 
        "profile\\intro\\boot.png",
        "img\\logo.png" 
    };
    
    for (const char* path : imgPaths) {
        std::string fullPath = GetFullPath(path);
        if (fs::exists(fullPath)) {
            logoTex = LoadTexture(fullPath.c_str());
            if (logoTex.id > 0) { 
                hasLogo = true; 
                DebugLog("Loaded logo: " + fullPath);
                break; 
            }
        }
    }

    float elapsed = 0.0f;
    float duration = 3.5f;
    float glowPhase = 0.0f;

    while (!WindowShouldClose() && elapsed < duration) {
        elapsed += GetFrameTime();
        glowPhase += GetFrameTime() * 2.0f;
        
        // Skip on input
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ESCAPE)) break;
        for (int i = 0; i < 4; i++) {
            if (IsGamepadAvailable(i) && (IsGamepadButtonPressed(i, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
                IsGamepadButtonPressed(i, GAMEPAD_BUTTON_MIDDLE_RIGHT))) {
                elapsed = duration;
                break;
            }
        }
        
        // Fade in/out
        float alpha = 1.0f;
        if (elapsed < 0.5f) alpha = elapsed / 0.5f;
        if (elapsed > duration - 0.5f) alpha = (duration - elapsed) / 0.5f;
        alpha = Clamp(alpha, 0.0f, 1.0f);

        BeginDrawing();
        ClearBackground(BLACK);
        
        if (hasLogo && logoTex.id > 0) {
            float scale = fminf((float)screenWidth / logoTex.width, (float)screenHeight / logoTex.height) * 0.5f;
            float x = (screenWidth - logoTex.width * scale) / 2;
            float y = (screenHeight - logoTex.height * scale) / 2;
            
            float glow = (sinf(glowPhase) + 1.0f) / 2.0f * 0.3f;
            DrawTextureEx(logoTex, {x - 4, y - 4}, 0, scale * 1.02f, Fade(SKYBLUE, alpha * glow));
            DrawTextureEx(logoTex, {x, y}, 0, scale, Fade(WHITE, alpha));
        } else {
            const char* logo = "Q-SHELL";
            int fontSize = 120;
            int textW = MeasureText(logo, fontSize);
            
            for (int i = 8; i >= 0; i--) {
                float glowAlpha = alpha * 0.08f * (8 - i) * (0.5f + sinf(glowPhase + i * 0.2f) * 0.5f);
                DrawText(logo, screenWidth/2 - textW/2 + i*2, screenHeight/2 - fontSize/2 + i*2, 
                    fontSize, Fade(SKYBLUE, glowAlpha));
            }
            DrawText(logo, screenWidth/2 - textW/2, screenHeight/2 - fontSize/2, fontSize, Fade(WHITE, alpha));
            
            const char* sub = "GAMING CONSOLE";
            int subW = MeasureText(sub, 24);
            DrawText(sub, screenWidth/2 - subW/2, screenHeight/2 + 70, 24, Fade(GRAY, alpha * 0.8f));
            
            int dots = ((int)(elapsed * 3)) % 4;
            std::string dotStr(dots, '.');
            int dotW = MeasureText(dotStr.c_str(), 40);
            DrawText(dotStr.c_str(), screenWidth/2 - dotW/2, screenHeight/2 + 120, 40, Fade(WHITE, alpha * 0.6f));
        }
        
        DrawText("v2.5", screenWidth - 60, screenHeight - 30, 16, Fade(GRAY, alpha * 0.3f));
        
        EndDrawing();
    }

    if (hasLogo && logoTex.id > 0) UnloadTexture(logoTex);
    CloseWindow();

    DebugLog("Boot screen finished");
}


// ============================================================================
// DRAWING HELPERS
// ============================================================================

void DrawCircularAvatar(Vector2 center, float radius, Texture2D avatar, bool hasAvatar, const std::string& username) {
    if (hasAvatar && avatar.id > 0) {
        float scale = (radius * 2) / (float)avatar.width;
        DrawTextureEx(avatar, {center.x - radius, center.y - radius}, 0, scale, WHITE);
    } else {
        DrawCircleGradient((int)center.x, (int)center.y, radius, {70, 75, 95, 255}, {45, 48, 62, 255});
        char initial[2] = {username.empty() ? 'P' : (char)toupper(username[0]), '\0'};
        int fontSize = (int)(radius * 1.1f);
        int textW = MeasureText(initial, fontSize);
        DrawText(initial, (int)(center.x - textW/2), (int)(center.y - fontSize/2), fontSize, WHITE);
    }
    DrawCircleLines((int)center.x, (int)center.y, radius, Fade(WHITE, 0.5f));
}

void DrawGameCard(Rectangle card, UIGame& game, bool focused, float time) {
    DrawRectangleRounded({card.x + 5, card.y + 5, card.width, card.height}, 0.05f, 12, Fade(BLACK, 0.25f));
    DrawRectangleRounded(card, 0.05f, 12, {35, 35, 40, 255});
    float alpha = focused ? 1.0f : 0.25f;

    if (game.hasPoster && game.poster.id > 0) {
        float texAspect = (float)game.poster.width / (float)game.poster.height;
        float cardAspect = card.width / card.height;
        Rectangle source = {0, 0, (float)game.poster.width, (float)game.poster.height};
        if (texAspect > cardAspect) {
            source.width = game.poster.height * cardAspect;
            source.x = (game.poster.width - source.width) / 2;
        } else {
            source.height = game.poster.width / cardAspect;
            source.y = (game.poster.height - source.height) / 2;
        }
        DrawTexturePro(game.poster, source, card, {0, 0}, 0, Fade(WHITE, alpha));
    } else {
        char initial[2] = {game.info.name.empty() ? '?' : (char)toupper(game.info.name[0]), '\0'};
        int initW = MeasureText(initial, 80);
        DrawText(initial, (int)(card.x + card.width/2 - initW/2), (int)(card.y + card.height/2 - 40), 80, Fade(WHITE, alpha * 0.2f));
    }

    if (focused) {
        float pulse = (sinf(time * 4) + 1) / 2;
        DrawRectangleRoundedLinesEx(card, 0.05f, 12, 4.0f, Fade(WHITE, 0.4f + pulse * 0.4f));
    }
}

void DrawMediaCard(Rectangle rect, MediaApp& app, bool focused, float time) {
    float scale = focused ? 1.04f : 1.0f;
    float lift = focused ? 8.0f : 0.0f;
    Rectangle scaled = {
        rect.x - (rect.width * (scale - 1.0f) / 2),
        rect.y - (rect.height * (scale - 1.0f) / 2) - lift,
        rect.width * scale, rect.height * scale
    };

    DrawRectangleRounded({scaled.x + 4, scaled.y + 6, scaled.width, scaled.height}, 0.1f, 12, Fade(BLACK, focused ? 0.4f : 0.2f));
    DrawRectangleRounded(scaled, 0.1f, 12, {24, 26, 34, 255});
    
    Rectangle imageRect = {scaled.x, scaled.y, scaled.width, scaled.height * 0.65f};
    if (app.hasTexture && app.texture.id > 0) {
        DrawTexturePro(app.texture, {0, 0, (float)app.texture.width, (float)app.texture.height}, imageRect, {0, 0}, 0, WHITE);
    } else {
        DrawRectangleGradientV((int)imageRect.x, (int)imageRect.y, (int)imageRect.width, (int)imageRect.height,
            ColorBrightness(app.accentColor, -0.15f), ColorBrightness(app.accentColor, -0.45f));
        char initial[2] = {app.name[0], '\0'};
        int textW = MeasureText(initial, 50);
        DrawText(initial, (int)(imageRect.x + (imageRect.width - textW)/2), (int)(imageRect.y + (imageRect.height - 50)/2), 50, Fade(WHITE, 0.25f));
    }

    float contentY = scaled.y + scaled.height * 0.68f;
    DrawRectangle((int)(scaled.x + 15), (int)(contentY + 5), 30, 3, app.accentColor);
    DrawText(app.name.c_str(), (int)(scaled.x + 15), (int)(contentY + 12), 18, WHITE);

    if (focused) {
        float pulse = (sinf(time * 4.5f) + 1.0f) / 2.0f;
        DrawRectangleRoundedLines(scaled, 0.1f, 12, Fade(app.accentColor, 0.5f + pulse * 0.4f));
    }
}

void DrawSettingsTile(Rectangle rect, const char* icon, const char* title, Color accent, bool focused, float time) {
    float scale = focused ? 1.04f : 1.0f;
    Rectangle scaled = {
        rect.x - (rect.width * (scale - 1.0f) / 2),
        rect.y - (rect.height * (scale - 1.0f) / 2),
        rect.width * scale, rect.height * scale
    };

    DrawRectangleRounded({scaled.x + 4, scaled.y + 4, scaled.width, scaled.height}, 0.15f, 12, Fade(BLACK, focused ? 0.3f : 0.18f));
    Color bg = focused ? Color{40, 44, 56, 255} : Color{26, 28, 38, 255};
    DrawRectangleRounded(scaled, 0.15f, 12, bg);

    int iconW = MeasureText(icon, 42);
    DrawText(icon, (int)(scaled.x + (scaled.width - iconW)/2), (int)(scaled.y + scaled.height * 0.28f), 42, focused ? accent : Fade(accent, 0.5f));
    int titleW = MeasureText(title, 16);
    DrawText(title, (int)(scaled.x + (scaled.width - titleW)/2), (int)(scaled.y + scaled.height * 0.7f), 16, focused ? WHITE : Fade(WHITE, 0.55f));

    if (focused) {
        float pulse = (sinf(time * 4) + 1.0f) / 2.0f;
        DrawRectangleRoundedLines(scaled, 0.15f, 12, Fade(accent, 0.35f + pulse * 0.3f));
    }
}

// ============================================================================
// TASK SWITCHER OVERLAY
// ============================================================================

bool HandleTaskSwitcherOverlay(int screenWidth, int screenHeight, InputAdapter& input, float deltaTime) {
    g_taskSwitcherAnimTime += deltaTime;
    g_taskSwitcherSlideIn = Lerp(g_taskSwitcherSlideIn, 1.0f, 0.12f);
    
    int cols = std::max(2, std::min(4, (screenWidth - 100) / 350));
    
    // Navigation
    if (input.IsMoveLeft()) g_taskFocusIndex = std::max(0, g_taskFocusIndex - 1);
    if (input.IsMoveRight()) g_taskFocusIndex = std::min((int)g_tasks.size() - 1, g_taskFocusIndex + 1);
    if (input.IsMoveUp()) g_taskFocusIndex = std::max(0, g_taskFocusIndex - cols);
    if (input.IsMoveDown()) g_taskFocusIndex = std::min((int)g_tasks.size() - 1, g_taskFocusIndex + cols);
    
    // Select task
    if (input.IsConfirm() && !g_tasks.empty()) {
        SwitchToTask(g_taskFocusIndex);
        g_currentMode = UIMode::MAIN;
        g_taskSwitcherSlideIn = 0.0f;
        return true;
    }
    
    // Cancel
    if (input.IsBack()) {
        g_currentMode = UIMode::MAIN;
        g_taskSwitcherSlideIn = 0.0f;
        return true;
    }
    
    // Close task with X
    if (input.IsDeletePressed() && g_taskFocusIndex < (int)g_tasks.size()) {
        PostMessage(g_tasks[g_taskFocusIndex].hwnd, WM_CLOSE, 0, 0);
        Sleep(100);
        RefreshTaskList();
        g_taskFocusIndex = Clamp(g_taskFocusIndex, 0, std::max(0, (int)g_tasks.size() - 1));
        if (g_tasks.empty()) {
            g_currentMode = UIMode::MAIN;
            g_taskSwitcherSlideIn = 0.0f;
            return true;
        }
    }
    
    float slideIn = g_taskSwitcherSlideIn;
    
    // Draw overlay
    DrawRectangle(0, 0, screenWidth, screenHeight, Fade(BLACK, 0.88f * slideIn));
    for (int i = 0; i < 200; i++) {
        float alpha = (1.0f - (float)i / 200.0f) * 0.15f * slideIn;
        DrawRectangle(0, i, screenWidth, 1, Fade(SKYBLUE, alpha));
    }
    
    const char* title = "RUNNING APPLICATIONS";
    int titleW = MeasureText(title, 40);
    float titleY = 60 - (1.0f - slideIn) * 50;
    DrawText(title, screenWidth/2 - titleW/2, (int)titleY, 40, Fade(WHITE, slideIn));
    DrawRectangle((int)(screenWidth/2 - 100 * slideIn), (int)(titleY + 50), (int)(200 * slideIn), 4, Fade(SKYBLUE, slideIn));
    
    const char* subtitle = TextFormat("%d apps running", (int)g_tasks.size());
    int subW = MeasureText(subtitle, 18);
    DrawText(subtitle, screenWidth/2 - subW/2, (int)(titleY + 65), 18, Fade(GRAY, slideIn * 0.7f));
    
    if (g_tasks.empty()) {
        const char* noTasks = "No other applications running";
        int noTasksW = MeasureText(noTasks, 24);
        DrawText(noTasks, screenWidth/2 - noTasksW/2, screenHeight/2, 24, Fade(GRAY, slideIn));
        DrawText("Press [B] to return", screenWidth/2 - 80, screenHeight/2 + 40, 18, Fade(WHITE, slideIn * 0.5f));
    } else {
        float cardW = 320, cardH = 220, gap = 25;
        int maxCards = std::min((int)g_tasks.size(), 12);
        float gridWidth = cols * cardW + (cols - 1) * gap;
        float startX = (screenWidth - gridWidth) / 2;
        float startY = 150;
        
        for (int i = 0; i < maxCards; i++) {
            int row = i / cols;
            int col = i % cols;
            float cardX = startX + col * (cardW + gap);
            float cardY = startY + row * (cardH + gap);
            
            float delay = i * 0.04f;
            float cardSlide = Clamp((slideIn - delay) * 2.5f, 0.0f, 1.0f);
            cardY += (1.0f - cardSlide) * 40;
            float cardAlpha = cardSlide;
            
            bool selected = (i == g_taskFocusIndex);
            RunningTask& task = g_tasks[i];
            
            float scale = selected ? 1.03f : 1.0f;
            float scaledW = cardW * scale, scaledH = cardH * scale;
            float scaledX = cardX - (scaledW - cardW) / 2;
            float scaledY = cardY - (scaledH - cardH) / 2 - (selected ? 5 : 0);
            
            DrawRectangleRounded({scaledX + 6, scaledY + 8, scaledW, scaledH}, 0.08f, 12, Fade(BLACK, 0.4f * cardAlpha));
            Color bgCol = selected ? Color{55, 60, 80, 255} : Color{32, 36, 48, 255};
            DrawRectangleRounded({scaledX, scaledY, scaledW, scaledH}, 0.08f, 12, Fade(bgCol, cardAlpha));
            
            if (selected) {
                float pulse = (sinf(g_taskSwitcherAnimTime * 4.5f) + 1) / 2;
                DrawRectangleRoundedLinesEx({scaledX - 2, scaledY - 2, scaledW + 4, scaledH + 4}, 0.08f, 12, 3.0f, Fade(SKYBLUE, (0.5f + pulse * 0.5f) * cardAlpha));
            }
            
            Rectangle iconRect = {scaledX + 20, scaledY + 25, 70, 70};
            DrawRectangleRounded(iconRect, 0.2f, 8, Fade({65, 70, 90, 255}, cardAlpha));
            char initial = task.name.empty() ? '?' : toupper(task.name[0]);
            char initStr[2] = {initial, 0};
            int initW = MeasureText(initStr, 36);
            DrawText(initStr, (int)(iconRect.x + 35 - initW/2), (int)(iconRect.y + 17), 36, Fade(selected ? SKYBLUE : WHITE, cardAlpha * 0.8f));
            
            std::string name = task.name;
            if (name.length() > 4 && name.substr(name.length() - 4) == ".exe") name = name.substr(0, name.length() - 4);
            if (name.length() > 20) name = name.substr(0, 18) + "..";
            DrawText(name.c_str(), (int)(scaledX + 105), (int)(scaledY + 35), 20, Fade(WHITE, cardAlpha));
            
            std::string winTitle = task.windowTitle;
            if (winTitle.length() > 32) winTitle = winTitle.substr(0, 30) + "..";
            DrawText(winTitle.c_str(), (int)(scaledX + 105), (int)(scaledY + 62), 13, Fade(GRAY, cardAlpha * 0.8f));
            
            DrawCircle((int)(scaledX + 30), (int)(scaledY + 115), 6, Fade(GREEN, cardAlpha));
            DrawText("Running", (int)(scaledX + 45), (int)(scaledY + 107), 14, Fade(GREEN, cardAlpha * 0.9f));
            
            if (selected) {
                float hintsY = scaledY + scaledH - 45;
                DrawRectangle((int)(scaledX + 15), (int)(hintsY - 8), (int)(scaledW - 30), 1, Fade(WHITE, 0.1f * cardAlpha));
                DrawRectangleRounded({scaledX + 20, hintsY, 90, 28}, 0.3f, 8, Fade(GREEN, 0.2f * cardAlpha));
                DrawText("[A] Switch", (int)(scaledX + 30), (int)(hintsY + 6), 14, Fade(GREEN, cardAlpha));
                DrawRectangleRounded({scaledX + 125, hintsY, 85, 28}, 0.3f, 8, Fade(RED, 0.2f * cardAlpha));
                DrawText("[X] Close", (int)(scaledX + 135), (int)(hintsY + 6), 14, Fade(RED, cardAlpha));
            }
        }
    }
    
    float barY = screenHeight - 80;
    DrawRectangle(0, (int)barY, screenWidth, 80, Fade(BLACK, 0.6f * slideIn));
    DrawRectangle(0, (int)barY, screenWidth, 1, Fade(WHITE, 0.1f * slideIn));
    const char* hints = "DPAD: Navigate    |    [A]: Switch    |    [X]: Close    |    [B]: Cancel";
    int hintsW = MeasureText(hints, 16);
    DrawText(hints, screenWidth/2 - hintsW/2, (int)(barY + 30), 16, Fade(WHITE, 0.5f * slideIn));
    
    return false;
}

// ============================================================================
// SHELL MENU OVERLAY
// ============================================================================

static int g_shellMenuFocused = 0;
static float g_shellMenuSlideIn = 0.0f;

ShellAction HandleShellMenuOverlay(int screenWidth, int screenHeight, InputAdapter& input, float deltaTime) {
    g_shellMenuSlideIn = Lerp(g_shellMenuSlideIn, 1.0f, 0.12f);
    float animTime = (float)GetTime();
    
    const char* options[] = {"File Explorer", "Keyboard", "Settings", "Task Manager", "Restart Q-Shell", "Exit Shell", "Power"};
    const char* descs[] = {"Open Windows Explorer", "On-screen keyboard", "System settings", "View processes", "Restart interface", "Return to Explorer", "Shutdown/Restart/Sleep"};
    Color colors[] = {SKYBLUE, ORANGE, PURPLE, GREEN, YELLOW, RED, GRAY};
    const char* icons[] = {"E", "K", "S", "T", "R", "X", "P"};
    int optCount = 7;
    
    if (input.IsMoveUp()) g_shellMenuFocused = (g_shellMenuFocused - 1 + optCount) % optCount;
    if (input.IsMoveDown()) g_shellMenuFocused = (g_shellMenuFocused + 1) % optCount;
    
    if (input.IsBack() || input.IsMenu()) {
        g_currentMode = UIMode::MAIN;
        g_shellMenuSlideIn = 0.0f;
        return ShellAction::NONE;
    }
    
    if (input.IsConfirm()) {
        g_currentMode = UIMode::MAIN;
        g_shellMenuSlideIn = 0.0f;
        return (ShellAction)(g_shellMenuFocused + 1);
    }
    
    float slideIn = g_shellMenuSlideIn;
    DrawRectangle(0, 0, screenWidth, screenHeight, Fade(BLACK, 0.7f * slideIn));
    
    float menuW = 450, menuH = 90 + optCount * 60;
    float menuX = screenWidth - (menuW + 50) * slideIn;
    float menuY = (screenHeight - menuH) / 2;
    
    DrawRectangleRounded({menuX + 8, menuY + 10, menuW, menuH}, 0.05f, 14, Fade(BLACK, 0.5f));
    DrawRectangleRounded({menuX, menuY, menuW, menuH}, 0.05f, 14, {18, 22, 32, 250});
    DrawRectangleRoundedLines({menuX, menuY, menuW, menuH}, 0.05f, 14, Fade(SKYBLUE, 0.3f));
    
    DrawText("SHELL MENU", (int)(menuX + 28), (int)(menuY + 22), 28, WHITE);
    DrawRectangle((int)(menuX + 28), (int)(menuY + 58), 130, 3, SKYBLUE);
    
    for (int i = 0; i < optCount; i++) {
        Rectangle btn = {menuX + 18, menuY + 78 + i * 58, menuW - 36, 52};
        bool focused = (g_shellMenuFocused == i);
        DrawRectangleRounded(btn, 0.18f, 10, focused ? Fade(colors[i], 0.15f) : Fade(WHITE, 0.02f));
        if (focused) {
            float pulse = (sinf(animTime * 4) + 1) / 2;
            DrawRectangleRoundedLines(btn, 0.18f, 10, Fade(colors[i], 0.4f + pulse * 0.3f));
        }
        float iconX = btn.x + btn.width - 38;
        DrawCircle((int)iconX, (int)(btn.y + 26), 18, Fade(colors[i], focused ? 0.22f : 0.1f));
        int iconW = MeasureText(icons[i], 16);
        DrawText(icons[i], (int)(iconX - iconW/2), (int)(btn.y + 18), 16, Fade(colors[i], focused ? 1.0f : 0.55f));
        DrawText(options[i], (int)(btn.x + 18), (int)(btn.y + 9), 17, focused ? WHITE : Fade(WHITE, 0.6f));
        DrawText(descs[i], (int)(btn.x + 18), (int)(btn.y + 31), 11, Fade(WHITE, 0.35f));
    }
    
    time_t now = time(0);
    struct tm* ltm = localtime(&now);
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%H:%M  |  %a, %b %d", ltm);
    int timeW = MeasureText(timeStr, 14);
    DrawText(timeStr, (int)(menuX + menuW - timeW - 22), (int)(menuY + menuH - 35), 14, Fade(WHITE, 0.45f));
    DrawText("[A] Select  |  [B] Close", (int)(menuX + 22), (int)(menuY + menuH - 35), 12, Fade(WHITE, 0.35f));
    
    return ShellAction::NONE;
}

// ============================================================================
// POWER MENU OVERLAY
// ============================================================================

static int g_powerMenuFocused = 0;
static float g_powerMenuSlideIn = 0.0f;

PowerChoice HandlePowerMenuOverlay(int screenWidth, int screenHeight, InputAdapter& input, float deltaTime) {
    g_powerMenuSlideIn = Lerp(g_powerMenuSlideIn, 1.0f, 0.15f);
    float animTime = (float)GetTime();
    
    const char* options[] = {"Restart", "Shutdown", "Sleep", "Cancel"};
    const char* icons[] = {"R", "S", "Z", "X"};
    Color colors[] = {ORANGE, RED, BLUE, GRAY};
    
    if (input.IsMoveLeft()) g_powerMenuFocused = (g_powerMenuFocused - 1 + 4) % 4;
    if (input.IsMoveRight()) g_powerMenuFocused = (g_powerMenuFocused + 1) % 4;
    
    if (input.IsBack()) {
        g_currentMode = UIMode::MAIN;
        g_powerMenuSlideIn = 0.0f;
        return PowerChoice::CANCEL;
    }
    
    if (input.IsConfirm()) {
        g_currentMode = UIMode::MAIN;
        g_powerMenuSlideIn = 0.0f;
        return (PowerChoice)g_powerMenuFocused;
    }
    
    float slideIn = g_powerMenuSlideIn;
    DrawRectangle(0, 0, screenWidth, screenHeight, Fade(BLACK, 0.85f * slideIn));
    
    const char* title = "POWER OPTIONS";
    int titleW = MeasureText(title, 36);
    DrawText(title, screenWidth/2 - titleW/2, screenHeight/2 - 120, 36, Fade(WHITE, slideIn));
    DrawRectangle(screenWidth/2 - 80, screenHeight/2 - 75, 160, 3, Fade(WHITE, 0.5f * slideIn));
    
    float btnW = 160, btnH = 110, gap = 30;
    float startX = (screenWidth - (btnW * 4 + gap * 3)) / 2;
    float btnY = screenHeight / 2 - 20;
    
    for (int i = 0; i < 4; i++) {
        float x = startX + i * (btnW + gap);
        bool sel = (i == g_powerMenuFocused);
        DrawRectangleRounded({x, btnY, btnW, btnH}, 0.15f, 12, Fade(sel ? colors[i] : WHITE, sel ? 0.2f : 0.03f));
        int iconW = MeasureText(icons[i], 40);
        DrawText(icons[i], (int)(x + (btnW - iconW) / 2), (int)(btnY + 25), 40, Fade(colors[i], sel ? 1.0f : 0.5f));
        int labelW = MeasureText(options[i], 18);
        DrawText(options[i], (int)(x + (btnW - labelW) / 2), (int)(btnY + 75), 18, Fade(WHITE, sel ? 1.0f : 0.6f));
        if (sel) {
            float pulse = (sinf(animTime * 4) + 1) / 2;
            DrawRectangleRoundedLines({x, btnY, btnW, btnH}, 0.15f, 12, Fade(colors[i], 0.5f + pulse * 0.35f));
        }
    }
    
    return PowerChoice::NONE;
}

// ============================================================================
// DIALOGS
// ============================================================================

StartupChoice ShowLaunchDialog() {
    const int dialogWidth = 550, dialogHeight = 400;
    SetConfigFlags(FLAG_WINDOW_UNDECORATED | FLAG_MSAA_4X_HINT);
    InitWindow(dialogWidth, dialogHeight, "Q-Shell");
    SetWindowPosition((GetMonitorWidth(0) - dialogWidth) / 2, (GetMonitorHeight(0) - dialogHeight) / 2);
    SetTargetFPS(60);

    HWND hwnd = (HWND)GetWindowHandle();
    SetWindowLong(hwnd, GWL_EXSTYLE, (GetWindowLong(hwnd, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW);
    ShowWindow(hwnd, SW_HIDE);
    ShowWindow(hwnd, SW_SHOW);

    InputAdapter input;
    int selected = 0;
    bool isCurrentlyShellMode = CheckIfShellMode();
    float animTime = 0;
    StartupChoice result = StartupChoice::NONE;

    while (!WindowShouldClose() && result == StartupChoice::NONE) {
        input.Update();
        animTime += GetFrameTime();
        
        if (input.IsMoveUp()) selected = (selected + 2) % 3;
        if (input.IsMoveDown()) selected = (selected + 1) % 3;
        
        if (input.IsConfirm()) {
            if (selected == 0) result = StartupChoice::NORMAL_APP;
            else if (selected == 1) result = isCurrentlyShellMode ? StartupChoice::EXIT_SHELL : StartupChoice::SHELL_MODE;
            else { CloseWindow(); return StartupChoice::NONE; }
        }
        if (input.IsBack()) { CloseWindow(); return StartupChoice::NONE; }
        
        BeginDrawing();
        for (int i = 0; i < dialogHeight; i++) {
            float t = (float)i / dialogHeight;
            Color c = ColorLerp({20, 25, 35, 255}, {12, 14, 20, 255}, t);
            DrawLine(0, i, dialogWidth, i, c);
        }
        
        DrawText("Q-SHELL", 40, 35, 48, WHITE);
        DrawText(isCurrentlyShellMode ? "Running as Windows Shell" : "Select Mode", 40, 90, 16, isCurrentlyShellMode ? GREEN : GRAY);
        DrawRectangle(40, 120, 150, 2, Fade(WHITE, 0.1f));
        
        const char* options[] = {"Normal Application", isCurrentlyShellMode ? "Exit Shell Mode" : "Shell Mode", "Cancel"};
        const char* descs[] = {"Run alongside Explorer", isCurrentlyShellMode ? "Restore Explorer & restart" : "Replace Explorer (restart)", "Close"};
        Color colors[] = {SKYBLUE, isCurrentlyShellMode ? ORANGE : GREEN, GRAY};
        
        for (int i = 0; i < 3; i++) {
            Rectangle btn = {40, (float)(145 + i * 70), (float)(dialogWidth - 80), 60};
            bool focused = (selected == i);
            DrawRectangleRounded(btn, 0.15f, 12, focused ? Fade(colors[i], 0.12f) : Fade(WHITE, 0.02f));
            if (focused) {
                float pulse = (sinf(animTime * 4) + 1) / 2;
                DrawRectangleRoundedLines(btn, 0.15f, 12, Fade(colors[i], 0.4f + pulse * 0.3f));
                DrawText(">", (int)(btn.x + 15), (int)(btn.y + 18), 24, colors[i]);
            }
            DrawText(options[i], (int)(btn.x + 40), (int)(btn.y + 12), 20, focused ? colors[i] : Fade(WHITE, 0.7f));
            DrawText(descs[i], (int)(btn.x + 40), (int)(btn.y + 36), 13, Fade(WHITE, 0.4f));
        }
        
        if (selected == 1 && !isCurrentlyShellMode) {
            DrawRectangle(0, dialogHeight - 50, dialogWidth, 50, Fade(ORANGE, 0.1f));
            DrawText("! Requires restart. Emergency restore will be created.", 40, dialogHeight - 32, 13, ORANGE);
        }
        EndDrawing();
    }

    CloseWindow();
    return result;
}

bool ShowExitShellConfirmation() {
    const int dialogWidth = 480, dialogHeight = 260;
    SetConfigFlags(FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TOPMOST);
    InitWindow(dialogWidth, dialogHeight, "Exit Shell");
    SetWindowPosition((GetMonitorWidth(0) - dialogWidth) / 2, (GetMonitorHeight(0) - dialogHeight) / 2);
    SetTargetFPS(60);
    SetForegroundWindow((HWND)GetWindowHandle());

    InputAdapter input;
    int selected = 0;
    float animTime = 0;
    bool result = false, done = false;

    while (!WindowShouldClose() && !done) {
        input.Update();
        animTime += GetFrameTime();
        if (input.IsMoveLeft()) selected = 0;
        if (input.IsMoveRight()) selected = 1;
        if (input.IsConfirm()) { result = (selected == 0); done = true; }
        if (input.IsBack()) { result = false; done = true; }
        
        BeginDrawing();
        ClearBackground({18, 20, 28, 255});
        DrawText("Exit Shell Mode?", 40, 35, 26, WHITE);
        DrawText("This will restore Windows Explorer and restart.", 40, 75, 14, GRAY);
        DrawText("Your settings will be preserved.", 40, 100, 13, GREEN);
        
        Rectangle btnYes = {40, 160, 190, 55}, btnNo = {250, 160, 190, 55};
        DrawRectangleRounded(btnYes, 0.25f, 12, selected == 0 ? Fade(GREEN, 0.25f) : Fade(WHITE, 0.05f));
        DrawRectangleRounded(btnNo, 0.25f, 12, selected == 1 ? Fade(RED, 0.25f) : Fade(WHITE, 0.05f));
        if (selected == 0) DrawRectangleRoundedLines(btnYes, 0.25f, 12, Fade(GREEN, 0.5f + sinf(animTime*4)*0.3f));
        if (selected == 1) DrawRectangleRoundedLines(btnNo, 0.25f, 12, Fade(RED, 0.5f + sinf(animTime*4)*0.3f));
        DrawText("Yes, Exit", (int)(btnYes.x + 55), (int)(btnYes.y + 18), 18, selected == 0 ? GREEN : WHITE);
        DrawText("Cancel", (int)(btnNo.x + 65), (int)(btnNo.y + 18), 18, selected == 1 ? RED : Fade(WHITE, 0.7f));
        EndDrawing();
    }

    CloseWindow();
    return result;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    SetWorkingDirectoryToExe();
    DebugLog("========================================");
    DebugLog("Q-Shell v2.5 Starting");
    DebugLog("Directory: " + g_exeDirectory);

    SetUnhandledExceptionFilter(CrashHandler);
    fs::create_directories(GetFullPath("img"));
    fs::create_directories(GetFullPath("profile"));
    fs::create_directories(GetFullPath("profile\\intro"));
    fs::create_directories(GetFullPath("backup"));
    CreateEmergencyRestoreBatch();

    SystemConfig sysCfg = ReadSystemConfig();
    g_isShellMode = sysCfg.isShellMode || CheckIfShellMode();
        DebugLog(g_isShellMode ? "MODE: SHELL" : "MODE: NORMAL");

    // Start global input monitoring
    StartInputMonitoring();

    // Startup
    if (g_isShellMode) {
        DebugLog("Shell mode - showing boot screen");
        ShowBootScreen();
        DebugLog("Boot screen complete, terminating explorer");
        TerminateExplorer();
        Sleep(500);
    } else {
        StartupChoice choice = ShowLaunchDialog();
        
        if (choice == StartupChoice::NONE) {
            StopInputMonitoring();
            return 0;
        }
        else if (choice == StartupChoice::SHELL_MODE) {
            if (!CheckAdminRights()) { 
                StopInputMonitoring();
                RequestAdminRights(); 
                return 0; 
            }
            CreateSystemBackup();
            CreateEmergencyRestoreBatch();
            if (ActivateShellMode()) {
                sysCfg.isShellMode = true;
                WriteSystemConfig(sysCfg);
                MessageBoxA(NULL, "Shell mode activated! Your PC will restart.\n\nEmergency: Run backup\\EMERGENCY_RESTORE.bat from Safe Mode.", "Q-Shell", MB_OK);
                StopInputMonitoring();
                PerformRestart();
                return 0;
            }
        }
        else if (choice == StartupChoice::EXIT_SHELL) {
            if (ShowExitShellConfirmation()) {
                if (!CheckAdminRights()) { 
                    StopInputMonitoring();
                    RequestAdminRights(); 
                    return 0; 
                }
                DeactivateShellMode();
                LaunchExplorer();
                sysCfg.isShellMode = false;
                WriteSystemConfig(sysCfg);
                MessageBoxA(NULL, "Shell mode deactivated! Your PC will restart.", "Q-Shell", MB_OK);
                StopInputMonitoring();
                PerformRestart();
                return 0;
            }
        }
    }

    // Main window setup
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    if (screenWidth <= 0) screenWidth = 1920;
    if (screenHeight <= 0) screenHeight = 1080;

    std::string currentBgPath = "";
    UserProfile userProfile;
    LoadProfile(currentBgPath, userProfile);

    std::vector<UIGame> library;
    std::string libPath = GetFullPath("profile\\library.txt");
    if (fs::exists(libPath)) {
        std::ifstream libFile(libPath);
        std::string line;
        while (std::getline(libFile, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string name, path, platform, id;
            std::getline(ss, name, '|');
            std::getline(ss, path, '|');
            std::getline(ss, platform, '|');
            std::getline(ss, id, '|');
            if (!name.empty() && !path.empty()) {
                library.push_back({{ name, path, platform, id }, {0}, false, 0.0f, 0.0f});
            }
        }
        libFile.close();
    }
    RefreshLibrary(library, currentBgPath, userProfile);

    std::vector<MediaApp> mediaApps = {
        {"Google", "https://www.google.com", "google.png", {0}, false, {66, 133, 244, 255}},
        {"YouTube", "https://www.youtube.com", "youtube.png", {0}, false, {255, 0, 0, 255}},
        {"Steam", "steam://open/main", "steam.png", {0}, false, {102, 192, 244, 255}},
        {"Spotify", "https://open.spotify.com", "spotify.png", {0}, false, {30, 215, 96, 255}},
        {"Twitch", "https://www.twitch.tv", "twitch.png", {0}, false, {145, 70, 255, 255}},
        {"Discord", "https://discord.com/app", "discord.png", {0}, false, {88, 101, 242, 255}},
        {"Netflix", "https://www.netflix.com", "netflix.png", {0}, false, {229, 9, 20, 255}},
        {"Epic", "com.epicgames.launcher://", "epic.png", {0}, false, {180, 180, 180, 255}},
    };

    std::vector<ShareOption> shareOptions = {
        {"Screenshot", "Capture current screen", SKYBLUE},
        {"Record Clip", "Record last 30 seconds", RED},
        {"Stream", "Start streaming to Twitch", PURPLE},
        {"Share to Discord", "Share with friends", {88, 101, 242, 255}},
    };

    // UI State
    int focused = 0;
    int barFocused = 0;
    bool inTopBar = false;
    float scrollY = 0;
    float mediaScrollX = 0;
    int mediaFocusX = 0, mediaFocusY = 0;
    int shareFocused = 0;
    int settingsFocusX = 0, settingsFocusY = 0;
    bool showDetails = false;
    bool shouldExit = false;

    bool showDeleteWarning = false;
    bool isFullUninstall = false;
    float holdTimer = 0.0f;
    const float HOLD_THRESHOLD = 1.5f;

    Texture2D bgTexture = {0};
    Image bgAnimImg = {0};
    int animFrames = 0, currentAnimFrame = 0, frameDelayCounter = 0;
    bool isAnimatedBG = false;
    float transAlpha = 0.0f;

    const int MENU_COUNT = 4;
    ShellAction pendingShellAction = ShellAction::NONE;

    // Initialize main window
    if (g_isShellMode) {
        SetConfigFlags(FLAG_WINDOW_UNDECORATED | FLAG_FULLSCREEN_MODE | FLAG_VSYNC_HINT);
    } else {
        SetConfigFlags(FLAG_WINDOW_UNDECORATED | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    }
    
    InitWindow(screenWidth, screenHeight, "Q-Shell Launcher");
    SetWindowPosition(0, 0);
    SetTargetFPS(60);
    
    g_mainWindow = (HWND)GetWindowHandle();
    
    if (g_isShellMode) {
        SetWindowPos(g_mainWindow, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_SHOWWINDOW);
        SetForegroundWindow(g_mainWindow);
        HideCursor();
        g_windowOnTop = true;
    } else {
        SetWindowLong(g_mainWindow, GWL_EXSTYLE, (GetWindowLong(g_mainWindow, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW);
        g_windowOnTop = false;
    }
    
    // Load background
    if (!currentBgPath.empty() && fs::exists(currentBgPath)) {
        if (IsFileExtension(currentBgPath.c_str(), ".gif")) {
            bgAnimImg = LoadImageAnim(currentBgPath.c_str(), &animFrames);
            bgTexture = LoadTextureFromImage(bgAnimImg);
            isAnimatedBG = true;
        } else {
            bgTexture = LoadTexture(currentBgPath.c_str());
            isAnimatedBG = false;
        }
    }
    
    LoadMediaAppTextures(mediaApps);
    
    // Load game posters
    for (auto& game : library) {
        if (!game.hasPoster) {
            std::string path = GetFullPath("img/" + game.info.name + ".png");
            if (!fs::exists(path)) path = GetFullPath("img/" + game.info.name + ".jpg");
            if (fs::exists(path)) {
                game.poster = LoadTexture(path.c_str());
                game.hasPoster = (game.poster.id > 0);
            } else if (!game.info.appId.empty()) {
                std::string url = "https://cdn.akamai.steamstatic.com/steam/apps/" + game.info.appId + "/library_hero.jpg";
                std::string dest = GetFullPath("img/" + game.info.name + ".jpg");
                DownloadFileAsync(url, dest);
            }
        }
    }
    
    InputAdapter input;

    DebugLog("Entering main loop...");

    while (!WindowShouldClose() && !shouldExit) {
        float deltaTime = GetFrameTime();
        float currentTime = (float)GetTime();
        
        // Check for global task switcher request
        if (g_taskSwitcherRequested.exchange(false)) {
            DebugLog("Task Switcher requested via global hotkey");
            RefreshTaskList();
            g_taskFocusIndex = 0;
            g_taskSwitcherSlideIn = 0.0f;
            g_taskSwitcherAnimTime = 0.0f;
            
            if (g_mainWindow) {
                if (g_isShellMode) {
                    MakeQShellTopmost();
                } else {
                    ShowWindow(g_mainWindow, SW_RESTORE);
                    SetForegroundWindow(g_mainWindow);
                }
            }
            g_currentMode = UIMode::TASK_SWITCHER;
        }
        
        input.Update();
        int totalItems = (int)library.size() + 1;
        
        // Animated background
        if (isAnimatedBG && bgAnimImg.data) {
            frameDelayCounter++;
            if (frameDelayCounter >= 4) {
                currentAnimFrame = (currentAnimFrame + 1) % animFrames;
                int frameSize = bgAnimImg.width * bgAnimImg.height * 4;
                UpdateTexture(bgTexture, ((unsigned char*)bgAnimImg.data) + (frameSize * currentAnimFrame));
                frameDelayCounter = 0;
            }
        }
        
        // Process pending shell actions
        if (pendingShellAction != ShellAction::NONE) {
            switch (pendingShellAction) {
                case ShellAction::EXPLORER:
                    LaunchExplorer();
                    break;
                case ShellAction::KEYBOARD:
                    ShellExecuteA(NULL, "open", "osk.exe", NULL, NULL, SW_SHOWNORMAL);
                    break;
                case ShellAction::SETTINGS:
                    ShellExecuteA(NULL, "open", "ms-settings:", NULL, NULL, SW_SHOWNORMAL);
                    break;
                case ShellAction::TASKMGR:
                    ShellExecuteA(NULL, "open", "taskmgr.exe", NULL, NULL, SW_SHOWNORMAL);
                    break;
                case ShellAction::RESTART_SHELL:
                    g_shouldRestart = true;
                    shouldExit = true;
                    break;
                case ShellAction::EXIT_SHELL:
                    if (ShowExitShellConfirmation()) {
                        if (CheckAdminRights()) {
                            DeactivateShellMode();
                            LaunchExplorer();
                            sysCfg.isShellMode = false;
                            WriteSystemConfig(sysCfg);
                            StopInputMonitoring();
                            PerformRestart();
                            shouldExit = true;
                        }
                    }
                    break;
                case ShellAction::POWER:
                    g_currentMode = UIMode::POWER_MENU;
                    g_powerMenuFocused = 0;
                    g_powerMenuSlideIn = 0.0f;
                    break;
                default: break;
            }
            pendingShellAction = ShellAction::NONE;
        }
        
        // Handle overlays based on current mode
        if (g_currentMode == UIMode::TASK_SWITCHER) {
            BeginDrawing();
            // Draw main UI dimmed in background
            ClearBackground({12, 12, 15, 255});
            if (bgTexture.id > 0) {
                DrawTexturePro(bgTexture, {0, 0, (float)bgTexture.width, (float)bgTexture.height},
                    {0, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0, Fade(WHITE, 0.3f));
            }
            // Draw task switcher overlay
            HandleTaskSwitcherOverlay(screenWidth, screenHeight, input, deltaTime);
            EndDrawing();
            continue;
        }
        
        if (g_currentMode == UIMode::SHELL_MENU) {
            BeginDrawing();
            ClearBackground({12, 12, 15, 255});
            if (bgTexture.id > 0) {
                DrawTexturePro(bgTexture, {0, 0, (float)bgTexture.width, (float)bgTexture.height},
                    {0, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0, Fade(WHITE, 0.3f));
            }
            ShellAction action = HandleShellMenuOverlay(screenWidth, screenHeight, input, deltaTime);
            if (action != ShellAction::NONE) {
                pendingShellAction = action;
            }
            EndDrawing();
            continue;
        }
        
        if (g_currentMode == UIMode::POWER_MENU) {
            BeginDrawing();
            ClearBackground({12, 12, 15, 255});
            if (bgTexture.id > 0) {
                DrawTexturePro(bgTexture, {0, 0, (float)bgTexture.width, (float)bgTexture.height},
                    {0, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0, Fade(WHITE, 0.3f));
            }
            PowerChoice pwr = HandlePowerMenuOverlay(screenWidth, screenHeight, input, deltaTime);
            if (pwr == PowerChoice::RESTART) { 
                StopInputMonitoring();
                LaunchExplorer(); 
                PerformRestart(); 
                shouldExit = true; 
            }
            else if (pwr == PowerChoice::SHUTDOWN) { 
                StopInputMonitoring();
                LaunchExplorer(); 
                PerformShutdown(); 
                shouldExit = true; 
            }
            else if (pwr == PowerChoice::SLEEP) {
                PerformSleep();
                g_currentMode = UIMode::MAIN;
            }
            else if (pwr == PowerChoice::CANCEL) {
                g_currentMode = UIMode::MAIN;
            }
            EndDrawing();
            continue;
        }
        
        // === MAIN UI MODE ===
        
        // Background shortcut (B key)
        if (input.IsBackgroundKey() && !showDeleteWarning) {
            std::string path = OpenFilePicker(false);
            if (!path.empty()) {
                if (bgTexture.id > 0) UnloadTexture(bgTexture);
                if (isAnimatedBG && bgAnimImg.data) UnloadImage(bgAnimImg);
                currentBgPath = path;
                if (IsFileExtension(path.c_str(), ".gif")) {
                    bgAnimImg = LoadImageAnim(path.c_str(), &animFrames);
                    bgTexture = LoadTextureFromImage(bgAnimImg);
                    isAnimatedBG = true;
                } else {
                    bgTexture = LoadTexture(path.c_str());
                    isAnimatedBG = false;
                }
                SaveProfile(library, currentBgPath, userProfile);
            }
        }
        
        // View = Task Switcher
        if (input.IsView()) {
            RefreshTaskList();
            g_taskFocusIndex = 0;
            g_taskSwitcherSlideIn = 0.0f;
            g_taskSwitcherAnimTime = 0.0f;
            g_currentMode = UIMode::TASK_SWITCHER;
        }
        
        // Menu = Shell Menu (in shell mode)
        if (input.IsMenu() && g_isShellMode) {
            g_shellMenuFocused = 0;
            g_shellMenuSlideIn = 0.0f;
            g_currentMode = UIMode::SHELL_MENU;
        }
        
        // Tab switching with LB/RB
        if (!showDeleteWarning) {
            if (input.IsLB()) {
                barFocused = (barFocused + MENU_COUNT - 1) % MENU_COUNT;
                focused = 0; mediaFocusX = mediaFocusY = 0; shareFocused = 0; settingsFocusX = settingsFocusY = 0;
                inTopBar = false; showDetails = false;
                transAlpha = 0.2f;
            }
            if (input.IsRB()) {
                barFocused = (barFocused + 1) % MENU_COUNT;
                focused = 0; mediaFocusX = mediaFocusY = 0; shareFocused = 0; settingsFocusX = settingsFocusY = 0;
                inTopBar = false; showDetails = false;
                transAlpha = 0.2f;
            }
        }
        
        // Delete warning handling
        if (showDeleteWarning) {
            if (input.IsConfirm()) {
                if (isFullUninstall) PhysicallyUninstall(library[focused]);
                if (library[focused].hasPoster) UnloadTexture(library[focused].poster);
                library.erase(library.begin() + focused);
                SaveProfile(library, currentBgPath, userProfile);
                showDeleteWarning = false;
                focused = Clamp(focused - 1, 0, std::max(0, (int)library.size() - 1));
            }
            if (input.IsBack()) showDeleteWarning = false;
        }
        // Navigation
        else if (inTopBar) {
            if (input.IsMoveDown()) {
                inTopBar = false;
                if (barFocused == 0) RefreshLibrary(library, currentBgPath, userProfile);
            }
            if (input.IsMoveRight()) { barFocused = (barFocused + 1) % MENU_COUNT; transAlpha = 0.1f; }
            if (input.IsMoveLeft()) { barFocused = (barFocused + MENU_COUNT - 1) % MENU_COUNT; transAlpha = 0.1f; }
        }
        else {
            if (barFocused == 0) {
                // Library
                if (input.IsMoveDown()) { focused++; showDetails = false; }
                if (input.IsMoveUp()) {
                    if (focused == 0) inTopBar = true;
                    else { focused--; showDetails = false; }
                }
                focused = Clamp(focused, 0, totalItems - 1);
                
                if (input.IsMoveRight() && focused < (int)library.size()) showDetails = true;
                if (input.IsMoveLeft()) showDetails = false;
                
                if (focused < (int)library.size()) {
                    if (input.IsChangeArt()) {
                        std::string newImg = OpenFilePicker(false);
                        if (!newImg.empty()) {
                            std::string target = GetFullPath("img/" + library[focused].info.name + ".png");
                            if (library[focused].hasPoster) UnloadTexture(library[focused].poster);
                            fs::copy_file(newImg, target, fs::copy_options::overwrite_existing);
                            library[focused].poster = LoadTexture(target.c_str());
                            library[focused].hasPoster = (library[focused].poster.id > 0);
                            SaveProfile(library, currentBgPath, userProfile);
                        }
                    }
                    
                    if (input.IsDeleteDown()) {
                        holdTimer += deltaTime;
                        if (holdTimer >= HOLD_THRESHOLD) {
                            showDeleteWarning = true;
                            isFullUninstall = true;
                            holdTimer = 0;
                        }
                    }
                    if (input.IsDeleteReleased()) {
                        if (holdTimer > 0.1f && holdTimer < HOLD_THRESHOLD) {
                            showDeleteWarning = true;
                            isFullUninstall = false;
                        }
                        holdTimer = 0;
                    }
                }
                
                if (input.IsConfirm() && !showDeleteWarning) {
                    if (focused < (int)library.size()) {
                        LaunchGame(library[focused].info.exePath);
                    } else {
                        std::string path = OpenFilePicker(true);
                        if (!path.empty()) {
                            fs::path p(path);
                            GameInfo manualGame = {p.stem().string(), path, "Manual", ""};
                            library.push_back({manualGame, {0}, false, 0.0f, 0.0f});
                            SaveProfile(library, currentBgPath, userProfile);
                            focused = (int)library.size() - 1;
                        }
                    }
                }
            }
            else if (barFocused == 1) {
                // Media
                int cols = 4;
                int rows = ((int)mediaApps.size() + cols - 1) / cols;
                
                if (input.IsMoveUp()) {
                    if (mediaFocusY == 0) inTopBar = true;
                    else mediaFocusY--;
                }
                if (input.IsMoveDown()) mediaFocusY = std::min(mediaFocusY + 1, rows - 1);
                if (input.IsMoveLeft()) mediaFocusX = std::max(mediaFocusX - 1, 0);
                if (input.IsMoveRight()) mediaFocusX = std::min(mediaFocusX + 1, cols - 1);
                
                int maxIdx = (int)mediaApps.size() - 1;
                int currentIdx = mediaFocusY * cols + mediaFocusX;
                if (currentIdx > maxIdx) {
                    mediaFocusY = maxIdx / cols;
                    mediaFocusX = maxIdx % cols;
                }
                
                if (input.IsConfirm()) {
                    int idx = mediaFocusY * cols + mediaFocusX;
                    if (idx < (int)mediaApps.size()) {
                        OpenURL(mediaApps[idx].url);
                    }
                }
            }
            else if (barFocused == 2) {
                // Share
                if (input.IsMoveUp()) {
                    if (shareFocused == 0) inTopBar = true;
                    else shareFocused--;
                }
                if (input.IsMoveDown()) shareFocused = std::min(shareFocused + 1, (int)shareOptions.size() - 1);
                
                if (input.IsConfirm()) {
                    DebugLog("Share action: " + shareOptions[shareFocused].name);
                }
            }
            else if (barFocused == 3) {
                // Settings
                if (input.IsMoveUp()) {
                    if (settingsFocusY == 0) inTopBar = true;
                    else settingsFocusY--;
                }
                if (input.IsMoveDown()) settingsFocusY = std::min(settingsFocusY + 1, 1);
                if (input.IsMoveLeft()) settingsFocusX = std::max(settingsFocusX - 1, 0);
                if (input.IsMoveRight()) settingsFocusX = std::min(settingsFocusX + 1, 2);
                
                if (input.IsConfirm()) {
                    int idx = settingsFocusY * 3 + settingsFocusX;
                    switch (idx) {
                        case 0: { // Background
                            std::string path = OpenFilePicker(false);
                            if (!path.empty()) {
                                if (bgTexture.id > 0) UnloadTexture(bgTexture);
                                if (isAnimatedBG && bgAnimImg.data) UnloadImage(bgAnimImg);
                                currentBgPath = path;
                                if (IsFileExtension(path.c_str(), ".gif")) {
                                    bgAnimImg = LoadImageAnim(path.c_str(), &animFrames);
                                    bgTexture = LoadTextureFromImage(bgAnimImg);
                                    isAnimatedBG = true;
                                } else {
                                    bgTexture = LoadTexture(path.c_str());
                                    isAnimatedBG = false;
                                }
                                SaveProfile(library, currentBgPath, userProfile);
                            }
                        } break;
                        case 2: RefreshLibrary(library, currentBgPath, userProfile); break;
                        case 3: {
                            if (bgTexture.id > 0) UnloadTexture(bgTexture);
                            if (isAnimatedBG && bgAnimImg.data) UnloadImage(bgAnimImg);
                            currentBgPath = "";
                            bgTexture = {0};
                            isAnimatedBG = false;
                            SaveProfile(library, currentBgPath, userProfile);
                        } break;
                        case 5: shouldExit = true; break;
                    }
                }
            }
        }
        
        // Lerps
        float targetY = (float)-(focused * 320) + (screenHeight / 2) - 135;
        scrollY = Lerp(scrollY, targetY, 0.12f);
        float targetMediaScrollY = -mediaFocusY * 280;
        mediaScrollX = Lerp(mediaScrollX, targetMediaScrollY, 0.12f);
        transAlpha = Lerp(transAlpha, 0.0f, 0.30f);
        
        for (int i = 0; i < (int)library.size(); i++) {
            float target = (!inTopBar && showDetails && i == focused && barFocused == 0) ? 1.0f : 0.0f;
            library[i].detailAlpha = Lerp(library[i].detailAlpha, target, 0.15f);
        }
        
        // ================================================================
        // DRAWING
        // ================================================================
        
        BeginDrawing();
        ClearBackground({12, 12, 15, 255});
        
        // Background
        if (bgTexture.id > 0) {
            DrawTexturePro(bgTexture, {0, 0, (float)bgTexture.width, (float)bgTexture.height},
                {0, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0, WHITE);
            DrawRectangle(0, 0, screenWidth, screenHeight, {12, 12, 15, 200});
        } else {
            DrawCircleGradient(screenWidth/2, screenHeight/2, 800, {22, 22, 28, 255}, {12, 12, 15, 255});
        }
        
        // Top bar
        float topBarY = 60;
        DrawRectangle(0, 0, screenWidth, 110, Fade(BLACK, 0.4f));
        DrawRectangle(0, 109, screenWidth, 1, Fade(WHITE, 0.05f));
        
        // Avatar
        DrawCircularAvatar({55, topBarY + 5}, 25, userProfile.avatar, userProfile.hasAvatar, userProfile.username);
        DrawText(userProfile.username.c_str(), 90, (int)(topBarY - 5), 18, WHITE);
        
        // Menu tabs
        const char* menuItems[] = {"LIBRARY", "MEDIA", "SHARE", "SETTINGS"};
        float menuStartX = (screenWidth - (MENU_COUNT * 180)) / 2;
        for (int m = 0; m < MENU_COUNT; m++) {
            bool isSelected = (barFocused == m);
            Color mCol = isSelected ? WHITE : Fade(GRAY, 0.4f);
            DrawText(menuItems[m], (int)(menuStartX + (m * 180)), (int)topBarY, 22, mCol);
            if (isSelected) DrawRectangle((int)(menuStartX + (m * 180)), (int)(topBarY + 35), 30, 3, WHITE);
        }
        
        // Battery & Time
        SYSTEM_POWER_STATUS sps;
        GetSystemPowerStatus(&sps);
        int batteryPct = (sps.BatteryLifePercent <= 100) ? sps.BatteryLifePercent : 100;
        time_t now = time(0);
        struct tm* ltm = localtime(&now);
        char timeBuf[16];
        strftime(timeBuf, sizeof(timeBuf), "%H:%M", ltm);
        
        DrawRectangleLines(screenWidth - 300, (int)(topBarY + 4), 35, 18, Fade(WHITE, 0.6f));
        DrawRectangle(screenWidth - 298, (int)(topBarY + 6), (int)(31.0f * batteryPct / 100.0f), 14, (batteryPct < 20) ? RED : GREEN);
        DrawText(TextFormat("%d%%", batteryPct), screenWidth - 255, (int)(topBarY + 4), 18, WHITE);
        DrawText(timeBuf, screenWidth - 120, (int)topBarY, 26, WHITE);
        
        if (g_isShellMode) {
            DrawRectangleRounded({(float)(screenWidth - 200), topBarY - 5, 70, 22}, 0.5f, 8, Fade(GREEN, 0.2f));
            DrawText("SHELL", screenWidth - 190, (int)(topBarY - 1), 12, GREEN);
        }
        
        // Content
        float contentTop = 120;
        
        if (barFocused == 0) {
            // LIBRARY
            for (int i = 0; i < totalItems; i++) {
                float itemY = scrollY + (i * 320);
                if (itemY < -400 || itemY > screenHeight + 400) continue;
                
                float alpha = (!inTopBar && i == focused) ? 1.0f : 0.25f;
                if (inTopBar) alpha = 0.15f;
                Rectangle card = {120, itemY, 480, 270};
                
                if (i < (int)library.size()) {
                    UIGame& game = library[i];
                    bool isFocused = (!inTopBar && i == focused);
                    
                    if (game.detailAlpha > 0.01f) {
                        float dAlpha = game.detailAlpha;
                        Rectangle dBox = {card.x + card.width + 40, card.y, 600 * dAlpha, card.height};
                        DrawRectangleRounded(dBox, 0.05f, 12, Fade({25, 25, 30, 255}, dAlpha));
                        if (dAlpha > 0.8f) {
                            DrawText("STATUS", (int)(dBox.x + 40), (int)(dBox.y + 35), 16, Fade(GRAY, dAlpha));
                            DrawText("READY TO PLAY", (int)(dBox.x + 40), (int)(dBox.y + 55), 24, Fade(GREEN, dAlpha));
                            DrawText("PLATFORM", (int)(dBox.x + 40), (int)(dBox.y + 115), 16, Fade(GRAY, dAlpha));
                            DrawText(game.info.platform.c_str(), (int)(dBox.x + 40), (int)(dBox.y + 135), 22, Fade(WHITE, dAlpha));
                        }
                    }
                    
                    DrawGameCard(card, game, isFocused, currentTime);
                    
                    if (isFocused && !showDetails) {
                        DrawText(game.info.name.c_str(), (int)(card.x + card.width + 50), (int)(itemY + 90), 40, Fade(WHITE, alpha));
                        if (holdTimer > 0) {
                            DrawRectangle((int)(card.x + card.width + 50), (int)(itemY + 145), (int)((holdTimer / HOLD_THRESHOLD) * 200), 4, RED);
                        }
                    }
                } else {
                    DrawRectangleRounded(card, 0.05f, 12, Fade({45, 45, 50, 255}, alpha));
                    DrawText("+", (int)(card.x + card.width/2 - 20), (int)(card.y + card.height/2 - 40), 80, Fade(WHITE, alpha));
                    DrawText("Add Game", (int)(card.x + card.width/2 - 45), (int)(card.y + card.height/2 + 35), 16, Fade(WHITE, alpha * 0.6f));
                }
                
                if (!inTopBar && i == focused) {
                    float pulse = (sinf(currentTime * 4) + 1) / 2;
                    DrawRectangleRoundedLinesEx(card, 0.05f, 12, 4.0f, Fade(WHITE, 0.4f + pulse * 0.4f));
                }
            }
        }
        else if (barFocused == 1) {
            // MEDIA
            float startX = 50;
            float startY = contentTop + mediaScrollX + 20;
            float cardW = (screenWidth - 100 - 75) / 4;
            float cardH = 250;
            int cols = 4;
            int focusIdx = mediaFocusY * cols + mediaFocusX;
            
            for (int i = 0; i < (int)mediaApps.size(); i++) {
                int row = i / cols;
                int col = i % cols;
                float cardX = startX + col * (cardW + 25);
                float cardY = startY + row * (cardH + 30);
                if (cardY < -cardH - 50 || cardY > screenHeight + 50) continue;
                Rectangle cardRect = {cardX, cardY, cardW, cardH};
                DrawMediaCard(cardRect, mediaApps[i], (!inTopBar && focusIdx == i), currentTime);
            }
        }
        else if (barFocused == 2) {
            // SHARE
            float centerX = screenWidth / 2;
            float startY = contentTop + 50;
            float optionW = 500, optionH = 80, gap = 20;
            
            DrawText("SHARE & CAPTURE", (int)(centerX - 120), (int)(startY), 28, WHITE);
            DrawRectangle((int)(centerX - 120), (int)(startY + 40), 100, 3, SKYBLUE);
            
            for (int i = 0; i < (int)shareOptions.size(); i++) {
                Rectangle optRect = {centerX - optionW/2, startY + 70 + i * (optionH + gap), optionW, optionH};
                bool isFocused = (!inTopBar && shareFocused == i);
                
                DrawRectangleRounded(optRect, 0.15f, 12, isFocused ? Fade(shareOptions[i].accentColor, 0.15f) : Fade(WHITE, 0.03f));
                if (isFocused) {
                    float pulse = (sinf(currentTime * 4) + 1) / 2;
                    DrawRectangleRoundedLines(optRect, 0.15f, 12, Fade(shareOptions[i].accentColor, 0.4f + pulse * 0.3f));
                }
                
                DrawCircle((int)(optRect.x + 50), (int)(optRect.y + optRect.height/2), 25, Fade(shareOptions[i].accentColor, 0.2f));
                char icon = shareOptions[i].name[0];
                char iconStr[2] = {icon, 0};
                int iconW = MeasureText(iconStr, 24);
                DrawText(iconStr, (int)(optRect.x + 50 - iconW/2), (int)(optRect.y + optRect.height/2 - 12), 24, 
                    isFocused ? shareOptions[i].accentColor : Fade(shareOptions[i].accentColor, 0.5f));
                DrawText(shareOptions[i].name.c_str(), (int)(optRect.x + 100), (int)(optRect.y + 18), 20, 
                    isFocused ? WHITE : Fade(WHITE, 0.7f));
                DrawText(shareOptions[i].description.c_str(), (int)(optRect.x + 100), (int)(optRect.y + 45), 14, Fade(WHITE, 0.4f));
            }
        }
        else if (barFocused == 3) {
            // SETTINGS
            float startX = screenWidth/2 - 480;
            float startY = contentTop + 80;
            float tileW = 290, tileH = 180, gap = 20;
            
            struct Setting { const char* icon; const char* title; Color accent; };
            Setting items[] = {
                {"B", "Background", SKYBLUE},
                {"P", "Profile", PURPLE},
                {"R", "Refresh", GREEN},
                {"X", "Reset BG", ORANGE},
                {"?", "About", GRAY},
                {"Q", "Exit", RED}
            };
            
            int focusIdx = settingsFocusY * 3 + settingsFocusX;
            
            for (int row = 0; row < 2; row++) {
                for (int col = 0; col < 3; col++) {
                    int idx = row * 3 + col;
                    Rectangle tile = {startX + col * (tileW + gap), startY + row * (tileH + gap), tileW, tileH};
                    DrawSettingsTile(tile, items[idx].icon, items[idx].title, items[idx].accent, (!inTopBar && focusIdx == idx), currentTime);
                }
            }
            
            float infoY = startY + 2 * (tileH + gap) + 30;
            DrawText("Mode:", 50, (int)infoY, 15, Fade(WHITE, 0.4f));
            DrawText(g_isShellMode ? "SHELL" : "Normal", 100, (int)infoY, 15, g_isShellMode ? GREEN : SKYBLUE);
            DrawText("Games:", 50, (int)(infoY + 25), 15, Fade(WHITE, 0.4f));
            DrawText(TextFormat("%d", (int)library.size()), 115, (int)(infoY + 25), 15, SKYBLUE);
        }
        
        // Delete warning overlay
        if (showDeleteWarning) {
            DrawRectangle(0, 0, screenWidth, screenHeight, Fade(BLACK, 0.8f));
            Rectangle box = {(float)(screenWidth/2 - 300), (float)(screenHeight/2 - 150), 600, 300};
            DrawRectangleRounded(box, 0.1f, 10, {40, 40, 45, 255});
            DrawRectangleRoundedLinesEx(box, 0.1f, 10, 2, isFullUninstall ? RED : YELLOW);
            DrawText(isFullUninstall ? "FULL UNINSTALL" : "REMOVE FROM LIST", (int)(box.x + 150), (int)(box.y + 50), 28, isFullUninstall ? RED : YELLOW);
            DrawText("Confirm with [A] or [B] to cancel", (int)(box.x + 120), (int)(box.y + 150), 20, WHITE);
            if (isFullUninstall) {
                DrawText("This will delete game files!", (int)(box.x + 140), (int)(box.y + 200), 16, RED);
            }
        }
        
        // Bottom bar
        float hintY = screenHeight - 70;
        DrawRectangle(0, (int)hintY, screenWidth, 70, Fade(BLACK, 0.5f));
        DrawRectangle(0, (int)hintY, screenWidth, 1, Fade(WHITE, 0.1f));
        
        DrawRectangleRounded({30, hintY + 12, 280, 45}, 0.5f, 10, Fade(PURPLE, 0.15f));
        DrawRectangleRoundedLines({30, hintY + 12, 280, 45}, 0.5f, 10, Fade(PURPLE, 0.3f));
        DrawText("TAB+O / SHARE+X: Task Switcher", 50, (int)(hintY + 26), 14, Fade(WHITE, 0.8f));
        
        float pulse = (sinf(currentTime * 4) + 1.0f) / 2.0f;
        Rectangle btnRect = {(float)(screenWidth - 280), hintY + 12, 250, 45};
        DrawRectangleRounded(btnRect, 0.5f, 10, Fade(SKYBLUE, 0.1f + pulse * 0.1f));
        DrawRectangleRoundedLinesEx(btnRect, 0.5f, 10, 2, Fade(WHITE, 0.3f + pulse * 0.4f));
        DrawText("[B] SET BACKGROUND", (int)(btnRect.x + 40), (int)(btnRect.y + 15), 14, WHITE);
        
        const char* centerHint = "";
        if (barFocused == 0) centerHint = "[A] Launch  |  [Y] Art  |  [X] Delete  |  [LB/RB] Tabs";
        else if (barFocused == 1) centerHint = "[A] Open  |  [LB/RB] Tabs";
        else if (barFocused == 2) centerHint = "[A] Action  |  [LB/RB] Tabs";
        else if (barFocused == 3) centerHint = "[A] Select  |  [LB/RB] Tabs";
        int hintW = MeasureText(centerHint, 14);
        DrawText(centerHint, screenWidth/2 - hintW/2, (int)(hintY + 28), 14, Fade(WHITE, 0.5f));
        
        if (transAlpha > 0.01f) {
            DrawRectangle(0, 110, screenWidth, screenHeight - 110, Fade({12, 12, 15, 255}, transAlpha));
        }
        
        EndDrawing();
    }

    // Cleanup
    if (bgTexture.id > 0) UnloadTexture(bgTexture);
    if (isAnimatedBG && bgAnimImg.data) UnloadImage(bgAnimImg);
    for (auto& app : mediaApps) {
        if (app.hasTexture) UnloadTexture(app.texture);
    }
    for (auto& game : library) {
        if (game.hasPoster) UnloadTexture(game.poster);
    }
    
    CloseWindow();
    StopInputMonitoring();

    DebugLog("Q-Shell exiting...");

    if (g_isShellMode) {
        DebugLog("Shell mode - launching explorer before exit");
        LaunchExplorer();
    }
    
    if (g_shouldRestart) {
        DebugLog("Restarting Q-Shell...");
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        ShellExecuteA(NULL, "open", exePath, NULL, NULL, SW_SHOWNORMAL);
    }

    return 0;
}

// ============================================================================
// END OF Q-SHELL v2.5
// ============================================================================
