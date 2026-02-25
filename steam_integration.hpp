// ============================================================================
// STEAM_INTEGRATION.HPP  —  v3.0  (D2D backend  —  no raylib)
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include "qshell_plugin_api.h"   // D2DColor, D2DBitmapHandle

// ─── Data structures ─────────────────────────────────────────────────────────

struct SteamProfile {
    std::string username    = "Player";
    std::string steamID;
    std::string status      = "Offline";
    int hoursPlayed         = 0;
    int gamesOwned          = 0;
    int friendsCount        = 0;
    bool profileLoaded      = false;
    D2DBitmapHandle profilePicture = {};   // replaces Texture2D
};

struct ResumeEntry {
    std::string gameName;
    std::string lastPlayedTime;
    int hoursPlayed        = 0;
    bool isRecentlyPlayed  = false;
};

struct ShareAction {
    std::string name;
    std::string icon;
    std::string description;
    D2DColor    color = {};
};

struct SteamFriend {
    std::string name;
    std::string steamId;
    std::string lastSeen;
    bool isOnline      = false;
    bool isFromSteam   = false;
};

struct GamingAccount {
    std::string platform;
    std::string username;
    std::string userId;
    bool        isConnected = false;
    D2DColor    accentColor = {};
    std::string icon;
    std::string statusText;
};

struct CustomAppSlot {
    std::string name;
    std::string exePath;
    std::string description;
    D2DColor    accentColor = {};
};

// ─── Data functions ───────────────────────────────────────────────────────────

SteamProfile              GetSteamProfile();
D2DBitmapHandle           LoadSteamProfilePicture();
std::vector<ResumeEntry>  GetRecentGames();
std::vector<SteamFriend>  GetRealSteamFriends();
std::vector<GamingAccount> GetGamingAccounts();

// ─── Account functions ────────────────────────────────────────────────────────

void ConnectAccount   (const std::string& platform);
void DisconnectAccount(const std::string& platform);
bool IsAccountConnected(const std::string& platform);

// ─── Action functions ─────────────────────────────────────────────────────────

void OpenResumeDossier();
void ShareToDiscord   (const std::string& message);
void ShareToSteam     (const std::string& gameID);
void OpenTwitchProfile();
void OpenSteamCommunity();
void LaunchCloudStreaming();
void LaunchCustomAppSlot(int slot);
std::string GetCustomAppPath(int slot);
void SetCustomAppPath(int slot, const std::string& path, const std::string& name);

// ─── Render functions (all colours now D2DColor) ──────────────────────────────

void RenderSteamProfile(int x, int y, const SteamProfile& profile,
                        D2DColor accent, D2DColor secondary,
                        D2DColor text,   D2DColor textDim,
                        float time, bool focused = false);

void RenderResumeHub(int x, int y, const std::vector<ResumeEntry>& entries,
                     int focused,
                     D2DColor accent, D2DColor secondary,
                     D2DColor text,   D2DColor textDim,
                     float time,
                     int scrollOffset = 0,
                     D2DBitmapHandle artCover = {});

void RenderShareHub(int x, int y, int w, int h,
                    const std::vector<ShareAction>& actions,
                    const std::vector<SteamFriend>& friends,
                    int focusSection, int focusedItem, int scrollOffset,
                    D2DColor accent, D2DColor text, D2DColor textDim, float time);

void RenderAccountsOverlay(int x, int y, int w, int h,
                           const std::vector<GamingAccount>& accounts,
                           int focusedIndex,
                           D2DColor accent, D2DColor text, float time);

void RenderSpecialHubBoxes(int x, int y, D2DBitmapHandle artCover,
                           int focusedBox,
                           D2DColor accent, D2DColor text, D2DColor textDim,
                           float time);
