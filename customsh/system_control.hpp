// ============================================================================
// SYSTEM_CONTROL.HPP - Q-SHELL v2.5
// ============================================================================

#pragma once

#include <string>

// System configuration structure
struct SystemConfig {
    bool showIntro = true;
    float introDuration = 3.5f;
    bool isShellMode = false;
    bool isOptimized = false;
    bool hasBackup = false;
    bool hideBootLogo = true;
    bool hideLockScreen = true;
    bool autoLogin = false;
    std::string username = "Player";
    std::string autoLoginUser;
    std::string introImagePath;
    std::string introVideoPath;
};

// Utility functions
std::string GetCurrentExePath();
std::string GetExeDirectory();

// Admin functions
bool CheckAdminRights();
bool RequestAdminRights();

// Shell mode functions
bool CheckIfShellMode();
bool ActivateShellMode();
bool DeactivateShellMode();

// Boot customization
bool HideWindowsBootLogo();
bool HideLockScreen();
bool HideLogonUI();
bool RestoreWindowsBootSettings();

// Auto-login
bool SetupAutoLogin(const std::string& username, const std::string& password = "");
bool DisableAutoLogin();

// Backup system
bool HasBackup();
bool CreateSystemBackup();
bool RestoreSystemBackup();
void CreateSafetyRestore();

// Performance mode
bool ApplyPerformanceMode();
bool RemovePerformanceMode();

// Process control
bool TerminateExplorer();
bool LaunchExplorer();
void KillWindowsShellProcesses();

// System actions
void PerformRestart();
void PerformShutdown();
void PerformSleep();
void PerformHibernate();
void PerformSignOut();

// Configuration
SystemConfig ReadSystemConfig();
void WriteSystemConfig(const SystemConfig& cfg);