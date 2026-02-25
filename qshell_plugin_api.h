// ============================================================================
//  qshell_plugin_api.h  — Q-Shell Plugin API  v4.0
//
//  THE ONE HEADER that every plugin DLL includes.
//  Do NOT link against raylib or d2d1 — the host passes a D2DPluginAPI
//  function-pointer table instead.
//
//  ── Type layer ───────────────────────────────────────────────────────────────
//  This header is compiled both inside the host (qshell.cpp, plugin_manager.cpp)
//  and inside plugin DLLs.  All types are self-contained: no raylib, no D2D
//  headers are required in plugin code.
//
//  ── QRect ────────────────────────────────────────────────────────────────────
//  A plain {x,y,width,height} float struct.  Previously there was a conditional
//  typedef-to-raylib-Rectangle here; that is now gone.  QRect is always our
//  own struct, which means plugin and host always agree on layout with zero
//  macro tricks.
//
//  ── D2DColor ─────────────────────────────────────────────────────────────────
//  Matches D2D1_COLOR_F exactly (four floats, r g b a in [0,1]).
//  Conversion helpers are provided at the bottom of this file.
//
//  ── D2DBitmapHandle ──────────────────────────────────────────────────────────
//  Replaces Texture2D.  An opaque handle the host returns from LoadBitmap;
//  plugins store it and pass it back to DrawBitmap.  The actual
//  ID2D1Bitmap* lives inside the host — plugins never touch COM directly.
// ============================================================================

#pragma once

#include <stddef.h>   // size_t
#include <stdint.h>

// ─── Guard conflicting Windows names ─────────────────────────────────────────
#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

// ─── Plugin export macro ──────────────────────────────────────────────────────
#if defined(_WIN32)
#define QSHELL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define QSHELL_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif


// ============================================================================
//  Core types
// ============================================================================

// QRect — position + size in screen pixels (float)
typedef struct QRect {
    float x, y, width, height;
} QRect;

// D2DColor — linear RGBA in [0,1], matches D2D1_COLOR_F exactly.
typedef struct D2DColor {
    float r, g, b, a;
} D2DColor;

// D2DBitmapHandle — opaque GPU bitmap handle returned by LoadBitmap.
// The void* is the underlying ID2D1Bitmap* cast; plugins must never
// dereference it — only pass it back through the D2DPluginAPI table.
typedef struct D2DBitmapHandle {
    void* opaque;   // ID2D1Bitmap* (host-owned, ref-counted by host)
    int   w;        // pixel width
    int   h;        // pixel height
} D2DBitmapHandle;

// Vector2 — 2-D float vector (used by DrawLine, DrawBitmapCropped origin, etc.)
typedef struct QVec2 {
    float x, y;
} QVec2;


// ============================================================================
//  D2DPluginAPI — function-pointer table the host fills and passes to plugins.
//  Plugins call RL->FillRect(...) etc. — never link against d2d1.lib.
//
//  Naming convention:
//    Fill*      — filled shapes
//    Stroke*    — outlined shapes
//    Draw*      — lines, bitmaps, text
//    Measure*   — returns a float metric without drawing
//    Push/Pop*  — stateful operations (clip stack)
// ============================================================================

typedef struct D2DPluginAPI {

    // ── Solid filled rectangles ───────────────────────────────────────────────
    void (*FillRect)      (float x, float y, float w, float h, D2DColor c);
    void (*FillRoundRect) (float x, float y, float w, float h,
                           float rx, float ry, D2DColor c);
    void (*StrokeRoundRect)(float x, float y, float w, float h,
                            float rx, float ry, float strokeW, D2DColor c);

    // ── Gradient rectangles (linear, axis-aligned) ────────────────────────────
    // V = top → bottom,  H = left → right
    void (*FillGradientV) (float x, float y, float w, float h,
                           D2DColor top, D2DColor bot);
    void (*FillGradientH) (float x, float y, float w, float h,
                           D2DColor left, D2DColor right);

    // ── Frosted-glass / blur rect ─────────────────────────────────────────────
    // sigma: blur radius in pixels.  tint: colour multiplied over the region.
    // Falls back to a semi-transparent filled rect on hardware without D2D 1.1.
    void (*FillBlurRect)  (float x, float y, float w, float h,
                           float sigma, D2DColor tint);

    // ── Circles ───────────────────────────────────────────────────────────────
    void (*FillCircle)    (float cx, float cy, float r, D2DColor c);
    void (*StrokeCircle)  (float cx, float cy, float r, float strokeW, D2DColor c);

    // ── Lines ─────────────────────────────────────────────────────────────────
    void (*DrawLine)      (float x0, float y0, float x1, float y1,
                           float strokeW, D2DColor c);

    // ── Text — UTF-16 (preferred, exact Unicode) ──────────────────────────────
    // weight: 400 = normal, 700 = bold
    void  (*DrawTextW)    (const wchar_t* text, float x, float y, float size,
                           D2DColor c, int weight);
    float (*MeasureTextW) (const wchar_t* text, float size, int weight);

    // ── Text — UTF-8 convenience wrappers ─────────────────────────────────────
    // Internally converts to UTF-16 and calls DrawTextW/MeasureTextW.
    void  (*DrawTextA)    (const char* text, float x, float y, float size,
                           D2DColor c, int weight);
    float (*MeasureTextA) (const char* text, float size, int weight);

    // ── Bitmaps ───────────────────────────────────────────────────────────────
    // LoadBitmap: loads a PNG/JPG/BMP/etc. from disk. Returns {nullptr,0,0} on fail.
    // UnloadBitmap: releases GPU memory.
    // DrawBitmap: stretch-draws to [x,y,w,h] with optional opacity [0,1].
    // DrawBitmapCropped: like DrawTexturePro — explicit source rect + dest rect.
    D2DBitmapHandle (*LoadBitmapW)     (const wchar_t* path);
    D2DBitmapHandle (*LoadBitmapA)     (const char*    path);
    void            (*UnloadBitmap)    (D2DBitmapHandle bmp);
    void            (*DrawBitmap)      (D2DBitmapHandle bmp,
                                        float x, float y, float w, float h,
                                        float opacity);
    void            (*DrawBitmapCropped)(D2DBitmapHandle bmp,
                                         float srcX, float srcY,
                                         float srcW, float srcH,
                                         float dstX, float dstY,
                                         float dstW, float dstH,
                                         float opacity);

    // ── Scissor / clip ────────────────────────────────────────────────────────
    void (*PushClip)      (float x, float y, float w, float h);
    void (*PopClip)       (void);

    // ── Time / screen ─────────────────────────────────────────────────────────
    float (*GetTime)        (void);   // seconds since app start
    int   (*GetScreenWidth) (void);
    int   (*GetScreenHeight)(void);

    // ── Math helpers ──────────────────────────────────────────────────────────
    float (*sinf_)(float x);          // sinf wrapper (avoids CRT dependency issues)

} D2DPluginAPI;


// ============================================================================
//  QShellGameInfo  — snapshot of a single library entry passed to plugins
// ============================================================================

typedef struct QShellGameInfo {
    const char* name;          // display name
    const char* path;          // executable path
    const char* platform;      // "Steam", "Epic", "Manual", etc.
    const char* coverPath;     // path to cover art file (may be "")
    long long   playtime_sec;  // total playtime in seconds (0 if unknown)
    long long   last_played;   // Unix timestamp of last play (0 if unknown)
} QShellGameInfo;


// ============================================================================
//  QShellTheme  — current host UI colour palette snapshot (D2DColor)
// ============================================================================

typedef struct QShellTheme {
    D2DColor primary;    // background / darkest
    D2DColor secondary;  // card / panel background
    D2DColor accent;     // highlight / active colour
    D2DColor accentAlt;  // secondary highlight
    D2DColor text;       // primary text
    D2DColor textDim;    // secondary / muted text
    D2DColor cardBg;     // card body background
    D2DColor success;    // green confirmation
    D2DColor warning;    // yellow warning
    D2DColor danger;     // red error / destructive
} QShellTheme;


// ============================================================================
//  QShellInput  — one-frame snapshot of controller + keyboard state
// ============================================================================

typedef struct QShellInput {
    // Face buttons
    bool confirm;   // A / Cross
    bool back;      // B / Circle
    bool action1;   // X / Square
    bool action2; 
     bool cancel;      // ← add this
    bool menu;        // ← add this
    bool view;        // ← add this
    bool triangle;    // ← add this
    bool square;      // ← add this
    bool square_held; // ← add this
    int  gamepadId;   // Y / Triangle

    // Shoulders
    bool lb, rb;    // L1 / R1
    bool lt, rt;    // L2 / R2 (digital threshold)

    // D-pad
    bool up, down, left, right;

    // System
    bool start, select;

    // Left stick (normalised -1..1)
    float lx, ly;
} QShellInput;


// ============================================================================
//  QShellHostAPI  — functions the host exposes to plugins
// ============================================================================

typedef struct QShellHostAPI {

    // ── Notification toast ────────────────────────────────────────────────────
    void (*PushNotification)(const char* title, const char* msg,
                              D2DColor col, float lifetime);

    // ── Game library ─────────────────────────────────────────────────────────
    int  (*GetGameCount)(void);
    void (*GetGame)     (int index, QShellGameInfo* out);
    void (*LaunchGame)  (int index);
    void (*RemoveGame)  (int index);

    // ── Navigation state ──────────────────────────────────────────────────────
    int  (*GetFocusedIdx)(void);
    void (*SetFocusedIdx)(int idx);
    int  (*GetActiveTab) (void);   // 0=Library 1=Media 2=Share 3=Settings
    void (*SetActiveTab) (int tab);

    // ── Theme ────────────────────────────────────────────────────────────────
    const QShellTheme* (*GetTheme)        (void);
    void               (*SetThemeByIndex) (int i);

    // ── Input snapshot ────────────────────────────────────────────────────────
    const QShellInput* (*GetInput)(void);

    // ── Plugin persistent settings ────────────────────────────────────────────
    void        (*WritePluginSetting)(const char* pluginName,
                                       const char* key, const char* value);
    const char* (*ReadPluginSetting) (const char* pluginName,
                                       const char* key, const char* defaultVal);

    // ── Texture helpers (delegates to D2DRenderer) ────────────────────────────
    D2DBitmapHandle (*LoadPluginBitmapW) (const wchar_t* path);
    D2DBitmapHandle (*LoadPluginBitmapA) (const char*    path);
    void            (*UnloadPluginBitmap)(D2DBitmapHandle bmp);

    // ── Screen info ───────────────────────────────────────────────────────────
    int   (*GetScreenWidth) (void);
    int   (*GetScreenHeight)(void);
    float (*GetTime)        (void);

    // ── Shell mode ────────────────────────────────────────────────────────────
    bool (*IsShellMode)(void);

} QShellHostAPI;


// ============================================================================
//  QShellPluginDesc  — filled by RegisterPlugin(), read by the host
// ============================================================================

typedef struct QShellPluginDesc {

    // ── Pre-filled by host (read-only for plugin) ─────────────────────────────
    const D2DPluginAPI*  rl;    // draw API  (named rl for source compatibility)
    const QShellHostAPI* host;

    // ── Plugin metadata ───────────────────────────────────────────────────────
    const char* name;
    const char* author;
    const char* version;
    const char* description;
    bool        isSkin;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    void (*OnLoad)          (void);
    void (*OnUnload)        (void);
    void (*OnTick)          (float dt);
    void (*OnLibraryChanged)(void);

    // ── Draw overrides — return true to suppress host default drawing ──────────
    bool (*DrawBackground)  (int sw, int sh, float time);
    bool (*DrawTopBar)      (int sw, int sh, float time);
    bool (*DrawBottomBar)   (int sw, int sh, float time);
    bool (*DrawGameCard)    (QRect card, const char* name, bool focused,
                              D2DBitmapHandle poster, float time);
    bool (*DrawSettingsTile)(QRect rect, const char* icon, const char* title,
                              D2DColor accent, bool focused, float time);
    bool (*DrawLibraryTab)  (int sw, int sh, int focusedIdx, float time);
    void (*DrawSidePanel)   (QRect panelRect, int activeTab, float time);

    // ── Context-menu extension ────────────────────────────────────────────────
    int  (*GetContextMenuItems)(int gameIdx, const char** items, int maxItems);
    void (*OnContextMenuAction)(int gameIdx, int itemIdx);

} QShellPluginDesc;


// ============================================================================
//  Entry-point — every plugin DLL must export this
// ============================================================================

typedef void (*RegisterPluginFn)(QShellPluginDesc* desc);


// ============================================================================
//  Layout constants
// ============================================================================

#ifndef SKIN_TOP_BAR_H
static const int SKIN_TOP_BAR_H = 110;
#endif
#ifndef SKIN_BOT_BAR_H
static const int SKIN_BOT_BAR_H =  70;
#endif
#ifndef SKIN_CARD_W
static const int SKIN_CARD_W    = 480;
#endif
#ifndef SKIN_CARD_H
static const int SKIN_CARD_H    = 270;
#endif


// ============================================================================
//  Colour helper macros (optional — mirrors the old Color literal syntax)
//
//  Usage:  D2DColor c = QRGBA(100, 149, 237, 255);
//          D2DColor c = QFADE(c, 0.5f);
// ============================================================================

#ifndef QRGBA
#define QRGBA(r,g,b,a) D2DColor{ (r)/255.f, (g)/255.f, (b)/255.f, (a)/255.f }
#endif
#ifndef QFADE
#define QFADE(c,alpha) D2DColor{ (c).r, (c).g, (c).b, (c).a * (alpha) }
#endif
