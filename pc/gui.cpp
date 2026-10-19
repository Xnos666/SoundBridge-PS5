// gui.cpp - SoundBridge for Windows: the receiver with a PS5-style window.
//
// Direct2D + DirectWrite, drawn at 30 fps: dark blue PS5 backdrop with
// drifting triangle / circle / cross / square outlines, a delay gauge, sound
// meters, mode tiles with the PS5 white focus ring, and an output picker.
// All audio work is in sb_engine.c.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "sb_engine.h"

#define APP_VERSION L"1.0"
#define IDI_APP 1
#define WM_APP_OUTPUT_DONE (WM_APP + 1)

static const float W = 460.f, H = 700.f;      // client size in DIPs
static const float PI = 3.14159265f;

template <class T> static void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

static D2D1_COLOR_F rgba(unsigned hex, float a = 1.f)
{
    return D2D1::ColorF(((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a);
}

static D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t)
{
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

// PS5 palette
static const unsigned C_BG_TOP = 0x0C1F3F, C_BG_BOTTOM = 0x03060C;
static const unsigned C_BLUE = 0x1F8FFF, C_BLUE_DEEP = 0x0A4FB8, C_WHITE = 0xFFFFFF;
static const unsigned C_TEXT2 = 0xA7B1C2, C_TEXT3 = 0x667186;
static const unsigned C_GREEN = 0x2FD3A1, C_AMBER = 0xF5B83D, C_RED = 0xFF5A6E;
static const unsigned C_TRI = 0x3FD0B7, C_CIR = 0xFF5A6E, C_CRS = 0x7FA8FF, C_SQR = 0xE78FD9;

enum Shape { TRI, CIR, CRS, SQR };

struct Particle { float x, y, size, speed, rot, spin, alpha; int shape; };

enum Hit { HIT_NONE, HIT_LOW, HIT_SMOOTH, HIT_OUTPUT };

static struct App {
    HWND hwnd = nullptr;
    float dpi = 96.f;
    ID2D1Factory *fac = nullptr;
    IDWriteFactory *dw = nullptr;
    ID2D1HwndRenderTarget *rt = nullptr;
    ID2D1SolidColorBrush *br = nullptr;
    ID2D1LinearGradientBrush *bg = nullptr, *tileSel = nullptr, *meter = nullptr;
    ID2D1RadialGradientBrush *glow1 = nullptr, *glow2 = nullptr;
    ID2D1StrokeStyle *roundCap = nullptr;
    ID2D1PathGeometry *triGeom = nullptr;
    IDWriteTextFormat *fTitle = nullptr, *fSub = nullptr, *fBig = nullptr, *fUnit = nullptr,
                      *fLabel = nullptr, *fTileTitle = nullptr, *fTileDesc = nullptr, *fDevice = nullptr,
                      *fSmall = nullptr, *fIcon = nullptr, *fPill = nullptr;

    sb_status st{};
    double shownDelay = 0;
    float lvL = 0, lvR = 0;
    float selAnim[2] = {1, 0}, hoverAnim[3] = {0, 0, 0};
    int hover = HIT_NONE;
    D2D1_RECT_F rLow{}, rSmooth{}, rOut{};
    Particle parts[18];
    double tStart = 0, tPrev = 0;
    volatile LONG outputBusy = 0;
    double lastRetry = 0;
    bool tracking = false;
} A;

static double secs()
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static float frand(float a, float b) { return a + (b - a) * (float)rand() / (float)RAND_MAX; }

// ---- resources -----------------------------------------------------------------

static IDWriteTextFormat *font(const wchar_t *family, float size, DWRITE_FONT_WEIGHT w)
{
    IDWriteTextFormat *f = nullptr;
    A.dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                           L"en-us", &f);
    if (f) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return f;
}

static void createIndependent()
{
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &A.fac);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&A.dw);

    A.fTitle = font(L"Segoe UI Variable Display", 27.f, DWRITE_FONT_WEIGHT_SEMI_LIGHT);
    A.fSub = font(L"Segoe UI", 13.f, DWRITE_FONT_WEIGHT_NORMAL);
    A.fBig = font(L"Segoe UI Variable Display", 66.f, DWRITE_FONT_WEIGHT_LIGHT);
    A.fUnit = font(L"Segoe UI", 14.f, DWRITE_FONT_WEIGHT_NORMAL);
    A.fLabel = font(L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    A.fTileTitle = font(L"Segoe UI Variable Text", 16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    A.fTileDesc = font(L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    A.fDevice = font(L"Segoe UI", 15.f, DWRITE_FONT_WEIGHT_NORMAL);
    A.fSmall = font(L"Segoe UI", 12.f, DWRITE_FONT_WEIGHT_NORMAL);
    A.fIcon = font(L"Segoe MDL2 Assets", 17.f, DWRITE_FONT_WEIGHT_NORMAL);
    A.fPill = font(L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (A.fDevice) {
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject *ellipsis = nullptr;
        A.dw->CreateEllipsisTrimmingSign(A.fDevice, &ellipsis);
        A.fDevice->SetTrimming(&trim, ellipsis);
        release(ellipsis);
    }

    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                  D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    A.fac->CreateStrokeStyle(sp, nullptr, 0, &A.roundCap);

    // unit triangle (pointing up), centred on 0,0
    A.fac->CreatePathGeometry(&A.triGeom);
    ID2D1GeometrySink *s = nullptr;
    A.triGeom->Open(&s);
    s->BeginFigure(D2D1::Point2F(0, -0.55f), D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine(D2D1::Point2F(0.5f, 0.38f));
    s->AddLine(D2D1::Point2F(-0.5f, 0.38f));
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    release(s);
}

static ID2D1LinearGradientBrush *linear(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c0, D2D1_COLOR_F c1)
{
    D2D1_GRADIENT_STOP stops[2] = {{0.f, c0}, {1.f, c1}};
    ID2D1GradientStopCollection *col = nullptr;
    ID2D1LinearGradientBrush *br = nullptr;
    A.rt->CreateGradientStopCollection(stops, 2, &col);
    A.rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), col, &br);
    release(col);
    return br;
}

static ID2D1RadialGradientBrush *radial(D2D1_COLOR_F c, float r)
{
    D2D1_GRADIENT_STOP stops[2] = {{0.f, c}, {1.f, D2D1::ColorF(c.r, c.g, c.b, 0.f)}};
    ID2D1GradientStopCollection *col = nullptr;
    ID2D1RadialGradientBrush *br = nullptr;
    A.rt->CreateGradientStopCollection(stops, 2, &col);
    A.rt->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 0), r, r), col, &br);
    release(col);
    return br;
}

static void createTarget()
{
    if (A.rt) return;
    RECT rc;
    GetClientRect(A.hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties();
    p.dpiX = p.dpiY = A.dpi;
    A.fac->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(A.hwnd, D2D1::SizeU(rc.right, rc.bottom)),
                                  &A.rt);
    if (!A.rt) return;
    A.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    A.rt->CreateSolidColorBrush(rgba(C_WHITE), &A.br);
    A.bg = linear(D2D1::Point2F(0, 0), D2D1::Point2F(0, H), rgba(C_BG_TOP), rgba(C_BG_BOTTOM));
    A.tileSel = linear(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1), rgba(0x2A8CFF), rgba(C_BLUE_DEEP));
    A.meter = linear(D2D1::Point2F(0, 0), D2D1::Point2F(1, 0), rgba(C_BLUE_DEEP), rgba(0x7CC4FF));
    A.glow1 = radial(rgba(0x0A5BD6, 0.55f), 320.f);
    A.glow2 = radial(rgba(0x3A2FBF, 0.30f), 300.f);
}

static void discardTarget()
{
    release(A.br);
    release(A.bg);
    release(A.tileSel);
    release(A.meter);
    release(A.glow1);
    release(A.glow2);
    release(A.rt);
}

// ---- drawing helpers -------------------------------------------------------------

static ID2D1SolidColorBrush *col(unsigned hex, float a = 1.f)
{
    A.br->SetColor(rgba(hex, a));
    return A.br;
}

static ID2D1SolidColorBrush *col(D2D1_COLOR_F c)
{
    A.br->SetColor(c);
    return A.br;
}

static void text(const wchar_t *s, IDWriteTextFormat *f, D2D1_RECT_F r, ID2D1Brush *b,
                 DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING)
{
    if (!f) return;
    f->SetTextAlignment(al);
    A.rt->DrawText(s, (UINT32)wcslen(s), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static float textWidth(const wchar_t *s, IDWriteTextFormat *f)
{
    IDWriteTextLayout *l = nullptr;
    DWRITE_TEXT_METRICS m{};
    if (!f) return 0;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    A.dw->CreateTextLayout(s, (UINT32)wcslen(s), f, 1000.f, 100.f, &l);
    if (!l) return 0;
    l->GetMetrics(&m);
    release(l);
    return m.widthIncludingTrailingWhitespace;
}

static void roundRect(D2D1_RECT_F r, float rad, ID2D1Brush *fill, ID2D1Brush *stroke = nullptr, float sw = 1.f)
{
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, rad, rad);
    if (fill) A.rt->FillRoundedRectangle(rr, fill);
    if (stroke) A.rt->DrawRoundedRectangle(rr, stroke, sw);
}

static D2D1_RECT_F inflate(D2D1_RECT_F r, float d) { return D2D1::RectF(r.left - d, r.top - d, r.right + d, r.bottom + d); }

static D2D1_RECT_F scaleAround(D2D1_RECT_F r, float s)
{
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, w = (r.right - r.left) * s / 2,
          h = (r.bottom - r.top) * s / 2;
    return D2D1::RectF(cx - w, cy - h, cx + w, cy + h);
}

// One PlayStation shape, outline, centred at x,y.
static void shape(int kind, float x, float y, float size, float rot, D2D1_COLOR_F c, float sw)
{
    D2D1_MATRIX_3X2_F old;
    A.rt->GetTransform(&old);
    A.rt->SetTransform(D2D1::Matrix3x2F::Rotation(rot, D2D1::Point2F(0, 0)) *
                       D2D1::Matrix3x2F::Translation(x, y) * old);
    ID2D1SolidColorBrush *b = col(c);
    float h = size / 2;
    switch (kind) {
    case TRI: {
        D2D1_MATRIX_3X2_F cur;
        A.rt->GetTransform(&cur);
        A.rt->SetTransform(D2D1::Matrix3x2F::Scale(size, size) * cur);
        A.rt->DrawGeometry(A.triGeom, b, sw / size, A.roundCap);
        break;
    }
    case CIR:
        A.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), h * 0.9f, h * 0.9f), b, sw);
        break;
    case CRS:
        A.rt->DrawLine(D2D1::Point2F(-h * 0.8f, -h * 0.8f), D2D1::Point2F(h * 0.8f, h * 0.8f), b, sw, A.roundCap);
        A.rt->DrawLine(D2D1::Point2F(h * 0.8f, -h * 0.8f), D2D1::Point2F(-h * 0.8f, h * 0.8f), b, sw, A.roundCap);
        break;
    default:
        A.rt->DrawRectangle(D2D1::RectF(-h * 0.8f, -h * 0.8f, h * 0.8f, h * 0.8f), b, sw);
        break;
    }
    A.rt->SetTransform(old);
}

static unsigned shapeColor(int k) { return k == TRI ? C_TRI : k == CIR ? C_CIR : k == CRS ? C_CRS : C_SQR; }

// Arc from angle a0 to a1 (degrees, 0 = right, clockwise on screen).
static void arc(float cx, float cy, float r, float a0, float a1, ID2D1Brush *b, float sw)
{
    if (a1 - a0 < 0.5f) return;
    ID2D1PathGeometry *g = nullptr;
    ID2D1GeometrySink *s = nullptr;
    float r0 = a0 * PI / 180, r1 = a1 * PI / 180;
    A.fac->CreatePathGeometry(&g);
    if (!g) return;
    g->Open(&s);
    s->BeginFigure(D2D1::Point2F(cx + r * cosf(r0), cy + r * sinf(r0)), D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * cosf(r1), cy + r * sinf(r1)), D2D1::SizeF(r, r), 0,
                               D2D1_SWEEP_DIRECTION_CLOCKWISE,
                               a1 - a0 > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    A.rt->DrawGeometry(g, b, sw, A.roundCap);
    release(s);
    release(g);
}

// ---- the screen -------------------------------------------------------------------

static void drawBackground(float t)
{
    A.rt->FillRectangle(D2D1::RectF(0, 0, W, H), A.bg);
    A.glow1->SetCenter(D2D1::Point2F(W * 0.82f + 30 * sinf(t * 0.21f), 70 + 25 * cosf(t * 0.17f)));
    A.rt->FillRectangle(D2D1::RectF(0, 0, W, H), A.glow1);
    A.glow2->SetCenter(D2D1::Point2F(W * 0.05f + 20 * cosf(t * 0.13f), H * 0.86f + 20 * sinf(t * 0.19f)));
    A.rt->FillRectangle(D2D1::RectF(0, 0, W, H), A.glow2);

    for (Particle &p : A.parts)
        shape(p.shape, p.x, p.y, p.size, p.rot, rgba(shapeColor(p.shape), p.alpha), 1.4f);
}

static void drawHeader(float t)
{
    // the four symbols, small, like a controller's face buttons
    static const int order[4] = {TRI, CIR, CRS, SQR};
    for (int i = 0; i < 4; i++)
        shape(order[i], 36.f + i * 17.f, 34.f, 11.f, 0, rgba(shapeColor(order[i]), 0.95f), 1.6f);

    text(L"SoundBridge", A.fTitle, D2D1::RectF(28, 46, 300, 82), col(C_WHITE));
    text(L"PS5 game sound on this PC", A.fSub, D2D1::RectF(29, 80, 300, 100), col(C_TEXT2));

    // status pill
    const sb_status &s = A.st;
    wchar_t msg[96];
    unsigned dot;
    bool pulse = false;
    if (!s.out_ok) { swprintf(msg, 96, L"Sound output unavailable"); dot = C_RED; }
    else if (s.live) { swprintf(msg, 96, L"Connected  ·  %hs", s.ip); dot = C_GREEN; }
    else if (s.known) { swprintf(msg, 96, L"PS5 found  ·  no sound yet"); dot = C_AMBER; pulse = true; }
    else { swprintf(msg, 96, L"Searching for PS5"); dot = C_AMBER; pulse = true; }
    float tw = textWidth(msg, A.fPill), pw = tw + 40, px = W - 28 - pw;
    D2D1_RECT_F pr = D2D1::RectF(px, 40, W - 28, 68);
    roundRect(pr, 14, col(C_WHITE, 0.07f), col(C_WHITE, 0.12f), 1);
    float a = pulse ? 0.45f + 0.55f * (0.5f + 0.5f * sinf(t * 4.f)) : 1.f;
    A.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px + 16, 54), 7, 7), col(dot, 0.18f * a));
    A.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px + 16, 54), 4, 4), col(dot, a));
    text(msg, A.fPill, D2D1::RectF(px + 28, 40, W - 28, 68), col(C_WHITE, 0.92f));
}

static void drawGauge(float t)
{
    const sb_status &s = A.st;
    const float cx = W / 2, cy = 236, r = 96;
    const float a0 = 135, sweep = 270;

    // card
    roundRect(D2D1::RectF(28, 118, W - 28, 370), 18, col(C_WHITE, 0.045f), col(C_WHITE, 0.08f), 1);

    arc(cx, cy, r, a0, a0 + sweep, col(C_WHITE, 0.08f), 10);
    for (int i = 0; i <= 8; i++) {                  // a tick every 25 ms, 0..200
        float a = (a0 + sweep * i / 8.f) * PI / 180, r1 = r + 13, r2 = r + (i % 4 == 0 ? 20.f : 17.f);
        A.rt->DrawLine(D2D1::Point2F(cx + r1 * cosf(a), cy + r1 * sinf(a)),
                       D2D1::Point2F(cx + r2 * cosf(a), cy + r2 * sinf(a)), col(C_WHITE, 0.22f), 1.5f, A.roundCap);
    }
    if (s.live) {
        float frac = (float)(A.shownDelay / 200.0);
        if (frac > 1) frac = 1;
        if (frac < 0.02f) frac = 0.02f;
        float warm = (float)((A.shownDelay - 90.0) / 60.0);
        warm = warm < 0 ? 0 : warm > 1 ? 1 : warm;
        D2D1_COLOR_F c = mix(rgba(C_BLUE), rgba(C_AMBER), warm);
        arc(cx, cy, r, a0, a0 + sweep * frac, col(D2D1::ColorF(c.r, c.g, c.b, 0.22f)), 22);
        arc(cx, cy, r, a0, a0 + sweep * frac, col(c), 10);

        wchar_t num[16];
        swprintf(num, 16, L"%.0f", A.shownDelay);
        text(num, A.fBig, D2D1::RectF(cx - 100, cy - 52, cx + 100, cy + 26), col(C_WHITE),
             DWRITE_TEXT_ALIGNMENT_CENTER);
        text(L"ms delay", A.fUnit, D2D1::RectF(cx - 100, cy + 26, cx + 100, cy + 48), col(C_TEXT2),
             DWRITE_TEXT_ALIGNMENT_CENTER);

        wchar_t parts[96];
        swprintf(parts, 96, L"Buffer %.0f  ·  Output %.0f  ·  Network %.0f ms", s.buffer_ms, s.out_ms,
                 s.net_ms);
        text(parts, A.fSmall, D2D1::RectF(28, 338, W - 28, 358), col(C_TEXT2), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        // searching: a short arc chasing round the ring
        float head = fmodf(t * 220.f, 360.f);
        arc(cx, cy, r, head, head + 70, col(C_BLUE, 0.25f), 22);
        arc(cx, cy, r, head, head + 70, col(C_BLUE), 10);
        text(L"—", A.fBig, D2D1::RectF(cx - 100, cy - 52, cx + 100, cy + 26), col(C_WHITE, 0.6f),
             DWRITE_TEXT_ALIGNMENT_CENTER);
        text(s.known ? L"waiting for sound" : L"looking for your PS5", A.fUnit,
             D2D1::RectF(cx - 120, cy + 26, cx + 120, cy + 48), col(C_TEXT2), DWRITE_TEXT_ALIGNMENT_CENTER);
        text(L"Send SoundBridge_PS5 to the PS5 if it isn't running", A.fSmall,
             D2D1::RectF(28, 338, W - 28, 358), col(C_TEXT3), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static float meterValue(float peak)
{
    if (peak < 0.001f) return 0;
    float db = 20.f * log10f(peak);
    float v = (db + 50.f) / 50.f;
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static void drawMeters()
{
    const float x = 58, w = W - 28 - 14 - x, y0 = 392;
    text(L"L", A.fLabel, D2D1::RectF(36, y0 - 8, 52, y0 + 8), col(C_TEXT3));
    text(L"R", A.fLabel, D2D1::RectF(36, y0 + 8, 52, y0 + 24), col(C_TEXT3));
    float v[2] = {meterValue(A.lvL), meterValue(A.lvR)};
    for (int i = 0; i < 2; i++) {
        float y = y0 - 3 + i * 16;
        roundRect(D2D1::RectF(x, y, x + w, y + 6), 3, col(C_WHITE, 0.08f));
        if (v[i] > 0.01f) {
            A.meter->SetStartPoint(D2D1::Point2F(x, 0));
            A.meter->SetEndPoint(D2D1::Point2F(x + w, 0));
            roundRect(D2D1::RectF(x, y, x + w * v[i], y + 6), 3, A.meter);
        }
    }
}

static void drawTile(int idx, D2D1_RECT_F r, const wchar_t *title, const wchar_t *desc, int sym)
{
    float sel = A.selAnim[idx], hov = A.hoverAnim[idx];
    D2D1_RECT_F rr = scaleAround(r, 1.f + 0.025f * sel);

    if (sel > 0.01f) {
        A.tileSel->SetStartPoint(D2D1::Point2F(0, rr.top));
        A.tileSel->SetEndPoint(D2D1::Point2F(0, rr.bottom));
        A.tileSel->SetOpacity(sel);
        roundRect(rr, 14, A.tileSel);
        A.tileSel->SetOpacity(1);
    }
    roundRect(rr, 14, col(C_WHITE, (0.06f + 0.06f * hov) * (1 - sel)), col(C_WHITE, 0.09f * (1 - sel)), 1);
    if (sel > 0.01f) {
        // PS5 focus ring: soft halo, then a crisp white line
        roundRect(inflate(rr, 4), 17, nullptr, col(C_WHITE, 0.10f * sel), 6);
        roundRect(inflate(rr, 2.5f), 16, nullptr, col(C_WHITE, 0.95f * sel), 2);
    }
    shape(sym, rr.right - 22, rr.top + 22, 13, 0,
          sel > 0.5f ? rgba(C_WHITE, 0.95f) : rgba(shapeColor(sym), 0.9f), 1.8f);
    text(title, A.fTileTitle, D2D1::RectF(rr.left + 16, rr.top + 14, rr.right - 36, rr.top + 40), col(C_WHITE));
    text(desc, A.fTileDesc, D2D1::RectF(rr.left + 16, rr.top + 40, rr.right - 10, rr.top + 60),
         col(sel > 0.5f ? 0xDCE8FF : C_TEXT2));
}

static void drawModes()
{
    text(L"MODE", A.fLabel, D2D1::RectF(30, 424, 200, 440), col(C_TEXT3));
    float tw = (W - 56 - 14) / 2;
    A.rLow = D2D1::RectF(28, 446, 28 + tw, 520);
    A.rSmooth = D2D1::RectF(28 + tw + 14, 446, W - 28, 520);
    drawTile(0, A.rLow, L"Low delay", L"Fastest  ·  short gaps on Wi-Fi", CRS);
    drawTile(1, A.rSmooth, L"Smooth", L"No gaps  ·  more delay", CIR);
}

static void drawOutput()
{
    text(L"OUTPUT", A.fLabel, D2D1::RectF(30, 538, 200, 554), col(C_TEXT3));
    A.rOut = D2D1::RectF(28, 560, W - 28, 616);
    float hov = A.hoverAnim[2];
    roundRect(A.rOut, 14, col(C_WHITE, 0.06f + 0.06f * hov), col(C_WHITE, 0.09f + 0.25f * hov), 1);
    text(L"", A.fIcon, D2D1::RectF(44, 560, 70, 616), col(C_BLUE));    // headphone
    const wchar_t *dev = A.st.device[0] ? A.st.device : L"No output - click to choose";
    text(dev, A.fDevice, D2D1::RectF(78, 560, W - 64, 616), col(A.st.device[0] ? C_WHITE : C_RED));
    text(L"", A.fIcon, D2D1::RectF(W - 60, 560, W - 40, 616), col(C_TEXT2));    // chevron
}

static void drawFooter()
{
    const sb_status &s = A.st;
    wchar_t line[160];
    text(L"", A.fIcon, D2D1::RectF(30, 628, 52, 652), col(C_TEXT3));          // wi-fi
    if (s.live)
        swprintf(line, 160, L"Wi-Fi pauses up to %.0f ms  ·  %u gap%s", s.wifi_pause_ms, s.gaps,
                 s.gaps == 1 ? L"" : L"s");
    else
        swprintf(line, 160, L"Waiting for the PS5");
    text(line, A.fSmall, D2D1::RectF(56, 628, W - 28, 652), col(C_TEXT2));
    if (s.live && s.wifi_pause_ms > 45 && s.low)
        text(L"An Ethernet cable on the PS5 removes the gaps", A.fSmall, D2D1::RectF(56, 648, W - 28, 670),
             col(C_TEXT3));
    text(L"← →  switch mode   ·   close to stop", A.fSmall, D2D1::RectF(28, 672, W - 28, 694),
         col(C_TEXT3, 0.8f));
    text(L"v" APP_VERSION, A.fSmall, D2D1::RectF(28, 672, W - 28, 694), col(C_TEXT3, 0.8f),
         DWRITE_TEXT_ALIGNMENT_TRAILING);
}

static void update()
{
    double now = secs();
    float dt = (float)(now - A.tPrev);
    if (dt > 0.2f) dt = 0.2f;
    A.tPrev = now;

    sb_status_get(&A.st);
    double target = A.st.live ? A.st.delay_ms : 0;
    A.shownDelay += (target - A.shownDelay) * (1.0 - exp(-dt * 6.0));

    // meters: fast attack, ~300 ms fall
    float fall = expf(-dt / 0.3f);
    A.lvL = A.st.level_l > A.lvL * fall ? A.st.level_l : A.lvL * fall;
    A.lvR = A.st.level_r > A.lvR * fall ? A.st.level_r : A.lvR * fall;

    float k = 1.f - expf(-dt * 12.f);
    A.selAnim[0] += ((A.st.low ? 1.f : 0.f) - A.selAnim[0]) * k;
    A.selAnim[1] += ((A.st.low ? 0.f : 1.f) - A.selAnim[1]) * k;
    for (int i = 0; i < 3; i++) A.hoverAnim[i] += ((A.hover == i + 1 ? 1.f : 0.f) - A.hoverAnim[i]) * k;

    for (Particle &p : A.parts) {
        p.y -= p.speed * dt;
        p.rot += p.spin * dt;
        if (p.y < -40) {
            p.y = H + 40;
            p.x = frand(10, W - 10);
        }
    }
}

static void paint()
{
    createTarget();
    if (!A.rt) return;
    float t = (float)(secs() - A.tStart);
    A.rt->BeginDraw();
    A.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    drawBackground(t);
    drawHeader(t);
    drawGauge(t);
    drawMeters();
    drawModes();
    drawOutput();
    drawFooter();
    if (A.rt->EndDraw() == D2DERR_RECREATE_TARGET) discardTarget();
}

// ---- actions ------------------------------------------------------------------------

static DWORD WINAPI outputWorker(LPVOID arg)
{
    wchar_t *want = (wchar_t *)arg;
    wchar_t picked[256];
    sb_output_open(want && want[0] ? want : nullptr, picked, 256);
    free(want);
    InterlockedExchange(&A.outputBusy, 0);
    return 0;
}

static void openOutput(const wchar_t *want)
{
    if (InterlockedExchange(&A.outputBusy, 1)) return;
    wchar_t *copy = (wchar_t *)calloc(256, sizeof(wchar_t));
    if (want) wcsncpy(copy, want, 255);
    HANDLE h = CreateThread(nullptr, 0, outputWorker, copy, 0, nullptr);
    if (h) CloseHandle(h);
    else { free(copy); InterlockedExchange(&A.outputBusy, 0); }
}

static void chooseOutput()
{
    static wchar_t names[SB_MAX_DEVICES][256];
    int n = sb_device_list(names, SB_MAX_DEVICES);
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < n; i++)
        AppendMenuW(m, MF_STRING | (wcscmp(names[i], A.st.device) == 0 ? MF_CHECKED : 0), 100 + i, names[i]);
    if (!n) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No sound outputs found");
    POINT pt = {(LONG)(A.rOut.left * A.dpi / 96.f), (LONG)(A.rOut.bottom * A.dpi / 96.f + 4)};
    ClientToScreen(A.hwnd, &pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, A.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 100 && cmd < 100 + n) openOutput(names[cmd - 100]);
}

static int hitTest(int px, int py)
{
    float x = px * 96.f / A.dpi, y = py * 96.f / A.dpi;
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
    if (in(A.rLow)) return HIT_LOW;
    if (in(A.rSmooth)) return HIT_SMOOTH;
    if (in(A.rOut)) return HIT_OUTPUT;
    return HIT_NONE;
}

static void click(int hit)
{
    if (hit == HIT_LOW) sb_set_low(1);
    else if (hit == HIT_SMOOTH) sb_set_low(0);
    else if (hit == HIT_OUTPUT) chooseOutput();
}

// ---- window ---------------------------------------------------------------------------

static void sizeWindow(HWND h, UINT dpi)
{
    RECT r = {0, 0, (LONG)ceilf(W * dpi / 96.f), (LONG)ceilf(H * dpi / 96.f)};
    DWORD style = (DWORD)GetWindowLongPtrW(h, GWL_STYLE);
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (!IsIconic(h)) {
            update();
            InvalidateRect(h, nullptr, FALSE);
        } else {
            sb_status_get(&A.st);
        }
        if (!A.st.out_ok && !A.outputBusy && secs() - A.lastRetry > 3.0) {
            // output went away (unplugged?) or never opened: try again
            A.lastRetry = secs();
            wchar_t saved[256] = L"";
            sb_saved_device(saved, 256);
            openOutput(saved);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (A.rt) A.rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_DPICHANGED: {
        A.dpi = (float)HIWORD(wp);
        RECT *r = (RECT *)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        discardTarget();
        sizeWindow(h, (UINT)A.dpi);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!A.tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            A.tracking = true;
        }
        A.hover = hitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_MOUSELEAVE:
        A.tracking = false;
        A.hover = HIT_NONE;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && A.hover != HIT_NONE) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
        click(hitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_LEFT) sb_set_low(1);
        else if (wp == VK_RIGHT) sb_set_low(0);
        else if (wp == VK_RETURN || wp == VK_SPACE) chooseOutput();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Dark popup menus (undocumented uxtheme ordinals; harmless if missing).
static void darkMenus()
{
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    typedef int(WINAPI * SetPreferredAppMode)(int);
    typedef void(WINAPI * FlushMenuThemes)(void);
    auto setMode = (SetPreferredAppMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemes)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(2);    // force dark
    if (flush) flush();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    HANDLE one = CreateMutexW(nullptr, TRUE, L"SoundBridgeReceiverGUI");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"SoundBridgeWindow", nullptr);
        if (other) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    (void)one;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    darkMenus();
    srand((unsigned)GetTickCount());

    if (sb_engine_start(nullptr) != 0) {
        MessageBoxW(nullptr, L"SoundBridge could not start networking.", L"SoundBridge", MB_ICONERROR);
        return 1;
    }
    wchar_t saved[256] = L"";
    sb_saved_device(saved, 256);
    openOutput(saved);

    createIndependent();
    for (Particle &p : A.parts) {
        p.x = frand(10, W - 10);
        p.y = frand(0, H);
        p.size = frand(14, 38);
        p.speed = frand(4, 12);
        p.rot = frand(0, 360);
        p.spin = frand(-8, 8);
        p.alpha = frand(0.05f, 0.13f);
        p.shape = rand() % 4;
    }
    A.tStart = A.tPrev = secs();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"SoundBridgeWindow";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    A.hwnd = CreateWindowExW(0, wc.lpszClassName, L"SoundBridge", style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                             nullptr, nullptr, inst, nullptr);
    A.dpi = (float)GetDpiForWindow(A.hwnd);
    sizeWindow(A.hwnd, (UINT)A.dpi);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(A.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    COLORREF cap = RGB((C_BG_TOP >> 16) & 255, (C_BG_TOP >> 8) & 255, C_BG_TOP & 255);
    DwmSetWindowAttribute(A.hwnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof cap);
    COLORREF capText = RGB(230, 236, 245);
    DwmSetWindowAttribute(A.hwnd, 36 /* DWMWA_TEXT_COLOR */, &capText, sizeof capText);

    ShowWindow(A.hwnd, show);
    UpdateWindow(A.hwnd);
    SetTimer(A.hwnd, 1, 33, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ShowWindow(A.hwnd, SW_HIDE);
    sb_engine_stop();
    return 0;
}
