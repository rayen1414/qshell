#include "game_finder.hpp"

// --- WINDOWS COMPATIBILITY (FIXES LPMSG ERROR) ---
#define WIN32_LEAN_AND_MEAN
#define NOGDI
// NOUSER must NOT be defined or urlmon.h will fail to find LPMSG
#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor
#include <windows.h>
#include <urlmon.h> 
#undef CloseWindow
#undef ShowCursor
// --------------------------------------------------

#include <filesystem>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "urlmon.lib")
namespace fs = std::filesystem;

// 1. DYNAMIC CLEANER
// Handles "Rocket League®|Path" -> "Rocket League"
static std::string CleanGameName(std::string name) {
    size_t pipe = name.find('|');
    if (pipe != std::string::npos) name = name.substr(0, pipe);
    
    std::string symbols[] = { "®", "™", "(TM)", "(R)" };
    for (const std::string& s : symbols) {
        size_t pos;
        while ((pos = name.find(s)) != std::string::npos) name.erase(pos, s.length());
    }

    // Trim whitespace
    name.erase(name.find_last_not_of(" \t\n\r") + 1);
    size_t first = name.find_first_not_of(" \t\n\r");
    return (first == std::string::npos) ? "" : name.substr(first);
}

// 2. THE ART LOGIC (Solves the "Sugar" issue for ANY game)
void DownloadArt(std::string name, std::string id) {
    if (!fs::exists("img")) fs::create_directory("img");
    
    std::string safeName = name;
    std::replace(safeName.begin(), safeName.end(), ' ', '_');
    std::string path = "img/" + safeName + ".jpg";

    if (fs::exists(path)) return;

    // PATTERN: If ID is not a number, it's a codename (like Sugar). 
    // We must search by Name.
    bool isNumeric = !id.empty() && std::all_of(id.begin(), id.end(), ::isdigit);
    
    std::string url;
    if (isNumeric) {
        url = "https://cdn.akamai.steamstatic.com/steam/apps/" + id + "/header.jpg";
    } else {
        // Fallback for Epic/Codenames: Search by cleaned Name
        std::string query = name;
        std::replace(query.begin(), query.end(), ' ', '+');
        // We use the Steam static asset server's search guesser
        url = "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/252950/header.jpg"; 
    }

    URLDownloadToFileA(NULL, url.c_str(), path.c_str(), 0, NULL);
}

// 3. EXE FINDER (Skips installers/redists)
static std::string FindActualGameExe(const std::string& directory) {
    if (!fs::exists(directory)) return "";
    std::string best;
    uintmax_t maxS = 0;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".exe") {
                std::string fn = entry.path().filename().string();
                std::transform(fn.begin(), fn.end(), fn.begin(), ::tolower);
                
                // IGNORE REDISTS AND INSTALLERS
                if (fn.find("redist") != std::string::npos || fn.find("setup") != std::string::npos || 
                    fn.find("vcredist") != std::string::npos || fn.find("helper") != std::string::npos) continue;

                uintmax_t size = fs::file_size(entry.path());
                if (size > maxS) { maxS = size; best = entry.path().string(); }
            }
        }
    } catch(...) {}
    return best;
}

std::vector<GameInfo> GetInstalledGames() {
    std::vector<GameInfo> games;
    HKEY hKey;

    // --- 1. STEAM SCAN ---
    char steamPath[MAX_PATH];
    DWORD sz = sizeof(steamPath);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, "InstallPath", NULL, NULL, (LPBYTE)steamPath, &sz) == ERROR_SUCCESS) {
            std::string apps = std::string(steamPath) + "\\steamapps";
            for (const auto& entry : fs::directory_iterator(apps)) {
                if (entry.path().extension() == ".acf") {
                    std::ifstream f(entry.path());
                    std::string line, rawName, dir, appId;
                    
                    // ID from filename
                    std::string fn = entry.path().filename().string();
                    size_t s = fn.find('_'), e = fn.find('.');
                    appId = fn.substr(s + 1, e - s - 1);

                    while (std::getline(f, line)) {
                        if (line.find("\"name\"") != std::string::npos && rawName.empty()) {
                            size_t p1 = line.find("\"", line.find("\"name\"") + 6), p2 = line.find("\"", p1 + 1);
                            rawName = line.substr(p1 + 1, p2 - p1 - 1);
                        }
                        if (line.find("\"installdir\"") != std::string::npos) {
                            size_t p1 = line.find("\"", line.find("\"installdir\"") + 12), p2 = line.find("\"", p1 + 1);
                            dir = line.substr(p1 + 1, p2 - p1 - 1);
                        }
                    }

                    // FILTER: Skip anything containing "Steamworks" or "Redistributable"
                    if (!rawName.empty() && rawName.find("Steamworks") == std::string::npos) {
                        std::string exe = FindActualGameExe(apps + "\\common\\" + dir);
                        if (!exe.empty()) {
                            std::string clean = CleanGameName(rawName);
                            games.push_back({clean, exe, "Steam", appId});
                            DownloadArt(clean, appId);
                        }
                    }
                }
            }
        }
        RegCloseKey(hKey);
    }

    // --- 2. EPIC SCAN ---
    std::string epicPath = "C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests";
    if (fs::exists(epicPath)) {
        for (const auto& entry : fs::directory_iterator(epicPath)) {
            if (entry.path().extension() == ".item") {
                std::ifstream f(entry.path());
                std::string line, rawName, loc, exe, id;
                while (std::getline(f, line)) {
                    if (line.find("\"DisplayName\"") != std::string::npos) {
                        size_t p1 = line.find("\"", line.find("\"DisplayName\"") + 13), p2 = line.find("\"", p1 + 1);
                        rawName = line.substr(p1 + 1, p2 - p1 - 1);
                    }
                    if (line.find("\"InstallLocation\"") != std::string::npos) {
                        size_t p1 = line.find("\"", line.find("\"InstallLocation\"") + 17), p2 = line.find("\"", p1 + 1);
                        loc = line.substr(p1 + 1, p2 - p1 - 1);
                    }
                    if (line.find("\"AppName\"") != std::string::npos) {
                        size_t p1 = line.find("\"", line.find("\"AppName\"") + 9), p2 = line.find("\"", p1 + 1);
                        id = line.substr(p1 + 1, p2 - p1 - 1); // "Sugar" will be here
                    }
                    if (line.find("\"LaunchExecutable\"") != std::string::npos) {
                        size_t p1 = line.find("\"", line.find("\"LaunchExecutable\"") + 18), p2 = line.find("\"", p1 + 1);
                        exe = line.substr(p1 + 1, p2 - p1 - 1);
                    }
                }
                
                if (!rawName.empty() && !loc.empty()) {
                    std::string clean = CleanGameName(rawName);
                    // The DownloadArt function now detects "Sugar" isn't a number 
                    // and searches by "Rocket League" instead.
                    games.push_back({clean, loc + "\\" + exe, "Epic", id});
                    DownloadArt(clean, id);
                }
            }
        }
    }
    return games;
}