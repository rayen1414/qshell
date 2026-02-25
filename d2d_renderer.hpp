// ============================================================================
//  d2d_renderer.hpp  —  Q-Shell Direct2D / DirectWrite render backend
//
//  Replaces raylib entirely for the host-side rendering layer.
//  Plugin DLLs never include this file; they call through D2DPluginAPI.
//
//  Link: d2d1.lib  dwrite.lib  windowscodecs.lib  (added via #pragma below)
// ============================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <unordered_map>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

// ─── D2DBitmap ────────────────────────────────────────────────────────────────
// Opaque handle to a GPU-resident bitmap loaded via WIC.
// Matches the D2DBitmapHandle in qshell_plugin_api.h (same layout).

struct D2DBitmap {
    ID2D1Bitmap* bmp = nullptr;
    int          w   = 0;
    int          h   = 0;

    bool Valid() const { return bmp != nullptr; }
};

// ─── D2DRenderer ─────────────────────────────────────────────────────────────

class D2DRenderer {
public:
    static D2DRenderer& Get() { static D2DRenderer s; return s; }
    D2DRenderer(const D2DRenderer&)            = delete;
    D2DRenderer& operator=(const D2DRenderer&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    bool Init   (HWND hwnd, int w, int h);
    void Shutdown();
    void Resize (int w, int h);       // call from WM_SIZE

    // ── Per-frame ─────────────────────────────────────────────────────────────
    void BeginFrame(D2D1_COLOR_F clearColor);
    void EndFrame  ();                // calls Present (EndDraw)
    bool IsDrawing () const { return m_drawing; }

    // ── Filled rectangles ─────────────────────────────────────────────────────
    void FillRect      (float x, float y, float w, float h, D2D1_COLOR_F c);
    void FillRoundRect (float x, float y, float w, float h, float rx, float ry,
                        D2D1_COLOR_F c);
    void StrokeRoundRect(float x, float y, float w, float h, float rx, float ry,
                         float strokeW, D2D1_COLOR_F c);
    void FillGradientV (float x, float y, float w, float h,
                        D2D1_COLOR_F top, D2D1_COLOR_F bot);
    void FillGradientH (float x, float y, float w, float h,
                        D2D1_COLOR_F left, D2D1_COLOR_F right);

    // ── Blur / frosted-glass (requires ID2D1DeviceContext effect path) ─────────
    // sigma: standard deviation in pixels (e.g. 8.f for heavy blur)
    // tint : colour multiplied over the blurred region
    void FillBlurRect  (float x, float y, float w, float h,
                        float sigma, D2D1_COLOR_F tint);

    // ── Circles ───────────────────────────────────────────────────────────────
    void FillCircle    (float cx, float cy, float r, D2D1_COLOR_F c);
    void StrokeCircle  (float cx, float cy, float r, float strokeW, D2D1_COLOR_F c);

    // ── Lines ─────────────────────────────────────────────────────────────────
    void DrawLine      (float x0, float y0, float x1, float y1,
                        float strokeW, D2D1_COLOR_F c);

    // ── Text (DirectWrite, UTF-16) ─────────────────────────────────────────────
    // weight: DWRITE_FONT_WEIGHT_NORMAL (400) or DWRITE_FONT_WEIGHT_BOLD (700)
    void  DrawTextW    (const wchar_t* text, float x, float y, float size,
                        D2D1_COLOR_F c,
                        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);

    // UTF-8 convenience wrapper (converts internally)
    void  DrawTextA    (const char* text, float x, float y, float size,
                        D2D1_COLOR_F c,
                        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);

    float MeasureTextW (const wchar_t* text, float size,
                        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
    float MeasureTextA (const char* text, float size,
                        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);

    // ── Bitmaps (WIC loader) ──────────────────────────────────────────────────
    D2DBitmap LoadBitmap  (const wchar_t* path);
    D2DBitmap LoadBitmapA (const char*    path);   // UTF-8 path helper
    void      UnloadBitmap(D2DBitmap& bmp);
    void      DrawBitmap  (const D2DBitmap& bmp,
                           float x, float y, float w, float h,
                           float opacity = 1.f);
    // Draw with source-rect cropping (like DrawTexturePro)
    void      DrawBitmapCropped(const D2DBitmap& bmp,
                                float srcX, float srcY, float srcW, float srcH,
                                float dstX, float dstY, float dstW, float dstH,
                                float opacity = 1.f);

    // ── Scissor / clip ────────────────────────────────────────────────────────
    void PushClip (float x, float y, float w, float h);
    void PopClip  ();

    // ── Queries ───────────────────────────────────────────────────────────────
    int   ScreenWidth ()  const { return m_w; }
    int   ScreenHeight()  const { return m_h; }
    HWND  Hwnd        ()  const { return m_hwnd; }

    // Raw access (plugin_manager.cpp uses this directly for the skin picker)
    ID2D1HwndRenderTarget* RT()  { return m_rt;  }
    IDWriteFactory*         DW()  { return m_dw;  }

    // ── Colour helpers ────────────────────────────────────────────────────────
    // Fade: multiply alpha by 'a' (mirrors raylib Fade)
    static D2D1_COLOR_F Fade(D2D1_COLOR_F c, float a) {
        return D2D1::ColorF(c.r, c.g, c.b, c.a * a);
    }
    // Convert 0-255 RGBA to D2D1_COLOR_F
    static D2D1_COLOR_F RGBA(int r, int g, int b, int a = 255) {
        return D2D1::ColorF(r/255.f, g/255.f, b/255.f, a/255.f);
    }

private:
    D2DRenderer() = default;

    // Get or create a solid brush for the given colour
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);

    // Get or create an IDWriteTextFormat for a given (size, weight) pair
    IDWriteTextFormat* TextFormat(float size, DWRITE_FONT_WEIGHT weight);

    // Internal text layout helper
    IDWriteTextLayout* MakeLayout(const wchar_t* text, float size,
                                  DWRITE_FONT_WEIGHT weight,
                                  float maxW = 4096.f, float maxH = 256.f);

    HWND                        m_hwnd    = nullptr;
    ID2D1Factory1*              m_fac     = nullptr;
    ID2D1HwndRenderTarget*      m_rt      = nullptr;
    IDWriteFactory*             m_dw      = nullptr;
    IWICImagingFactory*         m_wic     = nullptr;
    ID2D1SolidColorBrush*       m_brush   = nullptr;  // reused solid brush
    int                         m_w       = 0;
    int                         m_h       = 0;
    bool                        m_drawing = false;
    int                         m_clipDepth = 0;

    // Cache text formats to avoid recreating them every frame
    struct TFKey { float size; int weight; bool operator==(const TFKey& o) const { return size==o.size&&weight==o.weight; } };
    struct TFHash { size_t operator()(const TFKey& k) const { return std::hash<float>()(k.size) ^ (std::hash<int>()(k.weight)<<16); } };
    std::unordered_map<TFKey, IDWriteTextFormat*, TFHash> m_tfCache;
};

// Global shorthand — same pattern as PM()
inline D2DRenderer& D2D() { return D2DRenderer::Get(); }
