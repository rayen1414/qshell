// ============================================================================
// SYSTEM_CONTROL.CPP - Q-SHELL v2.5
// Complete shell replacement with hidden Windows branding
// ============================================================================

#include "system_control.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <ctime>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "powrprof.lib")

namespace fs = std::filesystem;

// ============================================================================
// CONSTANTS
// ============================================================================

static const std::string BACKUP_FOLDER = "profile/backup/";
static const std::string CONFIG_FILE = "profile/system_config.txt";
static const std::string INTRO_FOLDER = "profile/intro/";

// ============================================================================
// INTERNAL HELPERS - Command Execution
// ============================================================================

static void ExecuteCommand(const std::string& cmd, bool hide = true, bool waitForIt = true) {
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = hide ? SW_HIDE : SW_SHOW;
    
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    
    char buffer[4096];
    strncpy_s(buffer, cmd.c_str(), sizeof(buffer) - 1);
    
    if (CreateProcessA(nullptr, buffer, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (waitForIt) {
            WaitForSingleObject(pi.hProcess, 15000);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static void ExecuteCommandNoWait(const std::string& cmd) {
    ExecuteCommand(cmd, true, false);
}

static void EnsureFoldersExist() {
    try {
        fs::create_directories("profile/backup");
        fs::create_directories("profile/intro");
        fs::create_directories("img");
    } catch (...) {
        CreateDirectoryA("profile", NULL);
        CreateDirectoryA("profile\\backup", NULL);
        CreateDirectoryA("profile\\intro", NULL);
        CreateDirectoryA("img", NULL);
    }
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

std::string GetCurrentExePath() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    return std::string(path);
}

std::string GetExeDirectory() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string pathStr(path);
    size_t lastSlash = pathStr.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return pathStr.substr(0, lastSlash);
    }
    return "";
}

// ============================================================================
// ADMIN DETECTION
// ============================================================================

bool CheckAdminRights() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    
    return isAdmin == TRUE;
}

bool RequestAdminRights() {
    if (CheckAdminRights()) return true;
    
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    
    SHELLEXECUTEINFOA sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = "runas";
    sei.lpFile = path;
    sei.lpParameters = "--elevated";
    sei.nShow = SW_SHOW;
    
    if (ShellExecuteExA(&sei)) {
        exit(0);
        return true;
    }
    
    return false;
}

// ============================================================================
// SHELL MODE DETECTION
// ============================================================================

bool CheckIfShellMode() {
    char shellPath[MAX_PATH] = {0};
    DWORD size = sizeof(shellPath);
    HKEY hKey;
    
    // Check HKLM
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, 
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", 
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "Shell", nullptr, nullptr, (LPBYTE)shellPath, &size);
        RegCloseKey(hKey);
    }
    
    std::string currentShell(shellPath);
    std::string exePath = GetCurrentExePath();
    
    // Check if shell is set to our exe or contains QShell
    if (currentShell.find("QShell") != std::string::npos || 
        currentShell.find("qshell") != std::string::npos ||
        currentShell == exePath) {
        return true;
    }
    
    // Also check HKCU for user-level override
    size = sizeof(shellPath);
    ZeroMemory(shellPath, sizeof(shellPath));
    
    if (RegOpenKeyExA(HKEY_CURRENT_USER, 
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", 
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, "Shell", nullptr, nullptr, (LPBYTE)shellPath, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            std::string userShell(shellPath);
            if (!userShell.empty() && userShell.find("explorer.exe") == std::string::npos) {
                return true;
            }
        } else {
            RegCloseKey(hKey);
        }
    }
    
    // Check config file
    SystemConfig cfg = ReadSystemConfig();
    return cfg.isShellMode;
}

// ============================================================================
// HIDE WINDOWS BOOT LOGO
// ============================================================================

bool HideWindowsBootLogo() {
    if (!CheckAdminRights()) return false;
    
    // Disable boot animation via BCD
    ExecuteCommand("bcdedit /set {current} bootux disabled", true, true);
    ExecuteCommand("bcdedit /set {current} quietboot yes", true, true);
    
    // Disable boot animation via registry
    ExecuteCommand("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\BootControl\" /v DisableBootAnimation /t REG_DWORD /d 1 /f");
    
    // Disable startup sound
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\LogonUI\" /v DisableStartupSound /t REG_DWORD /d 1 /f");
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v DisableStartupSound /t REG_DWORD /d 1 /f");
    
    // Hide OEM/Windows logo
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v NoBootLogo /t REG_DWORD /d 1 /f");
    
    return true;
}

// ============================================================================
// HIDE LOCK SCREEN
// ============================================================================

bool HideLockScreen() {
    if (!CheckAdminRights()) return false;
    
    // Disable lock screen completely
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization\" /v NoLockScreen /t REG_DWORD /d 1 /f");
    
    // Disable lock screen background/spotlight
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\System\" /v DisableLogonBackgroundImage /t REG_DWORD /d 1 /f");
    
    // Disable Windows Spotlight
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent\" /v DisableWindowsSpotlightOnLockScreen /t REG_DWORD /d 1 /f");
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent\" /v DisableWindowsSpotlightFeatures /t REG_DWORD /d 1 /f");
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent\" /v DisableWindowsConsumerFeatures /t REG_DWORD /d 1 /f");
    
    // Hide network selection UI on login
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\System\" /v DontDisplayNetworkSelectionUI /t REG_DWORD /d 1 /f");
    
    return true;
}

// ============================================================================
// HIDE LOGON UI ELEMENTS
// ============================================================================

bool HideLogonUI() {
    if (!CheckAdminRights()) return false;
    
    // Disable first logon animation ("Hi" screen)
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v EnableFirstLogonAnimation /t REG_DWORD /d 0 /f");
    
    // Disable legal notice
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v LegalNoticeCaption /t REG_SZ /d \"\" /f");
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v LegalNoticeText /t REG_SZ /d \"\" /f");
    
    // Disable Ctrl+Alt+Del requirement
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v DisableCAD /t REG_DWORD /d 1 /f");
    
    // Speed up logon - no verbose status
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v VerboseStatus /t REG_DWORD /d 0 /f");
    
    // Reduce desktop switch timeout
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v DelayedDesktopSwitchTimeout /t REG_DWORD /d 0 /f");
    
    // Disable "Preparing Windows" message
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\" /v DelayedDesktopSwitchTimeout /t REG_DWORD /d 0 /f");
    
    return true;
}

// ============================================================================
// RESTORE WINDOWS BOOT SETTINGS
// ============================================================================

bool RestoreWindowsBootSettings() {
    // Restore boot animation via BCD
    ExecuteCommand("bcdedit /set {current} bootux standard", true, true);
    ExecuteCommand("bcdedit /deletevalue {current} quietboot", true, true);
    
    // Delete our custom boot settings
    ExecuteCommand("reg delete \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\BootControl\" /v DisableBootAnimation /f", true, true);
    ExecuteCommand("reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v NoBootLogo /f", true, true);
    ExecuteCommand("reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v DisableStartupSound /f", true, true);
    
    // Re-enable lock screen
    ExecuteCommand("reg delete \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization\" /v NoLockScreen /f", true, true);
    ExecuteCommand("reg delete \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\System\" /v DisableLogonBackgroundImage /f", true, true);
    
    // Re-enable Windows Spotlight
    ExecuteCommand("reg delete \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent\" /v DisableWindowsSpotlightOnLockScreen /f", true, true);
    ExecuteCommand("reg delete \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent\" /v DisableWindowsSpotlightFeatures /f", true, true);
    
    // Re-enable first logon animation
    ExecuteCommand("reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v EnableFirstLogonAnimation /f", true, true);
    
    // Re-enable Ctrl+Alt+Del if was disabled
    ExecuteCommand("reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v DisableCAD /f", true, true);
    
    return true;
}

// ============================================================================
// AUTO-LOGIN SETUP
// ============================================================================

bool SetupAutoLogin(const std::string& username, const std::string& password) {
    if (!CheckAdminRights()) return false;
    
    std::string winlogonKey = "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    
    // Enable auto admin logon
    ExecuteCommand("reg add \"" + winlogonKey + "\" /v AutoAdminLogon /t REG_SZ /d 1 /f");
    
    // Set default username
    ExecuteCommand("reg add \"" + winlogonKey + "\" /v DefaultUserName /t REG_SZ /d \"" + username + "\" /f");
    
    // Set password if provided
    if (!password.empty()) {
        ExecuteCommand("reg add \"" + winlogonKey + "\" /v DefaultPassword /t REG_SZ /d \"" + password + "\" /f");
    } else {
        // Remove password entry for passwordless accounts
        ExecuteCommand("reg delete \"" + winlogonKey + "\" /v DefaultPassword /f", true, true);
    }
    
    // Clear domain for local accounts
    ExecuteCommand("reg add \"" + winlogonKey + "\" /v DefaultDomainName /t REG_SZ /d \"\" /f");
    
    // Disable lock workstation
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v DisableLockWorkstation /t REG_DWORD /d 1 /f");
    
    return true;
}

bool DisableAutoLogin() {
    std::string winlogonKey = "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    
    ExecuteCommand("reg add \"" + winlogonKey + "\" /v AutoAdminLogon /t REG_SZ /d 0 /f");
    ExecuteCommand("reg delete \"" + winlogonKey + "\" /v DefaultPassword /f", true, true);
    ExecuteCommand("reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v DisableLockWorkstation /f", true, true);
    
    return true;
}

// ============================================================================
// BACKUP SYSTEM
// ============================================================================

bool HasBackup() {
    return fs::exists(BACKUP_FOLDER + "shell.reg") && 
           fs::exists(BACKUP_FOLDER + "state.txt");
}

bool CreateSystemBackup() {
    EnsureFoldersExist();
    
    // Backup shell registry
    ExecuteCommand("reg export \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" \"" 
                   + BACKUP_FOLDER + "shell.reg\" /y");
    
    // Backup boot settings
    ExecuteCommand("reg export \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\BootControl\" \"" 
                   + BACKUP_FOLDER + "boot.reg\" /y");
    
    // Backup policies
    ExecuteCommand("reg export \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization\" \"" 
                   + BACKUP_FOLDER + "personalization.reg\" /y");
    ExecuteCommand("reg export \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\System\" \"" 
                   + BACKUP_FOLDER + "system_policies.reg\" /y");
    ExecuteCommand("reg export \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent\" \"" 
                   + BACKUP_FOLDER + "cloudcontent.reg\" /y");
    
    // Backup services
    ExecuteCommand("reg export \"HKLM\\SYSTEM\\CurrentControlSet\\Services\\DiagTrack\" \"" 
                   + BACKUP_FOLDER + "svc_diagtrack.reg\" /y");
    ExecuteCommand("reg export \"HKLM\\SYSTEM\\CurrentControlSet\\Services\\SysMain\" \"" 
                   + BACKUP_FOLDER + "svc_sysmain.reg\" /y");
    ExecuteCommand("reg export \"HKLM\\SYSTEM\\CurrentControlSet\\Services\\WSearch\" \"" 
                   + BACKUP_FOLDER + "svc_wsearch.reg\" /y");
    
    // Backup telemetry settings
    ExecuteCommand("reg export \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection\" \"" 
                   + BACKUP_FOLDER + "telemetry.reg\" /y");
    
    // Backup game bar
    ExecuteCommand("reg export \"HKCU\\Software\\Microsoft\\GameBar\" \"" 
                   + BACKUP_FOLDER + "gamebar.reg\" /y");
    
    // Backup taskbar settings
    ExecuteCommand("reg export \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" \"" 
                   + BACKUP_FOLDER + "explorer_adv.reg\" /y");
    
    // Save state file
    std::ofstream stateFile(BACKUP_FOLDER + "state.txt");
    stateFile << "backup_created=1\n";
    stateFile << "timestamp=" << time(nullptr) << "\n";
    stateFile << "version=2.5\n";
    stateFile.close();
    
    return fs::exists(BACKUP_FOLDER + "shell.reg");
}

bool RestoreSystemBackup() {
    if (!HasBackup()) {
        // No backup - restore defaults
        ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" "
                       "/v Shell /t REG_SZ /d explorer.exe /f");
        ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" "
                       "/v AutoRestartShell /t REG_DWORD /d 1 /f");
        
        RestoreWindowsBootSettings();
        LaunchExplorer();
        return true;
    }
    
    // Restore from backup files
    if (fs::exists(BACKUP_FOLDER + "shell.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "shell.reg\"");
    }
    
    if (fs::exists(BACKUP_FOLDER + "boot.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "boot.reg\"");
    }
    
    if (fs::exists(BACKUP_FOLDER + "personalization.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "personalization.reg\"");
    }
    
    if (fs::exists(BACKUP_FOLDER + "system_policies.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "system_policies.reg\"");
    }
    
    if (fs::exists(BACKUP_FOLDER + "cloudcontent.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "cloudcontent.reg\"");
    }
    
    if (fs::exists(BACKUP_FOLDER + "svc_diagtrack.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "svc_diagtrack.reg\"");
    }
    
    if (fs::exists(BACKUP_FOLDER + "svc_sysmain.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "svc_sysmain.reg\"");
    }
    
    if (fs::exists(BACKUP_FOLDER + "svc_wsearch.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "svc_wsearch.reg\"");
    }
    
    if (fs::exists(BACKUP_FOLDER + "telemetry.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "telemetry.reg\"");
    }
    
    if (fs::exists(BACKUP_FOLDER + "gamebar.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "gamebar.reg\"");
    }
    
    if (fs::exists(BACKUP_FOLDER + "explorer_adv.reg")) {
        ExecuteCommand("reg import \"" + BACKUP_FOLDER + "explorer_adv.reg\"");
    }
    
    // Restore BCD
    ExecuteCommand("bcdedit /set {current} bootux standard", true, true);
    ExecuteCommand("bcdedit /deletevalue {current} quietboot", true, true);
    
    // Re-enable services
    ExecuteCommand("sc config DiagTrack start= auto");
    ExecuteCommand("sc config SysMain start= auto");
    ExecuteCommand("sc config WSearch start= delayed-auto");
    ExecuteCommandNoWait("sc start DiagTrack");
    ExecuteCommandNoWait("sc start SysMain");
    ExecuteCommandNoWait("sc start WSearch");
    
    // Start explorer
    LaunchExplorer();
    
    return true;
}

// ============================================================================
// SAFETY RESTORE FILES
// ============================================================================

void CreateSafetyRestore() {
    EnsureFoldersExist();
    
    // Create comprehensive emergency batch file
    std::ofstream bat(BACKUP_FOLDER + "EMERGENCY_RESTORE.bat");
    bat << "@echo off\n";
    bat << "color 0C\n";
    bat << "title Q-SHELL EMERGENCY RESTORE\n";
    bat << "echo.\n";
    bat << "echo  ============================================\n";
    bat << "echo      Q-SHELL EMERGENCY RESTORE v2.5\n";
    bat << "echo  ============================================\n";
    bat << "echo.\n";
    bat << "echo  This will restore Windows Explorer as shell\n";
    bat << "echo  and restore all Windows boot settings.\n";
    bat << "echo.\n";
    bat << "echo  Press any key to continue...\n";
    bat << "pause > nul\n";
    bat << "echo.\n";
    bat << "echo  [1/6] Restoring Windows Shell...\n";
    bat << "reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v Shell /t REG_SZ /d explorer.exe /f\n";
    bat << "reg delete \"HKCU\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v Shell /f 2>nul\n";
    bat << "reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v AutoRestartShell /t REG_DWORD /d 1 /f\n";
    bat << "echo.\n";
    bat << "echo  [2/6] Restoring Boot Animation...\n";
    bat << "bcdedit /set {current} bootux standard 2>nul\n";
    bat << "bcdedit /deletevalue {current} quietboot 2>nul\n";
    bat << "reg delete \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\BootControl\" /v DisableBootAnimation /f 2>nul\n";
    bat << "reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v NoBootLogo /f 2>nul\n";
    bat << "echo.\n";
    bat << "echo  [3/6] Restoring Lock Screen...\n";
    bat << "reg delete \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization\" /v NoLockScreen /f 2>nul\n";
    bat << "reg delete \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\System\" /v DisableLogonBackgroundImage /f 2>nul\n";
    bat << "reg delete \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent\" /v DisableWindowsSpotlightOnLockScreen /f 2>nul\n";
    bat << "echo.\n";
    bat << "echo  [4/6] Restoring Logon Settings...\n";
    bat << "reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v EnableFirstLogonAnimation /f 2>nul\n";
    bat << "reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v DisableCAD /f 2>nul\n";
    bat << "reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v LegalNoticeCaption /f 2>nul\n";
    bat << "reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v LegalNoticeText /f 2>nul\n";
    bat << "echo.\n";
    bat << "echo  [5/6] Disabling Auto-Login...\n";
    bat << "reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v AutoAdminLogon /t REG_SZ /d 0 /f 2>nul\n";
    bat << "reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v DefaultPassword /f 2>nul\n";
    bat << "echo.\n";
    bat << "echo  [6/6] Starting Explorer...\n";
    bat << "start explorer.exe\n";
    bat << "echo.\n";
    bat << "color 0A\n";
    bat << "echo  ============================================\n";
    bat << "echo      RESTORE COMPLETE!\n";
    bat << "echo  ============================================\n";
    bat << "echo.\n";
    bat << "echo  Please RESTART your computer for all\n";
    bat << "echo  changes to take full effect.\n";
    bat << "echo.\n";
    bat << "pause\n";
    bat.close();
    
    // Create .reg file for quick restore
    std::ofstream reg(BACKUP_FOLDER + "RESTORE_EXPLORER.reg");
    reg << "Windows Registry Editor Version 5.00\n\n";
    reg << "; Q-Shell Emergency Restore\n";
    reg << "; Double-click this file to restore Explorer as shell\n\n";
    reg << "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon]\n";
    reg << "\"Shell\"=\"explorer.exe\"\n";
    reg << "\"AutoRestartShell\"=dword:00000001\n\n";
    reg << "; Remove user-level shell override\n";
    reg << "[-HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon]\n\n";
    reg << "; Re-enable lock screen\n";
    reg << "[-HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization]\n";
    reg.close();
    
    // Create safe mode batch (minimal, for quick recovery)
    std::ofstream safeBat(BACKUP_FOLDER + "SafeModeRestore.cmd");
    safeBat << "@echo off\n";
    safeBat << "reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v Shell /t REG_SZ /d explorer.exe /f\n";
    safeBat << "reg delete \"HKCU\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v Shell /f 2>nul\n";
    safeBat << "reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v AutoRestartShell /t REG_DWORD /d 1 /f\n";
    safeBat << "bcdedit /set {current} bootux standard 2>nul\n";
    safeBat << "start explorer.exe\n";
    safeBat.close();
    
    // Create README
    std::ofstream readme(BACKUP_FOLDER + "README.txt");
    readme << "Q-SHELL EMERGENCY RESTORE INSTRUCTIONS\n";
    readme << "======================================\n\n";
    readme << "If Q-Shell crashes or you can't access Windows:\n\n";
    readme << "OPTION 1: Run EMERGENCY_RESTORE.bat\n";
    readme << "   - Right-click -> Run as Administrator\n";
    readme << "   - Follow the on-screen instructions\n\n";
    readme << "OPTION 2: Double-click RESTORE_EXPLORER.reg\n";
    readme << "   - Click Yes when prompted to add to registry\n";
    readme << "   - Restart your computer\n\n";
    readme << "OPTION 3: Boot into Safe Mode\n";
    readme << "   - Press F8 during boot OR\n";
    readme << "   - Hold Shift and click Restart from login screen\n";
    readme << "   - Navigate to Troubleshoot > Advanced > Startup Settings\n";
    readme << "   - Choose Safe Mode\n";
    readme << "   - Run EMERGENCY_RESTORE.bat or SafeModeRestore.cmd\n\n";
    readme << "OPTION 4: From Windows Recovery Command Prompt\n";
    readme << "   - Boot from Windows installation media\n";
    readme << "   - Choose 'Repair your computer'\n";
    readme << "   - Open Command Prompt\n";
    readme << "   - Type: reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v Shell /t REG_SZ /d explorer.exe /f\n";
    readme << "   - Restart\n\n";
    readme << "After restoring, RESTART your computer for changes to take effect.\n";
    readme.close();
}

// ============================================================================
// SHELL MODE ACTIVATION
// ============================================================================

bool ActivateShellMode() {
    if (!CheckAdminRights()) {
        return false;
    }
    
    // Create backup first
    if (!HasBackup()) {
        CreateSystemBackup();
    }
    
    // Always create safety restore files
    CreateSafetyRestore();
    
    std::string exePath = GetCurrentExePath();
    
    // Set as Windows shell (HKLM - affects all users after restart)
    std::string cmd = "reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" "
                      "/v Shell /t REG_SZ /d \"" + exePath + "\" /f";
    ExecuteCommand(cmd);
    
    // Disable explorer auto-restart
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" "
                   "/v AutoRestartShell /t REG_DWORD /d 0 /f");
    
    // === Hide Windows branding ===
    HideWindowsBootLogo();
    HideLockScreen();
    HideLogonUI();
    
    // Disable startup delay
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize\" /v StartupDelayInMSec /t REG_DWORD /d 0 /f");
    
    // Disable Windows Error Reporting popups
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\Windows Error Reporting\" /v DontShowUI /t REG_DWORD /d 1 /f");
    
    // Disable Action Center notifications
    ExecuteCommand("reg add \"HKCU\\Software\\Policies\\Microsoft\\Windows\\Explorer\" /v DisableNotificationCenter /t REG_DWORD /d 1 /f");
    
    // Disable Game Bar (we have our own overlay)
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\GameBar\" /v UseNexusForGameBarEnabled /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR\" /v AppCaptureEnabled /t REG_DWORD /d 0 /f");
    
    // Update config
    SystemConfig cfg = ReadSystemConfig();
    cfg.isShellMode = true;
    cfg.hasBackup = true;
    WriteSystemConfig(cfg);
    
    return true;
}

// ============================================================================
// SHELL MODE DEACTIVATION
// ============================================================================

bool DeactivateShellMode() {
    if (!CheckAdminRights()) {
        return false;
    }
    
    // Restore explorer as shell
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" "
                   "/v Shell /t REG_SZ /d explorer.exe /f");
    
    // Remove HKCU shell override if exists
    ExecuteCommand("reg delete \"HKCU\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v Shell /f", true, true);
    
    // Re-enable explorer auto-restart
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" "
                   "/v AutoRestartShell /t REG_DWORD /d 1 /f");
    
    // Restore Windows boot settings
    RestoreWindowsBootSettings();
    
    // Re-enable Action Center
    ExecuteCommand("reg delete \"HKCU\\Software\\Policies\\Microsoft\\Windows\\Explorer\" /v DisableNotificationCenter /f", true, true);
    
    // Re-enable Game Bar
    ExecuteCommand("reg delete \"HKCU\\Software\\Microsoft\\GameBar\" /v UseNexusForGameBarEnabled /f", true, true);
    ExecuteCommand("reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR\" /v AppCaptureEnabled /f", true, true);
    
    // Update config
    SystemConfig cfg = ReadSystemConfig();
    cfg.isShellMode = false;
    WriteSystemConfig(cfg);
    
    return true;
}

// ============================================================================
// PERFORMANCE MODE
// ============================================================================

bool ApplyPerformanceMode() {
    if (!CheckAdminRights()) {
        return false;
    }
    
    // Create backup if needed
    if (!HasBackup()) {
        CreateSystemBackup();
    }
    
    // ===== DISABLE TELEMETRY =====
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection\" /v AllowTelemetry /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Privacy\" /v TailoredExperiencesWithDiagnosticDataEnabled /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\AdvertisingInfo\" /v DisabledByGroupPolicy /t REG_DWORD /d 1 /f");
    ExecuteCommand("reg add \"HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo\" /v Enabled /t REG_DWORD /d 0 /f");
    
    // ===== DISABLE COPILOT =====
    ExecuteCommand("reg add \"HKCU\\Software\\Policies\\Microsoft\\Windows\\WindowsCopilot\" /v TurnOffWindowsCopilot /t REG_DWORD /d 1 /f");
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsCopilot\" /v TurnOffWindowsCopilot /t REG_DWORD /d 1 /f");
    
    // ===== DISABLE CORTANA =====
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search\" /v AllowCortana /t REG_DWORD /d 0 /f");
    
    // ===== DISABLE SERVICES =====
    const char* servicesToDisable[] = {
        "DiagTrack",
        "dmwappushservice", 
        "MapsBroker",
        "lfsvc",
        "RetailDemo",
        "WMPNetworkSvc",
        "wisvc",
        "PhoneSvc",
        "WalletService",
        "SysMain",
        "WSearch"
    };
    
    for (const char* svc : servicesToDisable) {
        ExecuteCommand(std::string("sc config ") + svc + " start= disabled");
        ExecuteCommand(std::string("sc stop ") + svc);
    }
    
    // ===== GAMING OPTIMIZATIONS =====
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\GameBar\" /v AutoGameModeEnabled /t REG_DWORD /d 1 /f");
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\GameBar\" /v AllowAutoGameMode /t REG_DWORD /d 1 /f");
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR\" /v AppCaptureEnabled /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKCU\\System\\GameConfigStore\" /v GameDVR_Enabled /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\" /v HwSchMode /t REG_DWORD /d 2 /f");
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\" /v SystemResponsiveness /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games\" /v \"GPU Priority\" /t REG_DWORD /d 8 /f");
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games\" /v Priority /t REG_DWORD /d 6 /f");
    
    // ===== HIGH PERFORMANCE POWER =====
    ExecuteCommand("powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c");
    ExecuteCommand("powercfg /hibernate off");
    
    // ===== DISABLE BACKGROUND APPS =====
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications\" /v GlobalUserDisabled /t REG_DWORD /d 1 /f");
    
    // ===== CLEAN TASKBAR =====
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v TaskbarDa /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" /v SearchboxTaskbarMode /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v ShowTaskViewButton /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v TaskbarMn /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Feeds\" /v ShellFeedsTaskbarViewMode /t REG_DWORD /d 2 /f");
    
    // ===== DISABLE PREFETCH =====
    ExecuteCommand("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters\" /v EnablePrefetcher /t REG_DWORD /d 0 /f");
    ExecuteCommand("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters\" /v EnableSuperfetch /t REG_DWORD /d 0 /f");
    
    // Update config
    SystemConfig cfg = ReadSystemConfig();
    cfg.isOptimized = true;
    cfg.hasBackup = true;
    WriteSystemConfig(cfg);
    
    return true;
}

bool RemovePerformanceMode() {
    // Re-enable services
    const char* servicesToEnable[] = {
        "DiagTrack",
        "SysMain", 
        "WSearch"
    };
    
    for (const char* svc : servicesToEnable) {
        ExecuteCommand(std::string("sc config ") + svc + " start= auto");
        ExecuteCommandNoWait(std::string("sc start ") + svc);
    }
    
    // Reset telemetry to default
    ExecuteCommand("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection\" /v AllowTelemetry /t REG_DWORD /d 1 /f");
    
    // Re-enable background apps
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications\" /v GlobalUserDisabled /t REG_DWORD /d 0 /f");
    
    // Restore taskbar
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v TaskbarDa /t REG_DWORD /d 1 /f");
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" /v SearchboxTaskbarMode /t REG_DWORD /d 1 /f");
    ExecuteCommand("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v ShowTaskViewButton /t REG_DWORD /d 1 /f");
    
    // Enable prefetch
    ExecuteCommand("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters\" /v EnablePrefetcher /t REG_DWORD /d 3 /f");
    ExecuteCommand("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters\" /v EnableSuperfetch /t REG_DWORD /d 3 /f");
    
    // Update config
    SystemConfig cfg = ReadSystemConfig();
    cfg.isOptimized = false;
    WriteSystemConfig(cfg);
    
    return true;
}

// ============================================================================
// EXPLORER & PROCESS CONTROL
// ============================================================================

void KillWindowsShellProcesses() {
    const char* processesToKill[] = {
        "ShellExperienceHost.exe",
        "SearchUI.exe",
        "SearchApp.exe",
        "StartMenuExperienceHost.exe",
        "RuntimeBroker.exe",
        "TextInputHost.exe",
        "LockApp.exe"
    };
    
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    
    if (Process32First(hSnap, &pe)) {
        do {
            for (const char* procName : processesToKill) {
                if (_stricmp(pe.szExeFile, procName) == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                    }
                    break;
                }
            }
        } while (Process32Next(hSnap, &pe));
    }
    
    CloseHandle(hSnap);
}

bool TerminateExplorer() {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;
    
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    
    bool killed = false;
    
    if (Process32First(hSnap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "explorer.exe") == 0) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    TerminateProcess(hProc, 0);
                    CloseHandle(hProc);
                    killed = true;
                }
            }
        } while (Process32Next(hSnap, &pe));
    }
    
    CloseHandle(hSnap);
    
    // Also kill Windows shell-related processes
    KillWindowsShellProcesses();
    
    return killed;
}

bool LaunchExplorer() {
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    
    char cmd[] = "explorer.exe";
    
    if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    
    return false;
}

// ============================================================================
// SYSTEM ACTIONS
// ============================================================================

static bool EnableShutdownPrivilege() {
    HANDLE hToken;
    TOKEN_PRIVILEGES tkp;
    
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }
    
    LookupPrivilegeValue(nullptr, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    
    AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, nullptr, 0);
    CloseHandle(hToken);
    
    return GetLastError() == ERROR_SUCCESS;
}

void PerformRestart() {
    EnableShutdownPrivilege();
    ExitWindowsEx(EWX_REBOOT | EWX_FORCE, SHTDN_REASON_MAJOR_OTHER);
}

void PerformShutdown() {
    EnableShutdownPrivilege();
    ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCE, SHTDN_REASON_MAJOR_OTHER);
}

void PerformSleep() {
    SetSuspendState(FALSE, FALSE, FALSE);
}

void PerformHibernate() {
    SetSuspendState(TRUE, FALSE, FALSE);
}

void PerformSignOut() {
    EnableShutdownPrivilege();
    ExitWindowsEx(EWX_LOGOFF | EWX_FORCE, SHTDN_REASON_MAJOR_OTHER);
}

// ============================================================================
// CONFIGURATION
// ============================================================================

SystemConfig ReadSystemConfig() {
    SystemConfig cfg;
    
    // Defaults
    cfg.showIntro = true;
    cfg.introDuration = 3.5f;
    cfg.isShellMode = false;
    cfg.isOptimized = false;
    cfg.hasBackup = false;
    cfg.hideBootLogo = true;
    cfg.hideLockScreen = true;
    cfg.autoLogin = false;
    cfg.username = "Player";
    
    std::string configPath = CONFIG_FILE;
    
    if (!fs::exists(configPath)) {
        // Create default config
        EnsureFoldersExist();
        WriteSystemConfig(cfg);
        return cfg;
    }
    
    std::ifstream f(configPath);
    std::string line;
    
    while (std::getline(f, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#' || line[0] == '[') continue;
        
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        
        // Trim whitespace
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) {
            val.pop_back();
        }
        while (!key.empty() && (key.back() == ' ')) {
            key.pop_back();
        }
        while (!key.empty() && (key.front() == ' ')) {
            key.erase(0, 1);
        }
        
        if (key == "isShellMode") cfg.isShellMode = (val == "1" || val == "true");
        else if (key == "isOptimized") cfg.isOptimized = (val == "1" || val == "true");
        else if (key == "hasBackup") cfg.hasBackup = (val == "1" || val == "true");
        else if (key == "showIntro") cfg.showIntro = (val == "1" || val == "true");
        else if (key == "hideBootLogo") cfg.hideBootLogo = (val == "1" || val == "true");
        else if (key == "hideLockScreen") cfg.hideLockScreen = (val == "1" || val == "true");
        else if (key == "autoLogin") cfg.autoLogin = (val == "1" || val == "true");
        else if (key == "introDuration") {
            try { cfg.introDuration = std::stof(val); } catch (...) { cfg.introDuration = 3.5f; }
        }
        else if (key == "introImagePath") cfg.introImagePath = val;
        else if (key == "introVideoPath") cfg.introVideoPath = val;
        else if (key == "username") cfg.username = val;
        else if (key == "autoLoginUser") cfg.autoLoginUser = val;
    }
    
    return cfg;
}

void WriteSystemConfig(const SystemConfig& cfg) {
    EnsureFoldersExist();
    
    std::ofstream f(CONFIG_FILE);
    
    f << "# Q-Shell System Configuration v2.5\n";
    f << "# Generated: " << time(nullptr) << "\n";
    f << "# Do not edit manually unless you know what you're doing\n\n";
    
    f << "[Shell]\n";
    f << "isShellMode=" << (cfg.isShellMode ? "1" : "0") << "\n";
    f << "isOptimized=" << (cfg.isOptimized ? "1" : "0") << "\n";
    f << "hasBackup=" << (cfg.hasBackup ? "1" : "0") << "\n\n";
    
    f << "[Boot]\n";
    f << "showIntro=" << (cfg.showIntro ? "1" : "0") << "\n";
    f << "hideBootLogo=" << (cfg.hideBootLogo ? "1" : "0") << "\n";
    f << "hideLockScreen=" << (cfg.hideLockScreen ? "1" : "0") << "\n";
    f << "introDuration=" << cfg.introDuration << "\n";
    f << "introImagePath=" << cfg.introImagePath << "\n";
    f << "introVideoPath=" << cfg.introVideoPath << "\n\n";
    
    f << "[User]\n";
    f << "username=" << cfg.username << "\n";
    f << "autoLogin=" << (cfg.autoLogin ? "1" : "0") << "\n";
    f << "autoLoginUser=" << cfg.autoLoginUser << "\n";
    
    f.close();
}