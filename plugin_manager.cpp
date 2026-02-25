// ============================================================================
//  Q-Shell Plugin Manager  –  plugin_manager.cpp  v3.0
//
//  No raylib.  All drawing uses D2DRenderer::Get() directly (host-side module).
//  Plugin DLLs still call through the D2DPluginAPI table.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "d2d_renderer.hpp"
#include "plugin_manager.hpp"

#include <cstdio>

// ─── Init / Shutdown ─────────────────────────────────────────────────────────

void PluginManager::Init(const std::string& exeDir,
                         const D2DPluginAPI*  d2dAPI,
                         const QShellHostAPI* hostAPI)
{
    m_exeDir = exeDir;
    m_d2d    = d2dAPI;
    m_host   = hostAPI;

    std::string dir = exeDir + "\\plugins";
    if (!fs::exists(dir)) {
        try { fs::create_directories(dir); } catch (...) {}
    }
    Reload();
}

void PluginManager::Shutdown()
{
    SaveSkinChoice();
    for (auto& p : m_plugins) UnloadPlugin(p);
    m_plugins.clear();
}

// ─── Hot-reload ──────────────────────────────────────────────────────────────

void PluginManager::Reload()
{
    std::string dir = m_exeDir + "\\plugins";
    if (!fs::exists(dir)) return;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".dll") continue;
        std::string path = entry.path().string();
        if (!IsLoaded(path)) LoadPlugin(path);
    }
}

// ─── Per-frame ───────────────────────────────────────────────────────────────

void PluginManager::Tick(float dt)
{
    for (auto& p : m_plugins)
        if (p.enabled && p.desc.OnTick) p.desc.OnTick(dt);
}

void PluginManager::NotifyLibraryChanged()
{
    for (auto& p : m_plugins)
        if (p.enabled && p.desc.OnLibraryChanged) p.desc.OnLibraryChanged();
}

// ─── Load / Unload ───────────────────────────────────────────────────────────

void PluginManager::LoadPlugin(const std::string& dllPath)
{
    HMODULE hLib = LoadLibraryA(dllPath.c_str());
    if (!hLib) return;

    auto fn = reinterpret_cast<RegisterPluginFn>(
                  GetProcAddress(hLib, "RegisterPlugin"));
    if (!fn) { FreeLibrary(hLib); return; }

    LoadedPlugin lp;
    lp.hDll      = hLib;
    lp.dllPath   = dllPath;
    lp.enabled   = true;
    lp.desc.rl   = m_d2d;      // D2DPluginAPI* — field kept named 'rl' for compat
    lp.desc.host = m_host;

    fn(&lp.desc);

    if (!lp.desc.name || !lp.desc.name[0])
        lp.desc.name = fs::path(dllPath).stem().string().c_str();

    if (lp.desc.OnLoad) lp.desc.OnLoad();

    m_plugins.push_back(std::move(lp));
}

void PluginManager::UnloadPlugin(LoadedPlugin& p)
{
    if (p.desc.OnUnload) p.desc.OnUnload();
    if (p.hDll) { FreeLibrary(p.hDll); p.hDll = nullptr; }
}

bool PluginManager::IsLoaded(const std::string& path) const
{
    for (auto& p : m_plugins)
        if (p.dllPath == path) return true;
    return false;
}

// ─── Active skin ─────────────────────────────────────────────────────────────

const LoadedPlugin* PluginManager::ActiveSkin_() const
{
    if (m_activeSkin < 0 || m_activeSkin >= (int)m_plugins.size()) return nullptr;
    const auto& p = m_plugins[m_activeSkin];
    return (p.enabled && p.IsSkin()) ? &p : nullptr;
}

void PluginManager::SetActiveSkin(int idx)
{
    m_activeSkin = (idx < 0 || idx >= (int)m_plugins.size()) ? -1 : idx;
    m_pickerOpen = false;
    SaveSkinChoice();
}

// ─── Draw dispatch ────────────────────────────────────────────────────────────

bool PluginManager::DrawBackground(int sw, int sh, float time) const
{
    auto* s = ActiveSkin_();
    return s && s->desc.DrawBackground && s->desc.DrawBackground(sw, sh, time);
}

bool PluginManager::DrawTopBar(int sw, int sh, float time) const
{
    auto* s = ActiveSkin_();
    return s && s->desc.DrawTopBar && s->desc.DrawTopBar(sw, sh, time);
}

bool PluginManager::DrawBottomBar(int sw, int sh, float time) const
{
    auto* s = ActiveSkin_();
    return s && s->desc.DrawBottomBar && s->desc.DrawBottomBar(sw, sh, time);
}

bool PluginManager::DrawGameCard(QRect r, const char* name, bool foc,
                                  D2DBitmapHandle poster, float time) const
{
    auto* s = ActiveSkin_();
    return s && s->desc.DrawGameCard &&
           s->desc.DrawGameCard(r, name, foc, poster, time);
}

bool PluginManager::DrawSettingsTile(QRect r, const char* icon, const char* title,
                                      D2DColor accent, bool foc, float time) const
{
    auto* s = ActiveSkin_();
    return s && s->desc.DrawSettingsTile &&
           s->desc.DrawSettingsTile(r, icon, title, accent, foc, time);
}

bool PluginManager::DrawLibraryTab(int sw, int sh, int focusedIdx, float time) const
{
    auto* s = ActiveSkin_();
    return s && s->desc.DrawLibraryTab &&
           s->desc.DrawLibraryTab(sw, sh, focusedIdx, time);
}

void PluginManager::DrawSidePanel(QRect panelRect, int activeTab, float time) const
{
    for (auto& p : m_plugins)
        if (p.enabled && p.desc.DrawSidePanel)
            p.desc.DrawSidePanel(panelRect, activeTab, time);
}

// ─── Context-menu ─────────────────────────────────────────────────────────────

int PluginManager::GetContextMenuItems(int gameIdx,
                                        const char** items, int maxItems) const
{
    int total = 0;
    for (auto& p : m_plugins) {
        if (!p.enabled || !p.desc.GetContextMenuItems) continue;
        int added = p.desc.GetContextMenuItems(gameIdx, items + total, maxItems - total);
        total += added;
        if (total >= maxItems) break;
    }
    return total;
}

void PluginManager::OnContextMenuAction(int gameIdx,
                                         int pluginItemOffset, int itemIdx) const
{
    int offset = pluginItemOffset;
    for (auto& p : m_plugins) {
        if (!p.enabled || !p.desc.GetContextMenuItems) continue;
        const char* tmp[32];
        int n = p.desc.GetContextMenuItems(gameIdx, tmp, 32);
        if (itemIdx < offset + n) {
            if (p.desc.OnContextMenuAction)
                p.desc.OnContextMenuAction(gameIdx, itemIdx - offset);
            return;
        }
        offset += n;
    }
}

// ─── Persistence ─────────────────────────────────────────────────────────────

void PluginManager::SaveSkinChoice() const
{
    std::string dir = m_exeDir + "\\profile";
    if (!fs::exists(dir)) {
        try { fs::create_directories(dir); } catch (...) {}
    }
    std::ofstream f(dir + "\\active_plugin.txt");
    if (!f) return;
    if (m_activeSkin >= 0 && m_activeSkin < (int)m_plugins.size())
        f << m_plugins[m_activeSkin].dllPath;
    else
        f << "__DEFAULT__";
}

void PluginManager::LoadSkinChoice()
{
    std::ifstream f(m_exeDir + "\\profile\\active_plugin.txt");
    if (!f) return;
    std::string saved;
    std::getline(f, saved);
    if (saved == "__DEFAULT__") { m_activeSkin = -1; return; }
    for (int i = 0; i < (int)m_plugins.size(); i++)
        if (m_plugins[i].dllPath == saved) { m_activeSkin = i; return; }
    m_activeSkin = -1;
}

// ─── Skin Picker Overlay ─────────────────────────────────────────────────────
// All draw calls now go through D2DRenderer::Get() directly.

bool PluginManager::UpdateAndDrawSkinPicker(int sw, int sh,
                                             bool confirm, bool cancel,
                                             bool up, bool down)
{
    if (!m_pickerOpen) return false;

    if (m_pickerJustOpened) {
        m_pickerJustOpened = false;
    } else {
        if (cancel)  { m_pickerOpen = false; return false; }
        if (up   && m_pickerFocus > -1)                         m_pickerFocus--;
        if (down && m_pickerFocus < (int)m_plugins.size() - 1) m_pickerFocus++;
        if (confirm) { SetActiveSkin(m_pickerFocus); return false; }
    }

    auto& d2d = D2DRenderer::Get();

    const D2D1_COLOR_F bg    = {15/255.f,  15/255.f,  20/255.f,  0.90f};
    const D2D1_COLOR_F acc   = {100/255.f, 149/255.f, 237/255.f, 1.0f};
    const D2D1_COLOR_F white = {235/255.f, 235/255.f, 245/255.f, 1.0f};
    const D2D1_COLOR_F dim   = {130/255.f, 135/255.f, 160/255.f, 1.0f};

    // Full-screen dim overlay
    d2d.FillRect(0, 0, (float)sw, (float)sh, {0, 0, 0, 0.70f});

    float pw = 560, ph = 560;
    float px = (sw - pw) / 2.f, py = (sh - ph) / 2.f;
    float rx = pw * 0.03f;

    // Panel background + border
    d2d.FillRoundRect  (px,     py,     pw,   ph,   rx, rx, bg);
    d2d.StrokeRoundRect(px,     py,     pw,   ph,   rx, rx, 1.5f, acc);

    // Title
    float tw = d2d.MeasureTextA("Plugin / Skin Picker", 24);
    d2d.DrawTextA("Plugin / Skin Picker", px + (pw - tw) / 2.f, py + 22.f, 24.f, white);

    // Divider
    d2d.FillRect(px + 20, py + 58, pw - 40, 1,
                 {acc.r, acc.g, acc.b, 0.24f});

    // List
    float itemH      = 68.f;
    float listY      = py + 72;
    float listBottom = py + ph - 52;
    int   maxVis     = (int)((listBottom - listY) / itemH);
    int   scrollOff  = (m_pickerFocus >= maxVis) ? m_pickerFocus - maxVis + 1 : 0;

    auto drawItem = [&](int idx, float iy)
    {
        bool sel = (idx == m_activeSkin);
        bool foc = (idx == m_pickerFocus);
        D2D1_COLOR_F rowBg = foc ? D2D1_COLOR_F{acc.r, acc.g, acc.b, 0.16f}
                                 : D2D1_COLOR_F{0, 0, 0, 0};

        float irx = (pw - 24) * 0.05f;
        d2d.FillRoundRect  (px + 12, iy,     pw - 24, itemH - 6, irx, irx, rowBg);
        if (foc)
            d2d.StrokeRoundRect(px + 12, iy, pw - 24, itemH - 6, irx, irx, 1.f, acc);

        const char* nm  = (idx == -1) ? "Default (built-in)"
                                      : m_plugins[idx].desc.name;
        const char* au  = (idx == -1) ? "Q-Shell"
                        : (m_plugins[idx].desc.author  ? m_plugins[idx].desc.author  : "");
        const char* ver = (idx == -1) ? ""
                        : (m_plugins[idx].desc.version ? m_plugins[idx].desc.version : "");
        const char* dsc = (idx == -1) ? "Standard Q-Shell look"
                        : (m_plugins[idx].desc.description ? m_plugins[idx].desc.description : "");

        d2d.DrawTextA(nm, px + 30, iy + 8,  18.f, foc ? white : dim);

        char sub[128]; snprintf(sub, sizeof(sub), "%s %s", au, ver);
        d2d.DrawTextA(sub, px + 30, iy + 30, 12.f,
                      {dim.r, dim.g, dim.b, 0.63f});

        char dscT[64]; snprintf(dscT, sizeof(dscT), "%.58s", dsc);
        d2d.DrawTextA(dscT, px + 30, iy + 46, 12.f,
                      {dim.r, dim.g, dim.b, 0.47f});

        if (sel) {
            float aw = d2d.MeasureTextA("[active]", 13.f);
            d2d.DrawTextA("[active]", px + pw - aw - 14, iy + 24, 13.f, acc);
        }
    };

    d2d.PushClip(px, listY, pw, listBottom - listY);

    float baseY = listY;
    if (baseY < listBottom) drawItem(-1, baseY);
    baseY += itemH;

    for (int i = 0; i < (int)m_plugins.size(); i++) {
        float iy = baseY + (i - scrollOff) * itemH;
        if (iy < listY - itemH || iy > listBottom) continue;
        drawItem(i, iy);
    }

    d2d.PopClip();

    // Footer hint
    d2d.DrawTextA("[Up/Down] Navigate   [A/Enter] Activate   [B/Esc] Close",
                  px + 20, py + ph - 36, 13.f, dim);

    return true;
}
