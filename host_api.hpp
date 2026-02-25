// ============================================================================
//  Q-Shell Host API  –  host_api.hpp  v3.0
//
//  !! INCLUDE THIS ONLY ONCE, INSIDE qshell.cpp, AFTER AppState + g_app
//     ARE FULLY DEFINED. Do NOT include it in plugin_manager.cpp or any
//     other translation unit. !!
//
//  Correct include order inside qshell.cpp:
//      #include <windows.h>
//      #include "d2d_renderer.hpp"
//      struct AppState { ... };
//      static AppState g_app;
//      #include "qshell_plugin_api.h"
//      #include "plugin_manager.hpp"
//      #include "host_api.hpp"         // <-- THIS FILE
//
//  Changes from v2.3:
//  ─────────────────────
//  • Texture2D / LoadTexture / UnloadTexture → D2DBitmapHandle / D2DRenderer
//  • GetTime() → QueryPerformanceCounter (no raylib)
//  • GetScreenWidth/Height() → D2DRenderer::Get().ScreenWidth/Height()
//  • GetTheme() now fills QShellTheme with D2DColor (float rgba) not Color (byte rgba)
//  • PushNotification col parameter is now D2DColor not Color
// ============================================================================
#pragma once

#include "qshell_plugin_api.h"
#include "d2d_renderer.hpp"

#include <string>
#include <map>
#include <fstream>
#include <filesystem>

// ShowNotification is defined later in qshell.cpp (same translation unit).
void ShowNotification(const std::string& title, const std::string& msg,
                      D2DColor col, float dur);

// ─── Input bridge ─────────────────────────────────────────────────────────────

static QShellInput g_pluginInput = {};

static void UpdatePluginInput(const QShellInput& in) {
    g_pluginInput = in;
}

// ─── Time helper (QueryPerformanceCounter — no raylib dependency) ─────────────

static float HostGetTime()
{
    static LARGE_INTEGER freq = {}, start = {};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (float)((now.QuadPart - start.QuadPart) / (double)freq.QuadPart);
}

// ─── Host API implementations ─────────────────────────────────────────────────
//  Plain static functions — internal linkage, no namespace scope,
//  so `extern AppState g_app` correctly resolves to the global g_app.

static void hostimpl_push_notification(const char* t, const char* m,
                                        D2DColor col, float lt)
{
    ShowNotification(t ? t : "", m ? m : "", col, lt);
}

static int hostimpl_get_game_count() {
    extern AppState g_app;
    return (int)g_app.library.size();
}

static void hostimpl_get_game(int idx, QShellGameInfo* out) {
    extern AppState g_app;
    if (!out || idx < 0 || idx >= (int)g_app.library.size()) return;
    auto& g        = g_app.library[idx];
    out->name      = g.info.name.c_str();
    out->path      = g.info.exePath.c_str();
    out->platform  = g.info.platform.c_str();
    out->coverPath = "";
    out->playtime_sec = 0;
    out->last_played  = 0;
}

static void hostimpl_launch_game(int idx) {
    extern AppState g_app;
    if (idx < 0 || idx >= (int)g_app.library.size()) return;
    ShellExecuteA(nullptr, "open",
                  g_app.library[idx].info.exePath.c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
}

static void hostimpl_remove_game(int idx) {
    extern AppState g_app;
    if (idx < 0 || idx >= (int)g_app.library.size()) return;
    // D2DBitmap: unload via renderer
    if (g_app.library[idx].hasPoster)
        D2D().UnloadBitmap(g_app.library[idx].poster);
    g_app.library.erase(g_app.library.begin() + idx);
    if (g_app.focused >= (int)g_app.library.size())
        g_app.focused = std::max(0, (int)g_app.library.size() - 1);
}

static int hostimpl_get_focused_idx() {
    extern AppState g_app; return g_app.focused;
}
static void hostimpl_set_focused_idx(int i) {
    extern AppState g_app; g_app.focused = i;
}
static int hostimpl_get_active_tab() {
    extern AppState g_app; return g_app.barFocused;
}
static void hostimpl_set_active_tab(int t) {
    extern AppState g_app;
    g_app.barFocused = t;
    g_app.ResetTabFocus();
}

// Helper: convert byte-RGBA from Theme to D2DColor float
static inline D2DColor ByteToD2D(unsigned char r, unsigned char g,
                                   unsigned char b, unsigned char a = 255)
{
    return { r/255.f, g/255.f, b/255.f, a/255.f };
}

static const QShellTheme* hostimpl_get_theme() {
    extern AppState g_app;
    static QShellTheme qt;
    auto& th      = g_app.theme;
    // Theme stores D2D1_COLOR_F (float r,g,b,a in 0..1 range).
    // Copy directly — do NOT pass through ByteToD2D which divides by 255.
    qt.primary    = {th.primary.r,   th.primary.g,   th.primary.b,   th.primary.a};
    qt.secondary  = {th.secondary.r, th.secondary.g, th.secondary.b, th.secondary.a};
    qt.accent     = {th.accent.r,    th.accent.g,    th.accent.b,    th.accent.a};
    qt.accentAlt  = {th.accentAlt.r, th.accentAlt.g, th.accentAlt.b, th.accentAlt.a};
    qt.text       = {th.text.r,      th.text.g,      th.text.b,      th.text.a};
    qt.textDim    = {th.textDim.r,   th.textDim.g,   th.textDim.b,   th.textDim.a};
    qt.cardBg     = {th.cardBg.r,    th.cardBg.g,    th.cardBg.b,    th.cardBg.a};
    qt.success    = {th.success.r,   th.success.g,   th.success.b,   th.success.a};
    qt.warning    = {th.warning.r,   th.warning.g,   th.warning.b,   th.warning.a};
    qt.danger     = {th.danger.r,    th.danger.g,    th.danger.b,    th.danger.a};
    return &qt;
}

static void hostimpl_set_theme_by_index(int i) {
    extern AppState g_app; g_app.SetTheme(i);
}

static const QShellInput* hostimpl_get_input() { return &g_pluginInput; }

static std::map<std::string, std::map<std::string, std::string>> s_pluginSettings;

static void hostimpl_write_setting(const char* plugin, const char* key,
                                    const char* val)
{
    extern AppState g_app;
    if (!plugin || !key || !val) return;
    s_pluginSettings[plugin][key] = val;
    std::string dir = g_app.exeDir + "\\profile\\plugins";
    try { std::filesystem::create_directories(dir); } catch (...) {}
    std::ofstream f(dir + "\\" + plugin + ".ini");
    for (auto& [k, v] : s_pluginSettings[plugin]) f << k << "=" << v << "\n";
}

static const char* hostimpl_read_setting(const char* plugin, const char* key,
                                          const char* def)
{
    extern AppState g_app;
    if (!plugin || !key) return def;
    if (s_pluginSettings.find(plugin) == s_pluginSettings.end()) {
        std::ifstream f(g_app.exeDir + "\\profile\\plugins\\" + plugin + ".ini");
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq != std::string::npos)
                s_pluginSettings[plugin][line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    auto it = s_pluginSettings[plugin].find(key);
    if (it == s_pluginSettings[plugin].end()) return def;
    static std::string ret; ret = it->second; return ret.c_str();
}

// Bitmap load/unload — delegate to D2DRenderer
static D2DBitmapHandle hostimpl_load_plugin_bitmap_w(const wchar_t* path)
{
    D2DBitmap bmp = path ? D2D().LoadBitmap(path) : D2DBitmap{};
    return D2DBitmapHandle{ bmp.bmp, bmp.w, bmp.h };
}
static D2DBitmapHandle hostimpl_load_plugin_bitmap_a(const char* path)
{
    D2DBitmap bmp = path ? D2D().LoadBitmapA(path) : D2DBitmap{};
    return D2DBitmapHandle{ bmp.bmp, bmp.w, bmp.h };
}
static void hostimpl_unload_plugin_bitmap(D2DBitmapHandle h)
{
    D2DBitmap bmp{ reinterpret_cast<ID2D1Bitmap*>(h.opaque), h.w, h.h };
    D2D().UnloadBitmap(bmp);
}

static int   hostimpl_get_screen_width()  { return D2D().ScreenWidth();  }
static int   hostimpl_get_screen_height() { return D2D().ScreenHeight(); }
static float hostimpl_get_time()          { return HostGetTime();         }
static bool  hostimpl_is_shell_mode()     {
    extern AppState g_app; return g_app.isShellMode;
}

// ─── Filled host API table ────────────────────────────────────────────────────

static const QShellHostAPI g_hostAPI = {
    hostimpl_push_notification,
    hostimpl_get_game_count,
    hostimpl_get_game,
    hostimpl_launch_game,
    hostimpl_remove_game,
    hostimpl_get_focused_idx,
    hostimpl_set_focused_idx,
    hostimpl_get_active_tab,
    hostimpl_set_active_tab,
    hostimpl_get_theme,
    hostimpl_set_theme_by_index,
    hostimpl_get_input,
    hostimpl_write_setting,
    hostimpl_read_setting,
    hostimpl_load_plugin_bitmap_w,
    hostimpl_load_plugin_bitmap_a,
    hostimpl_unload_plugin_bitmap,
    hostimpl_get_screen_width,
    hostimpl_get_screen_height,
    hostimpl_get_time,
    hostimpl_is_shell_mode,
};
