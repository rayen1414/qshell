// ============================================================================
// GAME_FINDER.HPP - Q-SHELL v2.5
// ============================================================================

#pragma once

#include <string>
#include <vector>

struct GameInfo {
    std::string name;
    std::string exePath;
    std::string platform;
    std::string appId;
};

std::vector<GameInfo> GetInstalledGames();
void DownloadArt(std::string name, std::string id);