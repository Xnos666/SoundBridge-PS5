/* diag.c - SoundBridge has no diagnostics page: diag_set() lines from the
 * shared capture code go to the log instead. */
#include "diag.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

void diag_init(const char *path) { (void)path; }
void diag_set_path(const char *path) { (void)path; }
int  diag_text(char *out, size_t cap) { if (out && cap) out[0] = 0; return 0; }
int  diag_save(void) { return 0; }

void diag_set(const char *key, const char *fmt, ...)
{
    char val[DIAG_VAL_MAX];
    va_list va;

    va_start(va, fmt);
    (void)vsnprintf(val, sizeof val, fmt, va);
    va_end(va);
    log_line("diag: %s: %s", key, val);
}
