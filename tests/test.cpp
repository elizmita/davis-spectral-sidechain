// Host test: SEND on a kick track, RECEIVE on a bass track, both run block by block in either order.
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdint>
#include <string>
#include <cstring>
#include <cstdlib>
#include <utility>
extern "C" {
#include "engine.h"
}
extern "C" int mpc_engine_changed(void *, char *, int);
extern "C" int mpc_engine_process_f32(void *, const float *, float *, int);
static const double SR = 44100;
static const int F = 128;
static const mpc_engine_t *E = mpc_engine();

static double goertzel(const std::vector<float> &x, size_t a, size_t b, double hz)
{
    double w = 2 * M_PI * hz / SR, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (size_t i = a; i < b; ++i) { double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return sqrt(p) * 2 / (b - a);
}
static double db(double a, double b) { return 20 * log10(a / b); }

struct Result { double bass60, bass300; };
static const char *g_ratio = "4", *g_thresh = "-30";   // overridden by the ratio / threshold checks

// kickOn: whether the kick actually plays; order: 0 send first, 1 receive first; sender: whether a send instance exists
static Result run(bool sender, bool kickOn, int order, double amountTenths, double freqPos, std::vector<float> *outLog = nullptr)
{
    void *snd = E->create(nullptr), *rcv = E->create(nullptr);
    E->set_param(snd, "mode", "1"); E->set_param(snd, "chan", "0");
    E->set_param(rcv, "mode", "0"); E->set_param(rcv, "chan", "0");
    char b[32];
    snprintf(b, sizeof b, "%g", freqPos);    E->set_param(rcv, "freq", b);
    E->set_param(rcv, "q", "55");
    snprintf(b, sizeof b, "%g", amountTenths); E->set_param(rcv, "amount", b);
    E->set_param(rcv, "thresh", g_thresh); E->set_param(rcv, "ratio", g_ratio);
    E->set_param(rcv, "attack", "2"); E->set_param(rcv, "release", "120");

    const size_t N = (size_t)(SR * 3);
    std::vector<int16_t> kick(2 * F), bass(2 * F), ko(2 * F), bo(2 * F);
    std::vector<float> out(N);
    for (size_t pos = 0; pos + F <= N; pos += F) {
        for (int i = 0; i < F; ++i) {
            double t = (pos + i) / SR, ph = fmod(t, 1.5);            // kick at 0 s and 1.5 s
            double k = kickOn ? 0.8 * sin(2 * M_PI * 55 * ph) * exp(-ph * 12.0) : 0.0;
            double bs = 0.25 * sin(2 * M_PI * 60 * t) + 0.25 * sin(2 * M_PI * 300 * t);
            kick[2 * i] = kick[2 * i + 1] = (int16_t)lrint(k * 32767);
            bass[2 * i] = bass[2 * i + 1] = (int16_t)lrint(bs * 32767);
        }
        if (sender && order == 0) E->process(snd, kick.data(), ko.data(), F);
        E->process(rcv, bass.data(), bo.data(), F);
        if (sender && order == 1) E->process(snd, kick.data(), ko.data(), F);
        for (int i = 0; i < F; ++i) out[pos + i] = bo[2 * i] / 32768.0f;
    }
    // first kick hit, steady part of the duck: 50..150 ms after the kick (6 whole periods of 60 Hz, 30 of 300 Hz: no leakage between them)
    Result r;
    size_t a = (size_t)(0.05 * SR), z = (size_t)(0.15 * SR);
    r.bass60 = db(goertzel(out, a, z, 60), 0.25);
    r.bass300 = db(goertzel(out, a, z, 300), 0.25);
    if (outLog) *outLog = out;
    E->destroy(snd); E->destroy(rcv);
    return r;
}

int main()
{
    int fails = 0;
    auto check = [&](const char *name, bool ok, double v) { printf("%-58s %s (%.2f)\n", name, ok ? "ok" : "FAIL", v); if (!ok) ++fails; };
    char buf0[32];
    const double F60 = 1000.0 * log(60.0 / 15.0) / log(20000.0 / 15.0);     // knob position of 60 Hz

    for (int order = 0; order < 2; ++order) {
        Result c = run(true, true, order, -120, F60);
        std::string o = order ? " (receive runs first)" : " (send runs first)";
        check(("cut 12 dB: 60 Hz goes down" + o).c_str(), c.bass60 < -8.0 && c.bass60 > -13.0, c.bass60);
        check(("cut 12 dB: 300 Hz untouched" + o).c_str(), fabs(c.bass300) < 1.0, c.bass300);
    }
    Result boost = run(true, true, 0, 80, F60);
    check("boost +8 dB: 60 Hz goes up", boost.bass60 > 4.0 && boost.bass60 < 9.0, boost.bass60);
    Result nokick = run(true, false, 0, -120, F60);
    check("no kick playing: bass untouched", fabs(nokick.bass60) < 0.3 && fabs(nokick.bass300) < 0.3, nokick.bass60);
    Result nosend = run(false, true, 0, -120, F60);
    check("no SEND instance: bass untouched", fabs(nosend.bass60) < 0.3, nosend.bass60);
    Result off = run(true, true, 0, -120, 1000.0 * log(250.0 / 15.0) / log(20000.0 / 15.0));
    check("band moved to 250 Hz: 60 Hz barely changes", fabs(off.bass60) < 1.5, off.bass60);


    // ratio and threshold: 1:1 does nothing; a threshold above the kick does nothing; 2:1 cuts less than 20:1; AMOUNT caps the cut
    {
        g_ratio = "1"; Result r1 = run(true, true, 0, -120, F60); g_ratio = "4";
        check("ratio 1:1: bass untouched", fabs(r1.bass60) < 0.5, r1.bass60);
        g_thresh = "0"; Result th = run(true, true, 0, -120, F60); g_thresh = "-30";
        check("threshold 0 dB (above the kick): bass untouched", fabs(th.bass60) < 1.0, th.bass60);
        g_ratio = "1.5"; g_thresh = "-12"; Result lo = run(true, true, 0, -300, F60);
        g_ratio = "20"; Result hi = run(true, true, 0, -300, F60); g_ratio = "4"; g_thresh = "-30";
        check("ratio 20:1 cuts deeper than 1.5:1", hi.bass60 < lo.bass60 - 3.0, hi.bass60 - lo.bass60);
        Result cap = run(true, true, 0, -60, F60);
        check("AMOUNT caps the cut (-6 dB)", cap.bass60 > -6.8 && cap.bass60 < -5.0, cap.bass60);
        Result cut4 = run(true, true, 0, -600, F60);
        check("Amount CUT at a plain 4:1 cuts deep too (< -40 dB)", cut4.bass60 < -40, cut4.bass60);
    }
    // the detector listens through the band: a kick at 60 Hz does not trigger a band parked at 5 kHz
    {
        Result far = run(true, true, 0, -120, 1000.0 * log(5000.0 / 15.0) / log(20000.0 / 15.0));
        check("band at 5 kHz: the 60 Hz kick does not trigger it", fabs(far.bass60) < 1.0, far.bass60);
    }

    // no clicks: a pure 60 Hz bass under the fastest, deepest cut (Atk 1 ms, CUT, FULL). A click shows as a big second difference
    {
        void *sn = E->create(nullptr), *rv = E->create(nullptr);
        E->set_param(sn, "mode", "1");
        snprintf(buf0, sizeof buf0, "%g", F60); E->set_param(rv, "freq", buf0);
        E->set_param(rv, "amount", "-600"); E->set_param(rv, "ratio", "21"); E->set_param(rv, "attack", "0"); E->set_param(rv, "release", "20");
        E->set_param(rv, "thresh", "-30"); E->set_param(rv, "q", "55");
        std::vector<int16_t> kk(2 * F), bb(2 * F), o1(2 * F), o2(2 * F);
        double prev2 = 0, prev1 = 0, worst = 0; long n = 0;
        for (int blk = 0; blk < 1000; ++blk) {
            for (int i = 0; i < F; ++i) {
                double t = (blk * F + i) / SR, ph = fmod(t, 0.4);
                kk[2 * i] = kk[2 * i + 1] = (int16_t)(26000 * sin(2 * M_PI * 55 * ph) * exp(-ph * 10));
                bb[2 * i] = bb[2 * i + 1] = (int16_t)(8000 * sin(2 * M_PI * 60 * t));
            }
            E->process(sn, kk.data(), o1.data(), F); E->process(rv, bb.data(), o2.data(), F);
            for (int i = 0; i < F; ++i, ++n) {
                double x = o2[2 * i] / 32768.0, d2 = x - 2 * prev1 + prev2;
                if (n > 2 && fabs(d2) > worst) { worst = fabs(d2); if (getenv("KD_DBG")) fprintf(stderr, "worst %g at %ld (t=%.4f s)\n", worst, n, n / SR); }
                prev2 = prev1; prev1 = x;
            }
        }
        check("no clicks under a fast full cut (2nd difference x1000 < 5)", worst * 1000 < 5, worst * 1000);
        E->destroy(sn); E->destroy(rv);
    }

    // attack: how deep is the cut ~6 ms after the kick, fast vs slow follower
    auto grAfter = [&](const char *atk) {
        void *sn = E->create(nullptr), *rv = E->create(nullptr);
        E->set_param(sn, "mode", "1"); E->set_param(rv, "mode", "0");
        E->set_param(rv, "amount", "-120"); E->set_param(rv, "attack", atk); E->set_param(rv, "release", "150");
        std::vector<int16_t> silent(2 * F, 0), kk(2 * F), o(2 * F);
        for (int i = 0; i < F; ++i) kk[2 * i] = kk[2 * i + 1] = (int16_t)(0.8 * 32767 * ((i / 40) % 2 ? -1 : 1));   // loud low square-ish burst
        for (int blk = 0; blk < 60; ++blk) { E->process(sn, silent.data(), o.data(), F); E->process(rv, silent.data(), o.data(), F); }
        for (int blk = 0; blk < 3; ++blk) { E->process(sn, kk.data(), o.data(), F); E->process(rv, silent.data(), o.data(), F); }
        char b2[32]; E->get_param(rv, "gr_display", b2, 32);
        E->destroy(sn); E->destroy(rv);
        return atof(b2);
    };
    double fastGr = grAfter("0"), slowGr = grAfter("59");
    check("attack 1 ms cuts deeper than 40 ms right after the kick", fastGr < slowGr - 1.0, fastGr - slowGr);

    // the float path: no 16-bit rounding, no clipping above 0 dBFS, the same sound as the int16 path, and in place works
    {
        // +6 dBFS bass, no SC: comes out exactly as it went in
        void *rv = E->create(nullptr);
        std::vector<float> fi(2 * F), fo(2 * F); double worstHot = 0;
        for (int blk = 0; blk < 200; ++blk) {
            for (int i = 0; i < F; ++i) fi[2 * i] = fi[2 * i + 1] = (float)(2.0 * sin(2 * M_PI * 60 * (blk * F + i) / SR));
            mpc_engine_process_f32(rv, fi.data(), fo.data(), F);
            for (int i = 0; i < 2 * F; ++i) worstHot = fmax(worstHot, fabs(fo[i] - fi[i]));
        }
        check("float path: a +6 dBFS signal passes unclipped (max error x1e6)", worstHot * 1e6 < 1, worstHot * 1e6);
        E->destroy(rv);
        // with a duck: float and int16 paths agree to within the 16-bit step
        void *s1 = E->create(nullptr), *r1 = E->create(nullptr);
        E->set_param(s1, "mode", "1"); E->set_param(s1, "chan", "2"); E->set_param(r1, "chan", "2"); E->set_param(r1, "amount", "-150");
        void *s2 = E->create(nullptr), *r2 = E->create(nullptr);
        E->set_param(s2, "mode", "1"); E->set_param(s2, "chan", "3"); E->set_param(r2, "chan", "3"); E->set_param(r2, "amount", "-150");
        std::vector<int16_t> ki(2 * F), bi(2 * F), ko(2 * F), bo(2 * F);
        std::vector<float> kf(2 * F), bf(2 * F), kfo(2 * F), bfo(2 * F); double worstDiff = 0, worstInPlace = 0;
        void *r3 = E->create(nullptr); void *s3 = E->create(nullptr);   // the same, processed in place (MPC does that)
        E->set_param(s3, "mode", "1"); E->set_param(s3, "chan", "1"); E->set_param(r3, "chan", "1"); E->set_param(r3, "amount", "-150");
        std::vector<float> kp(2 * F), bp(2 * F);
        for (int blk = 0; blk < 600; ++blk) {
            for (int i = 0; i < F; ++i) {
                double t = (blk * F + i) / SR, ph = fmod(t, 0.5);
                int16_t k = (int16_t)lrint(20000 * sin(2 * M_PI * 55 * ph) * exp(-ph * 10)), b = (int16_t)lrint(8000 * sin(2 * M_PI * 60 * t));
                ki[2 * i] = ki[2 * i + 1] = k; bi[2 * i] = bi[2 * i + 1] = b;
                kf[2 * i] = kf[2 * i + 1] = kp[2 * i] = kp[2 * i + 1] = k / 32768.0f;
                bf[2 * i] = bf[2 * i + 1] = bp[2 * i] = bp[2 * i + 1] = b / 32768.0f;
            }
            E->process(s1, ki.data(), ko.data(), F); E->process(r1, bi.data(), bo.data(), F);
            mpc_engine_process_f32(s2, kf.data(), kfo.data(), F); mpc_engine_process_f32(r2, bf.data(), bfo.data(), F);
            mpc_engine_process_f32(s3, kp.data(), kp.data(), F); mpc_engine_process_f32(r3, bp.data(), bp.data(), F);
            for (int i = 0; i < 2 * F; ++i) {
                worstDiff = fmax(worstDiff, fabs(bo[i] / 32768.0 - bfo[i]));
                worstInPlace = fmax(worstInPlace, fabs(bp[i] - bfo[i]));
            }
        }
        check("float and int16 paths agree (max diff in 16-bit steps < 1)", worstDiff * 32768 < 1.0, worstDiff * 32768);
        check("in-place float processing is identical", worstInPlace < 1e-7, worstInPlace * 1e7);
        E->destroy(s1); E->destroy(r1); E->destroy(s2); E->destroy(r2); E->destroy(s3); E->destroy(r3);
    }


    // the graph is not touchable: a set on a column moves nothing, and the column is reported back (the screen redraws it)
    {
        void *t = E->create(nullptr); char kb[32], ch[32], f0[32], a0[32];
        E->get_param(t, "freq", f0, 32); E->get_param(t, "amount", a0, 32);
        { int16_t in[2 * F] = {0}, out[2 * F]; E->process(t, in, out, F); }
        E->set_param(t, "c20", "15");
        E->get_param(t, "freq", kb, 32);   check("graph: a column set leaves freq alone", !strcmp(kb, f0), atof(kb));
        E->get_param(t, "amount", kb, 32); check("graph: a column set leaves amount alone", !strcmp(kb, a0), atof(kb));
        int nc = 0, other = 0, guard = 0;
        while (mpc_engine_changed(t, ch, 32) && ++guard < 20) { if (!strcmp(ch, "c20")) ++nc; else ++other; }
        check("graph: the column is reported back, nothing else", nc == 1 && other == 0, nc * 10 + other);
        E->destroy(t);
    }
    // the curve sits at AMOUNT at FREQ (full depth); the live ring sits at the band's column, at 0 dB with no SC
    {
        void *t2 = E->create(nullptr); char kb[32];
        E->set_param(t2, "freq", "500"); E->set_param(t2, "amount", "-150");
        int16_t zin[256] = {0}, zout[256]; for (int i = 0; i < 10; ++i) E->process(t2, zin, zout, 128);
        int cc = (int)lrint(500 / 1000.0 * 111); snprintf(kb, 32, "c%d", cc); E->get_param(t2, kb, kb, 32);
        check("curve: at -15 dB on FREQ's column (frame 15)", abs(atoi(kb) - 15) <= 1, atoi(kb));
        int rc = -1, lvl = 0;
        for (int c = 0; c < 112; ++c) { char k2[8]; snprintf(k2, 8, "h%d", c); if (E->get_param(t2, k2, kb, 32) > 0 && atoi(kb)) { rc = c; lvl = atoi(kb); } }
        check("ring: on FREQ's column (every 2nd)", abs(rc - cc) <= 1, rc);
        check("ring: at 0 dB with no SC (frame 15, +1)", lvl == 16, lvl);
        E->destroy(t2);
    }
    // the live curve: flat with no SC, dips at FREQ while the SC plays (and is back to flat after)
    {
        void *sn = E->create(nullptr), *rv = E->create(nullptr); char kb[32];
        E->set_param(sn, "mode", "1"); E->set_param(sn, "chan", "1"); E->set_param(rv, "chan", "1");
        snprintf(kb, 32, "%g", F60); E->set_param(rv, "freq", kb); E->set_param(rv, "amount", "-240"); E->set_param(rv, "ratio", "21");
        std::vector<int16_t> kk(2 * F), bb(2 * F, 0), o(2 * F);
        int cc = 2 * (int)lrint(F60 / 1000.0 * 111 / 2), lmin = 99;   // the drawn live columns are the even ones: the dot's
        for (int blk = 0; blk < 60; ++blk) {
            for (int i = 0; i < F; ++i) kk[2 * i] = kk[2 * i + 1] = (int16_t)(24000 * sin(2 * M_PI * 60 * (blk * F + i) / SR));
            E->process(sn, kk.data(), o.data(), F); E->process(rv, bb.data(), o.data(), F);
            snprintf(kb, 32, "l%d", cc); E->get_param(rv, kb, kb, 32); if (atoi(kb) < lmin) lmin = atoi(kb);
        }
        check("live curve dips at FREQ while the SC plays (-24 dB = step 3)", lmin <= 4, lmin);
        std::fill(kk.begin(), kk.end(), 0);
        for (int blk = 0; blk < 400; ++blk) { E->process(sn, kk.data(), o.data(), F); E->process(rv, bb.data(), o.data(), F); }
        snprintf(kb, 32, "l%d", cc); E->get_param(rv, kb, kb, 32);
        check("live curve back to flat after the SC stops (step 15)", atoi(kb) == 15, atoi(kb));
        E->destroy(sn); E->destroy(rv);
    }
    // OUT: off, nothing drawn; on, the output's spectrum peaks on the 100 Hz tone's column; a -20 dBFS tone reads ~-20 dB; off again clears it
    {
        void *t = E->create(nullptr); char kb[32];
        std::vector<int16_t> x(2 * F), o(2 * F);
        auto play = [&](int blocks) { static long n = 0; for (int b = 0; b < blocks; ++b) { for (int i = 0; i < F; ++i, ++n) x[2 * i] = x[2 * i + 1] = (int16_t)(3277 * sin(2 * M_PI * 100 * n / SR)); E->process(t, x.data(), o.data(), F); } };
        auto any = [&]() { int s = 0; for (int c = 0; c < 112; c += 2) { char k2[8]; snprintf(k2, 8, "s%d", c); E->get_param(t, k2, kb, 32); s += atoi(kb); } return s; };
        play(200); check("OUT off: no spectrum drawn", any() == 0, any());
        E->set_param(t, "out", "1"); play(200);
        int best = -1, bv = 0;
        for (int c = 0; c < 112; c += 2) { char k2[8]; snprintf(k2, 8, "s%d", c); E->get_param(t, k2, kb, 32); if (atoi(kb) > bv) { bv = atoi(kb); best = c; } }
        int c100 = (int)lrint(log(100 / 15.0) / log(20000 / 15.0) * 111);
        check("OUT on: the spectrum peaks on 100 Hz's column", abs(best - c100) <= 2, best);
        check("OUT on: a -20 dBFS tone reads -20 dB (step 23)", abs(bv - 23) <= 2, bv);
        { char st[128]; E->get_param(t, "state", st, 128); check("OUT is kept in the state", st[strlen(st) - 1] == '1', 0); }
        E->set_param(t, "out", "0"); check("OUT off again: cleared", any() == 0, any());
        E->set_param(t, "mode", "1"); E->set_param(t, "out", "1"); play(200);
        check("OUT in SEND: nothing drawn", any() == 0, any());
        E->destroy(t);
    }
    // a full cut: AMOUNT -60 with Ratio FULL takes 60 Hz down by ~60 dB while the SC plays
    {
        g_ratio = "21"; Result full = run(true, true, 0, -600, F60); g_ratio = "4";
        check("Amount -60 + Ratio FULL: 60 Hz cut by more than 45 dB", full.bass60 < -45, full.bass60);
        void *t3 = E->create(nullptr); char kb[32];
        E->set_param(t3, "amount", "-600"); E->get_param(t3, "amount_display", kb, 32); check("Amount -60 reads -60.0 dB", !strcmp(kb, "-60.0 dB"), 0);
        E->set_param(t3, "ratio", "21"); E->get_param(t3, "ratio_display", kb, 32); check("Ratio top reads 21.0:1", !strcmp(kb, "21.0:1"), 0);
        E->destroy(t3);
    }
    // SC view: a kick (160 -> 57 Hz drop, then a 57 Hz body) on the SC draws grey bars at its body, only while SC view is on
    {
        void *sn = E->create(nullptr), *rv = E->create(nullptr); char kb[48];
        E->set_param(sn, "mode", "1");
        std::vector<int16_t> k(2 * F), z(2 * F, 0), o(2 * F);
        double ph = 0;
        for (int blk = 0; blk < 1200; ++blk) {   // a kick every 0.5 s: a 160 -> 57 Hz drop (the click), then a 57 Hz body
            for (int i = 0; i < F; ++i) {
                double tk = fmod((blk * F + i) / SR, 0.5), fr = 57 + 103 * exp(-tk * 40);
                ph += 2 * M_PI * fr / SR;
                k[2 * i] = k[2 * i + 1] = (int16_t)(26000 * sin(ph) * exp(-tk * 7));
            }
            E->process(sn, k.data(), o.data(), F); E->process(rv, z.data(), o.data(), F);
        }
        (void)kb;
        // SC view: the kick's bars are drawn at its body only while the button is on, and nothing is drawn when it is off
        auto kickMax = [&](void *inst) { int best = 0, at = 0;
            for (int c = 0; c < 112; c += 2) { char key[8]; snprintf(key, sizeof key, "k%d", c); char v[16] = {0}; E->get_param(inst, key, v, sizeof v); int n = atoi(v); if (n > best) { best = n; at = c; } }
            return std::make_pair(best, at); };
        E->set_param(rv, "scview", "0");
        auto off = kickMax(rv);
        check("SC view off draws no kick bars", off.first == 0, off.first);
        E->set_param(rv, "scview", "1");
        std::vector<int16_t> k2(2 * F), z2(2 * F, 0), o2(2 * F);
        double ph2 = 0;
        for (int blk = 0; blk < 800; ++blk) {
            for (int i = 0; i < F; ++i) {
                double tk = fmod((blk * F + i) / SR, 0.5), fr = 57 + 103 * exp(-tk * 40);
                ph2 += 2 * M_PI * fr / SR;
                k2[2 * i] = k2[2 * i + 1] = (int16_t)(26000 * sin(ph2) * exp(-tk * 7));
            }
            E->process(sn, k2.data(), o2.data(), F); E->process(rv, z2.data(), o2.data(), F);
        }
        auto on = kickMax(rv);
        check("SC view on draws the kick's bars", on.first > 0, on.first);
        double hzAt = 15.0 * pow(20000.0 / 15.0, on.second / 111.0);
        check("SC view bars sit at the kick's body (40-90 Hz)", hzAt > 40 && hzAt < 90, hzAt);
        E->destroy(sn); E->destroy(rv);
    }
    // a boost on a hi-hat's 8 kHz while the kick hits: the gain rises and falls once a hit, no flutter (it chirped when the
    // detector listened through the bell's band), and a +30 dB boost never reaches 0 dBFS
    {
        void *sn = E->create(nullptr), *rv = E->create(nullptr); char kb[32];
        E->set_param(sn, "mode", "1");
        snprintf(kb, 32, "%g", 1000.0 * log(8000.0 / 15) / log(20000.0 / 15)); E->set_param(rv, "freq", kb);
        E->set_param(rv, "amount", "300"); E->set_param(rv, "ratio", "21"); E->set_param(rv, "attack", "0"); E->set_param(rv, "release", "0");
        E->set_param(rv, "thresh", "-40");
        std::vector<int16_t> k(2 * F), h(2 * F), o(2 * F);
        double pk = 0, px = 0, hp = 0, last = 0, dir = 0; unsigned s1 = 7, s2 = 99; int flips = 0, clips = 0, nb = 0;
        for (int blk = 0; blk < 34 * 20; ++blk) {
            for (int i = 0; i < F; ++i) {
                long n = (long)blk * F + i; double t = fmod(n / SR, 0.5), fr = 57 + 103 * exp(-t * 40);
                pk += 2 * M_PI * fr / SR; s1 = s1 * 1103515245u + 12345u; double nz = ((s1 >> 16) & 0x7fff) / 16384.0 - 1;
                double kick = 26000 * sin(pk) * exp(-t * 7) + 9000 * nz * exp(-t * 400);
                s2 = s2 * 1664525u + 1013904223u; double n2 = ((s2 >> 16) & 0x7fff) / 16384.0 - 1, y = n2 - px; px = n2; hp = 0.6 * hp + y;
                double hat = 6000 * hp * exp(-fmod(n / SR, 0.125) * 60);
                k[2 * i] = k[2 * i + 1] = (int16_t)fmax(-32767, fmin(32767, kick)); h[2 * i] = h[2 * i + 1] = (int16_t)fmax(-32767, fmin(32767, hat));
            }
            E->process(sn, k.data(), o.data(), F); E->process(rv, h.data(), o.data(), F);
            for (int i = 0; i < F; ++i) if (abs(o[2 * i]) >= 32767) clips++;
            char g[16]; E->get_param(rv, "gr", g, 16); double gd = atof(g) / 10;
            if (blk > 34 * 2) { double d = gd - last; if (fabs(d) > 0.3) { if (dir * d < 0) flips++; dir = d; } nb++; }
            last = gd;
        }
        double perSec = flips / (nb * F / SR);
        check("hi-hat boost: the gain turns about twice a hit (no flutter)", perSec <= 6, perSec);
        check("hi-hat boost +30 dB stays under 0 dBFS", clips == 0, clips);
        E->destroy(sn); E->destroy(rv);
    }
    // the graph's data
    void *r = E->create(nullptr), *s = E->create(nullptr);
    E->set_param(s, "mode", "1");
    char buf[64];
    E->get_param(r, "freq_display", buf, 64); printf("display: freq %s\n", buf);
    E->get_param(r, "amount_display", buf, 64); printf("display: amount %s\n", buf);
    E->destroy(r); E->destroy(s);
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
