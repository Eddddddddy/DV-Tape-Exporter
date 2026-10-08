/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 Boris Timofeyenko
 *
 * Modified by DV Tape Exporter contributors, 2026-10-08:
 * Windows platform support, JSON frontend protocol, exclusive raw output,
 * shutdown drain handling, and separate stop / verified-end reporting.
 *
 * uvcdv-capture — capture a raw DV stream from a UVC DV-payload camcorder
 *                 over USB into a .dv file, using libusb (userspace, no
 *                 kernel driver).
 *
 * Works with any USB Video Class device that exposes the stream-based DV
 * payload format (VS_FORMAT_DV, subtype 0x0C) — in practice mid-2000s
 * Panasonic MiniDV camcorders in "USB FUNCTION > Motion DV" mode.
 * Tested: Panasonic NV-GS330 (04da:231e). macOS enumerates such devices
 * but Apple's UVC stack implements only frame-based payloads, so no
 * system driver binds — which is exactly why claiming the interface from
 * userspace works without kexts.
 *
 * v4 (uvcdv-capture 1.0.0) changes vs v3:
 *   - auto-detection: scans the bus for a device with VS_FORMAT_DV, no
 *     VID/PID needed when a single camera is plugged in; --list shows all
 *     UVC video devices and whether they expose a DV format
 *   - full UVC overrides for untested cameras: --iface, --format-index,
 *     --frame-index, --alt, --ep
 *   - --probe: negotiate and report stream parameters without capturing
 *   - --max-sec session limit; frame-size for diagnostics taken from the
 *     negotiated dwMaxVideoFrameSize (PAL/NTSC agnostic)
 *
 * v3: payloads of packets flagged with the UVC header error bit are
 *     WRITTEN, not discarded (default). DV is self-describing (80-byte DIF
 *     blocks with their own STA error flags + in-camera concealment), so
 *     keeping flagged data preserves stream structure. Validated on
 *     hardware: 0 damaged frames (dvrescue) vs 16.7% when dropping.
 *     Old behavior available via --drop-err. Error-distribution
 *     diagnostics (per-second rate, first/last offsets, longest run).
 *
 * v2: disk I/O on a dedicated writer thread behind a ring buffer (the iso
 *     callback only memcpy()s and resubmits), deep transfer queue
 *     (~512 ms), QoS boost on macOS, clean cancel-based shutdown,
 *     detailed drop accounting. Measured: bus drops = 0, overflow = 0.
 *
 * Build (macOS):
 *   brew install libusb pkg-config
 *   make
 *
 * Run:
 *   caffeinate -i ./uvcdv-capture --idle 15 tape01.dv
 *   (caffeinate prevents idle sleep during hour-long captures)
 */

#include <libusb.h>   /* pkg-config --cflags points -I into the libusb-1.0 dir */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include "capture_platform.h"
#ifndef _WIN32
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#else
#define strcasecmp _stricmp
#endif

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#define UVCDV_VERSION "1.7.0-windows-port.1"

/* ---- UVC constants ---- */
#define UVC_CC_VIDEO             0x0E
#define UVC_SC_VIDEOCONTROL      0x01
#define UVC_SC_VIDEOSTREAMING    0x02
#define UVC_CS_INTERFACE         0x24
#define UVC_VC_INPUT_TERMINAL    0x02
#define UVC_VS_FORMAT_DV         0x0C

#define UVC_SET_CUR              0x01
#define UVC_GET_CUR              0x81
#define UVC_GET_LEN              0x85
#define UVC_GET_INFO             0x86
#define UVC_GET_DEF              0x87

/* ---- Media Transport Terminal (companion spec "Video Media Transport
 * Terminal" rev 1.5) — tape transport control on camcorders/decks.
 * Same control mechanism Windows maps to DirectShow IAMExtTransport. ---- */
#define UVC_ITT_MEDIA_TRANSPORT     0x0202  /* input terminal type */
#define MTT_TRANSPORT_CONTROL       0x01    /* control selectors (Table A-1) */
#define MTT_ATN_INFORMATION         0x02
#define MTT_MEDIA_INFORMATION       0x03
#define MTT_TIME_CODE_INFORMATION   0x04
/* Transport Control mode values (Table 4-3) */
#define XPRT_PLAY_FORWARD           0x18
#define XPRT_PAUSE                  0x19
#define XPRT_STOP                   0x40
#define XPRT_FAST_FORWARD           0x41
#define XPRT_REWIND                 0x42
#define XPRT_HS_REWIND              0x43
#define XPRT_EJECT                  0x60

#define UVC_VS_PROBE_CONTROL     0x01
#define UVC_VS_COMMIT_CONTROL    0x02

#define OFF_bFormatIndex         2
#define OFF_bFrameIndex          3
#define OFF_dwMaxVideoFrameSize  18
#define OFF_dwMaxPayloadXfer     22

/* ---- defaults (CLI-tunable) ---- */
static int g_num_transfers = 32;
static int g_pkts_per_xfer = 128;
static int g_ring_mb       = 64;
static int g_idle_sec      = 0;      /* 0 = disabled */
static int g_max_sec       = 0;      /* 0 = disabled */
static int g_drop_err      = 0;      /* 1 = discard payloads flagged by the device */
static int g_verbose       = 0;
static int g_monitor_tape  = 0;      /* read-only transport/timecode monitoring */

/* UVC overrides; -1/0 = auto-detect */
static int g_force_iface   = -1;
static int g_force_fmt     = -1;
static int g_force_frame   = -1;
static int g_force_alt     = -1;
static int g_force_ep      = 0;

static uint32_t g_frame_size = 144000;  /* diagnostics only; PAL default, replaced
                                           by the negotiated dwMaxVideoFrameSize */

/* error-distribution diagnostics */
static uint64_t g_err_first_off = 0, g_err_last_off = 0;  /* byte offsets in the DV stream */
static int      g_err_seen = 0;
static uint64_t g_err_run = 0, g_err_run_max = 0;         /* consecutive flagged data packets */
static uint64_t g_err_dropped_bytes = 0;                  /* only if --drop-err */

enum stop_reason {
    STOP_NONE, STOP_SIGNAL, STOP_STDIN, STOP_PARENT_EOF, STOP_STDIN_ERROR,
    STOP_PHYSICAL_END, STOP_CAMERA_STOPPED, STOP_IDLE_TIMEOUT, STOP_MAX_SECONDS,
    STOP_DEVICE_LOST, STOP_TRANSPORT_ERROR, STOP_WRITE_ERROR, STOP_PLAY_FAILED
};
static atomic_int g_stop = 0;
static atomic_int g_stop_reason = STOP_NONE;
static atomic_int g_stdin_done = 0;
static int g_stop_on_stdin = 0;
static int g_stop_after_camera_stop = 0;
static int g_play_after_stream = 0;
static int g_play_set_result = INT_MIN;
static int g_physical_end_verified = 0;
static int g_observed_play = 0;
static int g_confirmed_end_reads = 0;
static uint64_t g_camera_stop_since_ms = 0;

static void request_stop(enum stop_reason reason)
{
    int expected = STOP_NONE;
    atomic_compare_exchange_strong(&g_stop_reason, &expected, reason);
    atomic_store(&g_stop, 1);
}

static const char *stop_reason_name(void)
{
    static const char *names[] = {"completed", "signal", "stdin_stop", "parent_stdin_closed",
        "stdin_error", "physical_end", "camera_stopped", "idle_timeout", "max_seconds",
        "device_lost", "transport_failed", "write_error", "play_failed"};
    int reason = atomic_load(&g_stop_reason);
    return reason >= 0 && reason < (int)(sizeof(names)/sizeof(names[0])) ? names[reason] : "unknown";
}

static void on_sigint(int s){ (void)s; request_stop(STOP_SIGNAL); }

/* Parent UI sends one ASCII line "stop"; parent death/closed pipe also stops.
 * This bounded-poll thread only sets atomics. USB ownership stays in main. */
static void *stdin_control_main(void *unused)
{
    (void)unused;
    unsigned char buffer[128];
    char line[128];
    size_t used = 0;
    int overflow = 0;
    while (!atomic_load(&g_stdin_done) && !atomic_load(&g_stop)) {
        int got = capture_stdin_poll(buffer, sizeof(buffer), 100);
        if (got < 0) {
            request_stop(got == -1 ? STOP_PARENT_EOF : STOP_STDIN_ERROR);
            break;
        }
        for (int i = 0; i < got; i++) {
            if (buffer[i] == '\n' || buffer[i] == '\r') {
                line[used] = 0;
                if (!overflow && (!strcasecmp(line, "stop") || !strcasecmp(line, "quit"))) {
                    request_stop(STOP_STDIN);
                    break;
                }
                used = 0; overflow = 0;
            } else if (used + 1 < sizeof(line)) {
                line[used++] = (char)buffer[i];
            } else overflow = 1;
        }
    }
    return NULL;
}

/* ---- stats (touched only from the USB event thread, printed from it too) ---- */
static uint64_t g_bytes = 0;          /* payload bytes pushed to ring */
static uint64_t g_pkts_ok = 0;
static uint64_t g_pkts_status_err = 0;/* iso packet status != COMPLETED (true bus drops) */
static uint64_t g_pkts_uvc_err = 0;   /* UVC header error bit set by the device */
static uint64_t g_hdr_bad = 0;        /* malformed UVC header */
static int      g_inflight = 0;
static time_t   g_last_data = 0;      /* last time payload bytes arrived */

/* Only transport states observed during this run contribute to completion.
 * Stop is not physical end. A failed read breaks every consecutive run. */
static void observe_transport(int result, uint8_t state, uint64_t now_ms,
                              uint64_t elapsed_ms)
{
    if (result != 1) {
        g_confirmed_end_reads = 0;
        g_camera_stop_since_ms = 0;
        return;
    }
    if (state == XPRT_PLAY_FORWARD || state == 0x06) g_observed_play = 1;
    if (state == 0x75 && elapsed_ms >= 10000 && g_bytes > 0) {
        if (++g_confirmed_end_reads >= 2) {
            g_physical_end_verified = 1;
            request_stop(STOP_PHYSICAL_END);
        }
    } else g_confirmed_end_reads = 0;
    if (state == XPRT_STOP && g_observed_play && g_bytes > 0 &&
            g_stop_after_camera_stop > 0) {
        if (!g_camera_stop_since_ms) g_camera_stop_since_ms = now_ms;
        if (now_ms - g_camera_stop_since_ms >=
                (uint64_t)g_stop_after_camera_stop * 1000)
            request_stop(STOP_CAMERA_STOPPED);
    } else g_camera_stop_since_ms = 0;
}

/* ---- ring buffer: producer = USB event thread, consumer = writer thread ---- */
typedef struct {
    uint8_t *buf;
    size_t   size, head, tail, fill;   /* head = write pos, tail = read pos */
    uint64_t ovf;                      /* bytes dropped because consumer fell behind */
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int eof;
} ring_t;

static ring_t g_ring;
static FILE  *g_out = NULL;
static uint64_t g_written = 0;
static atomic_int g_write_error = 0;
static int g_raw_on = 0;               /* raw .dv output enabled */

static int ring_init(ring_t *r, size_t size)
{
    memset(r, 0, sizeof(*r));
    r->buf = malloc(size);
    if (!r->buf) return -1;
    r->size = size;
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->cv, NULL);
    return 0;
}

/* push from the USB callback; never blocks — drops (and counts) on overflow */
static void ring_push(ring_t *r, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&r->mu);
    if (r->fill + len > r->size) {
        r->ovf += len;
    } else {
        size_t first = r->size - r->head;
        if (first >= len) {
            memcpy(r->buf + r->head, data, len);
        } else {
            memcpy(r->buf + r->head, data, first);
            memcpy(r->buf, data + first, len - first);
        }
        r->head = (r->head + len) % r->size;
        r->fill += len;
    }
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->mu);
}

static void ring_set_eof(ring_t *r)
{
    pthread_mutex_lock(&r->mu);
    r->eof = 1;
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->mu);
}

/* blocking pop of up to len bytes; returns 0 only at EOF */
static size_t ring_pop(ring_t *r, uint8_t *dst, size_t len)
{
    pthread_mutex_lock(&r->mu);
    while (r->fill == 0 && !r->eof)
        pthread_cond_wait(&r->cv, &r->mu);
    size_t n = r->fill < len ? r->fill : len;
    if (n) {
        size_t first = r->size - r->tail;
        if (first >= n) {
            memcpy(dst, r->buf + r->tail, n);
        } else {
            memcpy(dst, r->buf + r->tail, first);
            memcpy(dst + first, r->buf, n - first);
        }
        r->tail = (r->tail + n) % r->size;
        r->fill -= n;
    }
    pthread_mutex_unlock(&r->mu);
    return n;
}

/* writer thread: drain ring to disk in large chunks */
static void *writer_main(void *arg)
{
    ring_t *r = (ring_t *)arg;
    const size_t CHUNK = 1 << 20;              /* 1 MB per fwrite */
    uint8_t *tmp = malloc(CHUNK);
    if (!tmp) { g_write_error = 1; return NULL; }

    for (;;) {
        pthread_mutex_lock(&r->mu);
        while (r->fill == 0 && !r->eof)
            pthread_cond_wait(&r->cv, &r->mu);
        if (r->fill == 0 && r->eof) { pthread_mutex_unlock(&r->mu); break; }

        size_t n = r->fill < CHUNK ? r->fill : CHUNK;
        size_t first = r->size - r->tail;
        if (first >= n) {
            memcpy(tmp, r->buf + r->tail, n);
        } else {
            memcpy(tmp, r->buf + r->tail, first);
            memcpy(tmp + first, r->buf, n - first);
        }
        r->tail = (r->tail + n) % r->size;
        r->fill -= n;
        pthread_mutex_unlock(&r->mu);

        size_t written = fwrite(tmp, 1, n, g_out);
        g_written += written;
        if (written != n) { g_write_error = 1; break; }
    }
    free(tmp);
    return NULL;
}

static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p){ return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)); }

/* ---- terminal preview -------------------------------------------------
 * A side tap of the DV stream is decoded by an ffmpeg subprocess
 * (dv in → tiny rawvideo rgb24 out) and drawn with Unicode half-blocks
 * (▀: fg = top pixel, bg = bottom pixel) on the alternate screen.
 * Strictly best-effort: its ring drops data when full, frames are fed at
 * most ~12 fps, and none of it can stall the capture path. */

static int      g_preview   = 0;
static ring_t   g_pring;                   /* capture → preview tap */
static pid_t    g_pv_pid    = 0;
static int      g_pv_in     = -1;          /* → ffmpeg stdin  */
static int      g_pv_out    = -1;          /* ← ffmpeg stdout */
static int      g_pv_w = 0, g_pv_h = 0;    /* decoded preview size, pixels */
static int      g_truecolor = 0;
static volatile int g_pv_alive  = 0;
static volatile int g_pv_screen = 0;       /* alternate screen is active */
static char     g_tc_str[16] = "";         /* tape TC parsed from DV subcode */

/* resolved ffmpeg path; GUI-launched processes get launchd's minimal PATH
 * (no /opt/homebrew/bin), so a plain execlp("ffmpeg") fails there */
#ifndef _WIN32
static char g_ffmpeg[256] = "";
#endif

static int have_ffmpeg(void)
{
#ifdef _WIN32
    return 0;
#else
    if (g_ffmpeg[0]) return 1;
    FILE *f = popen("command -v ffmpeg 2>/dev/null", "r");   /* terminal case */
    if (f) {
        if (fgets(g_ffmpeg, sizeof(g_ffmpeg), f)) {
            char *nl = strchr(g_ffmpeg, '\n');
            if (nl) *nl = 0;
        }
        pclose(f);
        if (g_ffmpeg[0] == '/' && access(g_ffmpeg, X_OK) == 0) return 1;
        g_ffmpeg[0] = 0;
    }
    static const char *cands[] = {
        "/opt/homebrew/bin/ffmpeg",   /* Homebrew, Apple Silicon */
        "/usr/local/bin/ffmpeg",      /* Homebrew, Intel */
        "/opt/local/bin/ffmpeg",      /* MacPorts */
    };
    for (size_t i = 0; i < sizeof(cands)/sizeof(cands[0]); i++)
        if (access(cands[i], X_OK) == 0) {
            snprintf(g_ffmpeg, sizeof(g_ffmpeg), "%s", cands[i]);
            return 1;
        }
    return 0;
#endif
}

#define FFMPEG_BIN (g_ffmpeg[0] ? g_ffmpeg : "ffmpeg")

/* fit a 4:3 image into the terminal; half-blocks give 2 pixels per row */
static void pv_calc_size(void)
{
    int cols = 80, rows = 24;
    capture_terminal_size(&cols, &rows);
    int maxw = cols, maxh = (rows - 2) * 2;    /* 2 rows reserved for status */
    int w = maxw, h = w * 3 / 4;
    if (h > maxh) { h = maxh; w = h * 4 / 3; }
    g_pv_w = w & ~1; g_pv_h = h & ~1;
}

static int pv_spawn_ffmpeg(void)
{
#ifdef _WIN32
    return -1;
#else
    int inp[2], outp[2];
    if (pipe(inp) < 0 || pipe(outp) < 0) return -1;
    char vf[64];
    snprintf(vf, sizeof(vf), "scale=%d:%d", g_pv_w, g_pv_h);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(inp[0], 0);
        dup2(outp[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) dup2(nul, 2);
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        execlp(FFMPEG_BIN, "ffmpeg", "-loglevel", "quiet",
               "-probesize", "32768", "-analyzeduration", "0",
               "-fflags", "nobuffer", "-threads", "1",
               "-f", "dv", "-i", "pipe:0",
               "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24",
               "-threads", "1", "pipe:1",
               (char *)NULL);
        _exit(127);
    }
    close(inp[0]); close(outp[1]);
    g_pv_in = inp[1]; g_pv_out = outp[0]; g_pv_pid = pid;
    return 0;
#endif
}

/* ---- machine-readable status (--status-json) for GUI frontends ---- */
static int g_json = 0;

/* ---- scene splitting (--split): one output file per recorded scene ---- */
static int g_split = 0;
static volatile int g_scenes = 0;        /* current scene count, for status */
static int g_enc_scene_n = 0;            /* scenes produced by the encoder */

static int bcd2i(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
#define BCD_OK(v, max) ((((v) & 0x0F) <= 9) && ((v) <= (max)))

/* Extract two per-frame signatures used for scene detection:
 *  - tape timecode (subcode pack 0x13) as a frame counter,
 *  - recording wall-clock time (VAUX pack 0x63) in seconds.
 * Consumer camcorders keep TC continuous across in-camera cuts, but the
 * recording time jumps — checking both catches cuts of either kind
 * (same approach as dvgrab/Scenalyzer autosplit). -1 = not readable. */
static void dv_scene_sig(const uint8_t *f, uint32_t fsz, int *tc_out, int *rec_out)
{
    int tc = -1, rec = -1;
    int fps = (fsz == 120000) ? 30 : 25;
    for (uint32_t s = 0; s < fsz / (150 * 80) && (tc < 0 || rec < 0); s++) {
        const uint8_t *seq = f + s * 150 * 80;
        for (int b = 1; b <= 2 && tc < 0; b++) {          /* subcode blocks */
            const uint8_t *p = seq + b * 80;
            if ((p[0] >> 5) != 1) continue;
            for (int i = 3; i < 80 - 4; i++) {
                if (p[i] != 0x13) continue;
                uint8_t fr = p[i+1] & 0x3F, se = p[i+2] & 0x7F,
                        mi = p[i+3] & 0x7F, ho = p[i+4] & 0x3F;
                if (!BCD_OK(fr, 0x29) || !BCD_OK(se, 0x59) ||
                    !BCD_OK(mi, 0x59) || !BCD_OK(ho, 0x23)) continue;
                tc = ((bcd2i(ho) * 60 + bcd2i(mi)) * 60 + bcd2i(se)) * fps + bcd2i(fr);
                break;
            }
        }
        for (int b = 3; b <= 5 && rec < 0; b++) {         /* VAUX blocks */
            const uint8_t *p = seq + b * 80;
            if ((p[0] >> 5) != 2) continue;
            for (int off = 3; off + 5 <= 80; off += 5) {  /* VAUX = 15 packs x 5 B */
                if (p[off] != 0x63) continue;
                uint8_t se = p[off+2] & 0x7F, mi = p[off+3] & 0x7F, ho = p[off+4] & 0x3F;
                if (!BCD_OK(se, 0x59) || !BCD_OK(mi, 0x59) || !BCD_OK(ho, 0x23)) continue;
                rec = (bcd2i(ho) * 60 + bcd2i(mi)) * 60 + bcd2i(se);
                break;
            }
        }
    }
    *tc_out = tc;
    *rec_out = rec;
}

typedef struct { int tc, rec; uint64_t frames; } scene_state_t;

/* returns 1 when this frame starts a new scene; updates the tracked state */
static int scene_boundary(scene_state_t *st, int tc, int rec)
{
    int cut = 0;
    if (st->frames >= 15) {              /* anti-flap: scene >= 15 frames */
        if (tc >= 0 && st->tc >= 0) {
            int d = tc - st->tc;
            if (d < 0 || d > 3) cut = 1;             /* TC jump/reset */
        }
        if (rec >= 0 && st->rec >= 0) {
            int d = rec - st->rec;
            if (d < 0 || d > 1) cut = 1;             /* rec time jump >= 2 s */
        }
    }
    if (tc  >= 0) st->tc  = tc;
    if (rec >= 0) st->rec = rec;
    if (cut) st->frames = 0;
    return cut;
}

/* "dir/tape.dv" + n=2 -> "dir/tape-002.dv" */
static void scene_filename(const char *base, int n, char *out, size_t sz)
{
    const char *dot = strrchr(base, '.');
    const char *slash = strrchr(base, '/');
    if (dot && dot != base && (!slash || dot > slash))
        snprintf(out, sz, "%.*s-%03d%s", (int)(dot - base), base, n, dot);
    else
        snprintf(out, sz, "%s-%03d", base, n);
}

/* ---- periodic JPEG snapshot (--snapshot FILE) for GUI preview ---- */
static ring_t   g_snap_ring;
static pid_t    g_snap_pid   = 0;
static int      g_snap_in    = -1;
static volatile int g_snap_alive = 0;

static int snap_spawn_ffmpeg(const char *path)
{
#ifdef _WIN32
    (void)path;
    return -1;
#else
    int inp[2];
    if (pipe(inp) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(inp[0], 0);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        close(inp[0]); close(inp[1]);
        /* low-latency: default probesize buffers 5 MB (~2.7 s at the feed
         * rate) and the frame-threaded decoder adds ~1 core-count frames of
         * pipeline depth — measured 2.7 s snapshot lag before these flags */
        execlp(FFMPEG_BIN, "ffmpeg", "-loglevel", "fatal", "-n",
               "-probesize", "32768", "-analyzeduration", "0",
               "-fflags", "nobuffer", "-threads", "1",
               "-f", "dv", "-i", "pipe:0",
               "-vf", "bwdif=mode=0,scale=480:-2",
               "-q:v", "4", "-update", "1", "-threads", "1", path, (char *)NULL);
        _exit(127);
    }
    close(inp[0]);
    g_snap_in = inp[1];
    g_snap_pid = pid;
    return 0;
#endif
}

/* ---- on-the-fly encoding (second ffmpeg subprocess: DV in → H.264 out) ---- */

static ring_t   g_enc_ring;                /* capture → encoder tap */
static pid_t    g_enc_pid   = 0;
static int      g_enc_in    = -1;          /* → encoder ffmpeg stdin */
static volatile int g_enc_alive = 0;
static int      g_deint     = 0;           /* bwdif 50i → 50p */
static int      g_crf       = 18;          /* quality mode (when g_vbitrate == 0) */
static int      g_vbitrate  = 0;           /* kbps; 0 = CRF quality mode */
static int      g_abitrate  = 192;         /* AAC kbps */
static char     g_preset[16] = "veryfast"; /* x264 speed/size trade-off */
static char     g_scale[24]  = "";         /* "W:H"; empty = keep source size */

static int enc_spawn_ffmpeg(const char *path)
{
#ifdef _WIN32
    (void)path;
    return -1;
#else
    int inp[2];
    if (pipe(inp) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(inp[0], 0);
        close(inp[0]); close(inp[1]);
        /* stdout unused; stderr inherited so fatal encoder errors are visible.
         * -loglevel fatal: at "error" the DV decoder spams concealment
         * messages during the expected startup burst. */
        char crf[8], vb[16], ab[16], vf[64];
        const char *argv[36];
        int n = 0;
        argv[n++] = "ffmpeg"; argv[n++] = "-loglevel"; argv[n++] = "fatal"; argv[n++] = "-n";
        argv[n++] = "-f"; argv[n++] = "dv"; argv[n++] = "-i"; argv[n++] = "pipe:0";

        vf[0] = 0;                                   /* filter chain */
        if (g_deint) snprintf(vf, sizeof(vf), "bwdif=mode=1");   /* 50i -> 50p */
        if (g_scale[0]) {                            /* scaling implies deint (fields!) */
            size_t len = strlen(vf);
            snprintf(vf + len, sizeof(vf) - len, "%sscale=%s:flags=lanczos",
                     len ? "," : "", g_scale);
        }
        if (vf[0]) { argv[n++] = "-vf"; argv[n++] = vf; }

        argv[n++] = "-c:v"; argv[n++] = "libx264";
        argv[n++] = "-preset"; argv[n++] = g_preset;
        if (g_vbitrate > 0) {                        /* size-targeted mode */
            snprintf(vb, sizeof(vb), "%dk", g_vbitrate);
            argv[n++] = "-b:v"; argv[n++] = vb;
        } else {                                     /* quality-targeted mode */
            snprintf(crf, sizeof(crf), "%d", g_crf);
            argv[n++] = "-crf"; argv[n++] = crf;
        }
        argv[n++] = "-pix_fmt"; argv[n++] = "yuv420p";
        if (!g_deint)
            /* keep interlacing, BFF like DV PAL; only -x264opts signals BFF
             * correctly — -top/-field_order/setfield get mis-signaled as TFF */
            { argv[n++] = "-x264opts"; argv[n++] = "bff=1"; }

        argv[n++] = "-c:a"; argv[n++] = "aac";
        snprintf(ab, sizeof(ab), "%dk", g_abitrate);
        argv[n++] = "-b:a"; argv[n++] = ab;
        argv[n++] = path;
        argv[n] = NULL;
        execv(FFMPEG_BIN, (char *const *)argv);
        execvp("ffmpeg", (char *const *)argv);       /* fallback: PATH search */
        _exit(127);
    }
    close(inp[0]);
    g_enc_in = inp[1];
    g_enc_pid = pid;
    return 0;
#endif
}

/* pull the timecode pack (id 0x13) out of the frame's subcode DIF blocks */
static void pv_parse_tc(const uint8_t *f, uint32_t fsz)
{
    for (uint32_t s = 0; s < fsz / (150 * 80); s++) {
        for (int b = 1; b <= 2; b++) {          /* subcode blocks of each sequence */
            const uint8_t *p = f + (s * 150 + b) * 80;
            if ((p[0] >> 5) != 1) continue;     /* SCT != subcode */
            for (int i = 3; i < 80 - 4; i++) {
                if (p[i] != 0x13) continue;
                uint8_t fr = p[i+1] & 0x3F, se = p[i+2] & 0x7F,
                        mi = p[i+3] & 0x7F, ho = p[i+4] & 0x3F;
                if ((fr & 0x0F) > 9 || fr > 0x30) continue;   /* BCD sanity */
                if ((se & 0x0F) > 9 || se > 0x59) continue;
                if ((mi & 0x0F) > 9 || mi > 0x59) continue;
                if ((ho & 0x0F) > 9 || ho > 0x23) continue;
                snprintf(g_tc_str, sizeof(g_tc_str), "%02x:%02x:%02x:%02x", ho, mi, se, fr);
                return;
            }
        }
    }
}

/* frame feeder: align a stream tap to DV frame boundaries (header 1F 07 00,
 * resync on loss) and write complete frames to a subprocess pipe.
 * Shared by the preview (throttled, parses timecode) and the encoder
 * (every frame). */
typedef struct {
    ring_t       *ring;
    int          *fd;         /* pipe to the consumer subprocess */
    double        min_dt;     /* min seconds between fed frames; 0 = feed all */
    int           parse_tc;
    volatile int *alive;
} feed_cfg_t;

static feed_cfg_t g_pv_feed_cfg, g_enc_feed_cfg;

static void *frame_feeder_main(void *arg)
{
    const feed_cfg_t *fc = (const feed_cfg_t *)arg;
    const uint32_t fsz = g_frame_size;
    uint8_t *frame = malloc(fsz);
    if (!frame) return NULL;
    size_t have = 0;
    int synced = 0;
    struct timespec last = {0, 0};

    for (;;) {
        size_t n = ring_pop(fc->ring, frame + have, fsz - have);
        if (n == 0) break;                      /* EOF */
        have += n;
        if (!synced) {
            size_t p = 0;
            while (p + 3 <= have &&
                   !(frame[p] == 0x1F && frame[p+1] == 0x07 && frame[p+2] == 0x00)) p++;
            if (p + 3 > have) {
                if (have > 2) { memmove(frame, frame + have - 2, 2); have = 2; }
                continue;
            }
            memmove(frame, frame + p, have - p);
            have -= p;
            synced = 1;
        }
        if (have < fsz) continue;
        have = 0;
        if (!(frame[0] == 0x1F && frame[1] == 0x07 && frame[2] == 0x00)) {
            synced = 0;                          /* lost alignment, resync */
            continue;
        }
        if (fc->parse_tc) pv_parse_tc(frame, fsz);

        if (fc->min_dt > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
            if (dt < fc->min_dt) continue;
            last = now;
        }
        if (!*fc->alive) continue;
        size_t off = 0;
        while (off < fsz) {
            ssize_t w = write(*fc->fd, frame + off, fsz - off);
            if (w <= 0) { *fc->alive = 0; break; }
            off += (size_t)w;
        }
        if (!*fc->alive) break;                  /* consumer died */
    }
    free(frame);
    return NULL;
}

/* raw writer for --split: frame-aligned, rotates the output file on scene
 * boundaries (replaces writer_main; consumes the same g_ring) */
static void *split_writer_main(void *arg)
{
    const char *base = (const char *)arg;
    const uint32_t fsz = g_frame_size;
    uint8_t *frame = malloc(fsz);
    if (!frame) { g_write_error = 1; return NULL; }
    size_t have = 0;
    int synced = 0, n = 0;
    scene_state_t st = { -1, -1, 0 };
    FILE *out = NULL;
    char path[1024];

    for (;;) {
        size_t got = ring_pop(&g_ring, frame + have, fsz - have);
        if (got == 0) break;
        have += got;
        if (!synced) {
            size_t p = 0;
            while (p + 3 <= have &&
                   !(frame[p] == 0x1F && frame[p+1] == 0x07 && frame[p+2] == 0x00)) p++;
            if (p + 3 > have) {
                if (have > 2) { memmove(frame, frame + have - 2, 2); have = 2; }
                continue;
            }
            memmove(frame, frame + p, have - p);
            have -= p;
            synced = 1;
        }
        if (have < fsz) continue;
        have = 0;
        if (!(frame[0] == 0x1F && frame[1] == 0x07 && frame[2] == 0x00)) { synced = 0; continue; }

        int tc, rec;
        dv_scene_sig(frame, fsz, &tc, &rec);
        int cut = scene_boundary(&st, tc, rec);
        if (!out || cut) {
            if (out) fclose(out);
            scene_filename(base, ++n, path, sizeof(path));
            out = capture_open_new_utf8(path);
            if (!out) { perror("fopen(scene)"); g_write_error = 1; break; }
            g_scenes = n;
            if (g_json) { printf("{\"type\":\"scene\",\"n\":%d,\"file\":\"%s\"}\n", n, path); fflush(stdout); }
            else printf("\nscene %03d -> %s\n", n, path);
        }
        if (fwrite(frame, 1, fsz, out) != fsz) { g_write_error = 1; break; }
        g_written += fsz;
        st.frames++;
    }
    if (out) { fflush(out); fclose(out); }
    free(frame);
    return NULL;
}

/* encoder feeder for --split: one ffmpeg per scene, restarted on boundaries
 * (the 64 MB encoder ring absorbs the restart gap) */
static void *enc_split_feeder_main(void *arg)
{
    const char *base = (const char *)arg;
    const uint32_t fsz = g_frame_size;
    uint8_t *frame = malloc(fsz);
    if (!frame) return NULL;
    size_t have = 0;
    int synced = 0, n = 0;
    scene_state_t st = { -1, -1, 0 };
    char path[1024];

    for (;;) {
        size_t got = ring_pop(&g_enc_ring, frame + have, fsz - have);
        if (got == 0) break;
        have += got;
        if (!synced) {
            size_t p = 0;
            while (p + 3 <= have &&
                   !(frame[p] == 0x1F && frame[p+1] == 0x07 && frame[p+2] == 0x00)) p++;
            if (p + 3 > have) {
                if (have > 2) { memmove(frame, frame + have - 2, 2); have = 2; }
                continue;
            }
            memmove(frame, frame + p, have - p);
            have -= p;
            synced = 1;
        }
        if (have < fsz) continue;
        have = 0;
        if (!(frame[0] == 0x1F && frame[1] == 0x07 && frame[2] == 0x00)) { synced = 0; continue; }

        int tc, rec;
        dv_scene_sig(frame, fsz, &tc, &rec);
        int cut = scene_boundary(&st, tc, rec);
        if (n == 0 || cut) {
            if (n > 0) {                     /* finalize the previous scene */
                if (g_enc_in >= 0) { close(g_enc_in); g_enc_in = -1; }
                if (g_enc_pid > 0) { capture_wait_child(g_enc_pid, NULL); g_enc_pid = 0; }
            }
            scene_filename(base, ++n, path, sizeof(path));
            if (enc_spawn_ffmpeg(path) != 0) {
                fprintf(stderr, "WARNING: encoder respawn failed at scene %d\n", n);
                g_enc_alive = 0;
                break;
            }
            g_enc_scene_n = n;
            if (!g_raw_on) g_scenes = n;     /* raw splitter owns the counter otherwise */
        }
        size_t off = 0;
        while (off < fsz) {
            ssize_t w = write(g_enc_in, frame + off, fsz - off);
            if (w <= 0) { g_enc_alive = 0; break; }
            off += (size_t)w;
        }
        if (!g_enc_alive) break;
        st.frames++;
    }
    free(frame);
    return NULL;
}

static int pv_c256(const uint8_t *p)
{
    return 16 + 36 * (p[0] * 6 / 256) + 6 * (p[1] * 6 / 256) + (p[2] * 6 / 256);
}

/* renderer thread: read decoded frames from ffmpeg, draw half-blocks + status */
static void *pv_render_main(void *arg)
{
    (void)arg;
    const int w = g_pv_w, h = g_pv_h;
    const size_t fsz = (size_t)w * h * 3;
    uint8_t *img = malloc(fsz);
    char *out = malloc((size_t)w * (h / 2) * 48 + (size_t)h * 16 + 1024);
    if (!img || !out) { free(img); free(out); g_pv_alive = 0; return NULL; }

    uint64_t last_bytes = 0;
    time_t   last_t = time(NULL);
    double   rate = 0.0;

    while (g_pv_alive) {
        size_t got = 0;
        while (got < fsz) {
            ssize_t r = read(g_pv_out, img + got, fsz - got);
            if (r <= 0) goto done;
            got += (size_t)r;
        }
        if (!g_pv_screen) {                     /* enter alt screen on first frame */
            fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);
            fflush(stdout);
            g_pv_screen = 1;
        }
        char *o = out;
        o += sprintf(o, "\x1b[H");
        for (int y = 0; y + 1 < h; y += 2) {
            for (int x = 0; x < w; x++) {
                const uint8_t *t = img + ((size_t)y * w + x) * 3;
                const uint8_t *b = img + ((size_t)(y + 1) * w + x) * 3;
                if (g_truecolor)
                    o += sprintf(o, "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xe2\x96\x80",
                                 t[0], t[1], t[2], b[0], b[1], b[2]);
                else
                    o += sprintf(o, "\x1b[38;5;%dm\x1b[48;5;%dm\xe2\x96\x80",
                                 pv_c256(t), pv_c256(b));
            }
            o += sprintf(o, "\x1b[0m\x1b[K\n");
        }
        time_t now = time(NULL);
        if (now != last_t) {
            rate = (double)(g_bytes - last_bytes) / (1024.0 * 1024.0) / (double)(now - last_t);
            last_bytes = g_bytes; last_t = now;
        }
        o += sprintf(o, "\x1b[0m\x1b[KREC %.1f MB | %.2f MB/s | TC %s | "
                        "drop=%llu ovf=%llu uvc_err=%llu | Ctrl-C stops\x1b[K",
                     g_bytes / (1024.0 * 1024.0), rate,
                     g_tc_str[0] ? g_tc_str : "--:--:--:--",
                     (unsigned long long)g_pkts_status_err,
                     (unsigned long long)g_ring.ovf,
                     (unsigned long long)g_pkts_uvc_err);
        ssize_t wr = write(STDOUT_FILENO, out, (size_t)(o - out));
        (void)wr;
    }
done:
    g_pv_alive = 0;
    if (g_pv_screen) {                          /* ffmpeg died mid-run: restore now */
        fputs("\x1b[?1049l\x1b[?25h", stdout);
        fflush(stdout);
        g_pv_screen = 0;
    }
    free(img); free(out);
    return NULL;
}

/* A retired callback must invalidate its owner's slot before freeing the
 * transfer. Otherwise shutdown cancels a dangling pointer after NO_DEVICE
 * or a failed resubmit. All application callbacks run in the event thread. */
static int g_transport_failed = 0;
static int g_device_lost = 0;
static void free_owned_transfer(struct libusb_transfer *xfer)
{
    struct libusb_transfer **slot = xfer->user_data;
    if (slot && *slot == xfer) *slot = NULL;
    libusb_free_transfer(xfer);
}

/* ---- isochronous completion: strip UVC headers, push payload, resubmit fast ---- */
static void LIBUSB_CALL xfer_cb(struct libusb_transfer *xfer)
{
    if (xfer->status == LIBUSB_TRANSFER_CANCELLED ||
        xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
            g_transport_failed = g_device_lost = 1;
            request_stop(STOP_DEVICE_LOST);
        }
        g_inflight--;
        free_owned_transfer(xfer);               /* buffer freed via FREE_BUFFER flag */
        return;
    }

    int got_data = 0;

    for (int i = 0; i < xfer->num_iso_packets; i++) {
        struct libusb_iso_packet_descriptor *d = &xfer->iso_packet_desc[i];
        if (d->status != LIBUSB_TRANSFER_COMPLETED) { g_pkts_status_err++; continue; }
        unsigned int len = d->actual_length;
        if (len == 0) continue;

        uint8_t *pkt = libusb_get_iso_packet_buffer_simple(xfer, i);
        uint8_t hlen = pkt[0];
        if (hlen < 2 || hlen > len) { g_hdr_bad++; continue; }

        int flagged = (pkt[1] & 0x40) != 0;   /* device-side error bit */
        unsigned int payload = len - hlen;

        if (flagged) {
            g_pkts_uvc_err++;
            if (!g_err_seen) { g_err_first_off = g_bytes; g_err_seen = 1; }
            g_err_last_off = g_bytes;
            g_err_run++;
            if (g_err_run > g_err_run_max) g_err_run_max = g_err_run;
            if (g_drop_err) {                 /* old v1/v2 behavior, opt-in */
                g_err_dropped_bytes += payload;
                continue;
            }
        } else if (payload) {
            g_err_run = 0;
        }

        if (payload) {
            if (g_raw_on)
                ring_push(&g_ring, pkt + hlen, payload);
            if (g_enc_alive)                      /* encoder tap */
                ring_push(&g_enc_ring, pkt + hlen, payload);
            if (g_pv_alive)                       /* best-effort preview tap */
                ring_push(&g_pring, pkt + hlen, payload);
            if (g_snap_alive)                     /* GUI snapshot tap */
                ring_push(&g_snap_ring, pkt + hlen, payload);
            g_bytes += payload;
            got_data = 1;
        }
        g_pkts_ok++;
    }

    if (got_data) g_last_data = time(NULL);

    if (!g_stop && !g_write_error) {
        int r = libusb_submit_transfer(xfer);
        if (r < 0) {
            fprintf(stderr, "\nresubmit failed: %s\n", libusb_error_name(r));
            g_transport_failed = 1;
            if (r == LIBUSB_ERROR_NO_DEVICE) g_device_lost = 1;
            request_stop(g_device_lost ? STOP_DEVICE_LOST : STOP_TRANSPORT_ERROR);
            g_inflight--;
            free_owned_transfer(xfer);
        }
    } else {
        g_inflight--;
        free_owned_transfer(xfer);
    }
}

/* ---- device discovery ---- */

typedef struct {
    libusb_device *dev;      /* borrowed reference from the device list */
    uint16_t vid, pid;
    uint8_t  bus, addr;
    int      vs_iface;       /* -1 = no VideoStreaming interface */
    int      dv_fmt_index;   /* -1 = VS present but no VS_FORMAT_DV */
    int      vc_iface;       /* -1 = no VideoControl interface */
    int      mtt_id;         /* Media Transport Terminal ID, -1 = none */
    uint32_t mtt_controls;   /* bmControls: D0 transport, D1 ATN, D2 media, D3 timecode */
    uint64_t mtt_modes;      /* bmTransportModes bitmap */
    char     name[160];
} devinfo_t;

/* scan a class-specific descriptor blob for VS_FORMAT_DV, return bFormatIndex */
static int find_dv_format(const uint8_t *p, int rem)
{
    while (rem >= 3) {
        uint8_t blen = p[0];
        if (blen < 3 || blen > rem) break;
        if (p[1] == UVC_CS_INTERFACE && p[2] == UVC_VS_FORMAT_DV && blen >= 4)
            return p[3];
        p += blen; rem -= blen;
    }
    return -1;
}

/* scan VideoControl class-specific descriptors for a Media Transport Input
 * Terminal; fill terminal ID + control/mode bitmaps (spec Table 3-1) */
static void find_media_transport(const uint8_t *p, int rem, devinfo_t *info)
{
    while (rem >= 3) {
        uint8_t blen = p[0];
        if (blen < 3 || blen > rem) break;
        if (p[1] == UVC_CS_INTERFACE && p[2] == UVC_VC_INPUT_TERMINAL &&
            blen >= 9 && rd16(p + 4) == UVC_ITT_MEDIA_TRANSPORT) {
            info->mtt_id = p[3];
            int n = p[8];                            /* bControlSize */
            for (int i = 0; i < n && i < 4 && 9 + i < blen; i++)
                info->mtt_controls |= (uint32_t)p[9 + i] << (8 * i);
            if ((info->mtt_controls & 0x01) && 9 + n < blen) {
                int m = p[9 + n];                    /* bTransportModeSize */
                for (int i = 0; i < m && i < 8 && 10 + n + i < blen; i++)
                    info->mtt_modes |= (uint64_t)p[10 + n + i] << (8 * i);
            }
            return;
        }
        p += blen; rem -= blen;
    }
}

static void analyze_device(libusb_device *dev, devinfo_t *info)
{
    memset(info, 0, sizeof(*info));
    info->dev = dev;
    info->vs_iface = -1;
    info->dv_fmt_index = -1;
    info->vc_iface = -1;
    info->mtt_id = -1;

    struct libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(dev, &dd) < 0) return;
    info->vid  = dd.idVendor;
    info->pid  = dd.idProduct;
    info->bus  = libusb_get_bus_number(dev);
    info->addr = libusb_get_device_address(dev);

    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) < 0 &&
        libusb_get_config_descriptor(dev, 0, &cfg) < 0)
        return;

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        for (int a = 0; a < cfg->interface[i].num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[a];
            if (id->bInterfaceClass != UVC_CC_VIDEO) continue;
            if (id->bInterfaceSubClass == UVC_SC_VIDEOSTREAMING) {
                if (info->vs_iface < 0) info->vs_iface = id->bInterfaceNumber;
                if (info->dv_fmt_index < 0 && id->extra_length > 0)
                    info->dv_fmt_index = find_dv_format(id->extra, id->extra_length);
            } else if (id->bInterfaceSubClass == UVC_SC_VIDEOCONTROL) {
                if (info->vc_iface < 0) info->vc_iface = id->bInterfaceNumber;
                if (info->mtt_id < 0 && id->extra_length > 0)
                    find_media_transport(id->extra, id->extra_length, info);
            }
        }
    }
    /* some devices attach class-specific descriptors at config level */
    if (info->vs_iface >= 0 && info->dv_fmt_index < 0 && cfg->extra_length > 0)
        info->dv_fmt_index = find_dv_format(cfg->extra, cfg->extra_length);

    libusb_free_config_descriptor(cfg);

    /* product string, best effort (open can fail without permissions) */
    libusb_device_handle *h = NULL;
    if (libusb_open(dev, &h) == 0) {
        unsigned char m[64] = "", p[64] = "";
        if (dd.iManufacturer) libusb_get_string_descriptor_ascii(h, dd.iManufacturer, m, sizeof(m));
        if (dd.iProduct)      libusb_get_string_descriptor_ascii(h, dd.iProduct,      p, sizeof(p));
        snprintf(info->name, sizeof(info->name), "%s%s%s",
                 m, (m[0] && p[0]) ? " " : "", p);
        libusb_close(h);
    }
}

/* ---- tape transport control (Media Transport Terminal) ---- */

static const char *xprt_name(uint8_t m)
{
    switch (m) {
    case 0x06:              return "Play x1";
    case XPRT_PLAY_FORWARD: return "Play Forward";
    case XPRT_PAUSE:        return "Pause";
    case 0x1A:              return "Reverse Pause";
    case XPRT_STOP:         return "Stop";
    case XPRT_FAST_FORWARD: return "Fast Forward";
    case XPRT_REWIND:       return "Rewind";
    case XPRT_HS_REWIND:    return "High Speed Rewind";
    case 0x50:              return "Record Start";
    case 0x51:              return "Record Pause";
    case XPRT_EJECT:        return "Eject";
    case 0x70:              return "Playing (unspecified slow forward)";
    case 0x71:              return "Playing (unspecified fast forward)";
    case 0x72:              return "Playing (unspecified slow reverse)";
    case 0x73:              return "Playing (unspecified fast reverse)";
    case 0x74:              return "Stopped (top of tape)";
    case 0x75:              return "Stopped (end of tape)";
    case 0x76:              return "Stopped (emergency)";
    case 0x77:              return "Stopped (condensation)";
    case 0x7F:              return "Unspecified";
    default:                return "unknown mode";
    }
}

/* bmTransportModes bit index for a given SET_CUR mode value (Table 3-1 vs 4-3) */
static int xprt_mode_bit(uint8_t mode)
{
    switch (mode) {
    case XPRT_PLAY_FORWARD: return 0;
    case XPRT_PAUSE:        return 1;
    case XPRT_REWIND:       return 2;
    case XPRT_FAST_FORWARD: return 3;
    case XPRT_HS_REWIND:    return 4;
    case XPRT_STOP:         return 5;
    case XPRT_EJECT:        return 6;
    default:                return -1;
    }
}

static void xprt_print_supported(uint64_t modes)
{
    static const char *bits[] = {
        "Play Forward", "Pause", "Rewind", "Fast Forward", "High Speed Rewind",
        "Stop", "Eject", "Play Next Frame", "Play Slowest Fwd", "Play Slow Fwd 4",
        "Play Slow Fwd 3", "Play Slow Fwd 2", "Play Slow Fwd 1", "Play x1",
        "Play Fast Fwd 1", "Play Fast Fwd 2", "Play Fast Fwd 3", "Play Fast Fwd 4",
        "Play Fastest Fwd", "Play Prev Frame", "Play Slowest Rev", "Play Slow Rev 4",
        "Play Slow Rev 3", "Play Slow Rev 2", "Play Slow Rev 1", "Play x1 Rev",
        "Play Fast Rev 1", "Play Fast Rev 2", "Play Fast Rev 3", "Play Fast Rev 4",
        "Play Fastest Rev", "Record Start", "Record Pause", "Reverse Pause",
    };
    printf("tape transport modes:");
    for (int i = 0; i < (int)(sizeof(bits)/sizeof(bits[0])); i++)
        if (modes & (1ULL << i)) printf(" [%s]", bits[i]);
    printf("\n");
}

static int mtt_windex(const devinfo_t *d) { return (d->mtt_id << 8) | d->vc_iface; }

/* on a stalled request, ask the VC interface why (VC_REQUEST_ERROR_CODE_CONTROL) */
static void vc_print_request_error(libusb_device_handle *h, int vc_iface)
{
    static const char *reasons[] = {
        "no error", "not ready", "wrong state", "power", "out of range",
        "invalid unit", "invalid control", "invalid request", "invalid value",
    };
    uint8_t code = 0xFF;
    int r = libusb_control_transfer(h, 0xA1, UVC_GET_CUR,
            (0x02 << 8) /* VC_REQUEST_ERROR_CODE_CONTROL */, vc_iface, &code, 1, 1000);
    if (r == 1)
        fprintf(stderr, "  device error code: 0x%02x (%s)\n", code,
                code < sizeof(reasons)/sizeof(reasons[0]) ? reasons[code] : "?");
}

static int mtt_set_transport(libusb_device_handle *h, const devinfo_t *d, uint8_t mode)
{
    return libusb_control_transfer(h, 0x21, UVC_SET_CUR,
            (MTT_TRANSPORT_CONTROL << 8), mtt_windex(d), &mode, 1, 1000);
}

/* One SET only. Reset/retry can invalidate streaming interfaces and destroy
 * an otherwise valid capture. Retain the legacy internal name for callers. */
static int mtt_set_transport_retry(libusb_device_handle *h, const devinfo_t *d, uint8_t mode)
{
    return mtt_set_transport(h, d, mode);
}

/* print current transport state and (if supported) tape timecode */
static int mtt_print_status(libusb_device_handle *h, const devinfo_t *d)
{
    int rc = 0;
    uint8_t st = 0;
    int r = libusb_control_transfer(h, 0xA1, UVC_GET_CUR,
            (MTT_TRANSPORT_CONTROL << 8), mtt_windex(d), &st, 1, 1000);
    if (r == 1) {
        printf("transport state: 0x%02x (%s)\n", st, xprt_name(st));
    } else {
        fprintf(stderr, "GET_CUR(transport): %s\n", libusb_error_name(r));
        if (r == LIBUSB_ERROR_PIPE) {
            vc_print_request_error(h, d->vc_iface);
            fprintf(stderr, "  (known quirk: the control stalls after any transport command\n"
                            "   until the device resets; timecode below is still live)\n");
        }
        rc = 1;
    }

    uint8_t inf = 0;
    r = libusb_control_transfer(h, 0xA1, UVC_GET_INFO,
            (MTT_TRANSPORT_CONTROL << 8), mtt_windex(d), &inf, 1, 1000);
    if (r == 1)
        printf("transport control caps: GET=%s SET=%s\n",
               (inf & 1) ? "yes" : "no", (inf & 2) ? "yes" : "no");

    if (d->mtt_controls & 0x08) {   /* D3: Time Code Information */
        uint8_t tc[4] = {0};        /* bcdFrame, bcdSecond, bcdMinute, bcdHour */
        r = libusb_control_transfer(h, 0xA1, UVC_GET_CUR,
                (MTT_TIME_CODE_INFORMATION << 8), mtt_windex(d), tc, 4, 1000);
        if (r == 4) {               /* BCD prints correctly as hex */
            printf("tape timecode: %02x:%02x:%02x:%02x%s\n",
                   tc[3] & 0x3F, tc[2] & 0x7F, tc[1] & 0x7F, tc[0] & 0x3F,
                   (tc[0] & 0x80) ? " (possible discontinuity)" : "");
        } else {
            fprintf(stderr, "GET_CUR(timecode): %s\n", libusb_error_name(r));
            if (r == LIBUSB_ERROR_PIPE) vc_print_request_error(h, d->vc_iface);
        }
    }
    if (d->mtt_modes) xprt_print_supported(d->mtt_modes);
    return rc;
}

/* standalone --tape command; returns process exit code */
static void json_string(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

/* GET-only status; a single 0x75 report cannot verify physical end. */
static int mtt_print_status_json(libusb_device_handle *h, const devinfo_t *d)
{
    uint8_t state = 0, tc[4] = {0};
    int sr = libusb_control_transfer(h, 0xA1, UVC_GET_CUR,
            MTT_TRANSPORT_CONTROL << 8, mtt_windex(d), &state, 1, 1000);
    int tr = d->mtt_controls & 0x08 ? libusb_control_transfer(h, 0xA1, UVC_GET_CUR,
            MTT_TIME_CODE_INFORMATION << 8, mtt_windex(d), tc, 4, 1000) : LIBUSB_ERROR_NOT_SUPPORTED;
    printf("{\"type\":\"tape\",\"state_result\":%d,\"state\":", sr);
    if (sr == 1) printf("%u", state); else fputs("null", stdout);
    printf(",\"timecode_result\":%d,\"tc\":", tr);
    if (tr == 4) printf("\"%02x:%02x:%02x:%02x\"", tc[3]&0x3f, tc[2]&0x7f, tc[1]&0x7f, tc[0]&0x3f);
    else fputs("null", stdout);
    printf(",\"physical_end_verified\":false}\n");
    fflush(stdout);
    return sr == 1 ? 0 : 1;
}

static int do_tape_cmd(libusb_device_handle *h, const devinfo_t *d, const char *cmd)
{
    if (d->vc_iface < 0 || d->mtt_id < 0) {
        fprintf(stderr, "device does not advertise a Media Transport Terminal — "
                        "tape control unavailable\n");
        return 1;
    }
    libusb_set_auto_detach_kernel_driver(h, 1);
    int claimed = (libusb_claim_interface(h, d->vc_iface) == 0);
    if (!claimed)
        fprintf(stderr, "note: could not claim VideoControl interface %d, trying anyway\n",
                d->vc_iface);

    int rc;
    if (!strcmp(cmd, "status-json")) {
        rc = mtt_print_status_json(h, d);
    } else if (!strcmp(cmd, "status")) {
        rc = mtt_print_status(h, d);
    } else {
        static const struct { const char *name; uint8_t mode; } cmds[] = {
            { "play",     XPRT_PLAY_FORWARD },
            { "pause",    XPRT_PAUSE        },
            { "stop",     XPRT_STOP         },
            { "ff",       XPRT_FAST_FORWARD },
            { "rew",      XPRT_REWIND       },
            { "rew-fast", XPRT_HS_REWIND    },
            { "eject",    XPRT_EJECT        },
        };
        int mode = -1;
        for (int i = 0; i < (int)(sizeof(cmds)/sizeof(cmds[0])); i++)
            if (!strcmp(cmd, cmds[i].name)) { mode = cmds[i].mode; break; }
        if (mode < 0) {
            fprintf(stderr, "unknown tape command '%s' "
                            "(play|pause|stop|ff|rew|rew-fast|eject|status)\n", cmd);
            rc = 2;
        } else {
            int bit = xprt_mode_bit((uint8_t)mode);
            if (d->mtt_modes && bit >= 0 && !(d->mtt_modes & (1ULL << bit)))
                fprintf(stderr, "note: device does not advertise '%s' support, sending anyway\n", cmd);
            int r = mtt_set_transport_retry(h, d, (uint8_t)mode);
            if (g_json) {
                printf("{\"type\":\"transport_set\",\"action\":");
                json_string(cmd);
                printf(",\"mode\":%u,\"set_result\":%d}\n", mode, r);
                fflush(stdout);
            }
            if (r != 1) {
                fprintf(stderr, "tape %s failed: %s%s\n", cmd, libusb_error_name(r),
                        r == LIBUSB_ERROR_PIPE ? " (device rejected the mode)" : "");
                if (r == LIBUSB_ERROR_PIPE) vc_print_request_error(h, d->vc_iface);
                rc = 1;
            } else {
                if (!g_json) printf("tape: %s\n", xprt_name((uint8_t)mode));
                if (!g_json && (d->mtt_controls & 0x08)) {   /* timecode readable even after the quirk */
                    uint8_t tc[4] = {0};
                    if (libusb_control_transfer(h, 0xA1, UVC_GET_CUR,
                            (MTT_TIME_CODE_INFORMATION << 8), mtt_windex(d), tc, 4, 1000) == 4)
                        printf("tape timecode: %02x:%02x:%02x:%02x\n",
                               tc[3] & 0x3F, tc[2] & 0x7F, tc[1] & 0x7F, tc[0] & 0x3F);
                }
                rc = 0;
            }
        }
    }
    if (claimed) libusb_release_interface(h, d->vc_iface);
    return rc;
}

static int list_devices(libusb_context *ctx)
{
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) { fprintf(stderr, "device list: %s\n", libusb_error_name((int)n)); return 1; }

    int uvc = 0, dv = 0;
    printf("UVC video devices:\n");
    for (ssize_t i = 0; i < n; i++) {
        devinfo_t di;
        analyze_device(list[i], &di);
        if (di.vs_iface < 0) continue;
        uvc++;
        if (di.dv_fmt_index >= 0) dv++;
        printf("  %04x:%04x  bus %u addr %-3u  VS iface %d  DV payload: %s  tape-ctl: %s  %s\n",
               di.vid, di.pid, di.bus, di.addr, di.vs_iface,
               di.dv_fmt_index >= 0 ? "YES" : "no ",
               di.mtt_id >= 0 ? "yes" : "no ",
               di.name);
    }
    libusb_free_device_list(list, 1);

    if (uvc == 0)
        printf("  (none found)\n"
               "Camcorder checklist: AC power, USB FUNCTION > Motion DV, VCR mode, tape playing,\n"
               "then plug in USB.\n");
    else if (dv == 0)
        printf("No device exposes VS_FORMAT_DV. A camera in \"Web Camera\" USB mode shows up as a\n"
               "plain UVC webcam — switch it to the DV/Motion DV USB mode and reconnect.\n");
    else
        printf("%d capture candidate(s). Run without --vid/--pid if only one is connected.\n", dv);
    return 0;
}

static int list_devices_json(libusb_context *ctx)
{
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) { fprintf(stderr, "device list: %s\n", libusb_error_name((int)n)); return 1; }
    int count = 0, candidates = 0;
    fputs("{\"type\":\"devices\",\"devices\":[", stdout);
    for (ssize_t i = 0; i < n; i++) {
        devinfo_t di;
        analyze_device(list[i], &di);
        if (di.vs_iface < 0) continue;
        if (count++) putchar(',');
        if (di.dv_fmt_index >= 0) candidates++;
        printf("{\"vid\":%u,\"pid\":%u,\"bus\":%u,\"address\":%u,\"vs_iface\":%d,"
               "\"dv_format_index\":%d,\"vc_iface\":%d,\"mtt_id\":%d,\"dv\":%s,\"name\":",
               di.vid, di.pid, di.bus, di.addr, di.vs_iface, di.dv_fmt_index,
               di.vc_iface, di.mtt_id, di.dv_fmt_index >= 0 ? "true" : "false");
        json_string(di.name);
        putchar('}');
    }
    libusb_free_device_list(list, 1);
    printf("],\"count\":%d,\"capture_candidates\":%d}\n", count, candidates);
    fflush(stdout);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "uvcdv-capture %s — raw DV capture over USB (VS_FORMAT_DV)\n"
        "usage: %s [options] out.dv\n"
        "       %s --list | --list-json | --probe | --tape CMD\n"
        "device: --vid 0xXXXX --pid 0xXXXX --iface N --format-index N\n"
        "        --frame-index N --alt N --ep 0xNN\n"
        "capture: --transfers N (32) --pkts N (128) --ring-mb MB (64)\n"
        "         --drop-err (opt-in; ordinarily flagged DV payload is preserved)\n"
        "session: --idle SEC --max-sec SEC (0 disables either; neither verifies end)\n"
        "         --stop-on-stdin (stop line or parent pipe EOF; Windows needs pipe)\n"
        "         --status-json --monitor-tape\n"
        "         --stop-after-camera-stop SEC (continuous Stop64 after observed\n"
        "         playback and captured data; ordinary stop is not physical end)\n"
        "tape: --tape play|pause|stop|ff|rew|rew-fast|eject|status|status-json\n"
        "      --play-after-stream (iso ring ready, then one Play SET; no retries)\n"
        "      --play (same behavior) --stop (one SET after capture)\n"
        "legacy Unix features: --encode FILE --preview --snapshot FILE --split\n"
        "      --deint --crf N --vbitrate KBPS --abitrate KBPS --preset NAME --scale WxH\n"
        "      encoded positional .mp4/.mov/.mkv/.m4v output also remains accepted\n"
        "Windows supports one raw DV output: encode/preview/snapshot/split rejected.\n"
        "Existing output files are never overwritten.\n"
        "--verbose/-v --version --help/-h\n",
        UVCDV_VERSION, argv0, argv0);
}

static int uvcdv_main(int argc, char **argv)
{
    capture_init_stdio();
    uint16_t vid = 0, pid = 0;          /* 0 = any (auto-detect) */
    const char *rawpath = NULL, *encpath = NULL, *snappath = NULL;
    const char *tape_cmd = NULL;
    int do_list = 0, do_probe = 0, auto_play = 0, auto_stop = 0;
    int usb_cleanup_incomplete = 0;
    int exit_ok = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--vid") && i+1 < argc)          vid = (uint16_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pid") && i+1 < argc)          pid = (uint16_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--iface") && i+1 < argc)        g_force_iface = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--format-index") && i+1 < argc) g_force_fmt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frame-index") && i+1 < argc)  g_force_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--alt") && i+1 < argc)          g_force_alt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ep") && i+1 < argc)           g_force_ep = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--transfers") && i+1 < argc)    g_num_transfers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pkts") && i+1 < argc)         g_pkts_per_xfer = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ring-mb") && i+1 < argc)      g_ring_mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle") && i+1 < argc)         g_idle_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-sec") && i+1 < argc)      g_max_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--drop-err"))                   g_drop_err = 1;
        else if (!strcmp(argv[i], "--preview"))                    g_preview = 1;
        else if (!strcmp(argv[i], "--status-json"))                g_json = 1;
        else if (!strcmp(argv[i], "--split"))                      g_split = 1;
        else if (!strcmp(argv[i], "--snapshot") && i+1 < argc)     snappath = argv[++i];
        else if (!strcmp(argv[i], "--encode") && i+1 < argc)       encpath = argv[++i];
        else if (!strcmp(argv[i], "--deint"))                      g_deint = 1;
        else if (!strcmp(argv[i], "--crf") && i+1 < argc)          g_crf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vbitrate") && i+1 < argc)     g_vbitrate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--abitrate") && i+1 < argc)     g_abitrate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--preset") && i+1 < argc)       snprintf(g_preset, sizeof(g_preset), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i+1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
                snprintf(g_scale, sizeof(g_scale), "%d:%d", w, h);
            else { fprintf(stderr, "--scale expects WxH, e.g. 640x480\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--list"))                       do_list = 1;
        else if (!strcmp(argv[i], "--list-json"))                  { do_list = 2; g_json = 1; }
        else if (!strcmp(argv[i], "--play-after-stream"))          g_play_after_stream = 1;
        else if (!strcmp(argv[i], "--stop-on-stdin"))              g_stop_on_stdin = 1;
        else if (!strcmp(argv[i], "--stop-after-camera-stop") && i+1 < argc) {
            g_stop_after_camera_stop = atoi(argv[++i]); g_monitor_tape = 1;
        }
        else if (!strcmp(argv[i], "--probe"))                      do_probe = 1;
        else if (!strcmp(argv[i], "--monitor-tape"))               g_monitor_tape = 1;
        else if (!strcmp(argv[i], "--tape") && i+1 < argc)         tape_cmd = argv[++i];
        else if (!strcmp(argv[i], "--play"))                       auto_play = 1;
        else if (!strcmp(argv[i], "--stop"))                       auto_stop = 1;
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--version"))                    { printf("uvcdv-capture %s\n", UVCDV_VERSION); return 0; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\n\n", argv[i]); usage(argv[0]); return 2; }
        else {
            /* route the positional output by extension */
            const char *dot = strrchr(argv[i], '.');
            if (dot && (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".mov") ||
                        !strcasecmp(dot, ".mkv") || !strcasecmp(dot, ".m4v"))) {
                if (encpath) { fprintf(stderr, "two encoded outputs given\n"); return 2; }
                encpath = argv[i];
            } else {
                if (rawpath) { fprintf(stderr, "two raw outputs given\n"); return 2; }
                rawpath = argv[i];
            }
        }
    }
    if (tape_cmd && (rawpath || encpath)) {
        fprintf(stderr, "--tape is a standalone command; to control the tape around a capture\n"
                        "use --play / --stop together with the output file instead.\n");
        return 2;
    }
    if (!do_list && !do_probe && !tape_cmd &&
        ((!rawpath && !encpath) || g_num_transfers < 1 || g_pkts_per_xfer < 1 || g_ring_mb < 4)) {
        usage(argv[0]);
        return 2;
    }
    if (g_num_transfers < 1 || g_num_transfers > 128 || g_pkts_per_xfer < 1 ||
            g_pkts_per_xfer > 1024 || g_ring_mb < 4 || g_ring_mb > 1024 ||
            g_idle_sec < 0 || g_max_sec < 0 || g_stop_after_camera_stop < 0 ||
            g_stop_after_camera_stop > 86400) {
        fprintf(stderr, "capture limits invalid: transfers 1..128, pkts 1..1024, ring 4..1024 MB, nonnegative times\n");
        return 2;
    }
#ifdef _WIN32
    if (encpath || snappath || g_preview || g_split) {
        fprintf(stderr, "Windows backend supports one raw DV output only; --encode, encoded outputs, --snapshot, --preview, and --split are unavailable\n");
        return 2;
    }
#endif
    if (rawpath && !g_split && capture_path_exists_utf8(rawpath) != 0) {
        fprintf(stderr, "output exists or cannot be checked; refusing overwrite: %s\n", rawpath);
        return 2;
    }
    if (encpath && !g_split && capture_path_exists_utf8(encpath) != 0) {
        fprintf(stderr, "encoded output exists or cannot be checked; refusing overwrite: %s\n", encpath);
        return 2;
    }
    /* --play remains accepted, now with safer post-submission timing. */
    if (auto_play) g_play_after_stream = 1;
    auto_play = 0;
    if (g_play_after_stream || g_stop_after_camera_stop) g_monitor_tape = 1;
    if (encpath && !have_ffmpeg()) {
        fprintf(stderr, "encoding requires ffmpeg in PATH (brew install ffmpeg)\n");
        return 2;
    }
    if ((g_deint || g_crf != 18 || g_vbitrate || g_scale[0]) && !encpath)
        fprintf(stderr, "note: encoder options have no effect without an encoded output\n");
    if (encpath && g_scale[0] && !g_deint) {
        fprintf(stderr, "note: scaling interlaced video mixes the fields — enabling --deint\n");
        g_deint = 1;
    }

    signal(SIGINT, on_sigint);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   /* a dying preview ffmpeg must not kill the capture */
#endif

#ifdef __APPLE__
    /* keep the USB event thread hard to preempt */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    libusb_context *ctx = NULL;
    int r = libusb_init(&ctx);
    if (r < 0) { fprintf(stderr, "libusb_init: %s\n", libusb_error_name(r)); return 1; }

    if (do_list) { int rc = do_list == 2 ? list_devices_json(ctx) : list_devices(ctx); libusb_exit(ctx); return rc; }

    /* --- pick the device: scan for VS_FORMAT_DV, honoring the --vid/--pid filter --- */
    libusb_device **devlist = NULL;
    ssize_t ndev = libusb_get_device_list(ctx, &devlist);
    if (ndev < 0) { fprintf(stderr, "device list: %s\n", libusb_error_name((int)ndev)); libusb_exit(ctx); return 1; }

    devinfo_t match[8];
    int nmatch = 0, uvc_nodv = 0;
    for (ssize_t i = 0; i < ndev; i++) {
        devinfo_t di;
        analyze_device(devlist[i], &di);
        if (di.vs_iface < 0) continue;
        if (vid && di.vid != vid) continue;
        if (pid && di.pid != pid) continue;
        if (di.dv_fmt_index < 0 && g_force_fmt < 0) { uvc_nodv++; continue; }
        if (nmatch < (int)(sizeof(match)/sizeof(match[0]))) match[nmatch++] = di;
    }

    if (nmatch == 0) {
        if (uvc_nodv > 0)
            fprintf(stderr, "found %d UVC video device(s), but none exposes VS_FORMAT_DV.\n"
                            "A camera in \"Web Camera\" USB mode shows up as a plain webcam — switch it\n"
                            "to the DV/Motion DV USB mode and reconnect. (--list shows what was found;\n"
                            "--format-index N forces capture from a device without the descriptor.)\n",
                    uvc_nodv);
        else
            fprintf(stderr, "no UVC-DV device found%s.\n"
                            "Camcorder checklist: AC power, USB FUNCTION > Motion DV, VCR mode, tape\n"
                            "playing, then plug in USB. Run --list to see all UVC devices.\n",
                    vid || pid ? " matching --vid/--pid" : "");
        libusb_free_device_list(devlist, 1);
        libusb_exit(ctx); return 1;
    }
    if (nmatch > 1) {
        fprintf(stderr, "%d UVC-DV devices connected — pick one with --vid/--pid:\n", nmatch);
        for (int i = 0; i < nmatch; i++)
            fprintf(stderr, "  --vid 0x%04x --pid 0x%04x   %s\n", match[i].vid, match[i].pid, match[i].name);
        libusb_free_device_list(devlist, 1);
        libusb_exit(ctx); return 1;
    }

    devinfo_t sel = match[0];
    libusb_device_handle *handle = NULL;
    r = libusb_open(sel.dev, &handle);
    libusb_free_device_list(devlist, 1);   /* handle keeps its own device reference */
    if (r < 0) {
        fprintf(stderr, "open %04x:%04x: %s\n", sel.vid, sel.pid, libusb_error_name(r));
        libusb_exit(ctx); return 1;
    }
    libusb_device *dev = libusb_get_device(handle);
    if (!tape_cmd || strcmp(tape_cmd, "status-json"))
    fprintf(g_json ? stderr : stdout, "opened %04x:%04x %s%s%s\n", sel.vid, sel.pid,
           sel.name[0] ? "(" : "", sel.name, sel.name[0] ? ")" : "");

    if (tape_cmd) {
        int rc = do_tape_cmd(handle, &sel, tape_cmd);
        libusb_close(handle);
        libusb_exit(ctx);
        return rc;
    }

    if ((g_play_after_stream || auto_stop) && sel.mtt_id < 0)
        fprintf(stderr, "WARNING: --play/--stop requested but the device has no "
                        "Media Transport Terminal\n");

    struct libusb_config_descriptor *cfg = NULL;
    r = libusb_get_active_config_descriptor(dev, &cfg);
    if (r < 0) { fprintf(stderr, "get_config: %s\n", libusb_error_name(r)); goto fail; }

    int vs_iface = (g_force_iface >= 0) ? g_force_iface : sel.vs_iface;
    if (vs_iface < 0 || vs_iface >= cfg->bNumInterfaces) {
        fprintf(stderr, "VideoStreaming interface %d out of range\n", vs_iface);
        goto fail_cfg;
    }
    printf("VideoStreaming interface = %d%s\n", vs_iface, g_force_iface >= 0 ? " (forced)" : "");

    int dv_fmt_index = (g_force_fmt >= 0) ? g_force_fmt : sel.dv_fmt_index;
    if (dv_fmt_index < 0) {
        fprintf(stderr, "WARNING: no VS_FORMAT_DV descriptor; defaulting bFormatIndex=1 "
                        "(override with --format-index)\n");
        dv_fmt_index = 1;
    }
    printf("DV bFormatIndex = %d%s\n", dv_fmt_index, g_force_fmt >= 0 ? " (forced)" : "");

    /* --- claim --- */
    libusb_set_auto_detach_kernel_driver(handle, 1);
    r = libusb_claim_interface(handle, vs_iface);
    if (r < 0) {
        fprintf(stderr, "claim_interface(%d) failed: %s\n", vs_iface, libusb_error_name(r));
        goto fail_cfg;
    }
    printf("claimed interface %d\n", vs_iface);
    libusb_set_interface_alt_setting(handle, vs_iface, 0);

    /* --- PROBE/COMMIT --- */
    uint8_t lenbuf[2] = {26, 0};
    r = libusb_control_transfer(handle, 0xA1, UVC_GET_LEN,
            (UVC_VS_PROBE_CONTROL << 8), vs_iface, lenbuf, 2, 1000);
    int plen = (r == 2) ? rd16(lenbuf) : 26;
    if (plen < 26 || plen > 48) plen = 26;
    if (g_verbose) printf("probe control length = %d\n", plen);

    uint8_t probe[48]; memset(probe, 0, sizeof(probe));
    r = libusb_control_transfer(handle, 0xA1, UVC_GET_DEF,
            (UVC_VS_PROBE_CONTROL << 8), vs_iface, probe, plen, 1000);
    if (r < 0) memset(probe, 0, sizeof(probe));
    else if (g_verbose)
        printf("GET_DEF: bFormatIndex=%d bFrameIndex=%d\n",
               probe[OFF_bFormatIndex], probe[OFF_bFrameIndex]);
    probe[OFF_bFormatIndex] = (uint8_t)dv_fmt_index;
    if (g_force_frame >= 0)               probe[OFF_bFrameIndex] = (uint8_t)g_force_frame;
    else if (probe[OFF_bFrameIndex] == 0) probe[OFF_bFrameIndex] = 1;

    r = libusb_control_transfer(handle, 0x21, UVC_SET_CUR,
            (UVC_VS_PROBE_CONTROL << 8), vs_iface, probe, plen, 1000);
    if (r != plen) { fprintf(stderr, "SET_CUR(probe): short/error result %d\n", r); goto fail_iface; }

    uint8_t neg[48]; memset(neg, 0, sizeof(neg));
    r = libusb_control_transfer(handle, 0xA1, UVC_GET_CUR,
            (UVC_VS_PROBE_CONTROL << 8), vs_iface, neg, plen, 1000);
    if (r != plen) { fprintf(stderr, "GET_CUR(probe): short/error result %d\n", r); goto fail_iface; }

    uint32_t maxpayld  = rd32(neg + OFF_dwMaxPayloadXfer);
    uint32_t framesize = rd32(neg + OFF_dwMaxVideoFrameSize);
    const char *dvsys = framesize == 144000 ? " (PAL DV frame)"
                      : framesize == 120000 ? " (NTSC DV frame)" : "";
    printf("negotiated: bFormatIndex=%d bFrameIndex=%d dwMaxVideoFrameSize=%u%s "
           "dwMaxPayloadTransferSize=%u\n",
           neg[OFF_bFormatIndex], neg[OFF_bFrameIndex], framesize, dvsys, maxpayld);
    if (framesize >= 50000 && framesize <= 1000000)
        g_frame_size = framesize;           /* used for frame math in diagnostics */

    r = libusb_control_transfer(handle, 0x21, UVC_SET_CUR,
            (UVC_VS_COMMIT_CONTROL << 8), vs_iface, neg, plen, 1000);
    if (r != plen) { fprintf(stderr, "SET_CUR(commit): short/error result %d\n", r); goto fail_iface; }
    printf("committed\n");

    /* --- pick alt-setting --- */
    int best_alt = -1, best_pktsz = 0, best_interval = 0; uint8_t ep_addr = 0;
    for (int a = 0; a < cfg->interface[vs_iface].num_altsetting; a++) {
        const struct libusb_interface_descriptor *id = &cfg->interface[vs_iface].altsetting[a];
        if (id->bAlternateSetting == 0) continue;
        if (g_force_alt >= 0 && id->bAlternateSetting != g_force_alt) continue;
        for (int e = 0; e < id->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
            if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) continue;
            if ((ep->bEndpointAddress & 0x80) == 0) continue;
            if (g_force_ep && ep->bEndpointAddress != g_force_ep) continue;
            int sz = libusb_get_max_iso_packet_size(dev, ep->bEndpointAddress);
            if (sz <= 0) continue;
            int fits = (maxpayld == 0) || ((uint32_t)sz >= maxpayld);
            if (best_alt < 0 ||
                (fits && (best_pktsz < (int)maxpayld || sz < best_pktsz)) ||
                (!fits && sz > best_pktsz)) {
                best_alt = id->bAlternateSetting; best_pktsz = sz; ep_addr = ep->bEndpointAddress;
                best_interval = ep->bInterval;
            }
        }
    }
    if (best_alt < 0) {
        fprintf(stderr, "no usable isochronous IN endpoint on VS interface%s\n",
                (g_force_alt >= 0 || g_force_ep) ? " (matching --alt/--ep)" : "");
        goto fail_iface;
    }
    printf("selected alt=%d  endpoint=0x%02x  iso_packet_size=%d%s\n",
           best_alt, ep_addr, best_pktsz, g_force_alt >= 0 ? " (forced)" : "");

    if (do_probe) {
        if (sel.mtt_id >= 0) {
            printf("tape control: Media Transport Terminal id=%d (controls: transport%s%s%s)\n",
                   sel.mtt_id,
                   (sel.mtt_controls & 0x02) ? ", ATN" : "",
                   (sel.mtt_controls & 0x04) ? ", media-info" : "",
                   (sel.mtt_controls & 0x08) ? ", timecode" : "");
            if (sel.mtt_modes) xprt_print_supported(sel.mtt_modes);
        } else {
            printf("tape control: no Media Transport Terminal advertised\n");
        }
        printf("probe OK: device negotiates the DV stream — ready to capture.\n");
        libusb_release_interface(handle, vs_iface);
        libusb_free_config_descriptor(cfg);
        libusb_close(handle);
        libusb_exit(ctx);
        return 0;
    }

#ifdef _WIN32
    /* WinUSB registers iso buffers on complete USB-frame boundaries. */
    int speed = libusb_get_device_speed(dev);
    if (speed == LIBUSB_SPEED_HIGH || speed >= LIBUSB_SPEED_SUPER) {
        int packets_per_frame = best_interval >= 1 && best_interval <= 3 ?
                8 >> (best_interval - 1) : 1;
        if (g_pkts_per_xfer % packets_per_frame) {
            fprintf(stderr, "WinUSB iso --pkts must be a multiple of %d for bInterval=%d\n", packets_per_frame, best_interval);
            goto fail_iface;
        }
    } else if (g_pkts_per_xfer > 255) {
        fprintf(stderr, "WinUSB full-speed iso --pkts must not exceed 255\n");
        goto fail_iface;
    }
#else
    (void)best_interval;
#endif
    if (best_pktsz > INT_MAX / g_pkts_per_xfer) {
        fprintf(stderr, "iso transfer allocation exceeds supported size\n");
        goto fail_iface;
    }
    r = libusb_set_interface_alt_setting(handle, vs_iface, best_alt);
    if (r < 0) { fprintf(stderr, "set_alt(%d): %s\n", best_alt, libusb_error_name(r)); goto fail_iface; }

    /* --- raw output file + ring + writer thread (skipped in encode-only mode) --- */
    pthread_t writer;
    if (rawpath) {
        if (!g_split) {
            g_out = capture_open_new_utf8(rawpath);
            if (!g_out) { perror("fopen"); goto fail_iface; }
        }
        if (ring_init(&g_ring, (size_t)g_ring_mb << 20) < 0) { fprintf(stderr, "ring alloc failed\n"); goto fail_iface; }
        if (pthread_create(&writer, NULL,
                           g_split ? split_writer_main : writer_main,
                           g_split ? (void *)rawpath : (void *)&g_ring) != 0) {
            fprintf(stderr, "writer thread failed\n"); goto fail_iface;
        }
        g_raw_on = 1;
    }

    /* --- submit transfer ring --- */
    struct libusb_transfer **xfers = calloc(g_num_transfers, sizeof(*xfers));
    if (!xfers) {
        fprintf(stderr, "transfer ring alloc failed\n");
        g_transport_failed = 1;
        if (g_raw_on) { ring_set_eof(&g_ring); pthread_join(writer, NULL); }
        goto fail_iface;
    }
    int xfer_buf = best_pktsz * g_pkts_per_xfer;
    for (int t = 0; t < g_num_transfers; t++) {
        struct libusb_transfer *xfer = libusb_alloc_transfer(g_pkts_per_xfer);
        uint8_t *buf = malloc(xfer_buf);
        if (!xfer || !buf) {
            fprintf(stderr, "alloc failed at %d\n", t);
            free(buf);
            if (xfer) libusb_free_transfer(xfer);
            g_transport_failed = 1;
            break;
        }
        xfers[t] = xfer;
        libusb_fill_iso_transfer(xfer, handle, ep_addr, buf, xfer_buf, g_pkts_per_xfer, xfer_cb, &xfers[t], 1000);
        libusb_set_iso_packet_lengths(xfer, best_pktsz);
        xfer->flags |= LIBUSB_TRANSFER_FREE_BUFFER;   /* buffer freed with the transfer */
        r = libusb_submit_transfer(xfer);
        if (r < 0) {
            fprintf(stderr, "submit[%d]: %s\n", t, libusb_error_name(r));
            g_transport_failed = 1;
            if (r == LIBUSB_ERROR_NO_DEVICE) g_device_lost = 1;
            free_owned_transfer(xfer);
            continue;
        }
        g_inflight++;
    }
    if (g_inflight == 0) {
        fprintf(stderr, "no transfers in flight\n");
        if (g_raw_on) { ring_set_eof(&g_ring); pthread_join(writer, NULL); }
        free(xfers);
        goto fail_iface;
    }
    if (g_transport_failed) request_stop(g_device_lost ? STOP_DEVICE_LOST : STOP_TRANSPORT_ERROR);
    if (g_json) printf("{\"type\":\"ready\",\"inflight\":%d,\"packets_per_transfer\":%d,\"frame_size\":%u}\n",
                       g_inflight, g_pkts_per_xfer, g_frame_size);
    else printf("streaming: %d transfers x %d pkts (%.0f ms queue), ring %d MB. Ctrl-C to stop.\n",
                g_inflight, g_pkts_per_xfer, g_inflight * g_pkts_per_xfer / 8.0, g_ring_mb);
    fflush(stdout);
    pthread_t stdin_thread;
    int stdin_started = 0;
    if (g_stop_on_stdin && !g_stop) {
        if (pthread_create(&stdin_thread, NULL, stdin_control_main, NULL) == 0) stdin_started = 1;
        else { g_transport_failed = 1; request_stop(STOP_STDIN_ERROR); }
    }
    if (g_play_after_stream && !g_stop) {
        if (sel.mtt_id < 0) g_play_set_result = LIBUSB_ERROR_NOT_SUPPORTED;
        else {
            int vc_claimed = libusb_claim_interface(handle, sel.vc_iface) == 0;
            g_play_set_result = mtt_set_transport(handle, &sel, XPRT_PLAY_FORWARD);
            if (vc_claimed) libusb_release_interface(handle, sel.vc_iface);
        }
        if (g_json) printf("{\"type\":\"play\",\"set_result\":%d}\n", g_play_set_result);
        else printf("play SET result: %d\n", g_play_set_result);
        fflush(stdout);
        if (g_play_set_result == LIBUSB_ERROR_NO_DEVICE) {
            g_device_lost = g_transport_failed = 1;
            request_stop(STOP_DEVICE_LOST);
        } else if (g_play_set_result != 1) request_stop(STOP_PLAY_FAILED);
    }

    /* --- on-the-fly encoder (its ring gives ~18 s of slack; overflow hurts
     * only the encoded copy, never the capture) --- */
    int enc_on = 0;
    pthread_t enc_feeder;
    if (encpath) {
        if (ring_init(&g_enc_ring, (size_t)64 << 20) < 0) {
            fprintf(stderr, "WARNING: encoder buffer alloc failed — encoding disabled\n");
        } else if (!g_split && enc_spawn_ffmpeg(encpath) != 0) {
            fprintf(stderr, "WARNING: could not start encoder ffmpeg — encoding disabled\n");
        } else {
            g_enc_feed_cfg = (feed_cfg_t){ .ring = &g_enc_ring, .fd = &g_enc_in,
                                           .min_dt = 0.0, .parse_tc = 0,
                                           .alive = &g_enc_alive };
            g_enc_alive = 1;
            int tr = g_split
                ? pthread_create(&enc_feeder, NULL, enc_split_feeder_main, (void *)encpath)
                : pthread_create(&enc_feeder, NULL, frame_feeder_main, &g_enc_feed_cfg);
            if (tr == 0) {
                enc_on = 1;
                if (g_vbitrate > 0)
                    printf("encoding to %s (x264 %s %d kbps%s, aac %dk%s)\n",
                           encpath, g_preset, g_vbitrate,
                           g_deint ? ", bwdif 50p" : ", interlaced",
                           g_abitrate, g_split ? ", split by scene" : "");
                else
                    printf("encoding to %s (x264 %s crf %d%s, aac %dk%s)\n",
                           encpath, g_preset, g_crf,
                           g_deint ? ", bwdif 50p" : ", interlaced",
                           g_abitrate, g_split ? ", split by scene" : "");
            } else {
                g_enc_alive = 0;
                fprintf(stderr, "WARNING: encoder feeder thread failed\n");
            }
        }
    }

    /* --- GUI snapshot tap (best effort, like the preview) --- */
    int snap_on = 0;
    pthread_t snap_feeder;
    static feed_cfg_t snap_cfg;
    if (snappath) {
        if (!have_ffmpeg()) {
            fprintf(stderr, "WARNING: ffmpeg not found in PATH — snapshots disabled\n");
        } else if (ring_init(&g_snap_ring, (size_t)2 << 20) < 0) {
            fprintf(stderr, "WARNING: snapshot buffer alloc failed\n");
        } else if (snap_spawn_ffmpeg(snappath) != 0) {
            fprintf(stderr, "WARNING: could not start snapshot ffmpeg\n");
        } else {
            snap_cfg = (feed_cfg_t){ .ring = &g_snap_ring, .fd = &g_snap_in,
                                     .min_dt = 0.08, .parse_tc = 1,
                                     .alive = &g_snap_alive };
            g_snap_alive = 1;
            if (pthread_create(&snap_feeder, NULL, frame_feeder_main, &snap_cfg) == 0)
                snap_on = 1;
            else { g_snap_alive = 0; fprintf(stderr, "WARNING: snapshot thread failed\n"); }
        }
    }

    /* --- terminal preview (best effort; never blocks the capture) --- */
    int preview_on = 0;
    pthread_t pv_feeder, pv_render;
    if (g_preview) {
        if (!have_ffmpeg()) {
            fprintf(stderr, "WARNING: ffmpeg not found in PATH — preview disabled\n");
        } else if (ring_init(&g_pring, (size_t)4 << 20) < 0) {
            fprintf(stderr, "WARNING: preview buffer alloc failed — preview disabled\n");
        } else {
            pv_calc_size();
            const char *ct = getenv("COLORTERM");
            g_truecolor = ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"));
            if (pv_spawn_ffmpeg() == 0) {
                g_pv_feed_cfg = (feed_cfg_t){ .ring = &g_pring, .fd = &g_pv_in,
                                              .min_dt = 0.08, .parse_tc = 1,
                                              .alive = &g_pv_alive };
                g_pv_alive = 1;
                if (pthread_create(&pv_feeder, NULL, frame_feeder_main, &g_pv_feed_cfg) == 0 &&
                    pthread_create(&pv_render, NULL, pv_render_main, NULL) == 0)
                    preview_on = 1;
                else { g_pv_alive = 0; fprintf(stderr, "WARNING: preview threads failed\n"); }
            } else {
                fprintf(stderr, "WARNING: could not start ffmpeg — preview disabled\n");
            }
        }
    }

    time_t t_start = time(NULL);
    g_last_data = t_start;
    time_t last_print = t_start; uint64_t last_bytes = 0, last_err = 0;
    time_t last_tape_read = 0;
    uint64_t start_ms = capture_monotonic_ms();

    while (!g_stop && g_inflight > 0 && !g_write_error) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int event_result = libusb_handle_events_timeout(ctx, &tv);
        if (event_result < 0 && event_result != LIBUSB_ERROR_INTERRUPTED) {
            g_transport_failed = 1;
            if (event_result == LIBUSB_ERROR_NO_DEVICE) g_device_lost = 1;
            request_stop(g_device_lost ? STOP_DEVICE_LOST : STOP_TRANSPORT_ERROR);
        }
        if (g_stop || g_inflight == 0 || g_write_error) break;

        time_t now = time(NULL);
        if (g_monitor_tape && sel.mtt_id >= 0 && now - last_tape_read >= 5) {
            uint8_t state = 0, tc[4] = {0};
            int state_result = libusb_control_transfer(handle, 0xA1, UVC_GET_CUR,
                    MTT_TRANSPORT_CONTROL << 8, mtt_windex(&sel), &state, 1, 100);
            int tc_result = libusb_control_transfer(handle, 0xA1, UVC_GET_CUR,
                    MTT_TIME_CODE_INFORMATION << 8, mtt_windex(&sel), tc, 4, 100);
            if (tc_result == 4)
                snprintf(g_tc_str, sizeof(g_tc_str), "%02x:%02x:%02x:%02x",
                         tc[3] & 0x3F, tc[2] & 0x7F, tc[1] & 0x7F, tc[0] & 0x3F);
            printf("{\"type\":\"tape\",\"elapsed\":%ld,\"state_result\":%d,"
                   "\"state\":%u,\"timecode_result\":%d,\"tc\":\"%s\"}\n",
                   (long)(now - t_start), state_result, state, tc_result, g_tc_str);
            fflush(stdout);
            last_tape_read = now;
            uint64_t now_ms = capture_monotonic_ms();
            observe_transport(state_result, state, now_ms, now_ms - start_ms);
            if (g_stop) break;
        }
        if (g_idle_sec > 0 && g_bytes > 0 && now - g_last_data >= g_idle_sec) {
            printf("\nno data for %d s — stopping without physical end verification.\n", g_idle_sec);
            request_stop(STOP_IDLE_TIMEOUT);
            break;
        }
        if (g_max_sec > 0 && now - t_start >= g_max_sec) {
            printf("\nreached --max-sec %d, stopping.\n", g_max_sec);
            request_stop(STOP_MAX_SECONDS);
            break;
        }
        if (now != last_print && g_json) {
            printf("{\"type\":\"status\",\"bytes\":%llu,\"rate_mbs\":%.2f,\"ok\":%llu,"
                   "\"drop\":%llu,\"uvc_err\":%llu,\"err_rate\":%llu,\"hdrbad\":%llu,"
                   "\"ovf\":%llu,\"enc_ovf\":%llu,\"tc\":\"%s\",\"elapsed\":%ld,\"scenes\":%d}\n",
                   (unsigned long long)g_bytes,
                   (double)(g_bytes - last_bytes)/(1024.0*1024.0),
                   (unsigned long long)g_pkts_ok,
                   (unsigned long long)g_pkts_status_err,
                   (unsigned long long)g_pkts_uvc_err,
                   (unsigned long long)(g_pkts_uvc_err - last_err),
                   (unsigned long long)g_hdr_bad,
                   (unsigned long long)g_ring.ovf,
                   (unsigned long long)g_enc_ring.ovf,
                   g_tc_str,
                   (long)(now - t_start), g_scenes);
            fflush(stdout);
            last_print = now; last_bytes = g_bytes; last_err = g_pkts_uvc_err;
        } else if (now != last_print && !g_pv_alive) {   /* preview renderer shows its own status */
            printf("\r%.1f MB | %.2f MB/s | ok=%llu drop=%llu uvc_err=%llu (%llu/s) hdrbad=%llu ovf=%llu   ",
                   g_bytes/(1024.0*1024.0),
                   (double)(g_bytes - last_bytes)/(1024.0*1024.0),
                   (unsigned long long)g_pkts_ok,
                   (unsigned long long)g_pkts_status_err,
                   (unsigned long long)g_pkts_uvc_err,
                   (unsigned long long)(g_pkts_uvc_err - last_err),
                   (unsigned long long)g_hdr_bad,
                   (unsigned long long)g_ring.ovf);
            fflush(stdout);
            last_print = now; last_bytes = g_bytes; last_err = g_pkts_uvc_err;
        }
    }

    if (g_write_error) request_stop(STOP_WRITE_ERROR);
    atomic_store(&g_stdin_done, 1);
    if (stdin_started) pthread_join(stdin_thread, NULL);

    /* --- clean shutdown: taps first (preview restores the terminal), then USB --- */
    if (snap_on) {
        ring_set_eof(&g_snap_ring);
        pthread_join(snap_feeder, NULL);
        if (g_snap_in >= 0) { close(g_snap_in); g_snap_in = -1; }
        if (g_snap_pid > 0) capture_wait_child(g_snap_pid, NULL);
        g_snap_alive = 0;
    }
    if (preview_on) {
        ring_set_eof(&g_pring);
        pthread_join(pv_feeder, NULL);
        if (g_pv_in >= 0) close(g_pv_in);        /* EOF → ffmpeg exits */
        pthread_join(pv_render, NULL);           /* renderer restores the screen */
        if (g_pv_out >= 0) close(g_pv_out);
        if (g_pv_pid > 0) capture_wait_child(g_pv_pid, NULL);
        g_pv_alive = 0;
    }

    printf("\nstopping...\n");
    g_stop = 1;
    for (int t = 0; t < g_num_transfers; t++)
        if (xfers[t]) libusb_cancel_transfer(xfers[t]);   /* safe: NOT_FOUND is fine for completed ones */
    { time_t t0 = time(NULL);
      while (g_inflight > 0 && time(NULL) - t0 < 4) {
          struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
          libusb_handle_events_timeout(ctx, &tv);
      } }
    if (g_inflight == 0) {
        free(xfers);
    } else {
        /* Pending callbacks still own &xfers[t]. Keep all USB resources
         * until process exit rather than freeing their storage or closing
         * the device with active transfers. No more event handling below;
         * already accepted payloads are drained from the raw ring first. */
        fprintf(stderr, "WARNING: %d USB cancellations remain pending; "
                        "draining saved data and exiting without USB cleanup\n", g_inflight);
        usb_cleanup_incomplete = 1;
        g_transport_failed = 1;
    }

    if (auto_stop && sel.mtt_id >= 0 && !g_device_lost && !usb_cleanup_incomplete) {
        /* Single best-effort stop after transfer callbacks have retired. */
        libusb_claim_interface(handle, sel.vc_iface);          /* best effort */
        int pr = mtt_set_transport_retry(handle, &sel, XPRT_STOP);
        if (pr != 1) fprintf(stderr, "WARNING: tape stop failed: result %d\n", pr);
        else printf("tape: Stop\n");
        libusb_release_interface(handle, sel.vc_iface);
    }

    if (g_raw_on) {
        ring_set_eof(&g_ring);
        pthread_join(writer, NULL);
        if (g_out) {
            if (fflush(g_out) != 0) g_write_error = 1;
            if (fclose(g_out) != 0) g_write_error = 1;
            g_out = NULL;
        }
    }

    /* drain the encoder backlog and let ffmpeg finalize the container */
    if (enc_on) {
        printf("finalizing encoded output...\n");
        ring_set_eof(&g_enc_ring);
        pthread_join(enc_feeder, NULL);
        if (g_enc_in >= 0) { close(g_enc_in); g_enc_in = -1; }
        int st = 0;
        if (g_enc_pid > 0) { capture_wait_child(g_enc_pid, &st); g_enc_pid = 0; }
        if (g_split) {
            long long total = 0;
            char sp[1024];
            for (int i = 1; i <= g_enc_scene_n; i++) {
                scene_filename(encpath, i, sp, sizeof(sp));
                int64_t size = capture_file_size_utf8(sp);
                if (size >= 0) total += size;
            }
            scene_filename(encpath, 1, sp, sizeof(sp));
            printf("encoded: %d scene file(s), %.1f MB total (%s ...)\n",
                   g_enc_scene_n, total/(1024.0*1024.0), sp);
        } else {
            int64_t size = capture_file_size_utf8(encpath);
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && size >= 0) {
                printf("encoded: %.1f MB to %s", size/(1024.0*1024.0), encpath);
                if (size > 0 && g_bytes > 0)
                    printf(" (%.1fx smaller than DV)", (double)g_bytes / (double)size);
                printf("\n");
            } else {
                fprintf(stderr, "WARNING: encoder exited abnormally — %s may be incomplete\n", encpath);
            }
        }
        if (g_enc_ring.ovf)
            fprintf(stderr, "WARNING: encoder fell behind, %.1f MB skipped in the encoded "
                            "output%s\n", g_enc_ring.ovf/(1024.0*1024.0),
                    g_raw_on ? " (raw capture unaffected)" : "");
    }

    int64_t file_size = rawpath && !g_split ? capture_file_size_utf8(rawpath) : -1;
    if (g_raw_on && !g_split && (file_size < 0 || (uint64_t)file_size != g_written)) g_write_error = 1;
    if (g_write_error) fprintf(stderr, "WARNING: disk write error or file size mismatch occurred\n");
    if (g_write_error) atomic_store(&g_stop_reason, STOP_WRITE_ERROR);
    else if (g_device_lost) atomic_store(&g_stop_reason, STOP_DEVICE_LOST);
    else if (g_transport_failed) atomic_store(&g_stop_reason, STOP_TRANSPORT_ERROR);
    if (g_json) {
        int64_t enc_size = enc_on && encpath ? capture_file_size_utf8(encpath) : 0;
        printf("{\"type\":\"done\",\"bytes\":%llu,\"written\":%llu,\"drop\":%llu,"
               "\"uvc_err\":%llu,\"hdrbad\":%llu,\"ovf\":%llu,\"enc_bytes\":%lld,\"write_error\":%d,"
               "\"stop_reason\":\"%s\",\"physical_end_verified\":%s,\"device_lost\":%s,"
               "\"transport_failed\":%s,\"file_bytes\":%lld,\"play_attempted\":%s,\"play_set_result\":",
               (unsigned long long)g_bytes, (unsigned long long)g_written,
               (unsigned long long)g_pkts_status_err, (unsigned long long)g_pkts_uvc_err,
               (unsigned long long)g_hdr_bad, (unsigned long long)g_ring.ovf,
               (long long)(enc_size >= 0 ? enc_size : 0), atomic_load(&g_write_error), stop_reason_name(),
               g_physical_end_verified ? "true" : "false", g_device_lost ? "true" : "false",
               g_transport_failed ? "true" : "false", (long long)file_size,
               g_play_set_result != INT_MIN ? "true" : "false");
        if (g_play_set_result != INT_MIN) printf("%d", g_play_set_result); else fputs("null", stdout);
        fputs("}\n", stdout);
        fflush(stdout);
    }
    printf("captured %.1f MB%s", g_bytes/(1024.0*1024.0), g_raw_on ? "" : "\n");
    if (g_raw_on && g_split) {
        char sp1[1024];
        scene_filename(rawpath, 1, sp1, sizeof(sp1));
        printf(", written %.1f MB across %d scene file(s) (%s ...)\n",
               g_written/(1024.0*1024.0), g_scenes, sp1);
    }
    else if (g_raw_on)
        printf(", written %.1f MB to %s\n", g_written/(1024.0*1024.0), rawpath);
    printf("packets: ok=%llu drop=%llu uvc_err=%llu hdrbad=%llu ring_overflow_bytes=%llu\n",
           (unsigned long long)g_pkts_ok,
           (unsigned long long)g_pkts_status_err,
           (unsigned long long)g_pkts_uvc_err,
           (unsigned long long)g_hdr_bad,
           (unsigned long long)g_ring.ovf);
    if (g_err_seen) {
        printf("flagged-packet span: first at ~frame %llu, last at ~frame %llu (of ~%llu total), "
               "longest consecutive run = %llu pkts (~%.2f frames)\n",
               (unsigned long long)(g_err_first_off / g_frame_size),
               (unsigned long long)(g_err_last_off / g_frame_size),
               (unsigned long long)(g_bytes / g_frame_size),
               (unsigned long long)g_err_run_max,
               g_err_run_max * (double)(best_pktsz > 12 ? best_pktsz - 12 : best_pktsz) / (double)g_frame_size);
        if (g_drop_err)
            printf("dropped %.2f MB of flagged payload (--drop-err)\n", g_err_dropped_bytes/(1024.0*1024.0));
        else
            printf("flagged payloads were WRITTEN to the file (DV DIF/STA + dvrescue will judge them)\n");
    }

    exit_ok = !g_write_error && !g_transport_failed && g_play_set_result != LIBUSB_ERROR_NOT_SUPPORTED &&
              atomic_load(&g_stop_reason) != STOP_PLAY_FAILED && atomic_load(&g_stop_reason) != STOP_STDIN_ERROR;
    if (usb_cleanup_incomplete) return 1;
    if (!g_device_lost) libusb_set_interface_alt_setting(handle, vs_iface, 0);

fail_iface:
    libusb_release_interface(handle, vs_iface);
fail_cfg:
    if (cfg) libusb_free_config_descriptor(cfg);
fail:
    if (g_out) fclose(g_out);
    libusb_close(handle);
    libusb_exit(ctx);
    return exit_ok && !g_write_error && !g_transport_failed ? 0 : 1;
}

#ifndef UVCDV_NO_ENTRY
#ifdef _WIN32
int wmain(int argc, wchar_t **wargv)
{
    char **argv = capture_argv_utf8(argc, wargv);
    if (!argv) { fputs("cannot convert Unicode command line to UTF-8\n", stderr); return 2; }
    int result = uvcdv_main(argc, argv);
    capture_free_argv_utf8(argc, argv);
    return result;
}
#else
int main(int argc, char **argv) { return uvcdv_main(argc, argv); }
#endif
#endif
