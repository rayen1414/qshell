// ============================================================================
// DESKTOP_APPS.CPP  —  v3.0  (D2D backend  —  no raylib)
// ============================================================================

#include "desktop_apps.hpp"
#include "d2d_renderer.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <initguid.h>
#include <shobjidl.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <set>
#include <cmath>

namespace fs = std::filesystem;

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// GUIDs for IShellLink
DEFINE_GUID(CLSID_ShellLink_Local, 0x00021401,0,0, 0xC0,0,0,0,0,0,0,0x46);
DEFINE_GUID(IID_IShellLinkA_Local, 0x000214EE,0,0, 0xC0,0,0,0,0,0,0,0x46);
DEFINE_GUID(IID_IPersistFile_Local,0x0000010B,0,0, 0xC0,0,0,0,0,0,0,0x46);

// ─── Colour helpers ───────────────────────────────────────────────────────────

static inline D2D1_COLOR_F C8(int r, int g, int b, int a = 255) {
    return { r/255.f, g/255.f, b/255.f, a/255.f };
}
static inline D2D1_COLOR_F Fade(D2D1_COLOR_F c, float a) { return {c.r,c.g,c.b,c.a*a}; }
static inline D2D1_COLOR_F D2CC(D2DColor c) { return {c.r,c.g,c.b,c.a}; }

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::string GetShellFolderPath(int csidl) {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, csidl, nullptr, 0, path)))
        return std::string(path);
    return "";
}

static std::string ResolveShortcut(const std::string& lnkPath) {
    CoInitialize(nullptr);
    IShellLinkA* psl = nullptr; IPersistFile* ppf = nullptr;
    char targetPath[MAX_PATH] = {};
    HRESULT hr = CoCreateInstance(CLSID_ShellLink_Local, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkA_Local, (void**)&psl);
    if (SUCCEEDED(hr)) {
        hr = psl->QueryInterface(IID_IPersistFile_Local, (void**)&ppf);
        if (SUCCEEDED(hr)) {
            WCHAR wPath[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, lnkPath.c_str(), -1, wPath, MAX_PATH);
            if (SUCCEEDED(ppf->Load(wPath, STGM_READ))) {
                WIN32_FIND_DATAA wfd;
                psl->GetPath(targetPath, MAX_PATH, &wfd, SLGP_RAWPATH);
            }
            ppf->Release();
        }
        psl->Release();
    }
    CoUninitialize();
    return std::string(targetPath);
}

static std::string GetAppCategory(const std::string& name, const std::string& path) {
    std::string ln = name, lp = path;
    std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
    std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);
    if (ln.find("chrome")!=std::string::npos || ln.find("firefox")!=std::string::npos ||
        ln.find("edge")  !=std::string::npos || ln.find("opera")  !=std::string::npos ||
        ln.find("brave")  !=std::string::npos|| ln.find("browser") !=std::string::npos) return "Web";
    if (lp.find("steam") !=std::string::npos || lp.find("epic games")!=std::string::npos ||
        lp.find("riot")  !=std::string::npos || lp.find("games")    !=std::string::npos) return "Games";
    if (ln.find("spotify")!=std::string::npos|| ln.find("vlc")    !=std::string::npos ||
        ln.find("netflix")!=std::string::npos|| ln.find("youtube") !=std::string::npos ||
        ln.find("music")  !=std::string::npos|| ln.find("video")   !=std::string::npos ||
        ln.find("player") !=std::string::npos) return "Media";
    if (ln.find("discord")!=std::string::npos|| ln.find("teams")  !=std::string::npos ||
        ln.find("slack")  !=std::string::npos|| ln.find("zoom")   !=std::string::npos ||
        ln.find("skype")  !=std::string::npos|| ln.find("telegram")!=std::string::npos) return "Social";
    if (ln.find("code")   !=std::string::npos|| ln.find("studio") !=std::string::npos ||
        ln.find("visual") !=std::string::npos|| ln.find("git")    !=std::string::npos) return "Dev";
    return "Desktop";
}

static bool IsJunkApp(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    const char* junk[] = {"uninstall","setup","install","update","updater",
                           "readme","help","manual","documentation","license",
                           "redistributable","runtime","framework","debug","repair"};
    for (auto j : junk) if (lower.find(j) != std::string::npos) return true;
    return false;
}

// ─── Scanning ────────────────────────────────────────────────────────────────

static std::vector<DesktopApp> ScanStartMenuApps() {
    std::vector<DesktopApp> apps;
    std::set<std::string> seenPaths;
    std::vector<std::string> basePaths = {
        GetShellFolderPath(CSIDL_COMMON_PROGRAMS),
        GetShellFolderPath(CSIDL_PROGRAMS),
    };
    for (const auto& bp : basePaths) {
        if (bp.empty() || !fs::exists(bp)) continue;
        try {
            for (const auto& e : fs::recursive_directory_iterator(bp)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".lnk") continue;
                std::string name = e.path().stem().string();
                if (IsJunkApp(name)) continue;
                std::string target = ResolveShortcut(e.path().string());
                if (target.empty() || !fs::exists(target)) continue;
                if (seenPaths.count(target)) continue;
                seenPaths.insert(target);
                DesktopApp app;
                app.name        = name;
                app.exePath     = target;
                app.category    = GetAppCategory(name, target);
                app.description = "Start Menu App";
                apps.push_back(app);
            }
        } catch (...) {}
    }
    return apps;
}

std::vector<DesktopApp> ScanDesktopApplications() {
    std::vector<DesktopApp> apps;
    std::set<std::string> seenNames;
    struct WebApp { const char* name; const char* url; const char* cat; };
    WebApp webApps[] = {
        {"Google",  "https://www.google.com",   "Web"},
        {"YouTube", "https://www.youtube.com",  "Media"},
        {"Twitch",  "https://www.twitch.tv",    "Media"},
        {"Netflix", "https://www.netflix.com",  "Media"},
        {"Spotify", "https://open.spotify.com", "Media"},
        {"Discord", "https://discord.com/app",  "Social"},
        {"Twitter", "https://twitter.com",      "Social"},
        {"GitHub",  "https://github.com",       "Dev"},
    };
    for (auto& wa : webApps) {
        DesktopApp app;
        app.name = wa.name; app.exePath = wa.url;
        app.category = wa.cat; app.description = "Web App";
        apps.push_back(app); seenNames.insert(app.name);
    }
    for (auto& app : ScanStartMenuApps())
        if (!seenNames.count(app.name)) { apps.push_back(app); seenNames.insert(app.name); }
    std::sort(apps.begin(), apps.end(), [](const DesktopApp& a, const DesktopApp& b) {
        return a.category != b.category ? a.category < b.category : a.name < b.name;
    });
    return apps;
}

std::vector<MediaContent> ScanMediaFiles() {
    std::vector<MediaContent> media;
    std::vector<std::string> mediaPaths = {
        GetShellFolderPath(CSIDL_MYVIDEO),
        GetShellFolderPath(CSIDL_MYMUSIC),
        GetShellFolderPath(CSIDL_MYPICTURES),
    };
    std::set<std::string> videoExts = {".mp4",".mkv",".avi",".mov",".wmv",".webm"};
    std::set<std::string> audioExts = {".mp3",".wav",".flac",".ogg",".m4a",".aac"};
    std::set<std::string> imageExts = {".jpg",".jpeg",".png",".gif",".bmp",".webp"};
    for (const auto& bp : mediaPaths) {
        if (bp.empty() || !fs::exists(bp)) continue;
        try {
            for (const auto& e : fs::directory_iterator(bp)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                MediaContent mc;
                mc.name = e.path().stem().string();
                mc.path = e.path().string();
                mc.extension = ext;
                try { mc.size = fs::file_size(e.path()); } catch (...) {}
                if      (videoExts.count(ext)) mc.type = "video";
                else if (audioExts.count(ext)) mc.type = "music";
                else if (imageExts.count(ext)) mc.type = "image";
                else continue;
                media.push_back(mc);
                if (media.size() >= 50) break;
            }
        } catch (...) {}
        if (media.size() >= 50) break;
    }
    std::sort(media.begin(), media.end(), [](const MediaContent& a, const MediaContent& b) {
        return a.name < b.name;
    });
    return media;
}

std::vector<AppCategory> GetAppCategories(const std::vector<DesktopApp>& apps) {
    std::vector<AppCategory> cats = {
        {"All",     "A", {100/255.f,149/255.f,237/255.f,1.f}, 0},
        {"Web",     "W", { 66/255.f,133/255.f,244/255.f,1.f}, 0},
        {"Desktop", "D", {100/255.f,200/255.f,100/255.f,1.f}, 0},
        {"Games",   "G", {255/255.f,100/255.f,100/255.f,1.f}, 0},
        {"Media",   "M", {255/255.f,180/255.f,  0/255.f,1.f}, 0},
        {"Social",  "S", { 88/255.f,101/255.f,242/255.f,1.f}, 0},
        {"Dev",     "C", {150/255.f,150/255.f,150/255.f,1.f}, 0},
    };
    for (const auto& app : apps) {
        cats[0].count++;
        for (size_t i = 1; i < cats.size(); i++)
            if (cats[i].name == app.category) { cats[i].count++; break; }
    }
    cats.erase(
        std::remove_if(cats.begin() + 1, cats.end(),
                       [](const AppCategory& c){ return c.count == 0; }),
        cats.end());
    return cats;
}

void LaunchDesktopApp(const std::string& path) {
    if (path.empty()) return;
    if (path.find("://") != std::string::npos)
        ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (fs::exists(path)) {
        std::string dir = fs::path(path).parent_path().string();
        ShellExecuteA(nullptr, "open", path.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    }
}

// ─── Category colour lookup (D2DColor from category name) ────────────────────

static D2D1_COLOR_F CategoryColor(const std::string& cat, D2D1_COLOR_F accent) {
    if (cat == "Web")    return C8(66,  133, 244);
    if (cat == "Games")  return C8(255, 100, 100);
    if (cat == "Media")  return C8(255, 180, 0);
    if (cat == "Social") return C8(88,  101, 242);
    if (cat == "Dev")    return C8(150, 150, 150);
    return accent;
}

// ─── Render: Category Filter ─────────────────────────────────────────────────

void RenderAppCategoryFilter(int x, int y,
                             const std::vector<AppCategory>& cats,
                             int selected,
                             D2DColor accent_, D2DColor text_, float time)
{
    auto& d2d   = D2D();
    auto  acc   = D2CC(accent_);
    auto  txt   = D2CC(text_);
    float pulse = (sinf(time * 4.f) + 1.f) / 2.f;
    float tabW = 110, tabH = 42, gap = 8;

    d2d.DrawTextA("CATEGORIES", (float)x, (float)y, 14.f, Fade(txt, 0.6f));
    d2d.FillRect((float)x, y + 20.f, 100, 2, Fade(acc, 0.5f));

    float ty = y + 35.f;
    for (int i = 0; i < (int)cats.size(); i++) {
        float tx    = x + i * (tabW + gap);
        bool  isSel = (i == selected);
        auto  col   = D2CC(cats[i].color);

        d2d.FillRoundRect(tx, ty, tabW, tabH, 6, 6,
            isSel ? Fade(col, 0.25f) : Fade(C8(30,35,45), 0.8f));

        if (isSel) {
            d2d.FillRect(tx, ty, tabW, 3, col);
            d2d.StrokeRoundRect(tx - 1.f, ty - 1.f, tabW + 2.f, tabH + 2.f, 6, 6,
                                1.f, Fade(col, 0.6f + pulse * 0.3f));
        }

        float iconX = tx + 22.f, iconY = ty + tabH / 2.f;
        d2d.FillCircle(iconX, iconY, 12.f, Fade(col, isSel ? 0.3f : 0.15f));
        float iw = d2d.MeasureTextA(cats[i].icon.c_str(), 12.f);
        d2d.DrawTextA(cats[i].icon.c_str(), iconX - iw/2.f, iconY - 6.f, 12.f,
                      isSel ? col : Fade(col, 0.7f));

        d2d.DrawTextA(cats[i].name.c_str(), tx + 40.f, ty + 8.f, 13.f,
                      isSel ? txt : Fade(txt, 0.7f));

        if (cats[i].count > 0) {
            char cs[16]; snprintf(cs, sizeof(cs), "%d", cats[i].count);
            d2d.DrawTextA(cs, tx + 40.f, ty + 25.f, 10.f, Fade(col, 0.7f));
        }
    }
}

// ─── Render: App Grid ─────────────────────────────────────────────────────────

void RenderDesktopAppGrid(int x, int y, int cols, int rows,
                          const std::vector<DesktopApp>& apps,
                          int focusX, int focusY,
                          D2DColor accent_, D2DColor bg_,
                          D2DColor text_, D2DColor textDim_, float time)
{
    auto& d2d   = D2D();
    auto  acc   = D2CC(accent_);
    auto  bg    = D2CC(bg_);
    auto  txt   = D2CC(text_);
    auto  dim   = D2CC(textDim_);
    float pulse = (sinf(time * 4.f) + 1.f) / 2.f;
    float cardW = 195, cardH = 135, gapX = 16, gapY = 14;
    int   focusIdx = focusY * cols + focusX;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            if (idx >= (int)apps.size()) continue;
            const auto& app = apps[idx];
            float cx = x + c * (cardW + gapX);
            float cy = y + r * (cardH + gapY);
            bool  isFoc = (idx == focusIdx);
            auto  cardCol = CategoryColor(app.category, acc);

            // Shadow
            d2d.FillRoundRect(cx + 4.f, cy + 4.f, cardW, cardH, 6, 6,
                              Fade(C8(0,0,0), isFoc ? 0.35f : 0.2f));

            d2d.FillRoundRect(cx, cy, cardW, cardH, 6, 6,
                              isFoc ? Fade(cardCol, 0.15f) : Fade(bg, 0.85f));

            d2d.FillRect(cx, cy, cardW, isFoc ? 4.f : 3.f,
                         isFoc ? cardCol : Fade(cardCol, 0.4f));

            if (isFoc)
                d2d.StrokeRoundRect(cx - 2.f, cy - 2.f, cardW + 4.f, cardH + 4.f, 6, 6,
                                    1.5f, Fade(cardCol, 0.5f + pulse * 0.35f));

            // Icon circle
            float iconX = cx + 35.f, iconY = cy + 55.f;
            d2d.FillCircle(iconX, iconY, 26.f, Fade(cardCol, isFoc ? 0.3f : 0.15f));
            d2d.StrokeCircle(iconX, iconY, 26.f, 1.f, Fade(cardCol, isFoc ? 0.7f : 0.35f));

            char ini[2] = { app.name.empty() ? '?' : (char)toupper(app.name[0]), 0 };
            float iniW = d2d.MeasureTextA(ini, 22.f);
            d2d.DrawTextA(ini, iconX - iniW/2.f, iconY - 11.f, 22.f,
                          isFoc ? cardCol : Fade(cardCol, 0.7f));

            std::string dn = app.name;
            if (dn.length() > 15) dn = dn.substr(0, 14) + "..";
            d2d.DrawTextA(dn.c_str(), cx + 70.f, cy + 35.f, 14.f,
                          isFoc ? txt : Fade(txt, 0.85f));

            float badgeW = d2d.MeasureTextA(app.category.c_str(), 9.f) + 12.f;
            d2d.FillRoundRect(cx + 70.f, cy + 58.f, badgeW, 16, 4, 4, Fade(cardCol, 0.15f));
            d2d.DrawTextA(app.category.c_str(), cx + 76.f, cy + 62.f, 9.f, Fade(cardCol, 0.8f));

            if (isFoc) {
                float arrowX = sinf(time * 5.f) * 3.f;
                d2d.DrawTextA(">", cx + cardW - 25.f + arrowX, cy + cardH/2.f - 10.f, 20.f, cardCol);
            }
        }
    }
}

// ─── Render: Media Browser ────────────────────────────────────────────────────

void RenderMediaBrowser(int x, int y,
                        const std::vector<MediaContent>& media,
                        int focused,
                        D2DColor accent_, D2DColor text_, D2DColor textDim_)
{
    auto& d2d = D2D();
    auto  acc  = D2CC(accent_);
    auto  txt  = D2CC(text_);
    auto  dim  = D2CC(textDim_);

    if (media.empty()) {
        d2d.DrawTextA("No media files found", (float)x, y + 10.f, 14.f, Fade(dim, 0.6f));
        return;
    }

    d2d.DrawTextA("RECENT MEDIA", (float)x, (float)y, 14.f, Fade(txt, 0.6f));
    d2d.FillRect((float)x, y + 20.f, 110, 2, Fade(acc, 0.5f));

    float itemY = y + 35.f, itemH = 45.f;
    int visItems = std::min((int)media.size(), 5);

    for (int i = 0; i < visItems; i++) {
        const auto& m = media[i];
        bool isFoc = (i == focused);

        d2d.FillRoundRect((float)x, itemY + i * itemH, 500, itemH - 5.f, 4, 4,
            isFoc ? Fade(acc, 0.12f) : Fade(C8(25,30,40), 0.6f));

        D2D1_COLOR_F typeCol = C8(100, 180, 255);
        const char* typeIcon = "?";
        if      (m.type == "video") { typeCol = C8(255,100,100); typeIcon = "V"; }
        else if (m.type == "music") { typeCol = C8(100,255,150); typeIcon = "M"; }
        else if (m.type == "image") { typeCol = C8(255,200,50);  typeIcon = "I"; }

        d2d.FillCircle(x + 22.f, itemY + i * itemH + 17.f, 14.f,
                       Fade(typeCol, isFoc ? 0.3f : 0.15f));
        float tw2 = d2d.MeasureTextA(typeIcon, 14.f);
        d2d.DrawTextA(typeIcon, x + 22.f - tw2/2.f, itemY + i * itemH + 10.f, 14.f,
                      isFoc ? typeCol : Fade(typeCol, 0.7f));

        std::string dn = m.name;
        if (dn.length() > 40) dn = dn.substr(0, 38) + "..";
        d2d.DrawTextA(dn.c_str(), x + 48.f, itemY + i * itemH + 5.f, 13.f,
                      isFoc ? txt : Fade(txt, 0.8f));
        d2d.DrawTextA(m.extension.c_str(), x + 48.f, itemY + i * itemH + 22.f, 10.f,
                      Fade(dim, 0.5f));

        char sizeStr[32];
        if      (m.size > 1024ULL*1024*1024) snprintf(sizeStr, sizeof(sizeStr), "%.1f GB", m.size / (1024.0*1024.0*1024.0));
        else if (m.size > 1024ULL*1024)      snprintf(sizeStr, sizeof(sizeStr), "%.1f MB", m.size / (1024.0*1024.0));
        else                                  snprintf(sizeStr, sizeof(sizeStr), "%.1f KB", m.size / 1024.0);
        float sw2 = d2d.MeasureTextA(sizeStr, 10.f);
        d2d.DrawTextA(sizeStr, x + 470.f - sw2, itemY + i * itemH + 14.f, 10.f, Fade(dim, 0.6f));

        if (isFoc) d2d.FillRect((float)x, itemY + i * itemH, 3, itemH - 5.f, acc);
    }

    if ((int)media.size() > visItems) {
        char more[32]; snprintf(more, sizeof(more), "+%d more files", (int)media.size() - visItems);
        d2d.DrawTextA(more, (float)x, itemY + visItems * itemH + 5.f, 11.f, Fade(dim, 0.5f));
    }
}
