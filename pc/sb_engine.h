/* sb_engine.h - the SoundBridge receiver without a user interface: finds the
 * PS5, receives its sound and plays it on a PC output. Used by the window
 * app (gui.cpp) and the console app (console.c). */
#ifndef SB_ENGINE_H
#define SB_ENGINE_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SB_MAX_DEVICES 16

typedef struct {
    int known;               /* PS5 address known */
    int live;                /* sound packets arrived in the last second */
    int playing;             /* output is playing (not waiting for buffer) */
    int out_ok;              /* output device open and running */
    int low;                 /* low-delay mode */
    char ip[32];
    double delay_ms;         /* buffer + output + network */
    double buffer_ms, out_ms, net_ms;
    double wifi_pause_ms;    /* longest wait between packets, last minute */
    unsigned gaps, lost;
    float level_l, level_r;  /* peak 0..1 since the previous call */
    wchar_t device[256];     /* output in use */
} sb_status;

/* Start networking (log and settings next to the exe). ps5_ip may be NULL
 * (search the network). Returns 0 on success. */
int  sb_engine_start(const char *ps5_ip);

/* Saved settings (from SoundBridge_Receiver.ini). */
int  sb_saved_low(int fallback);
int  sb_saved_device(wchar_t *out, size_t cap);   /* 1 if one was saved */

/* Active sound outputs; returns how many were written. */
int  sb_device_list(wchar_t names[][256], int max);

/* Open (or switch to) an output. want = full name, a lower-case part of
 * it, or NULL for the Scarlett / Windows default. Blocks until opened.
 * Returns 1 on success; *picked gets the name in use. */
int  sb_output_open(const wchar_t *want, wchar_t *picked, size_t cap);

void sb_set_low(int low);
void sb_set_margin_floor(double ms);
void sb_status_get(sb_status *s);
void sb_engine_stop(void);

/* Log line in SoundBridge_Receiver.log (printf style). */
void sb_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
