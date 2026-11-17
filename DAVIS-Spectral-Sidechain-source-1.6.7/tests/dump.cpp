// Runs a synthetic kick (SEND) and bass (RECEIVE) and prints the graph data the skin shows: key value per line.
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstdlib>
extern "C" {
#include "engine.h"
}
int main(int argc, char **argv)
{
    const mpc_engine_t *E = mpc_engine();
    void *snd = E->create(nullptr), *rcv = E->create(nullptr);
    E->set_param(snd, "mode", "1");
    E->set_param(rcv, "freq", argc > 1 ? argv[1] : "193");   // ~65 Hz
    E->set_param(rcv, "q", getenv("KD_Q") ? getenv("KD_Q") : "45"); E->set_param(rcv, "amount", getenv("KD_AMT") ? getenv("KD_AMT") : "-240"); E->set_param(rcv, "attack", "18"); E->set_param(rcv, "release", "81"); E->set_param(rcv, "thresh", "-26");
    if (getenv("KD_OUTON")) E->set_param(rcv, "out", "1");
    const int F = 128; const double SR = 44100;
    long total = (long)(SR * 3.2); double bt = 0.5;                 // kick every 0.5 s
    std::vector<int16_t> k(2 * F), b(2 * F), o(2 * F);
    int stop = (int)(total / F);
    for (int blk = 0; blk < stop; ++blk) {
        for (int i = 0; i < F; ++i) {
            double t = (blk * F + i) / SR, tk = fmod(t, bt);
            double fr = 50 + 110 * exp(-tk * 28);                    // pitch drop
            static double ph = 0; ph += 2 * M_PI * fr / SR; if (tk < 1.0 / SR) ph = 0;
            double kv = sin(ph) * exp(-tk * 16) * 0.85 + (tk < 0.004 ? 0.25 * sin(2 * M_PI * 900 * tk) : 0);
            static unsigned ns = 12345; ns = ns * 1664525u + 1013904223u; static double lpn = 0; lpn += 0.18 * (((int)(ns >> 9) - (1 << 22)) / (double)(1 << 22) - lpn);
            double bv = 0.30 * sin(2 * M_PI * 65 * t) + 0.16 * sin(2 * M_PI * 130 * t) + 0.10 * sin(2 * M_PI * 260 * t) + 0.55 * lpn;
            k[2 * i] = k[2 * i + 1] = (int16_t)(kv * 30000); b[2 * i] = b[2 * i + 1] = (int16_t)(bv * 30000);
        }
        E->process(snd, k.data(), o.data(), F); E->process(rcv, b.data(), o.data(), F);
    }
    char buf[64];
    const char *pre = "ckgbe"; (void)pre;
    for (const char *p = "chls"; *p; ++p) for (int i = 0; i < 112; ++i) { char key[8]; snprintf(key, 8, "%c%d", *p, i); E->get_param(rcv, key, buf, 64); printf("%s %s\n", key, buf); }
    for (const char *p = "er"; *p; ++p) for (int i = 0; i < 48; ++i) { char key[8]; snprintf(key, 8, "%c%d", *p, i); E->get_param(rcv, key, buf, 64); printf("%s %s\n", key, buf); }
    for (const char *key : {"freq_display", "q_display", "amount_display", "thresh_display", "ratio_display", "ratio", "attack_display", "release_display", "gr_display", "peak_display", "freq", "q", "amount", "thresh", "attack", "release", "link_name"}) { buf[0] = 0; E->get_param(rcv, key, buf, 64); printf("%s %s\n", key, buf); }
    return 0;
}
