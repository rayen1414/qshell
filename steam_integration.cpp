// ============================================================================
// STEAM_INTEGRATION.CPP  —  v3.0  (D2D backend  —  no raylib)
// ============================================================================

#include "steam_integration.hpp"
#include "d2d_renderer.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>
#include <map>

namespace fs = std::filesystem;

// ─── Colour helpers ───────────────────────────────────────────────────────────

static inline D2D1_COLOR_F C(float r, float g, float b, float a = 1.f) {
    return { r, g, b, a };
}
static inline D2D1_COLOR_F C8(int r, int g, int b, int a = 255) {
    return { r/255.f, g/255.f, b/255.f, a/255.f };
}
static inline D2D1_COLOR_F Fade(D2D1_COLOR_F c, float a) {
    return { c.r, c.g, c.b, c.a * a };
}
static inline D2D1_COLOR_F D2CC(D2DColor c) {
    return { c.r, c.g, c.b, c.a };
}

// ─── Internal data structures ─────────────────────────────────────────────────

struct SteamAchievement {
    std::string name, displayName, description;
    bool        unlocked = false;
    std::string unlockTime, appId, gameName;
};

struct SteamGameStats {
    std::string appId, gameName;
    int totalAchievements = 0, unlockedAchievements = 0;
    float completionPercent = 0.f;
    int hoursPlayed = 0;
};

// ─── Steam path helpers ───────────────────────────────────────────────────────

static std::string GetSteamInstallPath() {
    char steamPath[MAX_PATH] = {};
    DWORD sz = sizeof(steamPath);
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\WOW6432Node\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr,
            reinterpret_cast<LPBYTE>(steamPath), &sz);
        RegCloseKey(hKey);
    }
    return std::string(steamPath);
}

static std::string GetSteamUserDataPath() {
    std::string steamPath = GetSteamInstallPath();
    if (steamPath.empty()) return "";
    std::string userDataPath = steamPath + "\\userdata";
    try {
        if (fs::exists(userDataPath)) {
            for (const auto& e : fs::directory_iterator(userDataPath)) {
                if (!e.is_directory()) continue;
                std::string uid = e.path().filename().string();
                if (uid != "0" && uid != "ac") return e.path().string();
            }
        }
    } catch (...) {}
    return "";
}

static std::string ExtractVdfValue(const std::string& line, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = line.find(search);
    if (pos == std::string::npos) return "";
    size_t p1 = line.find('"', pos + search.size());
    if (p1 == std::string::npos) return "";
    size_t p2 = line.find('"', p1 + 1);
    if (p2 == std::string::npos) return "";
    return line.substr(p1 + 1, p2 - p1 - 1);
}

// ─── Data functions ───────────────────────────────────────────────────────────

SteamProfile GetSteamProfile() {
    SteamProfile profile;
    std::string steamPath = GetSteamInstallPath();
    if (steamPath.empty()) return profile;

    std::string vdfPath = steamPath + "\\config\\loginusers.vdf";
    std::ifstream vdf(vdfPath);
    if (!vdf.is_open()) return profile;

    std::string line;
    while (std::getline(vdf, line)) {
        std::string name = ExtractVdfValue(line, "PersonaName");
        if (!name.empty()) { profile.username = name; break; }
    }
    profile.status        = "Online";
    profile.profileLoaded = true;
    return profile;
}

D2DBitmapHandle LoadSteamProfilePicture() {
    std::string steamPath = GetSteamInstallPath();
    if (steamPath.empty()) return {};

    std::string userDataPath = steamPath + "\\userdata";
    try {
        if (fs::exists(userDataPath)) {
            for (const auto& e : fs::directory_iterator(userDataPath)) {
                if (!e.is_directory()) continue;
                std::string uid = e.path().filename().string();
                if (uid == "0" || uid == "ac") continue;
                std::string avatarPath = e.path().string() + "\\config\\avatar.jpg";
                if (fs::exists(avatarPath)) {
                    D2DBitmap bmp = D2D().LoadBitmapA(avatarPath.c_str());
                    return { bmp.bmp, bmp.w, bmp.h };
                }
            }
        }
    } catch (...) {}
    return {};
}

std::vector<ResumeEntry> GetRecentGames() {
    std::vector<ResumeEntry> entries;
    std::string userDataPath = GetSteamUserDataPath();
    if (userDataPath.empty()) return entries;

    std::string localCfgPath = userDataPath + "\\config\\localconfig.vdf";
    std::ifstream f(localCfgPath);
    if (!f.is_open()) return entries;

    std::string line, currentAppId, currentName;
    int currentHours = 0;
    bool inApps = false;

    while (std::getline(f, line)) {
        if (line.find("\"apps\"") != std::string::npos) { inApps = true; continue; }
        if (!inApps) continue;

        if (line.find("\"LastPlayed\"") != std::string::npos) {
            std::string val = ExtractVdfValue(line, "LastPlayed");
            if (!val.empty() && !currentAppId.empty()) {
                ResumeEntry e;
                e.gameName       = currentAppId;
                e.lastPlayedTime = "Recently";
                e.hoursPlayed    = currentHours;
                e.isRecentlyPlayed = true;
                entries.push_back(e);
            }
            currentAppId = ""; currentHours = 0;
        }
        if (line.find("\"Playtime2wks\"") != std::string::npos) {
            std::string val = ExtractVdfValue(line, "Playtime2wks");
            if (!val.empty()) currentHours = std::stoi(val) / 60;
        }
    }
    return entries;
}

std::vector<SteamFriend> GetRealSteamFriends() { return {}; }

std::vector<GamingAccount> GetGamingAccounts() {
    std::vector<GamingAccount> accounts;

    // Steam
    {
        GamingAccount acc;
        acc.platform    = "Steam";
        acc.icon        = "S";
        acc.accentColor = { 102/255.f, 192/255.f, 244/255.f, 1.f };
        std::string steamPath = GetSteamInstallPath();
        bool installed = !steamPath.empty();
        if (installed) {
            std::string vdfPath = steamPath + "\\config\\loginusers.vdf";
            std::ifstream vdf(vdfPath);
            std::string line;
            while (std::getline(vdf, line)) {
                std::string name = ExtractVdfValue(line, "PersonaName");
                if (!name.empty()) { acc.username = name; break; }
            }
        }
        acc.isConnected = installed && !acc.username.empty();
        acc.statusText  = acc.isConnected ? "Connected" : "Click to sign in";
        accounts.push_back(acc);
    }
    // Epic
    {
        GamingAccount acc;
        acc.platform    = "Epic Games";
        acc.icon        = "E";
        acc.accentColor = { 0.7f, 0.7f, 0.7f, 1.f };
        acc.isConnected =
            fs::exists("C:\\Program Files\\Epic Games\\Launcher\\Portal\\Binaries\\Win32\\EpicGamesLauncher.exe") ||
            fs::exists("C:\\Program Files (x86)\\Epic Games\\Launcher\\Portal\\Binaries\\Win32\\EpicGamesLauncher.exe");
        acc.statusText = acc.isConnected ? "Connected" : "Not installed";
        accounts.push_back(acc);
    }
    // EA
    {
        GamingAccount acc;
        acc.platform    = "EA App";
        acc.icon        = "EA";
        acc.accentColor = { 1.f, 0.39f, 0.39f, 1.f };
        acc.isConnected = fs::exists("C:\\Program Files\\Electronic Arts\\EA Desktop\\EA Desktop\\EADesktop.exe");
        acc.statusText  = acc.isConnected ? "Connected" : "Not installed";
        accounts.push_back(acc);
    }
    // Xbox
    {
        GamingAccount acc;
        acc.platform    = "Xbox";
        acc.icon        = "X";
        acc.accentColor = { 16/255.f, 124/255.f, 16/255.f, 1.f };
        acc.isConnected = true;
        acc.statusText  = "Open Xbox App";
        accounts.push_back(acc);
    }
    // GOG
    {
        GamingAccount acc;
        acc.platform    = "GOG Galaxy";
        acc.icon        = "G";
        acc.accentColor = { 145/255.f, 71/255.f, 1.f, 1.f };
        acc.isConnected = fs::exists("C:\\Program Files (x86)\\GOG Galaxy\\GalaxyClient.exe");
        acc.statusText  = acc.isConnected ? "Connected" : "Not installed";
        accounts.push_back(acc);
    }
    // Ubisoft
    {
        GamingAccount acc;
        acc.platform    = "Ubisoft";
        acc.icon        = "U";
        acc.accentColor = { 0.f, 120/255.f, 215/255.f, 1.f };
        acc.isConnected = fs::exists("C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\upc.exe");
        acc.statusText  = acc.isConnected ? "Connected" : "Not installed";
        accounts.push_back(acc);
    }
    return accounts;
}

// ─── Action functions ─────────────────────────────────────────────────────────

void OpenResumeDossier() { ShellExecuteA(nullptr, "open", "steam://open/games", nullptr, nullptr, SW_SHOW); }
void ShareToDiscord(const std::string&) {}
void ShareToSteam(const std::string& gameID) {
    ShellExecuteA(nullptr, "open", ("steam://store/" + gameID).c_str(), nullptr, nullptr, SW_SHOW);
}
void OpenTwitchProfile()  { ShellExecuteA(nullptr, "open", "https://twitch.tv", nullptr, nullptr, SW_SHOW); }
void OpenSteamCommunity() { ShellExecuteA(nullptr, "open", "steam://url/SteamIDFriendsPage", nullptr, nullptr, SW_SHOW); }
void LaunchCloudStreaming() { ShellExecuteA(nullptr, "open", "parsec://", nullptr, nullptr, SW_SHOW); }

static std::string GetCustomAppFile(int slot) {
    char appData[MAX_PATH];
    if (SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData) == S_OK)
        return std::string(appData) + "\\QShell\\custom_app_" + std::to_string(slot) + ".txt";
    return "";
}

std::string GetCustomAppPath(int slot) {
    std::string file = GetCustomAppFile(slot);
    if (file.empty() || !fs::exists(file)) return "";
    std::ifstream f(file); std::string path; std::getline(f, path); return path;
}

void SetCustomAppPath(int slot, const std::string& path, const std::string& name) {
    std::string file = GetCustomAppFile(slot);
    if (file.empty()) return;
    try { fs::create_directories(fs::path(file).parent_path()); } catch (...) {}
    std::ofstream f(file); f << path << "\n" << name << "\n";
}

void LaunchCustomAppSlot(int slot) {
    std::string path = GetCustomAppPath(slot);
    if (!path.empty() && fs::exists(path))
        ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOW);
}

void ConnectAccount(const std::string& platform) {
    struct { const char* name; const char* cmd; } launchers[] = {
        {"Steam",      "steam://open/main"},
        {"Epic Games", "com.epicgames.launcher://"},
        {"Xbox",       "xbox:"},
    };
    for (auto& l : launchers) {
        if (platform == l.name) { ShellExecuteA(nullptr, "open", l.cmd, nullptr, nullptr, SW_SHOW); return; }
    }
    struct { const char* name; const char* path; } exes[] = {
        {"EA App",     "C:\\Program Files\\Electronic Arts\\EA Desktop\\EA Desktop\\EADesktop.exe"},
        {"GOG Galaxy", "C:\\Program Files (x86)\\GOG Galaxy\\GalaxyClient.exe"},
        {"Ubisoft",    "C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\upc.exe"},
    };
    for (auto& e : exes) {
        if (platform == e.name && fs::exists(e.path)) {
            ShellExecuteA(nullptr, "open", e.path, nullptr, nullptr, SW_SHOW); return;
        }
    }
}

void DisconnectAccount(const std::string&) {}

bool IsAccountConnected(const std::string& platform) {
    for (auto& acc : GetGamingAccounts())
        if (acc.platform == platform) return acc.isConnected;
    return false;
}

// ─── Render: Steam Profile ────────────────────────────────────────────────────

void RenderSteamProfile(int x, int y, const SteamProfile& profile,
                        D2DColor accent_, D2DColor /*secondary*/,
                        D2DColor text_,   D2DColor /*textDim*/,
                        float time, bool focused)
{
    auto& d2d = D2D();
    auto  acc  = D2CC(accent_);
    auto  txt  = D2CC(text_);
    float pulse = (sinf(time * 2.2f) + 1.f) / 2.f;
    float beat  = (sinf(time * 6.0f) + 1.f) / 2.f;
    const int W = 620, H = 140;

    d2d.FillGradientV((float)x, (float)y, (float)W, (float)H,
        C8(18, 22, 32), C8(12, 16, 24));

    if (focused) {
        for (int b = 0; b < 3; b++) {
            float a = (0.65f - b * 0.15f) + pulse * 0.3f;
            d2d.StrokeRoundRect(x - 3.f - b*2, y - 3.f - b*2,
                W + 6.f + b*4, H + 6.f + b*4,
                4, 4, 1.5f, Fade(acc, a));
        }
        const char* tag = "[A] VIEW ACCOUNTS";
        float tw = d2d.MeasureTextA(tag, 11);
        d2d.FillRoundRect(x + W - tw - 24.f, y + H - 22.f, tw + 18, 18, 4, 4, Fade(acc, 0.22f));
        d2d.DrawTextA(tag, x + W - tw - 15.f, y + H - 19.f, 11.f, Fade(txt, 0.9f));
    } else {
        d2d.StrokeRoundRect((float)x, (float)y, (float)W, (float)H, 4, 4, 1.f, Fade(acc, 0.3f));
    }

    // Left stripe
    d2d.FillRoundRect((float)x, (float)y, 5, (float)H, 2, 2, acc);

    // Avatar circle
    float ax = x + 75.f, ay = y + H / 2.f;
    d2d.FillCircle(ax, ay, 48.f + pulse * 3.f, Fade(acc, 0.08f));
    d2d.FillCircle(ax, ay, 44.f, C8(22, 28, 42));
    d2d.StrokeCircle(ax, ay, 44.f, 1.5f, Fade(acc, 0.6f + pulse * 0.25f));

    char init[2] = { profile.username.empty() ? 'K' : (char)toupper(profile.username[0]), 0 };
    float initW = d2d.MeasureTextA(init, 38.f);
    d2d.DrawTextA(init, ax - initW / 2.f, ay - 19.f, 38.f, acc);

    // Name & status
    d2d.DrawTextA(profile.username.c_str(), x + 140.f, y + 28.f, 28.f, txt);
    auto statusCol = (profile.status == "Online") ? C8(100, 255, 180) : C8(140, 150, 165);
    if (profile.status == "Online")
        d2d.FillCircle(x + 140.f, y + 68.f, 8.f + beat * 2.5f, Fade(statusCol, 0.15f));
    d2d.FillCircle(x + 140.f, y + 68.f, 6.f, statusCol);
    d2d.DrawTextA(profile.status.c_str(), x + 154.f, y + 62.f, 14.f, statusCol);

    // Stat boxes
    struct StatBox { const char* label; int value; D2D1_COLOR_F col; };
    StatBox stats[] = {
        {"GAMES",   profile.gamesOwned,   C8(100, 180, 255)},
        {"FRIENDS", profile.friendsCount, C8(255, 180, 100)},
        {"HOURS",   profile.hoursPlayed,  C8(100, 255, 180)},
    };
    float px = x + 300.f;
    for (auto& st : stats) {
        d2d.FillRoundRect(px, y + 20.f, 90, 64, 4, 4, Fade(st.col, 0.08f));
        d2d.StrokeRoundRect(px, y + 20.f, 90, 64, 4, 4, 1.f, Fade(st.col, 0.35f));
        d2d.FillRect(px, y + 20.f, 90, 3, Fade(st.col, 0.6f));
        char v[12]; sprintf(v, "%d", st.value);
        float vw = d2d.MeasureTextA(v, 24.f);
        d2d.DrawTextA(v, px + 45.f - vw/2.f, y + 34.f, 24.f, st.col);
        float lw = d2d.MeasureTextA(st.label, 10.f);
        d2d.DrawTextA(st.label, px + 45.f - lw/2.f, y + 65.f, 10.f, Fade(st.col, 0.7f));
        px += 100.f;
    }
}

// ─── Render: Resume Hub ────────────────────────────────────────────────────────

void RenderResumeHub(int x, int y, const std::vector<ResumeEntry>& entries,
                     int focused,
                     D2DColor accent_, D2DColor /*secondary*/,
                     D2DColor text_,   D2DColor textDim_,
                     float time, int scrollOffset, D2DBitmapHandle /*artCover*/)
{
    auto& d2d = D2D();
    auto  acc  = D2CC(accent_);
    auto  txt  = D2CC(text_);
    auto  dim  = D2CC(textDim_);
    auto  grn  = C8(100, 255, 180);
    float iconPulse = (sinf(time * 3.f) + 1.f) / 2.f;

    // Header
    int headerY = y;
    float iconX = x + 26.f, iconY = y + 20.f;
    d2d.FillCircle(iconX, iconY, 26.f + iconPulse * 6.f, Fade(grn, 0.18f));
    d2d.FillCircle(iconX, iconY, 26.f, C8(18, 24, 36));
    d2d.StrokeCircle(iconX, iconY, 26.f, 1.5f, Fade(grn, 0.7f + iconPulse * 0.25f));
    // Play triangle (approximate with text)
    d2d.DrawTextA(">", iconX - 6.f, iconY - 9.f, 18.f, Fade(grn, 0.95f));

    d2d.DrawTextA("CONTINUE PLAYING", x + 70.f, (float)headerY, 28.f, txt);
    d2d.FillRect(x + 70.f, headerY + 38.f, 240, 4, grn);

    // Count badge
    if (!entries.empty()) {
        int total = (int)entries.size();
        char countTxt[48];
        if (total > 4) snprintf(countTxt, sizeof(countTxt), "%d/%d Games", scrollOffset + 1, total);
        else           snprintf(countTxt, sizeof(countTxt), "%d Recent Game%s", total, total == 1 ? "" : "s");
        float badgeW = 150, badgeX = x + 330.f;
        d2d.FillRoundRect(badgeX, headerY + 2.f, badgeW, 32, 8, 8, Fade(grn, 0.18f));
        float cw = d2d.MeasureTextA(countTxt, 13.f);
        d2d.DrawTextA(countTxt, badgeX + (badgeW - cw) / 2.f, headerY + 10.f, 13.f, grn);
    }

    // Empty state
    if (entries.empty()) {
        float ey = y + 80.f;
        d2d.FillRoundRect((float)x, ey, 1200, 200, 4, 4, Fade(C8(16, 20, 30), 0.96f));
        d2d.FillCircle(x + 600.f, ey + 100.f, 48.f, Fade(grn, 0.1f));
        d2d.StrokeCircle(x + 600.f, ey + 100.f, 48.f, 1.5f, Fade(grn, 0.3f));
        float qw = d2d.MeasureTextA("?", 42.f);
        d2d.DrawTextA("?", x + 600.f - qw/2.f, ey + 79.f, 42.f, Fade(grn, 0.45f));
        d2d.DrawTextA("No recent games found", x + 480.f, ey + 155.f, 18.f, Fade(txt, 0.7f));
        d2d.DrawTextA("Launch a game from your Library to see it here",
                      x + 405.f, ey + 180.f, 14.f, Fade(dim, 0.55f));
        return;
    }

    // Game cards
    const float CARD_W = 300, CARD_H = 220, GAP = 24;
    const int   VISIBLE = 4;
    D2D1_COLOR_F cardAccents[] = {
        C8(100, 200, 255), C8(100, 255, 180), C8(255, 190, 100),
        C8(230, 120, 255), C8(255, 130, 160)
    };
    float startY = y + 75.f;

    for (int i = 0; i < VISIBLE && (scrollOffset + i) < (int)entries.size(); i++) {
        int gi  = scrollOffset + i;
        float cx = x + i * (CARD_W + GAP);
        bool isFoc = (gi == focused);
        auto  col  = cardAccents[gi % 5];
        float fp   = isFoc ? (sinf(time * 6.f) * 0.5f + 0.5f) : 0.f;

        d2d.FillGradientV(cx, startY, CARD_W, CARD_H,
            Fade(C8(26, 32, 46), isFoc ? 1.f : 0.96f),
            Fade(C8(16, 20, 32), isFoc ? 1.f : 0.96f));

        float barH = isFoc ? 6.f : 5.f;
        d2d.FillRect(cx, startY, CARD_W, barH, isFoc ? col : Fade(col, 0.65f));
        if (isFoc)
            d2d.FillGradientV(cx, startY + barH, CARD_W, 16, Fade(col, 0.35f), Fade(col, 0.f));

        if (isFoc)
            d2d.StrokeRoundRect(cx - 4, startY - 4, CARD_W + 8, CARD_H + 8, 5, 5,
                                1.5f, Fade(col, 0.8f + fp * 0.15f));
        else
            d2d.StrokeRoundRect(cx, startY, CARD_W, CARD_H, 5, 5,
                                1.f, Fade(C8(40, 48, 65), 0.7f));

        // Initial circle
        float circleY = startY + 68.f;
        if (isFoc) d2d.FillCircle(cx + CARD_W/2.f, circleY, 52.f + fp*4.f, Fade(col, 0.14f));
        d2d.FillCircle(cx + CARD_W/2.f, circleY, 52.f, Fade(col, isFoc ? 0.24f : 0.14f));
        d2d.StrokeCircle(cx + CARD_W/2.f, circleY, 52.f, 1.5f, Fade(col, isFoc ? 0.7f : 0.35f));

        std::string ltr = entries[gi].gameName.empty() ? "G" : entries[gi].gameName.substr(0, 1);
        float lw = d2d.MeasureTextA(ltr.c_str(), 52.f);
        d2d.DrawTextA(ltr.c_str(), cx + CARD_W/2.f - lw/2.f, circleY - 26.f, 52.f,
                      isFoc ? col : Fade(col, 0.8f));

        // Hours badge
        char hStr[20]; snprintf(hStr, sizeof(hStr), "%dh played", entries[gi].hoursPlayed);
        float bx = cx + CARD_W - 100.f;
        d2d.FillRoundRect(bx, startY + 10.f, 90, 30, 8, 8, Fade(C8(0,0,0), 0.95f));
        d2d.DrawTextA(hStr, bx + 8.f, startY + 20.f, 11.f, Fade(txt, 0.95f));

        // Name
        std::string gn = entries[gi].gameName;
        if (gn.length() > 28) gn = gn.substr(0, 27) + "..";
        float nw = d2d.MeasureTextA(gn.c_str(), 17.f);
        d2d.DrawTextA(gn.c_str(), cx + (CARD_W - nw)/2.f, startY + 155.f, 17.f,
                      isFoc ? txt : Fade(txt, 0.88f));

        // Last played
        float tw2 = d2d.MeasureTextA(entries[gi].lastPlayedTime.c_str(), 12.f);
        d2d.DrawTextA(entries[gi].lastPlayedTime.c_str(),
                      cx + (CARD_W - tw2)/2.f, startY + 177.f, 12.f, Fade(dim, 0.7f));

        // Progress bar
        float barY2  = startY + CARD_H - 22.f;
        float barW2  = CARD_W - 36.f;
        float progress = std::min(1.f, entries[gi].hoursPlayed / 200.f);
        d2d.FillRoundRect(cx + 18.f, barY2, barW2, 8, 4, 4, Fade(C8(24, 30, 44), 0.95f));
        if (progress > 0.f)
            d2d.FillRoundRect(cx + 18.f, barY2, barW2 * progress, 8, 4, 4,
                              Fade(col, 0.8f + fp * 0.15f));

        // [A] button on focus
        if (isFoc) {
            float ep   = (sinf(time * 5.f) + 1.f) / 2.f;
            float btnY = startY + CARD_H - 48.f;
            float btnW = 160.f, btnX = cx + CARD_W/2.f - btnW/2.f;
            d2d.FillRoundRect(btnX, btnY, btnW, 32, 8, 8, Fade(col, 0.2f + ep * 0.15f));
            d2d.StrokeRoundRect(btnX, btnY, btnW, 32, 8, 8, 1.f, Fade(col, 0.6f + ep * 0.35f));
            d2d.DrawTextA("[A]",     btnX + 16.f, btnY + 9.f, 15.f, Fade(col, 0.9f));
            d2d.DrawTextA("to enter",btnX + 50.f, btnY + 9.f, 14.f, Fade(txt, 0.85f));
        }
    }
}

// ─── Render: Special Hub Boxes ────────────────────────────────────────────────

void RenderSpecialHubBoxes(int x, int y, D2DBitmapHandle artCover,
                           int focusedBox,
                           D2DColor accent_, D2DColor text_, D2DColor textDim_,
                           float time)
{
    auto& d2d = D2D();
    auto  acc  = D2CC(accent_);
    auto  txt  = D2CC(text_);
    auto  dim  = D2CC(textDim_);
    float pulse = (sinf(time * 3.5f) + 1.f) / 2.f;

    const int BOX_W   = 340, GAP = 28;
    const int TOTAL_W = 4 * BOX_W + 3 * GAP;
    const int SUB_W   = (TOTAL_W - 2 * GAP) / 3;
    const int SUB_H   = 200, HUB_H = 380, ROW_GAP = 16;

    auto hubCol = C8(100, 180, 255);
    D2D1_COLOR_F subCols[3] = { C8(80, 255, 150), C8(255, 180, 60), C8(200, 100, 255) };
    struct SubInfo { const char* name; const char* desc; };
    SubInfo subs[3] = { {"RESUME","Quick States"}, {"CLOUD","Stream Play"}, {"SYNC","Save Sharing"} };

    // ── Social Hub Banner ──
    {
        bool isFoc = (focusedBox == 0);
        d2d.FillGradientV((float)x, (float)y, (float)TOTAL_W, (float)HUB_H,
            Fade(C8(16,20,34), 1.f), Fade(C8(10,13,24), 1.f));

        float barH = isFoc ? 10.f : 6.f;
        d2d.FillRect((float)x, (float)y, (float)TOTAL_W, barH, isFoc ? hubCol : Fade(hubCol, 0.6f));
        if (isFoc) {
            d2d.FillGradientV((float)x, y + barH, (float)TOTAL_W, 28, Fade(hubCol, 0.28f), Fade(hubCol, 0.f));
            for (int b = 0; b < 3; b++) {
                float a = (0.6f - b * 0.13f) + pulse * 0.3f;
                d2d.StrokeRoundRect(x - 4.f - b*2, y - 4.f - b*2,
                    TOTAL_W + 8.f + b*4, HUB_H + 8.f + b*4, 2, 2, 1.5f, Fade(hubCol, a));
            }
        } else {
            d2d.StrokeRoundRect((float)x, (float)y, (float)TOTAL_W, (float)HUB_H,
                2, 2, 1.f, Fade(C8(40, 50, 72), 0.5f));
        }

        // Art cover / placeholder
        int imgAreaW = BOX_W + GAP/2;
        float imgCX = x + imgAreaW/2.f, imgCY = y + HUB_H/2.f + 25.f;
        float imgMaxH = HUB_H - 62.f;

        d2d.DrawTextA("SOCIAL HUB", x + 14.f, y + 14.f, 28.f,
                      isFoc ? txt : Fade(txt, 0.85f));
        d2d.FillRect(x + 14.f, y + 48.f, 150, 3, Fade(hubCol, 0.85f));

        if (artCover.opaque) {
            D2DBitmap bmp{ reinterpret_cast<ID2D1Bitmap*>(artCover.opaque), artCover.w, artCover.h };
            float sc = std::min((imgAreaW - 28.f) / bmp.w, imgMaxH / bmp.h);
            float dw = bmp.w * sc, dh = bmp.h * sc;
            d2d.DrawBitmap(bmp, imgCX - dw/2.f, imgCY - dh/2.f, dw, dh);
        } else {
            d2d.FillCircle(imgCX, imgCY, imgMaxH/2.f - 8.f, Fade(hubCol, 0.09f));
            d2d.StrokeCircle(imgCX, imgCY, imgMaxH/2.f - 8.f, 1.5f, Fade(hubCol, 0.35f));
            float qw = d2d.MeasureTextA("?", 52.f);
            d2d.DrawTextA("?", imgCX - qw/2.f, imgCY - 28.f, 52.f, Fade(hubCol, 0.45f));
        }

        // Divider
        d2d.FillRect(x + imgAreaW + 4.f, y + 14.f, 1, HUB_H - 28.f, Fade(hubCol, 0.18f));

        // Activity feed
        float feedX = x + imgAreaW + 18.f, feedY = y + 14.f;
        d2d.DrawTextA("ACTIVITY", feedX, feedY, 15.f, Fade(dim, 0.9f));
        d2d.FillRect(feedX, feedY + 20.f, 80, 2, Fade(hubCol, 0.5f));
        const char* acts[] = {"Online and ready","No recent activity","Steam library synced","Quick Resume available"};
        for (int i = 0; i < 4; i++) {
            float ay = feedY + 30.f + i * 62.f;
            float rp = (sinf(time * 1.4f + i * 1.f) + 1.f) / 2.f;
            auto  rc = i == 0 ? hubCol : Fade(hubCol, 0.5f - i * 0.06f);
            d2d.FillCircle(feedX + 9.f, ay + 11.f, 5.f, Fade(rc, 0.22f + rp * 0.18f));
            d2d.StrokeCircle(feedX + 9.f, ay + 11.f, 5.f, 1.f, Fade(rc, 0.6f));
            d2d.DrawTextA(acts[i], feedX + 22.f, ay + 4.f, 13.f, Fade(txt, 0.73f - i * 0.1f));
            d2d.FillRect(feedX, ay + 26.f, 260, 1, Fade(C8(40,50,70), 0.45f));
        }

        // Quick stats
        float stX = x + TOTAL_W - 320.f, stY = y + 14.f;
        d2d.DrawTextA("QUICK STATS", stX, stY, 15.f, Fade(dim, 0.9f));
        d2d.FillRect(stX, stY + 20.f, 110, 2, Fade(hubCol, 0.5f));
        struct QS { const char* l; const char* v; D2D1_COLOR_F c; };
        QS qs[] = {
            {"STATUS", "Online",    C8(100,255,180)},
            {"SHARING","Ready",     C8(100,180,255)},
            {"RESUME", "1 saved",   C8(255,180,100)},
            {"CLOUD",  "Connected", C8(200,100,255)},
        };
        for (int i = 0; i < 4; i++) {
            float sy = stY + 30.f + i * 68.f;
            d2d.FillGradientH(stX, sy, 280, 52, Fade(qs[i].c, 0.1f), Fade(qs[i].c, 0.f));
            d2d.FillRect(stX, sy, 3, 52, Fade(qs[i].c, 0.75f));
            d2d.DrawTextA(qs[i].l, stX + 12.f, sy + 5.f,  12.f, Fade(dim, 0.72f));
            d2d.DrawTextA(qs[i].v, stX + 12.f, sy + 25.f, 16.f, qs[i].c);
        }

        // [A] hint
        if (isFoc) {
            float ep = (sinf(time * 6.f) + 1.f) / 2.f;
            const char* ht = "[A] OPEN ACCOUNTS";
            float hw = d2d.MeasureTextA(ht, 14.f);
            float hbx = x + TOTAL_W/2.f - hw/2.f - 12.f;
            float hby = y + HUB_H - 44.f;
            d2d.FillRoundRect(hbx, hby, hw + 24.f, 32, 8, 8, Fade(hubCol, 0.24f + ep * 0.18f));
            d2d.StrokeRoundRect(hbx, hby, hw + 24.f, 32, 8, 8, 1.f, Fade(hubCol, 0.7f + ep * 0.25f));
            d2d.DrawTextA(ht, hbx + 12.f, hby + 9.f, 14.f, Fade(txt, 0.95f));
        }
    }

    // ── Sub-boxes ──
    float subY = y + HUB_H + ROW_GAP;
    for (int i = 0; i < 3; i++) {
        float bx = x + i * (SUB_W + GAP);
        bool isFoc = (focusedBox == i + 1);
        auto col = subCols[i];
        float cx = bx + SUB_W / 2.f;

        d2d.FillGradientV(bx, subY, (float)SUB_W, (float)SUB_H,
            Fade(C8(18,22,34), isFoc ? 1.f : 0.93f),
            Fade(C8(12,15,26), isFoc ? 1.f : 0.93f));

        float barH = isFoc ? 8.f : 5.f;
        d2d.FillRect(bx, subY, (float)SUB_W, barH, isFoc ? col : Fade(col, 0.62f));
        if (isFoc) {
            d2d.FillGradientV(bx, subY + barH, (float)SUB_W, 20, Fade(col, 0.28f), Fade(col, 0.f));
            for (int b = 0; b < 2; b++) {
                float a = (0.55f - b * 0.14f) + pulse * 0.3f;
                d2d.StrokeRoundRect(bx - 3.f - b*2, subY - 3.f - b*2,
                    SUB_W + 6.f + b*4, SUB_H + 6.f + b*4, 4, 4, 1.f, Fade(col, a));
            }
        } else {
            d2d.StrokeRoundRect(bx, subY, (float)SUB_W, (float)SUB_H, 4, 4, 1.f, Fade(C8(40,50,70), 0.45f));
        }

        // Icon circle
        float icy = subY + SUB_H / 2.f - 18.f, icR = 42.f;
        if (isFoc) {
            for (int c = 0; c < 3; c++)
                d2d.FillCircle(cx, icy, icR + 12.f + c * 10.f + pulse * 7.f, Fade(col, 0.06f - c * 0.015f));
        }
        d2d.FillCircle(cx, icy, icR, Fade(col, isFoc ? 0.24f : 0.11f));
        d2d.StrokeCircle(cx, icy, icR, 1.5f, Fade(col, isFoc ? 0.85f : 0.42f));
        d2d.StrokeCircle(cx, icy, icR - 4.f, 1.f, Fade(col, isFoc ? 0.48f : 0.2f));

        char ltr[2] = { subs[i].name[0], 0 };
        float lw = d2d.MeasureTextA(ltr, 40.f);
        d2d.DrawTextA(ltr, cx - lw/2.f, icy - 20.f, 40.f, isFoc ? col : Fade(col, 0.72f));

        float nw = d2d.MeasureTextA(subs[i].name, 18.f);
        d2d.DrawTextA(subs[i].name, cx - nw/2.f, subY + SUB_H - 50.f, 18.f,
                      isFoc ? txt : Fade(txt, 0.7f));
        float dw = d2d.MeasureTextA(subs[i].desc, 12.f);
        d2d.DrawTextA(subs[i].desc, cx - dw/2.f, subY + SUB_H - 27.f, 12.f, Fade(dim, 0.6f));

        if (isFoc) {
            float ep = (sinf(time * 7.f) + 1.f) / 2.f;
            const char* ht = "[A]";
            float hw = d2d.MeasureTextA(ht, 13.f);
            float hbx = cx - hw/2.f - 9.f, hby = subY + SUB_H - 76.f;
            d2d.FillRoundRect(hbx, hby, hw + 18.f, 22, 6, 6, Fade(col, 0.26f + ep * 0.18f));
            d2d.StrokeRoundRect(hbx, hby, hw + 18.f, 22, 6, 6, 1.f, Fade(col, 0.68f + ep * 0.25f));
            d2d.DrawTextA(ht, hbx + 9.f, hby + 5.f, 13.f, Fade(txt, 0.95f));
        }
    }
}

// ─── Render: Accounts Overlay ─────────────────────────────────────────────────

void RenderAccountsOverlay(int x, int y, int w, int h,
                           const std::vector<GamingAccount>& accounts,
                           int focusedIndex,
                           D2DColor accent_, D2DColor text_, float time)
{
    auto& d2d = D2D();
    auto  acc  = D2CC(accent_);
    auto  txt  = D2CC(text_);
    float pulse = (sinf(time * 4.f) + 1.f) / 2.f;

    d2d.FillRect(0, 0, (float)w, (float)h, Fade(C8(0,0,0), 0.75f));

    float panelW = 1100, panelH = 720;
    float panelX = (w - panelW) / 2.f, panelY = (h - panelH) / 2.f;

    d2d.FillRoundRect(panelX, panelY, panelW, panelH, 6, 6, Fade(C8(18,22,32), 0.98f));
    d2d.StrokeRoundRect(panelX, panelY, panelW, panelH, 6, 6, 1.5f, Fade(acc, 0.4f));

    d2d.DrawTextA("GAMING ACCOUNTS", panelX + 40.f, panelY + 40.f, 28.f, txt);
    d2d.FillRect(panelX + 40.f, panelY + 78.f, 240, 3, acc);
    d2d.DrawTextA("Connect platforms", panelX + 40.f, panelY + 95.f, 13.f, Fade(txt, 0.6f));

    const float CARD_W = 330, CARD_H = 180, CGAP = 25;
    const int   COLS = 3;
    float startX = panelX + 40.f, startY = panelY + 140.f;

    for (int i = 0; i < (int)accounts.size(); i++) {
        float cx = startX + (i % COLS) * (CARD_W + CGAP);
        float cy = startY + (i / COLS) * (CARD_H + CGAP);
        bool isFoc = (i == focusedIndex);
        auto cardCol = D2CC(accounts[i].accentColor);
        auto bg = isFoc ? Fade(cardCol, 0.15f) : Fade(C8(16,20,30), 0.95f);

        d2d.FillRoundRect(cx, cy, CARD_W, CARD_H, 6, 6, bg);
        d2d.FillRect(cx, cy, CARD_W, 4, isFoc ? cardCol : Fade(cardCol, 0.4f));

        if (isFoc)
            d2d.StrokeRoundRect(cx - 3.f, cy - 3.f, CARD_W + 6.f, CARD_H + 6.f, 6, 6,
                                1.5f, Fade(cardCol, 0.7f + pulse * 0.3f));
        else
            d2d.StrokeRoundRect(cx, cy, CARD_W, CARD_H, 6, 6, 1.f, Fade(C8(32,38,52), 0.5f));

        // Icon
        float iconX = cx + 60.f, iconY = cy + 68.f;
        d2d.FillCircle(iconX, iconY, 36.f, Fade(cardCol, isFoc ? 0.3f : 0.18f));
        d2d.StrokeCircle(iconX, iconY, 36.f, 1.5f, Fade(cardCol, isFoc ? 0.8f : 0.5f));
        float iconFS = accounts[i].icon.length() > 1 ? 20.f : 26.f;
        float iconW  = d2d.MeasureTextA(accounts[i].icon.c_str(), iconFS);
        d2d.DrawTextA(accounts[i].icon.c_str(), iconX - iconW/2.f, iconY - iconFS/2.f, iconFS,
                      isFoc ? cardCol : Fade(cardCol, 0.8f));

        d2d.DrawTextA(accounts[i].platform.c_str(), cx + 120.f, cy + 44.f, 19.f,
                      isFoc ? txt : Fade(txt, 0.85f));

        if (!accounts[i].username.empty()) {
            std::string us = "as " + accounts[i].username;
            if (us.length() > 20) us = us.substr(0, 19) + "..";
            d2d.DrawTextA(us.c_str(), cx + 120.f, cy + 70.f, 12.f, Fade(C8(100,255,180), 0.85f));
        }

        auto statusCol = accounts[i].isConnected ? C8(100,255,180) : C8(255,180,80);
        std::string status = accounts[i].statusText;
        if (status.length() > 28) status = status.substr(0, 27) + "..";
        d2d.DrawTextA(status.c_str(), cx + 120.f, cy + 92.f, 11.f, Fade(statusCol, 0.75f));

        float dotX = cx + CARD_W - 24.f, dotY = cy + 24.f;
        d2d.FillCircle(dotX, dotY, 7.f, Fade(statusCol, isFoc ? 0.95f : 0.7f));
        if (accounts[i].isConnected && isFoc)
            d2d.StrokeCircle(dotX, dotY, 10.f, 1.5f, Fade(statusCol, 0.5f + pulse * 0.35f));

        if (isFoc) {
            const char* prompt = accounts[i].isConnected ? "[A] Open" : "[A] Connect";
            float pw = d2d.MeasureTextA(prompt, 14.f);
            float px2 = cx + (CARD_W - pw) / 2.f;
            float py2 = cy + CARD_H - 42.f;
            d2d.FillRoundRect(px2 - 14.f, py2 - 6.f, pw + 28.f, 28, 8, 8,
                              Fade(cardCol, 0.25f + pulse * 0.12f));
            d2d.DrawTextA(prompt, px2, py2, 14.f, Fade(txt, 0.9f));
        }
    }

    const char* instr = "[B] Close    [Arrows] Navigate    [A] Connect/Open";
    float instrW = d2d.MeasureTextA(instr, 14.f);
    d2d.DrawTextA(instr, (w - instrW) / 2.f, panelY + panelH - 50.f, 14.f, Fade(txt, 0.55f));
}

// ─── Render: Share Hub (stub — extend as needed) ─────────────────────────────

void RenderShareHub(int x, int y, int w, int h,
                    const std::vector<ShareAction>& /*actions*/,
                    const std::vector<SteamFriend>& /*friends*/,
                    int /*focusSection*/, int /*focusedItem*/, int /*scrollOffset*/,
                    D2DColor accent_, D2DColor text_, D2DColor /*textDim*/, float /*time*/)
{
    auto& d2d = D2D();
    d2d.FillRect((float)x, (float)y, (float)w, (float)h, Fade(D2CC(accent_), 0.05f));
    d2d.DrawTextA("SHARE HUB", (float)x + 20.f, (float)y + 20.f, 22.f, D2CC(text_));
}
