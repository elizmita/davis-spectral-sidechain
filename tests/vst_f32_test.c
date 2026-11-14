/* The whole plugin as MPC calls it (VSTPluginMain, processReplacing), built with this port's wrapper/vst2_wrap.c:
 * a +6 dBFS signal must come back unclipped and unrounded, in 128-frame blocks and in odd-sized blocks, and in place. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
typedef struct AEffect AEffect;
typedef intptr_t (*cb)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
struct AEffect { int32_t magic; intptr_t (*d)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
 void*p; void (*setP)(AEffect*,int32_t,float); float (*getP)(AEffect*,int32_t);
 int32_t np,npar,ni,no,flags; intptr_t r1,r2; int32_t a,b,c; float io; void*obj,*user; int32_t uid,ver;
 void (*pr)(AEffect*,float**,float**,int32_t); void*pdr; char f[56]; };
extern AEffect* VSTPluginMain(cb);
static intptr_t host(AEffect*e,int32_t op,int32_t i,intptr_t v,void*p,float o){ (void)e;(void)op;(void)i;(void)v;(void)p;(void)o; return 0; }
static int fails;
static double sig(long n) { return 2.0 * sin(2 * M_PI * 60.0 * n / 44100.0) + 0.3 * sin(2 * M_PI * 7000.0 * n / 44100.0); }
static void test(int bs, int inplace) {
    AEffect *a = VSTPluginMain(host);
    float L[256], R[256], OL[256], OR[256];
    double worst = 0, peak = 0; long n = 0;
    int lat = bs % 128 ? 128 : 0;                 /* odd blocks are collected: one 128-frame block late */
    for (int k = 0; k < 4000 / bs * 20; k++) {
        for (int i = 0; i < bs; i++) L[i] = R[i] = (float)sig(n + i);
        float *in[2] = {L, R}, *out[2] = {inplace ? L : OL, inplace ? R : OR};
        a->pr(a, in, out, bs);
        for (int i = 0; i < bs; i++, n++) if (n >= 1024 + lat) {
            double want = sig(n - lat), got = out[0][i];
            if (fabs(got - want) > worst) worst = fabs(got - want);
            if (fabs(got) > peak) peak = fabs(got);
        }
    }
    int ok = worst < 1e-5 && peak > 1.9;
    printf("%s block %3d%s: peak %.3f (want > 1.9, no clip at 1.0), max error %.2g\n", ok ? "ok  " : "FAIL", bs, inplace ? " in place" : "", peak, worst);
    if (!ok) fails++;
    a->d(a, 1, 0, 0, 0, 0);   /* effClose */
}
int main(void) { test(128, 0); test(128, 1); test(100, 0); test(64, 1); printf(fails ? "%d FAILED\n" : "PASSED\n", fails); return fails != 0; }
