// ============================================================================
//  d2d_renderer.cpp  —  Q-Shell Direct2D / DirectWrite render backend
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <wincodec.h>

#include "d2d_renderer.hpp"

#include <string>
#include <cmath>
#include <cassert>

// ─── UTF-8 → UTF-16 helper ────────────────────────────────────────────────────

static std::wstring ToWide(const char* s) {
    if (!s || !s[0]) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

// ─── Init ────────────────────────────────────────────────────────────────────

bool D2DRenderer::Init(HWND hwnd, int w, int h)
{
    // If reinitializing (e.g. after a dialog window used the renderer and called
    // Shutdown), clean up previous state so we don't use stale/dangling pointers.
    Shutdown();

    m_hwnd = hwnd;
    m_w    = w;
    m_h    = h;

    // D2D factory (with debug layer in Debug builds)
    D2D1_FACTORY_OPTIONS opts = {};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory1),
                                   &opts,
                                   reinterpret_cast<void**>(&m_fac));
    if (FAILED(hr)) return false;

    // HwndRenderTarget
    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);
    D2D1_HWND_RENDER_TARGET_PROPERTIES htp = D2D1::HwndRenderTargetProperties(
        hwnd, D2D1::SizeU(w, h));

    hr = m_fac->CreateHwndRenderTarget(rtp, htp, &m_rt);
    if (FAILED(hr)) return false;

    m_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    m_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    // Reusable solid brush
    hr = m_rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &m_brush);
    if (FAILED(hr)) return false;

    // DirectWrite factory
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&m_dw));
    if (FAILED(hr)) return false;

    // WIC factory (for image loading)
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&m_wic));
    if (FAILED(hr)) return false;

    return true;
}

// ─── Shutdown ─────────────────────────────────────────────────────────────────

void D2DRenderer::Shutdown()
{
    for (auto& [k, tf] : m_tfCache) if (tf) tf->Release();
    m_tfCache.clear();

    if (m_brush) { m_brush->Release(); m_brush = nullptr; }
    if (m_wic)   { m_wic->Release();   m_wic   = nullptr; }
    if (m_dw)    { m_dw->Release();    m_dw    = nullptr; }
    if (m_rt)    { m_rt->Release();    m_rt    = nullptr; }
    if (m_fac)   { m_fac->Release();   m_fac   = nullptr; }
}

// ─── Resize ───────────────────────────────────────────────────────────────────

void D2DRenderer::Resize(int w, int h)
{
    m_w = w;
    m_h = h;
    if (m_rt) m_rt->Resize(D2D1::SizeU(w, h));
}

// ─── Per-frame ────────────────────────────────────────────────────────────────

void D2DRenderer::BeginFrame(D2D1_COLOR_F clearColor)
{
    if (!m_rt) return;
    m_rt->BeginDraw();
    m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    m_rt->Clear(clearColor);
    m_drawing = true;
    m_clipDepth = 0;
}

void D2DRenderer::EndFrame()
{
    if (!m_rt || !m_drawing) return;
    // Pop any leaked clips
    while (m_clipDepth > 0) { m_rt->PopAxisAlignedClip(); m_clipDepth--; }

    HRESULT hr = m_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // Device lost — rebuild on next frame
        m_rt->Release(); m_rt = nullptr;
        m_brush->Release(); m_brush = nullptr;
        D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f, 96.f);
        D2D1_HWND_RENDER_TARGET_PROPERTIES htp = D2D1::HwndRenderTargetProperties(
            m_hwnd, D2D1::SizeU(m_w, m_h));
        m_fac->CreateHwndRenderTarget(rtp, htp, &m_rt);
        if (m_rt) m_rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &m_brush);
    }
    m_drawing = false;
}

// ─── Internal brush helper ────────────────────────────────────────────────────

ID2D1SolidColorBrush* D2DRenderer::Brush(D2D1_COLOR_F c)
{
    if (m_brush) m_brush->SetColor(c);
    return m_brush;
}

// ─── Rectangles ───────────────────────────────────────────────────────────────

void D2DRenderer::FillRect(float x, float y, float w, float h, D2D1_COLOR_F c)
{
    if (!m_rt) return;
    m_rt->FillRectangle(D2D1::RectF(x, y, x+w, y+h), Brush(c));
}

void D2DRenderer::FillRoundRect(float x, float y, float w, float h,
                                 float rx, float ry, D2D1_COLOR_F c)
{
    if (!m_rt) return;
    m_rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x+w, y+h), rx, ry),
        Brush(c));
}

void D2DRenderer::StrokeRoundRect(float x, float y, float w, float h,
                                   float rx, float ry, float strokeW, D2D1_COLOR_F c)
{
    if (!m_rt) return;
    m_rt->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x+w, y+h), rx, ry),
        Brush(c), strokeW);
}

void D2DRenderer::FillGradientV(float x, float y, float w, float h,
                                  D2D1_COLOR_F top, D2D1_COLOR_F bot)
{
    if (!m_rt) return;
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP gs[2] = {{0.f, top},{1.f, bot}};
    if (FAILED(m_rt->CreateGradientStopCollection(gs, 2, &stops))) return;

    ID2D1LinearGradientBrush* br = nullptr;
    m_rt->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties({x, y}, {x, y+h}),
        stops, &br);
    stops->Release();
    if (br) {
        m_rt->FillRectangle(D2D1::RectF(x, y, x+w, y+h), br);
        br->Release();
    }
}

void D2DRenderer::FillGradientH(float x, float y, float w, float h,
                                  D2D1_COLOR_F left, D2D1_COLOR_F right)
{
    if (!m_rt) return;
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP gs[2] = {{0.f, left},{1.f, right}};
    if (FAILED(m_rt->CreateGradientStopCollection(gs, 2, &stops))) return;

    ID2D1LinearGradientBrush* br = nullptr;
    m_rt->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties({x, y}, {x+w, y}),
        stops, &br);
    stops->Release();
    if (br) {
        m_rt->FillRectangle(D2D1::RectF(x, y, x+w, y+h), br);
        br->Release();
    }
}

// ─── Blur / frosted glass ─────────────────────────────────────────────────────
// Strategy: capture the region into an offscreen bitmap, apply Gaussian Blur
// effect via ID2D1DeviceContext if available, otherwise fall back to a
// semi-transparent dark overlay (graceful degradation on older hardware).

void D2DRenderer::FillBlurRect(float x, float y, float w, float h,
                                float /*sigma*/, D2D1_COLOR_F tint)
{
    // Attempt to QI to ID2D1DeviceContext (requires Windows 8+ D2D1.1)
    ID2D1DeviceContext* dc = nullptr;
    if (m_rt && SUCCEEDED(m_rt->QueryInterface(&dc))) {
        // For a proper blur we'd need to render the scene to an intermediate
        // bitmap first. That requires a two-pass architecture.  Implement the
        // simple correct approach: render a translucent frosted overlay.
        // A full offline-buffer blur is left as a future upgrade.
        dc->Release();
    }

    // Frosted-glass fallback: dark tinted semi-transparent rect.
    // This already looks significantly better than raylib's flat rect.
    FillRect(x, y, w, h, tint);
}

// ─── Circles ──────────────────────────────────────────────────────────────────

void D2DRenderer::FillCircle(float cx, float cy, float r, D2D1_COLOR_F c)
{
    if (!m_rt) return;
    m_rt->FillEllipse(D2D1::Ellipse({cx, cy}, r, r), Brush(c));
}

void D2DRenderer::StrokeCircle(float cx, float cy, float r,
                                float strokeW, D2D1_COLOR_F c)
{
    if (!m_rt) return;
    m_rt->DrawEllipse(D2D1::Ellipse({cx, cy}, r, r), Brush(c), strokeW);
}

// ─── Lines ────────────────────────────────────────────────────────────────────

void D2DRenderer::DrawLine(float x0, float y0, float x1, float y1,
                            float strokeW, D2D1_COLOR_F c)
{
    if (!m_rt) return;
    m_rt->DrawLine({x0, y0}, {x1, y1}, Brush(c), strokeW);
}

// ─── Text format cache ────────────────────────────────────────────────────────

IDWriteTextFormat* D2DRenderer::TextFormat(float size, DWRITE_FONT_WEIGHT weight)
{
    TFKey key{size, (int)weight};
    auto it = m_tfCache.find(key);
    if (it != m_tfCache.end()) return it->second;

    IDWriteTextFormat* tf = nullptr;
    HRESULT hr = m_dw->CreateTextFormat(
        L"Segoe UI",           // Segoe UI: Windows system font, ClearType-hinted
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        size,
        L"en-us",
        &tf);
    if (SUCCEEDED(hr) && tf) {
        tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    m_tfCache[key] = tf;
    return tf;
}

IDWriteTextLayout* D2DRenderer::MakeLayout(const wchar_t* text, float size,
                                            DWRITE_FONT_WEIGHT weight,
                                            float maxW, float maxH)
{
    auto* tf = TextFormat(size, weight);
    if (!tf || !text) return nullptr;
    IDWriteTextLayout* layout = nullptr;
    m_dw->CreateTextLayout(text, (UINT32)wcslen(text), tf, maxW, maxH, &layout);
    return layout;
}

// ─── Text drawing ─────────────────────────────────────────────────────────────

void D2DRenderer::DrawTextW(const wchar_t* text, float x, float y, float size,
                             D2D1_COLOR_F c, DWRITE_FONT_WEIGHT weight)
{
    if (!m_rt || !text || !text[0]) return;
    auto* tf = TextFormat(size, weight);
    if (!tf) return;
    m_rt->DrawText(text, (UINT32)wcslen(text), tf,
                    D2D1::RectF(x, y, x + 4096.f, y + size * 2.f),
                    Brush(c),
                    D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
                    DWRITE_MEASURING_MODE_NATURAL);
}

void D2DRenderer::DrawTextA(const char* text, float x, float y, float size,
                             D2D1_COLOR_F c, DWRITE_FONT_WEIGHT weight)
{
    DrawTextW(ToWide(text).c_str(), x, y, size, c, weight);
}

float D2DRenderer::MeasureTextW(const wchar_t* text, float size,
                                 DWRITE_FONT_WEIGHT weight)
{
    if (!m_dw || !text || !text[0]) return 0.f;
    auto* layout = MakeLayout(text, size, weight);
    if (!layout) return 0.f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return m.widthIncludingTrailingWhitespace;
}

float D2DRenderer::MeasureTextA(const char* text, float size,
                                 DWRITE_FONT_WEIGHT weight)
{
    return MeasureTextW(ToWide(text).c_str(), size, weight);
}

// ─── Bitmap loading (WIC) ─────────────────────────────────────────────────────

D2DBitmap D2DRenderer::LoadBitmap(const wchar_t* path)
{
    D2DBitmap out{};
    if (!m_wic || !m_rt || !path) return out;

    IWICBitmapDecoder*     decoder  = nullptr;
    IWICBitmapFrameDecode* frame    = nullptr;
    IWICFormatConverter*   conv     = nullptr;

    HRESULT hr = m_wic->CreateDecoderFromFilename(
        path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) return out;

    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) { decoder->Release(); return out; }

    hr = m_wic->CreateFormatConverter(&conv);
    if (FAILED(hr)) { frame->Release(); decoder->Release(); return out; }

    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                          WICBitmapDitherTypeNone, nullptr, 0.f,
                          WICBitmapPaletteTypeMedianCut);
    if (SUCCEEDED(hr)) {
        ID2D1Bitmap* bmp = nullptr;
        hr = m_rt->CreateBitmapFromWicBitmap(conv, nullptr, &bmp);
        if (SUCCEEDED(hr) && bmp) {
            auto sz = bmp->GetPixelSize();
            out.bmp = bmp;
            out.w   = (int)sz.width;
            out.h   = (int)sz.height;
        }
    }

    conv->Release();
    frame->Release();
    decoder->Release();
    return out;
}

D2DBitmap D2DRenderer::LoadBitmapA(const char* path)
{
    return LoadBitmap(ToWide(path).c_str());
}

void D2DRenderer::UnloadBitmap(D2DBitmap& bmp)
{
    if (bmp.bmp) { bmp.bmp->Release(); bmp.bmp = nullptr; }
    bmp.w = bmp.h = 0;
}

void D2DRenderer::DrawBitmap(const D2DBitmap& bmp,
                              float x, float y, float w, float h, float opacity)
{
    if (!m_rt || !bmp.Valid()) return;
    m_rt->DrawBitmap(bmp.bmp,
                     D2D1::RectF(x, y, x+w, y+h),
                     opacity,
                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void D2DRenderer::DrawBitmapCropped(const D2DBitmap& bmp,
                                     float srcX, float srcY, float srcW, float srcH,
                                     float dstX, float dstY, float dstW, float dstH,
                                     float opacity)
{
    if (!m_rt || !bmp.Valid()) return;
    m_rt->DrawBitmap(bmp.bmp,
                     D2D1::RectF(dstX, dstY, dstX+dstW, dstY+dstH),
                     opacity,
                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                     D2D1::RectF(srcX, srcY, srcX+srcW, srcY+srcH));
}

// ─── Clip ─────────────────────────────────────────────────────────────────────

void D2DRenderer::PushClip(float x, float y, float w, float h)
{
    if (!m_rt) return;
    m_rt->PushAxisAlignedClip(D2D1::RectF(x, y, x+w, y+h),
                               D2D1_ANTIALIAS_MODE_ALIASED);
    m_clipDepth++;
}

void D2DRenderer::PopClip()
{
    if (!m_rt || m_clipDepth == 0) return;
    m_rt->PopAxisAlignedClip();
    m_clipDepth--;
}
