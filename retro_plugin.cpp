// ============================================================================
//  RetroStation Plugin for Q-Shell  v2.0  (D2D backend)
//
//  CRT/DOS aesthetic skin:
//    • Scanlines + grid background
//    • DOS-style top bar
//    • Pixel button hints bottom bar
//    • Cassette-tape game cards
//    • Retro terminal settings tiles
//    • Full horizontal scrolling library
//    • Persistent accent colour setting
//
//  COMPILE (no raylib, no d2d1 link in DLL):
//    g++ -shared -O2 -std=c++17
//        retro_plugin.cpp -o RetroStation.dll
//        -I"." -static -static-libgcc -static-libstdc++
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include "qshell_plugin_api.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <cctype>

// ─── Module globals ───────────────────────────────────────────────────────────

static const D2DPluginAPI*  RL  = nullptr;
static const QShellHostAPI* HST = nullptr;

// ─── Palette ─────────────────────────────────────────────────────────────────

static const D2DColor RETRO_BLACK  = QRGBA(  8,   8,  12, 255);
static const D2DColor RETRO_GREEN  = QRGBA(  0, 255,  70, 255);
static const D2DColor RETRO_GREEN2 = QRGBA(  0, 180,  50, 255);
static const D2DColor RETRO_AMBER  = QRGBA(255, 176,   0, 255);
static const D2DColor RETRO_DIM    = QRGBA( 30,  60,  30, 255);
static const D2DColor RETRO_CARD   = QRGBA( 12,  24,  16, 255);
static const D2DColor RETRO_WHITE  = QRGBA(220, 240, 220, 255);

// ─── Lifecycle ───────────────────────────────────────────────────────────────

static void OnLoad() {
    const char* acc = HST->ReadPluginSetting("RetroStation", "accentColor", "green");
    (void)acc;
    HST->PushNotification("RetroStation", "Plugin activated — go retro!",
                           RETRO_GREEN, 3.5f);
}
static void OnUnload() {}
static void OnTick(float dt) { (void)dt; }

static void OnLibraryChanged() {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d games loaded", HST->GetGameCount());
    HST->PushNotification("RetroStation", buf, RETRO_GREEN2, 2.5f);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static inline D2DColor Fade_(D2DColor c, float a) { return QFADE(c, a); }

// Scanline pass — alternate dark horizontal stripes
static void DrawScanlines(float x, float y, float w, float h, float alpha) {
    for (float sy = y; sy < y + h; sy += 4)
        RL->FillRect(x, sy, w, 2, Fade_(RETRO_BLACK, alpha));
}

// Scrolling dot grid
static void DrawGrid(int sw, int sh, float time) {
    float scroll = RL->sinf_(time * 0.3f) * 40.f;
    D2DColor gc = QRGBA(0, 60, 0, 35);
    for (int x = 0; x < sw + 80; x += 80)
        RL->FillRect((float)x, 0, 1, (float)sh, gc);
    for (int y = (int)scroll; y < sh + 60; y += 60)
        RL->FillRect(0, (float)y, (float)sw, 1, gc);
}

// Screen-edge border lines
static void DrawBorder(int sw, int sh) {
    RL->FillRect(0,         0,        (float)sw, 3,          RETRO_GREEN);
    RL->FillRect(0,         (float)(sh - 3), (float)sw, 3,   RETRO_GREEN);
    RL->FillRect(0,         0,        3,          (float)sh,  RETRO_GREEN);
    RL->FillRect((float)(sw - 3), 0,  3,          (float)sh,  RETRO_GREEN);
}

// ─── Background ──────────────────────────────────────────────────────────────

static bool DrawBackground(int sw, int sh, float time) {
    RL->FillRect(0, 0, (float)sw, (float)sh, RETRO_BLACK);
    DrawGrid(sw, sh, time);
    DrawScanlines(0, 0, (float)sw, (float)sh, 0.15f);
    DrawBorder(sw, sh);
    return true;
}

// ─── Top Bar ─────────────────────────────────────────────────────────────────

static bool DrawTopBar(int sw, int sh, float time) {
    RL->FillRect(0,   0,   (float)sw, 110, QRGBA(0, 20, 0, 240));
    RL->FillRect(0, 107,   (float)sw,   3, RETRO_GREEN);

    RL->DrawTextA("Q-SHELL v2.0",       16, 10, 22.f, RETRO_GREEN,  700);
    RL->DrawTextA("> GAME LIBRARY OS",  16, 38, 14.f, RETRO_GREEN2, 400);

    // Blinking cursor
    if (RL->sinf_(time * 4.f) > 0)
        RL->FillRect(16, 58, 10, 18, RETRO_GREEN);

    // Tab buttons
    const char* tabs[] = {"F1:LIBRARY", "F2:MEDIA", "F3:SHARE", "F4:SETTINGS"};
    int activeTab = HST->GetActiveTab();
    float tx = (float)sw / 2.f - 360;

    for (int i = 0; i < 4; i++) {
        bool sel  = (activeTab == i);
        D2DColor bg = sel ? RETRO_GREEN : RETRO_DIM;
        D2DColor fg = sel ? RETRO_BLACK : RETRO_GREEN2;
        float   tw2 = RL->MeasureTextA(tabs[i], 16, sel ? 700 : 400);
        RL->FillRect(tx - 6, 38, tw2 + 12, 28, bg);
        RL->DrawTextA(tabs[i], tx, 44, 16.f, fg, sel ? 700 : 400);
        tx += tw2 + 30;
    }

    // Clock
    SYSTEMTIME st; GetLocalTime(&st);
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    float ctw = RL->MeasureTextA(tbuf, 24, 400);
    RL->DrawTextA(tbuf, sw - ctw - 20, 10, 24.f, RETRO_AMBER, 400);
    RL->DrawTextA("USR:PLAYER", sw - 140.f, 40, 13.f, RETRO_GREEN2, 400);

    (void)sh;
    return true;
}

// ─── Bottom Bar ──────────────────────────────────────────────────────────────

static bool DrawBottomBar(int sw, int sh, float time) {
    float y = (float)(sh - 70);
    RL->FillRect(0, y, (float)sw, 70, QRGBA(0, 16, 0, 230));
    RL->FillRect(0, y, (float)sw,  2, RETRO_GREEN);

    struct Hint { const char* key; const char* lbl; };
    Hint hints[] = {
        {"[A]","LAUNCH"}, {"[B]","BACK"}, {"[X]","ART"}, {"[Y]","DELETE"}, {"[MENU]","SHELL"}
    };
    float bx = sw / 2.f - 340.f;
    for (int i = 0; i < 5; i++) {
        float kw = RL->MeasureTextA(hints[i].key, 16, 700);
        RL->FillRect(bx, y + 18, kw + 8, 28, RETRO_GREEN);
        RL->DrawTextA(hints[i].key, bx + 4,      y + 21, 16.f, RETRO_BLACK,  700);
        RL->DrawTextA(hints[i].lbl, bx + kw + 14, y + 22, 14.f, RETRO_GREEN2, 400);
        bx += kw + 100;
    }

    char status[64];
    snprintf(status, sizeof(status), "GAMES:%d  PLUGIN:RetroStation", HST->GetGameCount());
    RL->DrawTextA(status, 12, y + 44, 12.f, Fade_(RETRO_GREEN2, 0.6f), 400);

    (void)time;
    return true;
}

// ─── Game Card ───────────────────────────────────────────────────────────────

static bool DrawGameCard(QRect card, const char* name, bool foc,
                          D2DBitmapHandle poster, float time)
{
    float pulse = (RL->sinf_(time * 4.f) + 1.f) / 2.f;
    float rx    = card.width * 0.02f;

    // Shadow
    RL->FillRoundRect(card.x + 5, card.y + 5, card.width, card.height, rx, rx,
                       Fade_(RETRO_BLACK, 0.6f));

    RL->FillRoundRect(card.x, card.y, card.width, card.height, rx, rx, RETRO_CARD);

    // Top accent bar
    RL->FillRect(card.x, card.y, card.width, 6, foc ? RETRO_GREEN : RETRO_DIM);

    // Poster or initial letter
    if (poster.opaque && poster.w > 0) {
        float ta = (float)poster.w / (float)poster.h;
        float ca = card.width / card.height;
        float srcX = 0, srcY = 0, srcW = (float)poster.w, srcH = (float)poster.h;
        if (ta > ca) { srcW = poster.h * ca; srcX = (poster.w - srcW) / 2.f; }
        else         { srcH = poster.w / ca; srcY = (poster.h - srcH) / 2.f; }
        RL->DrawBitmapCropped(poster, srcX, srcY, srcW, srcH,
                               card.x, card.y, card.width, card.height,
                               foc ? 1.f : 0.35f);
        DrawScanlines(card.x, card.y, card.width, card.height, 0.08f);
    } else {
        char init[2] = { name && name[0] ? (char)toupper((unsigned char)name[0]) : '?', 0 };
        float iw = RL->MeasureTextA(init, 72, 700);
        RL->DrawTextA(init,
                       card.x + card.width  / 2 - iw / 2,
                       card.y + card.height / 2 - 36,
                       72.f, Fade_(RETRO_GREEN, foc ? 0.6f : 0.2f), 700);
    }

    // Bottom gradient + name
    RL->FillGradientV(card.x, card.y + card.height - 60,
                       card.width, 60,
                       Fade_(RETRO_BLACK, 0.f), Fade_(RETRO_CARD, 0.95f));

    if (name) {
        char nm[32]; snprintf(nm, sizeof(nm), "%.28s", name);
        RL->DrawTextA(nm, card.x + 10, card.y + card.height - 48,
                       16.f, foc ? RETRO_GREEN : RETRO_GREEN2, 400);
    }

    // Focus ring + corner markers
    if (foc) {
        RL->StrokeRoundRect(card.x - 2, card.y - 2, card.width + 4, card.height + 4,
                             rx, rx, 2.f, Fade_(RETRO_GREEN, 0.4f + pulse * 0.5f));
        if (pulse > 0.5f) {
            RL->FillRect(card.x,      card.y,      12, 3, RETRO_AMBER);
            RL->FillRect(card.x,      card.y,       3, 12, RETRO_AMBER);
        }
    }
    return true;
}

// ─── Settings Tile ───────────────────────────────────────────────────────────

static bool DrawSettingsTile(QRect r, const char* icon, const char* title,
                              D2DColor accent, bool foc, float time)
{
    float pulse = (RL->sinf_(time * 4.f) + 1.f) / 2.f;
    float sc    = foc ? 1.06f : 1.f;
    QRect s = {
        r.x - r.width  * (sc - 1) / 2.f,
        r.y - r.height * (sc - 1) / 2.f,
        r.width  * sc,
        r.height * sc
    };
    float rx = s.width * 0.075f;

    // Shadow
    RL->FillRoundRect(s.x + 4, s.y + 4, s.width, s.height, rx, rx,
                       Fade_(RETRO_BLACK, 0.5f));

    // Body
    RL->FillRoundRect(s.x, s.y, s.width, s.height, rx, rx,
                       foc ? Fade_(RETRO_GREEN, 0.12f) : RETRO_CARD);

    // Top accent bar
    RL->FillRect(s.x, s.y, s.width, 4, foc ? RETRO_GREEN : RETRO_DIM);

    // Icon
    float iw = RL->MeasureTextA(icon, 36, 400);
    RL->DrawTextA(icon,
                   s.x + s.width / 2 - iw / 2,
                   s.y + s.height * 0.28f,
                   36.f, foc ? RETRO_GREEN : Fade_(RETRO_GREEN, 0.4f), 400);

    // Title
    float tw2 = RL->MeasureTextA(title, 14, 400);
    RL->DrawTextA(title,
                   s.x + s.width / 2 - tw2 / 2,
                   s.y + s.height * 0.7f,
                   14.f, foc ? RETRO_WHITE : Fade_(RETRO_GREEN2, 0.7f), 400);

    // Focus ring + corner markers
    if (foc) {
        RL->StrokeRoundRect(s.x, s.y, s.width, s.height, rx, rx,
                             1.f, Fade_(RETRO_GREEN, 0.35f + pulse * 0.45f));
        // Amber corner accents
        RL->FillRect(s.x,                s.y,                 10, 2, RETRO_AMBER);
        RL->FillRect(s.x,                s.y,                  2, 10, RETRO_AMBER);
        RL->FillRect(s.x + s.width - 10, s.y,                 10, 2, RETRO_AMBER);
        RL->FillRect(s.x + s.width -  2, s.y,                  2, 10, RETRO_AMBER);
        RL->FillRect(s.x,                s.y + s.height - 2,  10, 2, RETRO_AMBER);
        RL->FillRect(s.x,                s.y + s.height - 10,  2, 10, RETRO_AMBER);
    }

    (void)accent;
    return true;
}

// ─── Full Library Tab (horizontal scroller) ───────────────────────────────────

static bool DrawLibraryTab(int sw, int sh, int focusedIdx, float time)
{
    int count = HST->GetGameCount();
    if (count == 0) {
        const char* msg = "> NO GAMES FOUND.  ADD SOME IN SETTINGS.";
        float mw = RL->MeasureTextA(msg, 20, 400);
        RL->DrawTextA(msg, (sw - mw) / 2.f, sh / 2.f, 20.f, RETRO_GREEN2, 400);
        return true;
    }

    float cardW  = 300.f, cardH = 420.f, gap = 30.f;
    float rowY   = sh / 2.f - cardH / 2.f + 10.f;
    float targetX = sw / 2.f - focusedIdx * (cardW + gap) - cardW / 2.f;

    static float scrollX = 0.f;
    scrollX += (targetX - scrollX) * 0.12f;

    for (int i = 0; i < count; i++) {
        float cx = scrollX + i * (cardW + gap);
        if (cx < -cardW - 50 || cx > sw + 50) continue;

        bool  foc = (i == focusedIdx);
        float fy  = foc ? rowY - 20.f : rowY + 10.f;
        QRect card = { cx, fy, cardW, cardH };

        QShellGameInfo gi = {};
        HST->GetGame(i, &gi);

        D2DBitmapHandle noTex = {};
        DrawGameCard(card, gi.name, foc, noTex, time);

        // Platform badge
        if (gi.platform && gi.platform[0]) {
            float pw2 = RL->MeasureTextA(gi.platform, 12, 400);
            RL->FillRect(cx + cardW - pw2 - 14, fy + 8, pw2 + 10, 20,
                          Fade_(RETRO_GREEN, 0.25f));
            RL->DrawTextA(gi.platform, cx + cardW - pw2 - 9, fy + 11,
                           12.f, RETRO_GREEN2, 400);
        }
    }

    // Counter
    char counter[32];
    snprintf(counter, sizeof(counter), "> %d / %d", focusedIdx + 1, count);
    float cw = RL->MeasureTextA(counter, 18, 400);
    RL->DrawTextA(counter, (sw - cw) / 2.f, rowY + cardH + 24, 18.f, RETRO_GREEN2, 400);

    // Dot navigation indicators
    float dotStart = sw / 2.f - (count * 14.f) / 2.f;
    for (int i = 0; i < count && i < 20; i++) {
        D2DColor dc = (i == focusedIdx) ? RETRO_GREEN : RETRO_DIM;
        RL->FillRect(dotStart + i * 14, rowY + cardH + 52, 8, 8, dc);
    }

    return true;
}

// ─── Context menu extras ─────────────────────────────────────────────────────

static const char* s_extraItems[] = {"Open in Explorer", "Copy path"};

static int GetContextMenuItems(int /*gameIdx*/, const char** items, int maxItems) {
    int n = 0;
    for (auto& item : s_extraItems) {
        if (n >= maxItems) break;
        items[n++] = item;
    }
    return n;
}

static void OnContextMenuAction(int gameIdx, int itemIdx) {
    QShellGameInfo gi = {};
    HST->GetGame(gameIdx, &gi);
    if (!gi.path) return;

    if (itemIdx == 0) {
        std::string dir(gi.path);
        auto slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (itemIdx == 1) {
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            size_t len = strlen(gi.path) + 1;
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len);
            if (hg) { memcpy(GlobalLock(hg), gi.path, len); GlobalUnlock(hg); }
            SetClipboardData(CF_TEXT, hg);
            CloseClipboard();
            HST->PushNotification("RetroStation", "Path copied to clipboard",
                                  RETRO_GREEN2, 2.f);
        }
    }
}

// ─── Entry-point ─────────────────────────────────────────────────────────────

QSHELL_PLUGIN_EXPORT
void RegisterPlugin(QShellPluginDesc* desc)
{
    RL  = desc->rl;
    HST = desc->host;

    desc->name        = "RetroStation";
    desc->author      = "YourName";
    desc->version     = "2.0.0";
    desc->description = "CRT/DOS aesthetic skin with horizontal library scroller (D2D)";

    desc->OnLoad           = OnLoad;
    desc->OnUnload         = OnUnload;
    desc->OnTick           = OnTick;
    desc->OnLibraryChanged = OnLibraryChanged;

    desc->DrawBackground   = DrawBackground;
    desc->DrawTopBar       = DrawTopBar;
    desc->DrawBottomBar    = DrawBottomBar;
    desc->DrawGameCard     = DrawGameCard;
    desc->DrawSettingsTile = DrawSettingsTile;
    desc->DrawLibraryTab   = DrawLibraryTab;

    desc->GetContextMenuItems = GetContextMenuItems;
    desc->OnContextMenuAction = OnContextMenuAction;
}
