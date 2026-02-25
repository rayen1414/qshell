// ============================================================================
// DESKTOP_APPS.HPP  —  v3.0  (D2D backend  —  no raylib)
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "qshell_plugin_api.h"   // D2DColor

struct DesktopApp {
    std::string name;
    std::string exePath;
    std::string category;
    std::string description;
    std::string iconPath;
    bool isPinned = false;
    bool isCustom = false;
};

struct MediaContent {
    std::string name;
    std::string path;
    std::string type;
    std::string extension;
    uint64_t    size = 0;
};

struct AppCategory {
    std::string name;
    std::string icon;
    D2DColor    color = {};   // replaces raylib Color
    int         count = 0;
};

// Scanning
std::vector<DesktopApp>   ScanDesktopApplications();
std::vector<MediaContent> ScanMediaFiles();
std::vector<AppCategory>  GetAppCategories(const std::vector<DesktopApp>& apps);

// Launch
void LaunchDesktopApp(const std::string& path);

// Rendering  (all colour params now D2DColor)
void RenderAppCategoryFilter(int x, int y,
                             const std::vector<AppCategory>& cats,
                             int selected,
                             D2DColor accent, D2DColor text, float time);

void RenderDesktopAppGrid(int x, int y, int cols, int rows,
                          const std::vector<DesktopApp>& apps,
                          int focusX, int focusY,
                          D2DColor accent, D2DColor bg,
                          D2DColor text,  D2DColor textDim, float time);

void RenderMediaBrowser(int x, int y,
                        const std::vector<MediaContent>& media,
                        int focused,
                        D2DColor accent, D2DColor text, D2DColor textDim);
