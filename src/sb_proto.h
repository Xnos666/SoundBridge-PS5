/* sb_proto.h - SoundBridge wire format (UDP, little-endian), shared by the
 * PS5 payload and the PC receiver.
 *
 *   PC  -> PS5:9399  HELLO  every 500 ms; the PS5 streams to whoever sent the
 *                    last one and stops 5 s after the last one.
 *   PS5 -> PC        AUDIO  48 kHz stereo s16, at most SB_MAX_FRAMES a packet
 *                    PONG   HELLO's pc_time echoed (round-trip time)
 *                    STATS  one line of text, once a second */
#ifndef SB_PROTO_H
#define SB_PROTO_H

#include <stdint.h>

#define SB_PORT        9399
#define SB_RATE        48000
#define SB_MAX_FRAMES  240            /* 5 ms; 960 bytes of PCM, no IP fragments */
#define SB_CLIENT_TIMEOUT_MS 5000

#define SB_MAGIC_HELLO 0x31484253u    /* "SBH1" */
#define SB_MAGIC_AUDIO 0x31414253u    /* "SBA1" */
#define SB_MAGIC_PONG  0x31504253u    /* "SBP1" */
#define SB_MAGIC_STATS 0x31534253u    /* "SBS1" */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;                   /* SB_MAGIC_HELLO */
    uint32_t pc_time_us;              /* echoed in PONG */
    uint32_t version;                 /* receiver version, informational */
} sb_hello;

typedef struct {
    uint32_t magic;                   /* SB_MAGIC_AUDIO */
    uint32_t seq;                     /* +1 per packet */
    uint32_t ps5_ms;                  /* capture time on the PS5 clock */
    uint16_t frames;                  /* stereo frames that follow */
    uint16_t channels;                /* always 2 */
    /* int16_t pcm[frames * 2]; */
} sb_audio_hdr;

typedef struct {
    uint32_t magic;                   /* SB_MAGIC_PONG */
    uint32_t pc_time_us;
    uint32_t ps5_ms;
} sb_pong;
#pragma pack(pop)

#endif
