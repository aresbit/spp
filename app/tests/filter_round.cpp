#include "test.h"

#include <binance/filter_round.h>

namespace Bnc = spp::App::Binance;

i32 main() {
    Test test{"empty"_v};

    Trace("round_price rounds DOWN to tickSize") {
        Bnc::Filter_Cache<> cache;
        Bnc::Symbol_Filters f;
        f.tick_size = 0.01;
        f.step_size = 0.00001;
        f.min_qty   = 0.00001;
        f.min_notional = 10.0;
        cache.put("BTCUSDT"_v, f);

        // 42000.123 → 42000.12 (down to nearest 0.01).
        f64 p = cache.round_price("BTCUSDT"_v, 42000.123);
        assert(p > 42000.119 && p < 42000.121);

        // Already a multiple of tick → unchanged.
        f64 p2 = cache.round_price("BTCUSDT"_v, 42000.10);
        assert(p2 > 42000.099 && p2 < 42000.101);
    }

    Trace("round_qty rounds DOWN to stepSize, zeroes below minQty") {
        Bnc::Filter_Cache<> cache;
        Bnc::Symbol_Filters f;
        f.tick_size = 0.01;
        f.step_size = 0.0001;
        f.min_qty   = 0.001;
        cache.put("BTCUSDT"_v, f);

        // 0.001234567 → 0.0012 (4-decimal step, rounded down).
        f64 q = cache.round_qty("BTCUSDT"_v, 0.001234567);
        assert(q > 0.00119 && q < 0.00121);

        // Below minQty → 0.0 (caller should treat as "can't place").
        f64 q2 = cache.round_qty("BTCUSDT"_v, 0.0005);
        assert(q2 == 0.0);
    }

    Trace("notional_ok rejects orders smaller than minNotional") {
        Bnc::Filter_Cache<> cache;
        Bnc::Symbol_Filters f;
        f.min_notional = 10.0;
        cache.put("BTCUSDT"_v, f);

        assert(!cache.notional_ok("BTCUSDT"_v, 42000.0, 0.0001));      // 4.2 < 10
        assert(!cache.notional_ok("BTCUSDT"_v, 42000.0, 0.0002));      // 8.4 < 10
        assert(cache.notional_ok("BTCUSDT"_v, 42000.0, 0.0003));       // 12.6 >= 10
    }

    Trace("Unknown symbol passes inputs through unchanged") {
        Bnc::Filter_Cache<> cache;
        assert(cache.round_price("ETHUSDT"_v, 1234.567) == 1234.567);
        assert(cache.round_qty("ETHUSDT"_v, 0.111111) == 0.111111);
        assert(cache.notional_ok("ETHUSDT"_v, 1.0, 1.0));
    }

    return 0;
}
