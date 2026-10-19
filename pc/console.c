/* console.c - SoundBridge Receiver as a text window.
 *
 * Usage: SoundBridge_Receiver.exe [--smooth] [--ps5 IP] [--device "name part"]
 *                                 [--margin MS] [--list] */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "sb_engine.h"

static volatile LONG g_quit;

static BOOL WINAPI on_ctrl(DWORD t)
{
    (void)t;
    InterlockedExchange(&g_quit, 1);
    return TRUE;
}

int main(int argc, char **argv)
{
    wchar_t want[256] = L"", devname[256] = L"", names[SB_MAX_DEVICES][256];
    const char *ps5 = NULL;
    int i, list = 0, low = -1;
    double margin = -1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ps5") && i + 1 < argc) {
            ps5 = argv[++i];
        } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, want, 255);
            _wcslwr(want);
        } else if (!strcmp(argv[i], "--margin") && i + 1 < argc) {
            margin = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--list")) {
            list = 1;
        } else if (!strcmp(argv[i], "--smooth")) {
            low = 0;
        } else if (!strcmp(argv[i], "--low")) {
            low = 1;
        } else {
            printf("Usage: SoundBridge_Receiver.exe [--smooth] [--ps5 IP] [--device \"name part\"] [--margin MS] [--list]\n");
            return 1;
        }
    }

    printf("SoundBridge Receiver - PS5 game sound on this PC\n");
    if (list) {
        int n = sb_device_list(names, SB_MAX_DEVICES);
        printf("Sound outputs:\n");
        for (i = 0; i < n; i++) wprintf(L"  %ls\n", names[i]);
        return 0;
    }
    printf("Close this window (or press Ctrl+C) to stop.\n\n");
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    if (sb_engine_start(ps5) != 0) {
        printf("Cannot start networking.\n");
        return 1;
    }
    if (low >= 0) sb_set_low(low);
    if (margin >= 0) sb_set_margin_floor(margin);
    if (!want[0]) sb_saved_device(want, 256);
    if (!sb_output_open(want[0] ? want : NULL, devname, 256)) {
        printf("Cannot open the sound output. Run with --list to see the names.\n");
        return 1;
    }
    printf("Playing on: %ls\n", devname);

    while (!g_quit) {
        sb_status s;
        char line[400];
        float peak;

        Sleep(1000);
        sb_status_get(&s);
        peak = s.level_l > s.level_r ? s.level_l : s.level_r;
        if (!s.live)
            snprintf(line, sizeof line, "%s", s.known ? "PS5 found, waiting for sound... (is SoundBridge running?)"
                                                      : "Looking for the PS5... (is SoundBridge running on it?)");
        else
            snprintf(line, sizeof line,
                     "PS5 %s | %s | delay ~%3.0f ms (buffer %3.0f + output %2.0f + network %2.0f) | %s | gaps %u",
                     s.ip, s.low ? "low delay" : "smooth", s.delay_ms, s.buffer_ms, s.out_ms, s.net_ms,
                     peak > 0.0005f ? "sound " : "silent", s.gaps);
        printf("\r%-150.150s", line);
        fflush(stdout);
    }
    printf("\nStopped.\n");
    sb_engine_stop();
    return 0;
}
