// ============================================================================
//  Q-Shell Plugin Manager  –  plugin_manager.hpp  v3.0
//
//  No raylib dependency.  QRect and D2DBitmapHandle come from qshell_plugin_api.h.
// ============================================================================
#pragma once

#include "qshell_plugin_api.h"

#include <vector>
#include <string>
#include <functional>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>

namespace fs = std::filesystem;

struct LoadedPlugin {
    HMODULE          hDll    = nullptr;
    QShellPluginDesc desc    = {};
    bool             enabled = true;
    std::string      dllPath;

    bool IsSkin() const {
        return desc.DrawBackground   || desc.DrawTopBar       ||
               desc.DrawBottomBar    || desc.DrawGameCard     ||
               desc.DrawSettingsTile || desc.DrawLibraryTab;
    }
};

class PluginManager {
public:
    static PluginManager& Get() { static PluginManager pm; return pm; }
    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void Init(const std::string& exeDir,
              const D2DPluginAPI*  d2dAPI,
              const QShellHostAPI* hostAPI);
    void Shutdown();
    void Reload();
    void Tick(float dt);
    void NotifyLibraryChanged();

    bool DrawBackground (int sw, int sh, float time) const;
    bool DrawTopBar     (int sw, int sh, float time) const;
    bool DrawBottomBar  (int sw, int sh, float time) const;

    bool DrawGameCard(QRect r, const char* name, bool foc,
                      D2DBitmapHandle poster, float time) const;

    bool DrawSettingsTile(QRect r, const char* icon, const char* title,
                          D2DColor accent, bool foc, float time) const;

    bool DrawLibraryTab(int sw, int sh, int focusedIdx, float time) const;
    void DrawSidePanel (QRect panelRect, int activeTab, float time) const;

    int  GetContextMenuItems(int gameIdx, const char** items, int maxItems) const;
    void OnContextMenuAction(int gameIdx, int pluginItemOffset, int itemIdx) const;

    bool IsSkinPickerOpen() const { return m_pickerOpen; }
    void OpenSkinPicker()         { m_pickerOpen = true; m_pickerJustOpened = true; }
    void CloseSkinPicker()        { m_pickerOpen = false; }

    bool HasActiveCardSkin() const {
        if (m_activeSkin < 0 || m_activeSkin >= (int)m_plugins.size()) return false;
        return m_plugins[m_activeSkin].desc.DrawGameCard != nullptr;
    }

    bool UpdateAndDrawSkinPicker(int sw, int sh,
                                 bool confirm, bool cancel, bool up, bool down);

    int  ActiveSkinIndex() const { return m_activeSkin; }
    void SetActiveSkin(int idx);

    const std::vector<LoadedPlugin>& Plugins() const { return m_plugins; }
    int  PluginCount() const { return (int)m_plugins.size(); }

    void SaveSkinChoice() const;
    void LoadSkinChoice();

private:
    PluginManager() = default;

    void  LoadPlugin  (const std::string& dllPath);
    void  UnloadPlugin(LoadedPlugin& p);
    bool  IsLoaded    (const std::string& path) const;

    const LoadedPlugin* ActiveSkin_() const;

    std::vector<LoadedPlugin> m_plugins;
    int    m_activeSkin       = -1;
    bool   m_pickerOpen       = false;
    bool   m_pickerJustOpened = false;
    int    m_pickerFocus      = 0;
    std::string           m_exeDir;
    const D2DPluginAPI*   m_d2d  = nullptr;
    const QShellHostAPI*  m_host = nullptr;
};

inline PluginManager& PM() { return PluginManager::Get(); }
