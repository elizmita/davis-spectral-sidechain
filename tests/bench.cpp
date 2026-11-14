// Times every process() call of a SEND + RECEIVE pair: average and worst block (the worst one is what makes audio stutter).
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <cstdlib>
extern "C" {
#include "engine.h"
}
int main()
{
    const mpc_engine_t *E = mpc_engine();
    void *s = E->create(nullptr), *r = E->create(nullptr);
    E->set_param(s, "mode", "1");
    if (getenv("KD_OUT")) E->set_param(r, "out", "1");
    const int F = 128, N = 4000;                    // ~11.6 s of audio
    std::vector<int16_t> k(2 * F), b(2 * F), o(2 * F);
    double sum = 0, worst = 0; std::vector<double> step(80, 0);
    for (int blk = 0; blk < N; ++blk) {
        for (int i = 0; i < F; ++i) {
            double t = (blk * F + i) / 44100.0;
            k[2 * i] = k[2 * i + 1] = (int16_t)(20000 * sin(2 * M_PI * 55 * t) * exp(-fmod(t, .5) * 8));
            b[2 * i] = b[2 * i + 1] = (int16_t)(9000 * sin(2 * M_PI * 65 * t));
        }
        auto t0 = std::chrono::steady_clock::now();
        E->process(s, k.data(), o.data(), F); E->process(r, b.data(), o.data(), F);
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        if (blk < 400) continue;                     // skip warm-up (caches, emulator translation)
        sum += us; if (us > worst) worst = us; if (us > step[blk % 80]) step[blk % 80] = us;
    }
    printf("avg %.1f us  worst %.1f us  (block = 2902 us)\n", sum / (N - 400), worst);
    for (int i = 0; i < 80; ++i) printf("%d:%.0f ", i, step[i]); printf("\n");
}
