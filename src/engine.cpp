/*
 * DAVIS Spectral Sidechain -- a one-band sidechain dynamic EQ: a movable band on one track that follows another track (the SC).
 *
 * Put one instance on the KICK track in mode SEND (audio passes through untouched, a copy goes into a shared tap) and
 * one on the BASS track in mode RECEIVE (a single bell: FREQ moves it across the bass range, Q makes it wide or narrow,
 * AMOUNT cuts (negative) or lifts (positive) that band by the kick's level). CHAN pairs a SEND with a RECEIVE, 4 pairs.
 *
 * All instances of one plugin share one process and one copy of the library's statics (mpc-vst-plugins docs/NOTES.md),
 * and MPC runs them in no fixed order on different threads, so the tap is a lock-free ring buffer and the receiver reads
 * one block behind the sender with its own cursor: the same samples whichever instance runs first. Cost: ~3 ms.
 *
 * Contract: engine.h (44100 Hz, interleaved int16 stereo, 128-frame blocks). DSP: RBJ peaking biquad.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <atomic>
#include <new>

extern "C" {
#include "engine.h"
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const float SR = 44100.0f;
static const int NCHAN = 4;                    /* kick/bass pairs */
static const int RING = 16384;                 /* tap length (samples, mono) */
static const int NCOL = 112;                   /* log columns on the graph (6 px each), F_LO .. F_HI */
static const int RING_EVERY = 2;               /* the live ring can sit on every 2nd column */
static const float F_LO = 15.0f, F_HI = 20000.0f;
static const float CURVE_DB = 30.0f;           /* graph shows -30 .. +30 dB */
static const int CURVE_STEPS = 60;             /* used by the history and the ring: 0 = -30 dB .. 60 = +30 dB (1 dB a step) */
static const int RING_STEPS = 30;              /* the live dot: 2 dB a step */
static const int LIVE_STEPS = 30;              /* the live curve (the band as it is now): 2 dB a step, 0 = -30 dB .. 30 = +30 dB */
static const float AMT_MIN = -600.0f, AMT_MAX = 300.0f;   /* AMOUNT, tenths of a dB: -60 (a full cut) .. +30 */
static const float RATIO_FULL = 21.0f;         /* the Ratio knob's last position: FULL, the whole AMOUNT as soon as the SC is 6 dB over */
static const int FFT_N = 4096;                 /* 10.8 Hz bins (interpolated below one bin a column) */
static const int FFT_LOG = 12;
#ifndef AN_CYCLE
#define AN_CYCLE 80
#endif
static const int AN_BLOCKS = AN_CYCLE;   /* blocks per display read (128 samples each): 80 = ~232 ms */
static const int SPEC_STEPS = 30;              /* the OUT spectrum: 2 dB a step, -66 .. -6 dBFS; 0 = nothing drawn */
#ifndef BAR_EVERY
#define BAR_EVERY 2
#endif
static const int SPEC_EVERY = BAR_EVERY;               /* one spectrum column per 4 graph columns (24 px): half the screen traffic */
static const int CURVE_EVERY = 2;              /* the band's curve: one column per 2 (12 px) */
static const int WIN_N = 56;                   /* detector window: 56 sub-blocks of 16 = 20 ms, one period of a 50 Hz kick: even Rel 1 ms follows the hit, not the wave */
static const float LIM_KNEE = 0.8f, LIM_CEIL = 0.97f;   /* while boosting: untouched up to -2 dBFS, then a soft knee to -0.3 dBFS */
#ifndef BAR_HOLD_N
#define BAR_HOLD_N 3
#endif
static const int BAR_HOLD = BAR_HOLD_N;                 /* SC view and OUT bars: a peak holds 3 reads (~0.7 s, ~4 reads a second) */
#ifndef BAR_FALL_DB
#define BAR_FALL_DB 1.5f
#endif
static const float BAR_FALL = BAR_FALL_DB;            /* then falls 3 dB a read (~13 dB/s): no strobe, and fewer redraws on MPC */
static const int KICK_EVERY = BAR_EVERY;               /* one SC-view bar per 4 graph columns (24 px): half the bars OUT has, half the screen traffic */
static const int KICK_HYST = 2;
#ifndef BAR_HYST
#define BAR_HYST 3
#endif
#ifndef BAR_SPLIT
#define BAR_SPLIT 1
#endif
/* BAR_HYST: SC/OUT bars redrawn only when moved this many 2 dB steps; BAR_SPLIT: bars committed in N interleaved turns, one turn a read */                /* a bar is only redrawn when it moved 2 steps (4 dB), or to/from nothing: MPC handles every change */
static const int OBUF = 4096;                  /* the output, mono, for the OUT spectrum */               /* the display work runs as a cycle of small steps, one per block (~232 ms) */

struct Tap {
    std::atomic<uint32_t> wpos{0};             /* samples written so far */
    std::atomic<uint32_t> beat{0};             /* bumped by every SEND block: tells the receiver a sender is alive */
    float buf[RING];
};
static Tap g_tap[NCHAN];

static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static float pos_to_hz(float p) { return F_LO * powf(F_HI / F_LO, clampf(p, 0, 1000) / 1000.0f); }
static float pos_to_q(float p) { return 0.1f * powf(8.0f / 0.1f, clampf(p, 0, 100) / 100.0f); }   /* 0.1 very wide .. 8 narrow */
static float pos_to_ms(float p) { return powf(500.0f, clampf(p, 0, 100) / 100.0f); }               /* 1 .. 500 ms, log */
static float pos_to_ratio(float p) { return clampf(p, 1.0f, RATIO_FULL); }                         /* 1:1 .. 20:1, then FULL */
static int16_t to_s16(float x) { x *= 32768.0f; return (int16_t)(x > 32767.0f ? 32767 : (x < -32768.0f ? -32768 : lrintf(x))); }

struct ColTrig { float cr1, si1, cr2, si2; };

/* the bell as two identical Regalia-Mitra sections in series (half the gain each, so a deep cut is wide enough to be a
 * real cut, not a pin-point notch). One section: H = ((1 + A) + G (1 - A)) / 2, with A a 2nd-order allpass set by FREQ and Q only.
 * The gain G is a plain multiplier, so it can move every sample with no clicks and no coefficient recompute. */
struct AP { float k2 = 0, b1 = 0; };
static AP allpass(float hz, float q)
{
    float w0 = 2.0f * (float)M_PI * hz / SR, bw = clampf(w0 / q, 0.0005f, 3.0f), t = tanf(0.5f * bw);
    AP a; a.k2 = (1.0f - t) / (1.0f + t); a.b1 = -cosf(w0) * (1.0f + a.k2);
    return a;
}
/* |H| in dB at one graph column, for gain gdb */
static float bell_db_g(const AP &a, float g, const ColTrig &t)
{
    /* A = (k2 + b1 z^-1 + z^-2) / (1 + b1 z^-1 + k2 z^-2), z^-1 = cr1 + j si1, z^-2 = cr2 + j si2 */
    float nr = a.k2 + a.b1 * t.cr1 + t.cr2, ni = a.b1 * t.si1 + t.si2;
    float dr = 1.0f + a.b1 * t.cr1 + a.k2 * t.cr2, di = a.b1 * t.si1 + a.k2 * t.si2;
    float dd = dr * dr + di * di + 1e-20f, ar = (nr * dr + ni * di) / dd, ai = (ni * dr - nr * di) / dd;
    float hr = 0.5f * ((1.0f + ar) + g * (1.0f - ar)), hi = 0.5f * (ai - g * ai);
    return 2.0f * 10.0f * log10f(hr * hr + hi * hi + 1e-12f);
}
static float bell_db(const AP &a, float gdb, const ColTrig &t)
{
    return bell_db_g(a, powf(10.0f, gdb / 40.0f), t);   /* two sections in series, half the gain each */
}

struct KD {
    /* parameters, in knob units */
    int mode = 0;                              /* 0 = RECEIVE (bass), 1 = SEND (kick) */
    int chan = 0;
    float freq = 193;                          /* 0..1000 -> 15..20000 Hz  (~60 Hz) */
    float qpos = 55;                           /* 0..100 -> Q 0.1..8 (~1.1) */
    float amount = -90;                        /* tenths of a dB, -300..+300: the most the band moves */
    float thresh = -20;                        /* dB: where the SC starts to act */
    float ratio = 4;                           /* 1..20 (:1) */
    float attack = 18, release = 81;           /* 0..100 knob units -> 1..500 ms (log): ~3 ms and ~150 ms */

    /* dsp */
    float gdb = 0, env = 0;
    float dz1 = 0, dz2 = 0;                    /* detector band-pass state */
    float zl1 = 0, zl2 = 0, zr1 = 0, zr2 = 0;  /* the allpass states, left and right: first section */
    float zl3 = 0, zl4 = 0, zr3 = 0, zr4 = 0;  /* second section */
    AP ap; float glin = 1.0f;                  /* the bell's allpass and its current linear gain */
    uint32_t rpos = 0; bool synced = false;
    uint32_t last_beat = 0; int silent_blocks = 1000;
    uint32_t noise = 1;

    /* display */
    unsigned display_rev = 0;
    int link = 0;
    int spec_wait = 0, curve_wait = 0;
    int ring[NCOL];  int col[NCOL];  int live[NCOL];                         /* live dot, the band's curve at Amount, the band now */
    float live_gdb = 0;                        /* the gain the live curve was last drawn at */
    float fft_re[FFT_N], fft_im[FFT_N], win[FFT_N];
    float tw_re[FFT_N / 2], tw_im[FFT_N / 2];  /* FFT twiddles */
    uint16_t bitrev[FFT_N];
    ColTrig ctrig[NCOL];                       /* each graph column's frequency, for the curve */
    float cb0[NCOL], cb1[NCOL];                /* each graph column's FFT bin range */
    int an_step = 0;                           /* where the display cycle is */
    int out_on = 0;                            /* OUT: the output's spectrum on the graph */
    float obuf[OBUF]; uint32_t opos = 0;       /* the output, mono, for that spectrum */
    float sdb[NCOL];                           /* each spectrum column's level, dB, smoothed */
    int spec[NCOL];                            /* and its step, as the skin draws it (0 = off) */
    float kdb[NCOL];                           /* the SC's spectrum (SC view), dB, held then falling */
    uint8_t khold[NCOL], shold[NCOL];          /* reads left before each SC view / OUT bar starts falling */
    uint8_t kturn, sturn;                       /* BAR_SPLIT: which interleaved turn of SC view / OUT bars is redrawn this read */
    int kick[NCOL];                            /* and its step, as the skin draws it (0 = nothing) */
    int kick_on = 0;                           /* SC view: the SC's spectrum on the graph */
    int kick_nz = 0;                           /* some kick step is drawn now */
    float wmax[WIN_N] = {0}; int wpos = 0;     /* detector: the SC's peak in each of the last 56 sub-blocks (20 ms) */
    float katk16 = 0, krel16 = 0;              /* Atk / Rel per 16-sample sub-block */
    int live_still = 0, live_exact = 1;        /* live curve: calls with the gain still, and whether the exact pass was done */
    float peak_hz = 0, peak_mag = 0; int peak_age = 0;
    float pavg[FFT_N / 2];                     /* the SC's power per bin, averaged over the last ~2 s: the body that lasts wins over the click */   /* the SC's strongest frequency, held ~2 s so it does not jump between hits */
    float last_gdb = 1e9f, last_hz = -1, last_q = -1;   /* the bell's coefficients are recomputed only when these move */
    float db0 = 0, da1 = 0, da2 = 0;           /* detector band-pass, recomputed when FREQ or Q move */
    float last_atk = -1, last_rel = -1, katk = 0, krel = 0;   /* envelope coefficients, recomputed when Atk or Rel move */
    bool curve_dirty = true;
    int touch_flags = 0;                       /* bit 0 freq, bit 1 amount: moved by a touch on the graph, to report back */
    unsigned char col_touched[NCOL];           /* graph columns a touch moved on the host's side: report the engine's back */
};

/* the band's curve at full AMOUNT (moves only when a knob does) */
static void update_curve(KD *p)
{
    float hz = pos_to_hz(p->freq), q = pos_to_q(p->qpos), amt = p->amount / 10.0f;
    AP a = allpass(hz, q);
    bool moved = false;
    for (int c = 0; c < NCOL; c += CURVE_EVERY) {      /* one 12 px column per 2: half the repaints on MPC's screen thread */
        float d0 = bell_db(a, amt, p->ctrig[c]), d1 = c + 1 < NCOL ? bell_db(a, amt, p->ctrig[c + 1]) : d0;
        float db = clampf(fabsf(d1) > fabsf(d0) ? d1 : d0, -CURVE_DB, CURVE_DB);   /* the deeper of the two: the bell's tip stays */
        int v = (int)lrintf((db + CURVE_DB) / (2 * CURVE_DB) * CURVE_STEPS);      /* 0 = -30 dB .. 60 = +30 dB, 1 dB a step */
        if (v != p->col[c]) { p->col[c] = v; moved = true; }
    }
    if (moved) ++p->display_rev;
}

/* the live ring: on the band's column, at the gain applied right now (it drops towards the curve's tip on each hit) */
/* the live curve: the band at the gain applied right now (its shape follows Q), redrawn when the gain moved by 1 dB or more */
static void update_live(KD *p)
{
    bool exact = false;
    if (fabsf(p->gdb - p->live_gdb) < 1.0f && !(p->gdb == 0.0f && p->live_gdb != 0.0f)) {
        if (p->live_exact || ++p->live_still < 8) return;    /* the gain held ~90 ms: one exact pass puts every column on its step */
        exact = true; p->live_exact = 1;
    } else { p->live_still = 0; p->live_exact = 0; }
    p->live_gdb = p->gdb;
    AP a = allpass(pos_to_hz(p->freq), pos_to_q(p->qpos));
    float g = powf(10.0f, p->gdb / 40.0f);
    bool moved = false;
    for (int c = 0; c < NCOL; c += RING_EVERY) {       /* only the drawn columns (l0, l2, ...) */
        int v = p->gdb == 0.0f ? LIVE_STEPS / 2
                               : (int)lrintf((clampf(bell_db_g(a, g, p->ctrig[c]), -CURVE_DB, CURVE_DB) + CURVE_DB) / (2 * CURVE_DB) * LIVE_STEPS);
        int d = v - p->live[c];                           /* redrawn when it moved 2 steps (4 dB), or back to flat */
        if (v != p->live[c] && (exact || v == LIVE_STEPS / 2 || d >= KICK_HYST || d <= -KICK_HYST)) { p->live[c] = v; moved = true; }
    }
    if (moved) ++p->display_rev;
}

static void update_ring(KD *p)
{
    int rc = RING_EVERY * (int)lrintf(clampf(p->freq, 0, 1000) / 1000.0f * (NCOL - 1) / RING_EVERY);
    if (rc > NCOL - RING_EVERY) rc = NCOL - RING_EVERY;
    int rl = (int)lrintf((clampf(p->gdb, -CURVE_DB, CURVE_DB) + CURVE_DB) / (2 * CURVE_DB) * RING_STEPS) + 1;
    bool moved = false;
    for (int c = 0; c < NCOL; c += RING_EVERY) {
        int rv = c == rc ? rl : 0;
        if (rv != p->ring[c]) { p->ring[c] = rv; moved = true; }
    }
    if (moved) ++p->display_rev;
}

/* the spectrum, as small steps spread over blocks so no single block carries a whole FFT:
 * load (window + bit reversal), FFT_LOG butterfly stages (two a step), then the columns */
static void an_load(KD *p, const float *ring, uint32_t end, uint32_t mask = RING - 1)
{
    for (int i = 0; i < FFT_N; ++i) { int j = p->bitrev[i]; p->fft_re[j] = ring[(end - FFT_N + (uint32_t)i) & mask] * p->win[i]; p->fft_im[j] = 0; }
}
static void an_stage(KD *p, int st)                  /* butterfly stage st (0 .. FFT_LOG-1) */
{
    float *re = p->fft_re, *im = p->fft_im;
    int len = 2 << st, half = len >> 1, step = FFT_N / len;
    for (int i = 0; i < FFT_N; i += len)
        for (int k = 0; k < half; ++k) {
            float cr = p->tw_re[k * step], ci = p->tw_im[k * step];
            int a = i + k, b = a + half;
            float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
            re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
        }
}
/* the SC's strongest frequency from its power averaged over the last few analyses (~2 s): a kick's body, which lasts, wins over
 * its short click; parabolic interpolation between bins. Returns true when the shown value moved. */
/* the output's spectrum, one column per SPEC_EVERY graph columns: the loudest bin in its range (or the nearest bin below a
 * bin a column), fast up and slower down, as 2 dB steps. Returns true when a drawn step moved. */
/* a bar's level: up at once to a new peak, held BAR_HOLD reads, then down BAR_FALL dB a read (never below now) */
#ifndef KICK_HOLD_N
#define KICK_HOLD_N 0
#endif
#ifndef KICK_FALL_DB
#define KICK_FALL_DB 6.0f
#endif
#ifndef KICK_HYST_N
#define KICK_HYST_N 3
#endif
/* the SC view follows each kick: no hold, 12 dB a read, so the bars are down before the next hit (~250 ms) */
static inline float bar_level(float cur, float db, uint8_t &hold, int holdn = BAR_HOLD, float fall = BAR_FALL)
{
    if (db >= cur) { hold = (uint8_t)holdn; return db; }
    if (hold > 0) { --hold; return cur; }
    float f = cur - fall;
    return f > db ? f : db;
}
static bool an_spec(KD *p)
{
    bool moved = false;
    const float norm = 4.0f / FFT_N;                    /* a full-scale sine reads 0 dB through the Hann window */
    auto pw = [p](int i) { return p->fft_re[i] * p->fft_re[i] + p->fft_im[i] * p->fft_im[i]; };
    for (int c = 0; c < NCOL; c += SPEC_EVERY) {
        int ce = c + SPEC_EVERY - 1 < NCOL ? c + SPEC_EVERY - 1 : NCOL - 1;
        float f0 = p->cb0[c], f1 = p->cb1[ce], m = 0;
        if (f1 - f0 < 1.5f) {                           /* narrower than a bin or so: read between the two nearest bins */
            float fc = 0.5f * (f0 + f1); int i = (int)fc; float t = fc - i;
            if (i < 1) { i = 1; t = 0; }
            if (i >= FFT_N / 2 - 1) { i = FFT_N / 2 - 2; t = 1; }
            m = (1 - t) * pw(i) + t * pw(i + 1);
        } else {                                        /* wider: its loudest bin */
            int b0 = (int)f0, b1 = (int)ceilf(f1);
            if (b0 < 1) b0 = 1;
            if (b1 > FFT_N / 2) b1 = FFT_N / 2;
            for (int i = b0; i < b1; ++i) { float v = pw(i); if (v > m) m = v; }
        }
        float db = 10.0f * log10f(m * norm * norm + 1e-12f);
        p->sdb[c] = bar_level(p->sdb[c], db, p->shold[c]);
    }
    for (int c = 0; c < NCOL; c += SPEC_EVERY) {        /* a light smoothing across columns, for the drawing only */
        float l = p->sdb[c > 0 ? c - SPEC_EVERY : c], r = p->sdb[c + SPEC_EVERY < NCOL ? c + SPEC_EVERY : c];
        float v = 0.5f * p->sdb[c] + 0.25f * (l + r);
        int k = (int)lrintf((clampf(v, -66.0f, -6.0f) + 66.0f) / 2.0f);
        if (k < 1) k = 1;                              /* on: always a line at the floor */
        int d = k - p->spec[c];                        /* redrawn only when it moved 2 steps (4 dB), or from off */
        if ((c / SPEC_EVERY) % BAR_SPLIT != p->sturn) continue;
        if (k != p->spec[c] && (p->spec[c] == 0 || d >= BAR_HYST || d <= -BAR_HYST)) { p->spec[c] = k; moved = true; }
    }
    p->sturn = (uint8_t)((p->sturn + 1) % BAR_SPLIT);
    return moved;
}
/* the SC's spectrum (SC view): the same columns as the output's, read from the SC's FFT: up at once, held, then
 * falling (bar_level); silence draws nothing. Returns true when a drawn step moved. */
static bool an_kick(KD *p)
{
    bool moved = false, any = false;
    const float norm = 4.0f / FFT_N;
    auto pw = [p](int i) { return p->fft_re[i] * p->fft_re[i] + p->fft_im[i] * p->fft_im[i]; };
    for (int c = 0; c < NCOL; c += KICK_EVERY) {
        int ce = c + KICK_EVERY - 1 < NCOL ? c + KICK_EVERY - 1 : NCOL - 1;
        float f0 = p->cb0[c], f1 = p->cb1[ce], m = 0;
        if (f1 - f0 < 1.5f) {
            float fc = 0.5f * (f0 + f1); int i = (int)fc; float t = fc - i;
            if (i < 1) { i = 1; t = 0; }
            if (i >= FFT_N / 2 - 1) { i = FFT_N / 2 - 2; t = 1; }
            m = (1 - t) * pw(i) + t * pw(i + 1);
        } else {
            int b0 = (int)f0, b1 = (int)ceilf(f1);
            if (b0 < 1) b0 = 1;
            if (b1 > FFT_N / 2) b1 = FFT_N / 2;
            for (int i = b0; i < b1; ++i) { float v = pw(i); if (v > m) m = v; }
        }
        float db = 10.0f * log10f(m * norm * norm + 1e-12f);
        p->kdb[c] = bar_level(p->kdb[c], db, p->khold[c], KICK_HOLD_N, KICK_FALL_DB);
    }
    for (int c = 0; c < NCOL; c += KICK_EVERY) {
        float l = p->kdb[c > 0 ? c - KICK_EVERY : c], r = p->kdb[c + KICK_EVERY < NCOL ? c + KICK_EVERY : c];
        float v = 0.5f * p->kdb[c] + 0.25f * (l + r);
        int k = v <= -60.0f ? 0 : (int)lrintf((clampf(v, -66.0f, -6.0f) + 66.0f) / 2.0f);
        if (k > 0) any = true;
        int d = k - p->kick[c];
        if ((c / KICK_EVERY) % BAR_SPLIT != p->kturn) continue;
        if (k != p->kick[c] && (k == 0 || p->kick[c] == 0 || d >= KICK_HYST_N || d <= -KICK_HYST_N)) { p->kick[c] = k; moved = true; }
    }
    p->kick_nz = any ? 1 : 0;
    p->kturn = (uint8_t)((p->kturn + 1) % BAR_SPLIT);
    return moved;
}
static void kick_clear(KD *p)
{
    bool any = false;
    for (int c = 0; c < NCOL; ++c) { if (p->kick[c]) any = true; p->kick[c] = 0; p->kdb[c] = -120.0f; }
    p->kick_nz = 0;
    if (any) ++p->display_rev;
}
static void spec_clear(KD *p)
{
    bool any = false;
    for (int c = 0; c < NCOL; ++c) { if (p->spec[c]) any = true; p->spec[c] = 0; p->sdb[c] = -120.0f; }
    for (int i = 0; i < OBUF; ++i) p->obuf[i] = 0;
    if (any) ++p->display_rev;
}
static void *create(const char *) 
{
    KD *p = new (std::nothrow) KD();
    if (!p) return nullptr;
    for (int i = 0; i < FFT_N; ++i) p->win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1));
    for (int i = 0; i < FFT_N / 2; ++i) p->pavg[i] = 0;
    for (int k = 0; k < FFT_N / 2; ++k) { p->tw_re[k] = cosf(-2.0f * (float)M_PI * k / FFT_N); p->tw_im[k] = sinf(-2.0f * (float)M_PI * k / FFT_N); }
    for (int i = 0; i < FFT_N; ++i) { int r = 0; for (int b = 0; b < FFT_LOG; ++b) if (i & (1 << b)) r |= 1 << (FFT_LOG - 1 - b); p->bitrev[i] = (uint16_t)r; }
    for (int c = 0; c < NCOL; ++c) {
        float f = F_LO * powf(F_HI / F_LO, (float)c / (NCOL - 1)), w = 2.0f * (float)M_PI * f / SR;
        p->ctrig[c] = {cosf(w), -sinf(w), cosf(2 * w), -sinf(2 * w)};
        p->cb0[c] = F_LO * powf(F_HI / F_LO, (c - 0.5f) / (NCOL - 1)) / (SR / FFT_N);
        p->cb1[c] = F_LO * powf(F_HI / F_LO, (c + 0.5f) / (NCOL - 1)) / (SR / FFT_N);
    }
    for (int c = 0; c < NCOL; ++c) { p->spec[c] = 0; p->sdb[c] = -120.0f; p->kick[c] = 0; p->kdb[c] = -120.0f; p->khold[c] = p->shold[c] = 0; }
    for (int i = 0; i < OBUF; ++i) p->obuf[i] = 0;
    for (int c = 0; c < NCOL; ++c) { p->col_touched[c] = 0; p->ring[c] = 0; p->col[c] = CURVE_STEPS / 2; p->live[c] = LIVE_STEPS / 2; }
    return p;
}
static void destroy(void *inst) { delete static_cast<KD *>(inst); }
static void midi(void *, const uint8_t *, int) {}
static void render(void *, int16_t *out, int frames) { memset(out, 0, sizeof(int16_t) * 2 * (size_t)frames); }

static void set_param(void *inst, const char *key, const char *val)
{
    KD *p = static_cast<KD *>(inst);
    if (!p || !key || !val) return;
    float v = (float)atof(val);
    if (!strcmp(key, "mode")) { p->mode = v >= 0.5f ? 1 : 0; p->synced = false; ++p->display_rev; }
    else if (!strcmp(key, "chan")) { p->chan = (int)clampf(floorf(v + 0.5f), 0, NCHAN - 1); p->synced = false; ++p->display_rev; }
    else if (!strcmp(key, "freq")) { p->freq = clampf(v, 0, 1000); p->curve_dirty = true; }
    else if (!strcmp(key, "q")) { p->qpos = clampf(v, 0, 100); p->curve_dirty = true; }
    else if (!strcmp(key, "amount")) { p->amount = clampf(v, AMT_MIN, AMT_MAX); p->curve_dirty = true; }
    else if (!strcmp(key, "thresh")) { p->thresh = clampf(v, -60, 0); ++p->display_rev; }
    else if (!strcmp(key, "ratio")) { p->ratio = pos_to_ratio(v); ++p->display_rev; }
    else if (!strcmp(key, "out")) { int o = v >= 0.5f ? 1 : 0; if (o != p->out_on) { p->out_on = o; if (!o) spec_clear(p); ++p->display_rev; } }
    else if (!strcmp(key, "scview")) { int o = v >= 0.5f ? 1 : 0; if (o != p->kick_on) { p->kick_on = o; if (!o) kick_clear(p); ++p->display_rev; } }
    else if (!strcmp(key, "attack")) { p->attack = clampf(v, 0, 100); }
    else if (!strcmp(key, "release")) { p->release = clampf(v, 0, 100); }
    else if (key[0] == 'c' && key[1] >= '0' && key[1] <= '9') {
        /* the graph is for looking only: if a touch ever reaches a column, nothing moves and the column is reported back
         * so the screen redraws the engine's value (the skin also covers the graph with a clear plate) */
        int c = atoi(key + 1);
        if (c < 0 || c >= NCOL) return;
        p->col_touched[c] = 1;
    }
    else if (!strcmp(key, "snap")) {                  /* put the band on the SC's strongest frequency */
        if (v >= 0.5f && p->peak_hz > 0) {
            p->freq = clampf(1000.0f * logf(p->peak_hz / F_LO) / logf(F_HI / F_LO), 0, 1000);
            p->touch_flags |= 1; p->curve_dirty = true;
        }
    }
    else if (!strcmp(key, "state")) {
        int m, c, o = 0; float f, q, a, t, at, r, ra = 4;
        if (sscanf(val, "kd2 %d %d %f %f %f %f %f %f %f %d", &m, &c, &f, &q, &a, &t, &at, &r, &ra, &o) >= 9) {
            if ((o ? 1 : 0) != p->out_on) { p->out_on = o ? 1 : 0; if (!p->out_on) spec_clear(p); }
            p->mode = m ? 1 : 0; p->chan = (int)clampf((float)c, 0, NCHAN - 1);
            p->freq = clampf(f, 0, 1000); p->qpos = clampf(q, 0, 100); p->amount = clampf(a, AMT_MIN, AMT_MAX);
            p->thresh = clampf(t, -60, 0); p->attack = clampf(at, 0, 100); p->release = clampf(r, 0, 100); p->ratio = pos_to_ratio(ra);
            p->synced = false; p->curve_dirty = true; ++p->display_rev;
        }
    }
}

static int get_param(void *inst, const char *key, char *buf, int len)
{
    KD *p = static_cast<KD *>(inst);
    if (!p || !key || !buf || len <= 0) return 0;
    if (key[1] >= '0' && key[1] <= '9') {      /* the graph's columns first: ~300 of them, read every 100 ms with the audio waiting */
        int c = atoi(key + 1);
        if (c >= 0 && c < NCOL) switch (key[0]) {
            case 'c': return snprintf(buf, (size_t)len, "%d", p->col[c]);
            case 'l': return snprintf(buf, (size_t)len, "%d", p->live[c]);
            case 'h': return snprintf(buf, (size_t)len, "%d", p->ring[c]);
            case 's': return snprintf(buf, (size_t)len, "%d", p->spec[c]);
            case 'k': return snprintf(buf, (size_t)len, "%d", p->kick[c]);
        }
    }
    if (!strcmp(key, "state"))
        return snprintf(buf, (size_t)len, "kd2 %d %d %g %g %g %g %g %g %g %d", p->mode, p->chan, p->freq, p->qpos, p->amount, p->thresh, p->attack, p->release, p->ratio, p->out_on);
    if (!strcmp(key, "display_rev")) return snprintf(buf, (size_t)len, "%u", p->display_rev);
    if (!strcmp(key, "latency")) return snprintf(buf, (size_t)len, "0");
    if (!strcmp(key, "mode")) return snprintf(buf, (size_t)len, "%d", p->mode);
    if (!strcmp(key, "chan")) return snprintf(buf, (size_t)len, "%d", p->chan);
    if (!strcmp(key, "freq")) return snprintf(buf, (size_t)len, "%g", p->freq);
    if (!strcmp(key, "q")) return snprintf(buf, (size_t)len, "%g", p->qpos);
    if (!strcmp(key, "amount")) return snprintf(buf, (size_t)len, "%g", p->amount);
    if (!strcmp(key, "thresh")) return snprintf(buf, (size_t)len, "%g", p->thresh);
    if (!strcmp(key, "ratio")) return snprintf(buf, (size_t)len, "%g", p->ratio);
    if (!strcmp(key, "attack")) return snprintf(buf, (size_t)len, "%g", p->attack);
    if (!strcmp(key, "release")) return snprintf(buf, (size_t)len, "%g", p->release);
    if (!strcmp(key, "out")) return snprintf(buf, (size_t)len, "%d", p->out_on);
    if (key[0] == 's' && key[1] >= '0' && key[1] <= '9') { int c = atoi(key + 1); return c >= 0 && c < NCOL ? snprintf(buf, (size_t)len, "%d", p->spec[c]) : 0; }
    if (!strcmp(key, "scview")) return snprintf(buf, (size_t)len, "%d", p->kick_on);
    if (key[0] == 'k' && key[1] >= '0' && key[1] <= '9') { int c = atoi(key + 1); return c >= 0 && c < NCOL ? snprintf(buf, (size_t)len, "%d", p->kick[c]) : 0; }
    if (!strcmp(key, "link")) return snprintf(buf, (size_t)len, "%d", p->link);
    if (!strcmp(key, "gr")) return snprintf(buf, (size_t)len, "%d", (int)lrintf(p->gdb * 10.0f));
    if (!strcmp(key, "peak")) return snprintf(buf, (size_t)len, "%g", p->peak_hz > 0 ? clampf(1000.0f * logf(p->peak_hz / F_LO) / logf(F_HI / F_LO), 0, 1000) : 0.0f);
    if (!strcmp(key, "peak_display")) {
        if (p->peak_hz <= 0) return snprintf(buf, (size_t)len, "SC peak --");
        if (p->peak_hz < 1000.0f) return snprintf(buf, (size_t)len, "SC peak %.0f Hz", p->peak_hz);
        return snprintf(buf, (size_t)len, "SC peak %.1f kHz", p->peak_hz / 1000.0f);
    }
    if (!strcmp(key, "snap")) return snprintf(buf, (size_t)len, "0");
    if (!strcmp(key, "freq_display")) {
        float hz = pos_to_hz(p->freq);
        if (hz < 100.0f) return snprintf(buf, (size_t)len, "%.1f Hz", hz);
        if (hz < 1000.0f) return snprintf(buf, (size_t)len, "%d Hz", (int)lrintf(hz));
        return snprintf(buf, (size_t)len, "%.2f kHz", hz / 1000.0f);
    }
    if (!strcmp(key, "q_display")) return snprintf(buf, (size_t)len, "%.2f", pos_to_q(p->qpos));
    if (!strcmp(key, "amount_display")) return snprintf(buf, (size_t)len, "%+.1f dB", p->amount / 10.0f);
    if (!strcmp(key, "thresh_display")) return snprintf(buf, (size_t)len, "%d dB", (int)p->thresh);
    if (!strcmp(key, "ratio_display")) return snprintf(buf, (size_t)len, "%.1f:1", p->ratio);
    if (!strcmp(key, "attack_display")) { float ms = pos_to_ms(p->attack); return snprintf(buf, (size_t)len, ms < 10 ? "%.1f ms" : "%.0f ms", (double)ms); }
    if (!strcmp(key, "release_display")) { float ms = pos_to_ms(p->release); return snprintf(buf, (size_t)len, ms < 10 ? "%.1f ms" : "%.0f ms", (double)ms); }
    if (!strcmp(key, "gr_display")) return snprintf(buf, (size_t)len, "%+.1f dB", p->gdb);
    if (!strcmp(key, "mode_name")) return snprintf(buf, (size_t)len, "%s", p->mode ? "SEND (kick)" : "RECEIVE (bass)");
    if (!strcmp(key, "chan_name")) return snprintf(buf, (size_t)len, "%c", 'A' + p->chan);
    if (key[0] == 'c' && key[1] >= '0' && key[1] <= '9') { int c = atoi(key + 1); return c >= 0 && c < NCOL ? snprintf(buf, (size_t)len, "%d", p->col[c]) : 0; }
    if (key[0] == 'l' && key[1] >= '0' && key[1] <= '9') { int c = atoi(key + 1); return c >= 0 && c < NCOL ? snprintf(buf, (size_t)len, "%d", p->live[c]) : 0; }
    if (key[0] == 'h' && key[1] >= '0' && key[1] <= '9') { int c = atoi(key + 1); return c >= 0 && c < NCOL ? snprintf(buf, (size_t)len, "%d", p->ring[c]) : 0; }
    return 0;
}

/* the whole DSP, in 32-bit float, interleaved stereo, any level (no clipping: MPC mixes in float) */
static void run(KD *p, const float *in, float *out, int frames)
{
    Tap &tap = g_tap[p->chan];

    if (p->mode == 1) {                                   /* SEND: pass through, copy into the tap */
        if (p->spec[0] || p->spec[2]) spec_clear(p);
        if (p->kick_nz) kick_clear(p);
        uint32_t w = tap.wpos.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; ++i) {
            out[2 * i] = in[2 * i]; out[2 * i + 1] = in[2 * i + 1];
            tap.buf[(w + (uint32_t)i) & (RING - 1)] = 0.5f * (in[2 * i] + in[2 * i + 1]);
        }
        tap.wpos.store(w + (uint32_t)frames, std::memory_order_release);
        tap.beat.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    /* RECEIVE: is a sender alive on this channel? */
    uint32_t beat = tap.beat.load(std::memory_order_relaxed);
    if (!p->synced) p->last_beat = beat;                 /* new instance or new CHAN: only a beat that moves from now on counts */
    if (beat != p->last_beat) { p->last_beat = beat; p->silent_blocks = 0; } else if (p->silent_blocks < 100000) ++p->silent_blocks;
    int link = p->silent_blocks < 40;
    if (link != p->link) { p->link = link; ++p->display_rev; }

    uint32_t w = tap.wpos.load(std::memory_order_acquire);
    if (!p->synced || (w - p->rpos) < (uint32_t)frames || (w - p->rpos) > (uint32_t)(4 * frames + 64)) {
        p->rpos = w - (uint32_t)(2 * frames); p->synced = true;     /* read one block behind: same samples in either order */
    }
    if (p->curve_dirty) { p->curve_dirty = false; update_curve(p); p->live_gdb = 1e9f; }
    {   /* the display cycle, one small step a block: the SC's FFT (load, 12 stages), then its strongest frequency */
        const int SC0 = 0;
        int st = p->an_step;
        if (++p->an_step >= AN_BLOCKS) p->an_step = 0;
        /* only the SC's strongest frequency is shown (no live spectra): one FFT per cycle, spread over the blocks */
        /* the SC's FFT only feeds the SC view now (SC peak / TO FREQ are gone): nothing at all runs while it is off */
        const bool kick = link && p->kick_on;
#ifndef KICK_TWICE
#define KICK_TWICE 1
#endif
        /* KICK_TWICE: the SC view is read twice a cycle (~116 ms) at blocks 0 and 40, OUT once at block 20: they share
         * the FFT buffers and never overlap (each takes 14 blocks) */
        const int sst = KICK_TWICE ? st % (AN_BLOCKS / 2) : st;
        const int ost = KICK_TWICE ? (st >= AN_BLOCKS / 4 && st < AN_BLOCKS / 2 ? st - AN_BLOCKS / 4 : -1) : (st >= AN_BLOCKS / 2 ? st - AN_BLOCKS / 2 : -1);
        if (sst == SC0) { if (kick) an_load(p, tap.buf, tap.wpos.load(std::memory_order_acquire)); }
        else if (sst > SC0 && sst <= SC0 + FFT_LOG) { if (kick) an_stage(p, sst - SC0 - 1); }
        else if (sst == SC0 + FFT_LOG + 1) {
            if (kick) { if (an_kick(p)) ++p->display_rev; }
            else if (p->kick_nz) kick_clear(p);                              /* off, or no SC: nothing drawn */
        }
        /* OUT: the output's spectrum once a cycle (~4 times a second), half a cycle after the SC view, so the two graphs
         * are not repainted in the same screen read */
        else if (p->out_on) {
            int o = ost;
            if (o == 0) an_load(p, p->obuf, p->opos, OBUF - 1);
            else if (o > 0 && o <= FFT_LOG) an_stage(p, o - 1);
            else if (o == FFT_LOG + 1) { if (an_spec(p)) ++p->display_rev; }
        }
    }
    if ((p->an_step & 3) == 0) update_ring(p);                            /* the live dot, ~86 times a second (cheap: 1 or 2 values move) */
    if ((p->an_step & 3) == 2) update_live(p);                            /* the live curve: when the gain moved 1 dB; a column only moves 4 dB at a time */

    if (p->attack != p->last_atk || p->release != p->last_rel) {   /* envelope: only when Atk or Rel move */
        p->katk = expf(-1.0f / (0.001f * pos_to_ms(p->attack) * SR)); p->krel = expf(-1.0f / (0.001f * pos_to_ms(p->release) * SR));
        p->katk16 = powf(p->katk, 16.0f); p->krel16 = powf(p->krel, 16.0f);
        p->last_atk = p->attack; p->last_rel = p->release;
    }
    const float amt = p->amount / 10.0f;
    static const int SUB = 16;
    static const float glide = 1.0f - expf(-(float)SUB / (0.005f * SR));
    const float hz = pos_to_hz(p->freq), q = pos_to_q(p->qpos);   /* two powf a block: cheap next to the rest */
    if (hz != p->last_hz || q != p->last_q) {            /* the bell's allpass: only when FREQ or Q move */
        p->ap = allpass(hz, q);
        p->last_hz = hz; p->last_q = q;
    }

    for (int i0 = 0; i0 < frames; i0 += SUB) {
        int n = frames - i0 < SUB ? frames - i0 : SUB;
        /* the detector listens to the whole SC, wherever the kick is (its 50 Hz), and the bell acts wherever FREQ is (a
         * hi-hat's 8 kHz): as TB Equalizer does. It used to listen through the bell's band, where a kick has almost
         * nothing at 8 kHz, so a boost there fluttered on the click's noise and chirped. */
        {   /* detector: the SC's peak over the last 20 ms (a sliding max of sub-block peaks), then Atk / Rel on that */
            float m = 0.0f;
            if (link) for (int j = 0; j < n; ++j) { float v = fabsf(tap.buf[(p->rpos + (uint32_t)(i0 + j)) & (RING - 1)]); if (v > m) m = v; }
            p->wmax[p->wpos] = m; if (++p->wpos >= WIN_N) p->wpos = 0;
            float w = 0.0f; for (int t = 0; t < WIN_N; ++t) if (p->wmax[t] > w) w = p->wmax[t];
            p->env = w + (w > p->env ? p->katk16 : p->krel16) * (p->env - w);
        }
        float edb = 20.0f * log10f(p->env > 1e-6f ? p->env : 1e-6f);
        float over = edb - p->thresh; if (over < 0.0f) over = 0.0f;       /* dB above the threshold */
        /* AMOUNT is the depth reached; RATIO sets how far over the threshold the SC must go to reach it:
         * 24 / (ratio - 1) dB (2:1 -> 24 dB, 4:1 -> 8 dB, 20:1 -> 1.3 dB), FULL at once (0.5 dB), 1:1 never */
        float reach = p->ratio >= RATIO_FULL - 0.5f ? 0.5f : (p->ratio <= 1.001f ? 1e9f : 24.0f / (p->ratio - 1.0f));
        float tgt = amt * clampf(over / reach, 0.0f, 1.0f);
        p->gdb += (tgt - p->gdb) * glide;
        if (fabsf(p->gdb) < 1e-3f && tgt == 0.0f) p->gdb = 0.0f;
        /* the gain glides linearly across the sub-block, sample by sample: no zipper even on a fast, deep cut */
        const float g1 = p->gdb == 0.0f ? 1.0f : powf(10.0f, p->gdb / 40.0f);
        const float k2 = p->ap.k2, b1 = p->ap.b1, gk = 0.045f;   /* the gain follows its target through a ~0.5 ms one-pole, sample by sample */
        float g = p->glin;
        for (int j = 0; j < n; ++j) {
            int idx = 2 * (i0 + j);
            g += (g1 - g) * gk;
            p->noise = p->noise * 1664525u + 1013904223u;
            float tiny = ((int)(p->noise >> 9) - (1 << 22)) * 1e-28f;
            float l = in[idx] + tiny, r = in[idx + 1] + tiny;
            /* allpass, transposed direct form II */
            float al = k2 * l + p->zl1; p->zl1 = b1 * l - b1 * al + p->zl2; p->zl2 = l - k2 * al;
            float ar = k2 * r + p->zr1; p->zr1 = b1 * r - b1 * ar + p->zr2; p->zr2 = r - k2 * ar;
            float ml = 0.5f * ((l + al) + g * (l - al)), mr = 0.5f * ((r + ar) + g * (r - ar));
            float bl = k2 * ml + p->zl3; p->zl3 = b1 * ml - b1 * bl + p->zl4; p->zl4 = ml - k2 * bl;
            float br = k2 * mr + p->zr3; p->zr3 = b1 * mr - b1 * br + p->zr4; p->zr4 = mr - k2 * br;
            float yl = 0.5f * ((ml + bl) + g * (ml - bl)), yr = 0.5f * ((mr + br) + g * (mr - br));
            if (g > 1.0001f) {   /* only while boosting: what the plugin adds never passes 0 dBFS; anything else keeps the float headroom */
                if (fabsf(yl) > LIM_KNEE) yl = copysignf(LIM_KNEE + (LIM_CEIL - LIM_KNEE) * tanhf((fabsf(yl) - LIM_KNEE) / (LIM_CEIL - LIM_KNEE)), yl);
                if (fabsf(yr) > LIM_KNEE) yr = copysignf(LIM_KNEE + (LIM_CEIL - LIM_KNEE) * tanhf((fabsf(yr) - LIM_KNEE) / (LIM_CEIL - LIM_KNEE)), yr);
            }
            out[idx] = yl; out[idx + 1] = yr;
            if (p->out_on) p->obuf[(p->opos + (uint32_t)(i0 + j)) & (OBUF - 1)] = 0.5f * (yl + yr);
        }
        p->glin = g;
    }
    p->rpos += (uint32_t)frames;
    if (p->out_on) p->opos += (uint32_t)frames;


}

/* the framework's contract: interleaved int16 (the stock wrapper converts and clips at 0 dBFS) */
static void process(void *inst, const int16_t *in, int16_t *out, int frames)
{
    KD *p = static_cast<KD *>(inst);
    if (!p || !in || !out || frames <= 0) return;
    float fi[2 * 128], fo[2 * 128];
    for (int done = 0; done < frames; done += 128) {
        int n = frames - done < 128 ? frames - done : 128;
        for (int i = 0; i < 2 * n; ++i) fi[i] = in[2 * done + i] * (1.0f / 32768.0f);
        run(p, fi, fo, n);
        for (int i = 0; i < 2 * n; ++i) out[2 * done + i] = to_s16(fo[i]);
    }
}

/* the float path (wrapper/vst2_wrap.c, this port's copy of the framework's wrapper, calls it when it exists): 32-bit float
 * from MPC to MPC, no 16-bit rounding and no clipping. A weak symbol for the wrapper, as mpc_engine_changed */
extern "C" int mpc_engine_process_f32(void *inst, const float *in_lr, float *out_lr, int frames)
{
    KD *p = static_cast<KD *>(inst);
    if (!p || !in_lr || !out_lr || frames <= 0) return 0;
    for (int done = 0; done < frames; done += 128) {
        int n = frames - done < 128 ? frames - done : 128;
        run(p, in_lr + 2 * done, out_lr + 2 * done, n);
    }
    return 1;
}

/* what the engine changed itself, so the screen follows: the knobs a touch on the graph moved, then the touched columns
 * (the host's copy moved with the finger: put the engine's back, once the curve is redrawn) */
extern "C" int mpc_engine_changed(void *inst, char *key, int len)
{
    KD *p = static_cast<KD *>(inst);
    if (!p || !key || len <= 0) return 0;
    if (p->touch_flags & 1) { p->touch_flags &= ~1; snprintf(key, (size_t)len, "freq"); return 1; }
    if (p->touch_flags & 2) { p->touch_flags &= ~2; snprintf(key, (size_t)len, "amount"); return 1; }
    if (p->curve_dirty) return 0;
    for (int c = 0; c < NCOL; ++c)
        if (p->col_touched[c]) { p->col_touched[c] = 0; snprintf(key, (size_t)len, "c%d", c); return 1; }
    return 0;
}

static const mpc_engine_t engine = {create, destroy, midi, set_param, get_param, render, process, nullptr};
extern "C" const mpc_engine_t *mpc_engine(void) { return &engine; }
