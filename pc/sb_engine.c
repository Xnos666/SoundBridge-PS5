/* sb_engine.c - SoundBridge receiver engine: plays the PS5's game sound
 * (sent by the SoundBridge payload over UDP) on a PC output, with as little
 * buffering as the network allows.
 *
 *   net thread   UDP -> ring buffer (48 kHz stereo float)
 *   hello thread HELLO to the PS5 every 500 ms (broadcast until found)
 *   audio thread WASAPI shared mode, smallest engine period (IAudioClient3),
 *                resampling from the ring with a small speed adjustment that
 *                keeps the lowest buffer level just above the margin. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <timeapi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "sb_engine.h"
#include "sb_proto.h"

#define RX_VERSION 2
#define RING_FRAMES 65536              /* 1.36 s, power of two */

DEFINE_GUID(SB_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(SB_IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(SB_IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(SB_IID_IAudioClient3, 0x7ED4EE07, 0x8E67, 0x4CD4, 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42);
DEFINE_GUID(SB_IID_IAudioRenderClient, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(SB_SUBTYPE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
static const PROPERTYKEY SB_PKEY_FriendlyName =
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

/* ---- shared state ------------------------------------------------------ */

static volatile LONG g_quit;
static SOCKET g_sock = INVALID_SOCKET;
static FILE *g_log;
static char g_ini[MAX_PATH];
static HANDLE g_net_thr, g_hello_thr;

static CRITICAL_SECTION g_mx;          /* guards everything below */
static float g_ring[RING_FRAMES * 2];
static unsigned long long g_wr, g_rd;  /* frame counters, ring index = & (RING_FRAMES-1) */
static struct sockaddr_in g_ps5;       /* where HELLO goes once known */
static int g_ps5_known, g_ps5_fixed;
static long long g_last_audio_ms;
static unsigned g_seq_next, g_lost, g_late, g_pkts;
static int g_have_seq;
static double g_rtt_ms = -1;
static char g_ps5_stats[256];
static float g_peak_l, g_peak_r;       /* since the last sb_status_get() */

/* audio-thread figures for the status */
static double g_out_ms;                /* output device latency */
static double g_fill_avg_ms, g_margin_ms = 4.0;
static unsigned g_underruns, g_drops;
static int g_playing;
static double g_margin_floor = 2.0;
static volatile int g_low = 1;         /* low delay; 0 = smooth (buffers for Wi-Fi pauses) */
#define LONG_GAP_MS 36.0               /* longer waits than this are "Wi-Fi pauses" */
static double g_gap_sec, g_gap_short_sec; /* longest wait between packets this second */
static double g_gap_max60;             /* longest wait in the last minute */

/* settings (SoundBridge_Receiver.ini: ps5=, mode=, device=) */
static char g_set_ps5[32];
static int g_set_low = -1;
static wchar_t g_set_device[256];

/* output */
static HANDLE g_audio_thr, g_open_done;
static volatile LONG g_out_stop;
static volatile int g_out_ok;
static wchar_t g_out_want[256], g_out_name[256];

static long long now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (long long)(c.QuadPart * 1000 / f.QuadPart);
}

static unsigned now_us32(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (unsigned)(c.QuadPart * 1000000 / f.QuadPart);
}

void sb_log(const char *fmt, ...)
{
    va_list va;
    if (!g_log) return;
    fprintf(g_log, "[%lld] ", now_ms());
    va_start(va, fmt);
    vfprintf(g_log, fmt, va);
    va_end(va);
    fputc('\n', g_log);
    fflush(g_log);
}
#define logf_ sb_log

/* ---- settings --------------------------------------------------------------- */

static void settings_load(void)
{
    FILE *f = fopen(g_ini, "r");
    char line[600];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        line[strcspn(line, "\r\n")] = 0;
        if (!eq) {                                  /* 0.1.x: just the PS5 address */
            if (line[0]) snprintf(g_set_ps5, sizeof g_set_ps5, "%s", line);
            continue;
        }
        *eq++ = 0;
        if (!strcmp(line, "ps5")) snprintf(g_set_ps5, sizeof g_set_ps5, "%s", eq);
        else if (!strcmp(line, "mode")) g_set_low = strcmp(eq, "smooth") != 0;
        else if (!strcmp(line, "device")) MultiByteToWideChar(CP_UTF8, 0, eq, -1, g_set_device, 255);
    }
    fclose(f);
}

static void settings_save(void)
{
    FILE *f = fopen(g_ini, "w");
    char dev[600] = "";
    if (!f) return;
    WideCharToMultiByte(CP_UTF8, 0, g_set_device, -1, dev, sizeof dev, NULL, NULL);
    fprintf(f, "ps5=%s\nmode=%s\ndevice=%s\n", g_set_ps5, g_set_low == 0 ? "smooth" : "low", dev);
    fclose(f);
}

int sb_saved_low(int fallback) { return g_set_low < 0 ? fallback : g_set_low; }

int sb_saved_device(wchar_t *out, size_t cap)
{
    if (!g_set_device[0]) return 0;
    wcsncpy(out, g_set_device, cap - 1);
    out[cap - 1] = 0;
    return 1;
}

/* ---- network ------------------------------------------------------------ */

static void save_ps5_ip(const struct in_addr *a)
{
    inet_ntop(AF_INET, a, g_set_ps5, sizeof g_set_ps5);
    settings_save();
}

static void send_hello_to(const struct sockaddr_in *to)
{
    sb_hello h;
    h.magic = SB_MAGIC_HELLO;
    h.pc_time_us = now_us32();
    h.version = RX_VERSION;
    sendto(g_sock, (const char *)&h, sizeof h, 0, (const struct sockaddr *)to, sizeof *to);
}

/* Until the PS5 answers: limited broadcast, every adapter's subnet broadcast
 * and the address that worked last time. */
static void send_hello_search(void)
{
    struct sockaddr_in to;
    ULONG sz = 16384;
    IP_ADAPTER_ADDRESSES *aa = malloc(sz), *a;
    struct in_addr last;

    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(SB_PORT);
    to.sin_addr.s_addr = INADDR_BROADCAST;
    send_hello_to(&to);
    if (g_set_ps5[0] && inet_pton(AF_INET, g_set_ps5, &last) == 1) { to.sin_addr = last; send_hello_to(&to); }
    if (aa && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                   GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz) == NO_ERROR) {
        for (a = aa; a; a = a->Next) {
            IP_ADAPTER_UNICAST_ADDRESS *u;
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (u = a->FirstUnicastAddress; u; u = u->Next) {
                struct sockaddr_in *sa = (struct sockaddr_in *)u->Address.lpSockaddr;
                ULONG mask;
                if (sa->sin_family != AF_INET || u->OnLinkPrefixLength >= 31) continue;
                mask = u->OnLinkPrefixLength ? htonl(~0u << (32 - u->OnLinkPrefixLength)) : 0;
                to.sin_addr.s_addr = sa->sin_addr.s_addr | ~mask;
                send_hello_to(&to);
            }
        }
    }
    free(aa);
}

static DWORD WINAPI hello_thread(LPVOID arg)
{
    (void)arg;
    while (!g_quit) {
        struct sockaddr_in to;
        int known;
        long long silent;
        EnterCriticalSection(&g_mx);
        known = g_ps5_known;
        to = g_ps5;
        silent = now_ms() - g_last_audio_ms;
        LeaveCriticalSection(&g_mx);
        if (known) send_hello_to(&to);
        if (!g_ps5_fixed && (!known || silent > 3000)) send_hello_search();
        Sleep(500);
    }
    return 0;
}

static DWORD WINAPI net_thread(LPVOID arg)
{
    static unsigned char buf[2048];
    (void)arg;
    while (!g_quit) {
        struct sockaddr_in from;
        int fl = sizeof from, n;
        unsigned magic;

        n = recvfrom(g_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 4) continue;                       /* timeout or junk */
        memcpy(&magic, buf, 4);

        if (magic == SB_MAGIC_AUDIO && n >= (int)sizeof(sb_audio_hdr)) {
            sb_audio_hdr h;
            const short *pcm = (const short *)(buf + sizeof h);
            unsigned i, frames;
            memcpy(&h, buf, sizeof h);
            frames = h.frames;
            if (h.channels != 2 || sizeof h + frames * 4 > (unsigned)n) continue;

            EnterCriticalSection(&g_mx);
            if (!g_ps5_known || g_ps5.sin_addr.s_addr != from.sin_addr.s_addr) {
                char ip[32];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                logf_("PS5 found at %s", ip);
                g_ps5 = from;
                g_ps5.sin_port = htons(SB_PORT);
                g_ps5_known = 1;
                save_ps5_ip(&from.sin_addr);
                g_have_seq = 0;
            }
            {
                /* late-arrival diagnostics: was the pause on the PS5 or on the way? */
                static long long last_arr;
                static unsigned last_ps5;
                long long t = now_ms();
                if (last_arr) {
                    /* a pause the PS5 itself took (capture restart, payload
                     * resent) is not a network pause: count only the excess */
                    int ps5_gap = (int)(h.ps5_ms - last_ps5);
                    double gap = (double)(t - last_arr) - (ps5_gap > 30 ? ps5_gap - 30 : 0);
                    if (gap > g_gap_sec) g_gap_sec = gap;
                    if (gap <= LONG_GAP_MS && gap > g_gap_short_sec) g_gap_short_sec = gap;
                    if (gap > LONG_GAP_MS)
                        logf_("gap: %lld ms between packets here, %d ms between them on the PS5",
                              t - last_arr, (int)(h.ps5_ms - last_ps5));
                }
                last_arr = t;
                last_ps5 = h.ps5_ms;
            }
            g_last_audio_ms = now_ms();
            if (g_have_seq && (int)(h.seq - g_seq_next) < 0) {
                if ((int)(g_seq_next - h.seq) > 400) {
                    /* far behind: the payload was restarted and counts from 0 again */
                    logf_("PS5 stream restarted (seq %u, expected %u)", h.seq, g_seq_next);
                    g_have_seq = 0;
                } else {
                    g_late++;                          /* duplicate or out of order: drop */
                    LeaveCriticalSection(&g_mx);
                    continue;
                }
            }
            if (g_have_seq && h.seq != g_seq_next) g_lost += h.seq - g_seq_next;
            g_have_seq = 1;
            g_seq_next = h.seq + 1;
            g_pkts++;
            if (g_wr - g_rd + frames > RING_FRAMES) g_rd = g_wr + frames - RING_FRAMES;
            for (i = 0; i < frames; i++) {
                size_t k = (size_t)((g_wr + i) & (RING_FRAMES - 1)) * 2;
                float l = pcm[i * 2] / 32768.f, r = pcm[i * 2 + 1] / 32768.f;
                g_ring[k] = l;
                g_ring[k + 1] = r;
                if (fabsf(l) > g_peak_l) g_peak_l = fabsf(l);
                if (fabsf(r) > g_peak_r) g_peak_r = fabsf(r);
            }
            g_wr += frames;
            LeaveCriticalSection(&g_mx);
        } else if (magic == SB_MAGIC_PONG && n >= (int)sizeof(sb_pong)) {
            sb_pong p;
            memcpy(&p, buf, sizeof p);
            EnterCriticalSection(&g_mx);
            g_rtt_ms = (double)(unsigned)(now_us32() - p.pc_time_us) / 1000.0;
            if (!g_ps5_known) {
                g_ps5 = from;
                g_ps5.sin_port = htons(SB_PORT);
                g_ps5_known = 1;
                save_ps5_ip(&from.sin_addr);
            }
            LeaveCriticalSection(&g_mx);
        } else if (magic == SB_MAGIC_STATS) {
            int len = n - 4 < (int)sizeof g_ps5_stats - 1 ? n - 4 : (int)sizeof g_ps5_stats - 1;
            EnterCriticalSection(&g_mx);
            memcpy(g_ps5_stats, buf + 4, (size_t)len);
            g_ps5_stats[len] = 0;
            LeaveCriticalSection(&g_mx);
        }
    }
    return 0;
}

/* ---- audio output --------------------------------------------------------- */

static int device_name(IMMDevice *d, wchar_t *nm, size_t cap)
{
    IPropertyStore *ps = NULL;
    PROPVARIANT v;
    int ok = 0;
    PropVariantInit(&v);
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) &&
        SUCCEEDED(IPropertyStore_GetValue(ps, &SB_PKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
        wcsncpy(nm, v.pwszVal, cap - 1);
        nm[cap - 1] = 0;
        ok = 1;
    }
    PropVariantClear(&v);
    if (ps) IPropertyStore_Release(ps);
    return ok;
}

static IMMDeviceEnumerator *enumerator(void)
{
    IMMDeviceEnumerator *en = NULL;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;                                      /* S_FALSE / changed mode: COM is up anyway */
    if (FAILED(CoCreateInstance(&SB_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &SB_IID_IMMDeviceEnumerator,
                                (void **)&en)))
        return NULL;
    return en;
}

int sb_device_list(wchar_t names[][256], int max)
{
    IMMDeviceEnumerator *en = enumerator();
    IMMDeviceCollection *col = NULL;
    UINT n = 0, i;
    int got = 0;

    if (!en) return 0;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) {
        IMMDeviceCollection_GetCount(col, &n);
        for (i = 0; i < n && got < max; i++) {
            IMMDevice *d = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
            if (device_name(d, names[got], 256)) got++;
            IMMDevice_Release(d);
        }
        IMMDeviceCollection_Release(col);
    }
    IMMDeviceEnumerator_Release(en);
    return got;
}

/* want: exact name, else lower-case part of a name; NULL: Scarlett, else default */
static IMMDevice *pick_device(IMMDeviceEnumerator *en, const wchar_t *want, wchar_t *name, size_t cap)
{
    IMMDeviceCollection *col = NULL;
    IMMDevice *exact = NULL, *part = NULL, *scarlett = NULL, *best;
    wchar_t pn[256] = L"", sn[256] = L"";
    UINT n = 0, i;

    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) return NULL;
    IMMDeviceCollection_GetCount(col, &n);
    for (i = 0; i < n; i++) {
        IMMDevice *d = NULL;
        wchar_t nm[256] = L"?", low[256];
        if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
        device_name(d, nm, 256);
        wcscpy(low, nm);
        _wcslwr(low);
        if (want && !exact && !wcscmp(nm, want)) {
            exact = d;
            wcsncpy(name, nm, cap - 1);
            continue;
        }
        if (want && !part && wcsstr(low, want)) { part = d; wcscpy(pn, nm); continue; }
        if (!scarlett && wcsstr(nm, L"Scarlett")) { scarlett = d; wcscpy(sn, nm); continue; }
        IMMDevice_Release(d);
    }
    IMMDeviceCollection_Release(col);
    if (exact) {
        if (part) IMMDevice_Release(part);
        if (scarlett) IMMDevice_Release(scarlett);
        return exact;
    }
    if (part) {
        if (scarlett) IMMDevice_Release(scarlett);
        wcsncpy(name, pn, cap - 1);
        return part;
    }
    if (scarlett) {
        wcsncpy(name, sn, cap - 1);
        return scarlett;
    }
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &best))) {
        device_name(best, name, cap);
        return best;
    }
    return NULL;
}

typedef struct {
    IAudioClient *ac;
    IAudioRenderClient *rc;
    HANDLE ev;
    UINT32 buf_frames, period_frames;
    unsigned rate, ch, is_float, bits;
} out_dev;

static void close_output(out_dev *o)
{
    if (o->rc) IAudioRenderClient_Release(o->rc);
    if (o->ac) IAudioClient_Release(o->ac);
    if (o->ev) CloseHandle(o->ev);
    memset(o, 0, sizeof *o);
}

static int open_output(IMMDevice *dev, out_dev *o)
{
    WAVEFORMATEX *mf = NULL;
    IAudioClient3 *ac3 = NULL;
    HRESULT hr;
    REFERENCE_TIME lat = 0;

    memset(o, 0, sizeof *o);
    o->ev = CreateEventW(NULL, FALSE, FALSE, NULL);

    if (SUCCEEDED(IMMDevice_Activate(dev, &SB_IID_IAudioClient3, CLSCTX_ALL, NULL, (void **)&ac3))) {
        UINT32 def = 0, fund = 0, mn = 0, mx = 0;
        IAudioClient3_GetMixFormat(ac3, &mf);
        if (SUCCEEDED(IAudioClient3_GetSharedModeEnginePeriod(ac3, mf, &def, &fund, &mn, &mx))) {
            hr = IAudioClient3_InitializeSharedAudioStream(ac3, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mf, NULL);
            logf_("IAudioClient3: periods default %u fundamental %u min %u max %u frames; init(min) hr=%08lx",
                  def, fund, mn, mx, (unsigned long)hr);
            if (SUCCEEDED(hr)) {
                o->ac = (IAudioClient *)ac3;
                o->period_frames = mn;
            }
        }
        if (!o->ac) { IAudioClient3_Release(ac3); CoTaskMemFree(mf); mf = NULL; }
    }
    if (!o->ac) {
        if (FAILED(IMMDevice_Activate(dev, &SB_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&o->ac))) {
            close_output(o);
            return 0;
        }
        IAudioClient_GetMixFormat(o->ac, &mf);
        hr = IAudioClient_Initialize(o->ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     0, 0, mf, NULL);
        logf_("IAudioClient (fallback) init hr=%08lx", (unsigned long)hr);
        if (FAILED(hr)) {
            CoTaskMemFree(mf);
            close_output(o);
            return 0;
        }
    }
    o->rate = mf->nSamplesPerSec;
    o->ch = mf->nChannels;
    o->bits = mf->wBitsPerSample;
    o->is_float = mf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                  (mf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                   IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)mf)->SubFormat, &SB_SUBTYPE_FLOAT));
    CoTaskMemFree(mf);
    IAudioClient_SetEventHandle(o->ac, o->ev);
    IAudioClient_GetBufferSize(o->ac, &o->buf_frames);
    if (!o->period_frames) o->period_frames = o->buf_frames / 2;
    IAudioClient_GetStreamLatency(o->ac, &lat);
    if (FAILED(IAudioClient_GetService(o->ac, &SB_IID_IAudioRenderClient, (void **)&o->rc))) {
        close_output(o);
        return 0;
    }
    g_out_ms = (double)lat / 10000.0 + (double)o->buf_frames * 1000.0 / o->rate;
    logf_("output: %u Hz, %u ch, %u-bit %s, buffer %u frames, period %u frames, stream latency %.1f ms",
          o->rate, o->ch, o->bits, o->is_float ? "float" : "int", o->buf_frames, o->period_frames,
          (double)lat / 10000.0);
    return 1;
}

/* Once a second: remember the longest wait between packets, and set the
 * margin (lowest buffer level we aim for) from the last minute of waits.
 * Packets come in ~21 ms bursts, so a wait of G ms needs G - 21 ms left in
 * the buffer when it starts, plus a little. Low-delay mode ignores long waits. */
static void margin_tick(void)
{
    static double hist[60], hist_short[60];
    static int hi;
    double mx = 0, mx_short = 0, want;
    int i;

    hist[hi] = g_gap_sec;
    hist_short[hi] = g_gap_short_sec;
    hi = (hi + 1) % 60;
    g_gap_sec = g_gap_short_sec = 0;
    for (i = 0; i < 60; i++) {
        if (hist[i] > mx) mx = hist[i];
        if (hist_short[i] > mx_short) mx_short = hist_short[i];
    }
    g_gap_max60 = mx;
    want = (g_low ? mx_short : mx) - 18.0;
    if (want < g_margin_floor) want = g_margin_floor;
    if (want > 150.0) want = 150.0;
    if (want > g_margin_ms) g_margin_ms = want;
    else g_margin_ms -= (g_margin_ms - want) < 1.0 ? (g_margin_ms - want) : 1.0;
}

/* Fill `frames` output frames. Called on the audio thread. */
static void render(out_dev *o, BYTE *dst, UINT32 frames)
{
    static double pos;                 /* fractional position past g_rd */
    static double win_min = 1e9;       /* lowest buffer level this window (ms) */
    static long long win_t0, tick_t0, drop_t;
    static double adj;                 /* speed trim, +-0.5 % */
    static float gain, last_l, last_r; /* fade in after a gap, fade out into one */
    double base = (double)SB_RATE / o->rate;
    float gain_step = 1.0f / (0.005f * (float)o->rate);
    UINT32 f;
    unsigned c;

    EnterCriticalSection(&g_mx);
    {
        double fill = (double)(g_wr - g_rd) - pos;
        double fill_ms = fill * 1000.0 / SB_RATE;
        long long now = now_ms();

        if (!win_t0) win_t0 = tick_t0 = now;
        if (now - tick_t0 >= 1000) {
            tick_t0 = now;
            margin_tick();
        }
        if (!g_playing) {
            /* start once the margin plus one whole PS5 chunk (21 ms) is
             * queued; less runs dry again before the next chunk arrives */
            if (fill_ms >= g_margin_ms + 24.0) {
                if (fill_ms > g_margin_ms + 50.0) {
                    /* a backlog arrived in one go: start near its end */
                    g_rd += (unsigned long long)((fill_ms - g_margin_ms - 24.0) * SB_RATE / 1000.0);
                    drop_t = now;
                }
                g_playing = 1;
                win_min = 1e9;
                win_t0 = now;
            }
        } else {
            if (now - win_t0 >= (g_low ? 250 : 1000) && win_min < 1e8) {
                double err = win_min - g_margin_ms;   /* spare buffer we could lose */
                /* low delay: after a Wi-Fi pause the late packets all arrive
                 * at once; skip that backlog instead of slowly playing it off */
                double too_much = g_low ? 12.0 : 40.0, keep = g_low ? 3.0 : 5.0;
                if (err > too_much) {
                    unsigned long long skip = (unsigned long long)((err - keep) * SB_RATE / 1000.0);
                    g_rd += skip;
                    g_drops++;
                    drop_t = now;
                    logf_("drop %.1f ms (lowest level %.1f ms, margin %.1f)", err - keep, win_min, g_margin_ms);
                    err = keep;
                }
                adj = err * 0.0005;
                if (adj > 0.005) adj = 0.005;
                if (adj < -0.005) adj = -0.005;
                win_min = 1e9;
                win_t0 = now;
            }
            g_fill_avg_ms = g_fill_avg_ms * 0.98 + fill_ms * 0.02;
        }
    }

    for (f = 0; f < frames; f++) {
        float l = 0.f, r = 0.f;
        if (g_playing) {
            unsigned long long i0 = g_rd + (unsigned long long)pos;
            if (i0 + 1 >= g_wr) {
                /* ran dry: fade out, then wait for the margin again (a
                 * little higher for a while, unless it was our own skip) */
                long long t = now_ms();
                g_playing = 0;
                g_underruns++;
                if (!g_low && t - drop_t > 3000) {
                    g_margin_ms += 2.0;
                    if (g_margin_ms > 150.0) g_margin_ms = 150.0;
                }
                adj = 0;
                logf_("underrun, margin now %.1f ms, last packet %lld ms ago", g_margin_ms, t - g_last_audio_ms);
            } else {
                double t = pos - floor(pos);
                size_t a = (size_t)(i0 & (RING_FRAMES - 1)) * 2;
                size_t b = (size_t)((i0 + 1) & (RING_FRAMES - 1)) * 2;
                l = (float)(g_ring[a] + (g_ring[b] - g_ring[a]) * t);
                r = (float)(g_ring[a + 1] + (g_ring[b + 1] - g_ring[a + 1]) * t);
                pos += base * (1.0 + adj);
                if (pos >= 1.0) {
                    unsigned long long whole = (unsigned long long)pos;
                    g_rd += whole;
                    pos -= (double)whole;
                }
                if (gain < 1.0f) {
                    gain += gain_step;
                    if (gain > 1.0f) gain = 1.0f;
                    l *= gain;
                    r *= gain;
                }
                last_l = l;
                last_r = r;
            }
        }
        if (!g_playing) {
            last_l *= 0.97f;                /* ~2 ms fade, no click */
            last_r *= 0.97f;
            l = last_l;
            r = last_r;
            gain = 0.f;
        }
        for (c = 0; c < o->ch; c++) {
            float v = c == 0 ? l : c == 1 ? r : 0.f;
            if (o->is_float && o->bits == 32) ((float *)dst)[f * o->ch + c] = v;
            else if (o->bits == 16) ((short *)dst)[f * o->ch + c] = (short)(v * 32767.f);
            else if (o->bits == 32) ((int *)dst)[f * o->ch + c] = (int)(v * 2147483647.0);
        }
    }
    if (g_playing) {
        /* the low point is right after a chunk was taken, before the next packet */
        double after_ms = ((double)(g_wr - g_rd) - pos) * 1000.0 / SB_RATE;
        if (after_ms < win_min) win_min = after_ms;
    }
    LeaveCriticalSection(&g_mx);
}

/* Opens g_out_want, signals g_open_done, then plays until told to stop or
 * the device goes away (unplugged). */
static DWORD WINAPI audio_thread(LPVOID arg)
{
    IMMDeviceEnumerator *en;
    IMMDevice *dev = NULL;
    out_dev o;
    DWORD task = 0;
    HANDLE mm;
    wchar_t name[256] = L"";

    (void)arg;
    memset(&o, 0, sizeof o);
    en = enumerator();
    if (en) dev = pick_device(en, g_out_want[0] ? g_out_want : NULL, name, 256);
    g_out_ok = dev && open_output(dev, &o);
    wcscpy(g_out_name, g_out_ok ? name : L"");
    logf_("output %ls: %s", name, g_out_ok ? "open" : "FAILED");
    if (dev) IMMDevice_Release(dev);
    SetEvent(g_open_done);
    if (!g_out_ok) {
        if (en) IMMDeviceEnumerator_Release(en);
        return 0;
    }

    mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    IAudioClient_Start(o.ac);
    while (!g_quit && !g_out_stop) {
        UINT32 pad = 0, avail;
        BYTE *dst;
        if (WaitForSingleObject(o.ev, 200) != WAIT_OBJECT_0) continue;
        if (FAILED(IAudioClient_GetCurrentPadding(o.ac, &pad))) break;
        avail = o.buf_frames - pad;
        if (!avail) continue;
        if (FAILED(IAudioRenderClient_GetBuffer(o.rc, avail, &dst))) break;
        render(&o, dst, avail);
        IAudioRenderClient_ReleaseBuffer(o.rc, avail, 0);
    }
    if (!g_out_stop && !g_quit) logf_("output lost (device unplugged or changed)");
    g_out_ok = 0;
    IAudioClient_Stop(o.ac);
    if (mm) AvRevertMmThreadCharacteristics(mm);
    close_output(&o);
    IMMDeviceEnumerator_Release(en);
    return 0;
}

int sb_output_open(const wchar_t *want, wchar_t *picked, size_t cap)
{
    if (g_audio_thr) {
        InterlockedExchange(&g_out_stop, 1);
        WaitForSingleObject(g_audio_thr, 3000);
        CloseHandle(g_audio_thr);
        g_audio_thr = NULL;
    }
    InterlockedExchange(&g_out_stop, 0);
    wcsncpy(g_out_want, want ? want : L"", 255);
    ResetEvent(g_open_done);
    g_audio_thr = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    WaitForSingleObject(g_open_done, 5000);
    if (g_out_ok && want && wcscmp(want, g_out_name) == 0) {
        wcsncpy(g_set_device, want, 255);              /* remember an exact pick */
        settings_save();
    }
    if (picked && cap) {
        wcsncpy(picked, g_out_name, cap - 1);
        picked[cap - 1] = 0;
    }
    return g_out_ok;
}

void sb_set_low(int low)
{
    g_low = low ? 1 : 0;
    g_set_low = g_low;
    settings_save();
    logf_("mode: %s", g_low ? "low delay" : "smooth");
}

void sb_set_margin_floor(double ms)
{
    g_margin_ms = g_margin_floor = ms;
}

void sb_status_get(sb_status *s)
{
    static long long t_log;
    long long now = now_ms();
    char ps5st[256];
    double rtt;

    memset(s, 0, sizeof *s);
    EnterCriticalSection(&g_mx);
    s->known = g_ps5_known;
    if (s->known) inet_ntop(AF_INET, &g_ps5.sin_addr, s->ip, sizeof s->ip);
    s->live = s->known && now - g_last_audio_ms < 1000;
    s->playing = g_playing;
    rtt = g_rtt_ms;
    s->buffer_ms = g_fill_avg_ms;
    s->lost = g_lost;
    s->gaps = g_underruns;
    s->level_l = g_peak_l;
    s->level_r = g_peak_r;
    g_peak_l = g_peak_r = 0.f;
    strcpy(ps5st, g_ps5_stats);
    LeaveCriticalSection(&g_mx);

    s->out_ok = g_out_ok;
    s->low = g_low;
    s->out_ms = g_out_ms;
    s->net_ms = rtt > 0 ? rtt / 2 : 0;
    s->delay_ms = s->buffer_ms + s->out_ms + s->net_ms;
    s->wifi_pause_ms = g_gap_max60;
    wcsncpy(s->device, g_out_name, 255);

    if (now - t_log >= 5000) {
        t_log = now;
        if (s->live)
            logf_("PS5 %s | delay ~%.0f ms (buffer %.0f + output %.0f + network %.0f) | gaps %u | "
                  "Wi-Fi pauses up to %.0f ms | margin %.1f drops %u lost %u | %s",
                  s->ip, s->delay_ms, s->buffer_ms, s->out_ms, s->net_ms, s->gaps,
                  s->wifi_pause_ms, g_margin_ms, g_drops, s->lost, ps5st);
        else
            logf_("%s", s->known ? "PS5 known, no sound arriving" : "looking for the PS5");
    }
}

int sb_engine_start(const char *ps5_ip)
{
    WSADATA wsa;
    int one = 1, rbuf = 1 << 20;
    DWORD tmo = 200;
    struct sockaddr_in me;
    char exe[MAX_PATH], logpath[MAX_PATH], *slash;

    GetModuleFileNameA(NULL, exe, sizeof exe);
    slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    snprintf(g_ini, sizeof g_ini, "%sSoundBridge_Receiver.ini", exe);
    snprintf(logpath, sizeof logpath, "%sSoundBridge_Receiver.log", exe);
    g_log = fopen(logpath, "w");             /* fresh each run, so it never grows big */
    logf_("SoundBridge Receiver v%d starting", RX_VERSION);
    settings_load();
    if (g_set_low >= 0) g_low = g_set_low;

    InitializeCriticalSection(&g_mx);
    g_open_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    memset(&g_ps5, 0, sizeof g_ps5);
    g_ps5.sin_family = AF_INET;
    g_ps5.sin_port = htons(SB_PORT);
    if (ps5_ip && inet_pton(AF_INET, ps5_ip, &g_ps5.sin_addr) == 1) g_ps5_known = g_ps5_fixed = 1;
    timeBeginPeriod(1);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return -1;
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rbuf, sizeof rbuf);
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    bind(g_sock, (struct sockaddr *)&me, sizeof me);

    g_net_thr = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
    SetThreadPriority(g_net_thr, THREAD_PRIORITY_TIME_CRITICAL);
    g_hello_thr = CreateThread(NULL, 0, hello_thread, NULL, 0, NULL);
    return 0;
}

void sb_engine_stop(void)
{
    InterlockedExchange(&g_quit, 1);
    if (g_audio_thr) WaitForSingleObject(g_audio_thr, 2000);
    if (g_net_thr) WaitForSingleObject(g_net_thr, 1000);
    if (g_hello_thr) WaitForSingleObject(g_hello_thr, 1000);
    logf_("stopped");
    timeEndPeriod(1);
}
