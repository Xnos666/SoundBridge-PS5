/* SoundBridge PS5: the console's game sound, streamed over the home network
 * to the SoundBridge Receiver on a PC.
 *
 *   libSceAvcap2 (the Remote Play mix, HearBridge's avcap2.c)
 *     -> 48 kHz stereo s16 -> UDP packets of <= 5 ms -> PC
 *
 * The PC finds the console by sending HELLO to UDP 9399 (broadcast or
 * direct); the console streams to the last sender until its HELLOs stop.
 * State, log and stop file live in /data/soundbridge. Sending the payload
 * again replaces the running copy. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "avcap2.h"
#include "lock.h"
#include "log.h"
#include "notify.h"
#include "sb_proto.h"
#include "stop.h"
#include "util.h"

#define SB_VERSION  "0.1.1"
#define STATE_DIR   "/data/soundbridge"
#define LOG_PATH    STATE_DIR "/soundbridge.log"
#define LOCK_PATH   STATE_DIR "/soundbridge.lock"

#define READ_FRAMES 2048

/* sendto/recvfrom are not in the import list this console is known to load
 * (see check_imports.sh), so they go straight to the kernel. */
static long sb_sendto(int s, const void *buf, size_t len, const struct sockaddr_in *to)
{
    return syscall(SYS_sendto, s, buf, len, 0, to, sizeof *to);
}

static long sb_recvfrom(int s, void *buf, size_t len, struct sockaddr_in *from)
{
    socklen_t fl = sizeof *from;
    return syscall(SYS_recvfrom, s, buf, len, 0, from, &fl);
}

static int g_sock = -1;
static pthread_mutex_t g_mx;
static struct sockaddr_in g_client;      /* guarded by g_mx */
static long g_client_ms;                 /* last HELLO, 0 = none */

static int client_get(struct sockaddr_in *out)
{
    int ok;
    pthread_mutex_lock(&g_mx);
    ok = g_client_ms && now_ms() - g_client_ms < SB_CLIENT_TIMEOUT_MS;
    if (ok) *out = g_client;
    pthread_mutex_unlock(&g_mx);
    return ok;
}

static void *control_thread(void *arg)
{
    unsigned char buf[256];
    char ip[32];
    struct sockaddr_in from, last;
    long n;

    (void)arg;
    memset(&last, 0, sizeof last);
    while (!hb_stop_requested()) {
        struct pollfd pfd = { g_sock, POLLIN, 0 };
        if (poll(&pfd, 1, 300) <= 0) continue;
        memset(&from, 0, sizeof from);
        n = sb_recvfrom(g_sock, buf, sizeof buf, &from);
        if (n < (long)sizeof(sb_hello)) continue;
        {
            sb_hello h;
            sb_pong p;
            int fresh;
            memcpy(&h, buf, sizeof h);
            if (h.magic != SB_MAGIC_HELLO) continue;

            pthread_mutex_lock(&g_mx);
            fresh = !g_client_ms || now_ms() - g_client_ms >= SB_CLIENT_TIMEOUT_MS ||
                    g_client.sin_addr.s_addr != from.sin_addr.s_addr ||
                    g_client.sin_port != from.sin_port;
            g_client = from;
            g_client_ms = now_ms();
            pthread_mutex_unlock(&g_mx);

            p.magic = SB_MAGIC_PONG;
            p.pc_time_us = h.pc_time_us;
            p.ps5_ms = (uint32_t)now_ms();
            (void)sb_sendto(g_sock, &p, sizeof p, &from);

            if (fresh) {
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                log_line("client: %s:%u (receiver v%u)", ip, ntohs(from.sin_port), h.version);
                if (last.sin_addr.s_addr != from.sin_addr.s_addr)
                    notify("SoundBridge: sending sound to PC %s", ip);
                last = from;
            }
        }
    }
    return NULL;
}

static int open_socket(void)
{
    struct sockaddr_in a;
    int one = 1, sz = 256 * 1024, tos = 0xC0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);

    if (s < 0) return -1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    (void)setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    /* DSCP CS6: Wi-Fi (WMM) sends it in the voice queue, ahead of bulk traffic */
    (void)setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(SB_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        log_line("net: bind udp %d failed errno %d", SB_PORT, errno);
        close(s);
        return -1;
    }
    return s;
}

/* Capture statistics, reset every second (sent to the PC as STATS). */
typedef struct {
    long t0;
    unsigned reads, empty, frames, fmin, fmax, gap_max, packets, send_err;
    float peak;
    long last_data;
} cap_stats;

static void stats_reset(cap_stats *c, long now)
{
    long last = c->last_data;
    memset(c, 0, sizeof *c);
    c->t0 = now;
    c->fmin = ~0u;
    c->last_data = last;
}

int main(void)
{
    static int16_t pcm[READ_FRAMES * 2];
    unsigned char pkt[sizeof(sb_audio_hdr) + SB_MAX_FRAMES * 4];
    avcap2_session *cap = NULL;
    cap_stats st;
    struct sockaddr_in to;
    pthread_t cthr;
    uint32_t seq = 0;
    long old_pid = 0, next_open = 0, next_log = 0;
    int lock_rc, lock_err = 0, said_fail = 0, log_every = 10;

    umask(0022);
    (void)syscall(SYS_thr_set_name, -1, "SoundBridge");
    if (mkdir(STATE_DIR, 0755) != 0 && errno != EEXIST) {
        notify("SoundBridge: cannot create %s (errno %d)", STATE_DIR, errno);
        return 1;
    }
    log_open(LOG_PATH);
    log_line("SoundBridge %s (built %s) starting", SB_VERSION, __DATE__);
    lock_rc = lock_take_over(LOCK_PATH, HB_STOP_PATH, 6000, 2000, &lock_err, &old_pid);
    if (lock_rc != LOCK_OK) {
        log_line("soundbridge: lock %s (errno %d); exiting", lock_rc == LOCK_BUSY ? "busy" : "not writable", lock_err);
        notify("SoundBridge: another copy is still running and did not stop. Restart the PS5 and try again.");
        return 1;
    }
    if (old_pid) log_line("soundbridge: replaced the copy that was running (pid %ld)", old_pid);
    hb_stop_init();
    pthread_mutex_init(&g_mx, NULL);

    g_sock = open_socket();
    if (g_sock < 0) {
        notify("SoundBridge: network port %d is busy. Restart the PS5 and try again.", SB_PORT);
        lock_release();
        return 1;
    }
    if (pthread_create(&cthr, NULL, control_thread, NULL) != 0) {
        log_line("soundbridge: cannot start the control thread");
        close(g_sock);
        lock_release();
        return 1;
    }
    notify("SoundBridge %s running\nOpen SoundBridge Receiver on your PC.", SB_VERSION);

    memset(&st, 0, sizeof st);
    stats_reset(&st, now_ms());

    while (!hb_stop_requested()) {
        long now = now_ms();
        int got, have_client;
        float peak = 0.f;

        if (!cap) {
            if (now < next_open) { usleep(50 * 1000); continue; }
            cap = avcap2_session_open();
            if (!cap) {
                next_open = now + 3000;
                if (!said_fail) {
                    said_fail = 1;
                    notify("SoundBridge: could not start sound capture, retrying.\nLog: %s", LOG_PATH);
                }
                continue;
            }
            if (said_fail) notify("SoundBridge: sound capture started");
            said_fail = 0;
            log_line("capture: open");
        }

        got = avcap2_session_read_s16(cap, pcm, READ_FRAMES, &peak);
        now = now_ms();
        if (got < 0) {
            log_line("capture: session ended, reopening");
            avcap2_session_close(cap);
            cap = NULL;
            next_open = now + 1000;
            continue;
        }
        if (got == 0) {
            st.empty++;
            usleep(1000);
        } else {
            unsigned g = (unsigned)got;
            st.reads++;
            st.frames += g;
            if (g < st.fmin) st.fmin = g;
            if (g > st.fmax) st.fmax = g;
            if (st.last_data && (unsigned)(now - st.last_data) > st.gap_max)
                st.gap_max = (unsigned)(now - st.last_data);
            st.last_data = now;
            if (peak > st.peak) st.peak = peak;

            have_client = client_get(&to);
            if (have_client) {
                int off = 0;
                while (off < got) {
                    int n = got - off;
                    sb_audio_hdr h;
                    if (n > SB_MAX_FRAMES) n = SB_MAX_FRAMES;
                    h.magic = SB_MAGIC_AUDIO;
                    h.seq = seq++;
                    h.ps5_ms = (uint32_t)now;
                    h.frames = (uint16_t)n;
                    h.channels = 2;
                    memcpy(pkt, &h, sizeof h);
                    memcpy(pkt + sizeof h, pcm + off * 2, (size_t)n * 4);
                    if (sb_sendto(g_sock, pkt, sizeof h + (size_t)n * 4, &to) < 0) st.send_err++;
                    else st.packets++;
                    off += n;
                }
            }
        }

        if (now - st.t0 >= 1000) {
            char line[256];
            int len;
            unsigned avg = st.reads ? st.frames / st.reads : 0;
            len = snprintf(line + 4, sizeof line - 4,
                           "ps5: %u reads/s, %u frames/s, chunk %u-%u (avg %u) frames, max gap %u ms, "
                           "%u empty, peak %.3f, %u pkts, %u send errors",
                           st.reads, st.frames, st.reads ? st.fmin : 0, st.fmax, avg, st.gap_max,
                           st.empty, (double)st.peak, st.packets, st.send_err);
            if (client_get(&to)) {
                uint32_t m = SB_MAGIC_STATS;
                memcpy(line, &m, 4);
                (void)sb_sendto(g_sock, line, (size_t)len + 4, &to);
            }
            if (now >= next_log) {
                log_line("%s", line + 4);
                if (--log_every <= 0) next_log = now + 60000;
            }
            stats_reset(&st, now);
        }
    }

    log_line("soundbridge: stopping");
    if (cap) avcap2_session_close(cap);
    pthread_join(cthr, NULL);
    close(g_sock);
    hb_stop_clear();
    lock_release();
    log_close();
    return 0;
}
