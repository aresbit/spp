
#include "test.h"

#include <spp/core/rng.h>

i32 main() {
    Test test{"empty"_v};

    // RNG unit() distribution: must produce values in [0, 1). Prior bug used
    // base-10 scaling (1e-24 / 1e-53) instead of binary (2^-24 / 2^-53), which
    // returned values near zero and biased coin_flip toward false-for-positive-p.
    RNG::Stream rng(0x9E3779B97F4A7C15ULL);

    constexpr u64 N = 8192;
    f64 sum_f64 = 0.0;
    f32 sum_f32 = 0.0f;
    bool saw_above_quarter_f64 = false;
    bool saw_above_quarter_f32 = false;
    for(u64 i = 0; i < N; i++) {
        f64 d = rng.unit<f64>();
        assert(d >= 0.0 && d < 1.0);
        sum_f64 += d;
        if(d >= 0.25) saw_above_quarter_f64 = true;

        f32 s = rng.unit<f32>();
        assert(s >= 0.0f && s < 1.0f);
        sum_f32 += s;
        if(s >= 0.25f) saw_above_quarter_f32 = true;
    }

    f64 mean_f64 = sum_f64 / static_cast<f64>(N);
    f32 mean_f32 = sum_f32 / static_cast<f32>(N);
    // A uniform distribution on [0,1) has mean 0.5. With N=8192 samples even a
    // very loose 0.4..0.6 band catches the broken-scaling regression (which
    // produced means ~10^-8 and ~10^-17).
    assert(mean_f64 > 0.4 && mean_f64 < 0.6);
    assert(mean_f32 > 0.4f && mean_f32 < 0.6f);
    assert(saw_above_quarter_f64);
    assert(saw_above_quarter_f32);

    // coin_flip should split roughly evenly at p=0.5.
    u64 heads = 0;
    for(u64 i = 0; i < N; i++) {
        if(rng.coin_flip<f64>(0.5)) heads++;
    }
    assert(heads > N * 4 / 10 && heads < N * 6 / 10);

    return 0;
}
