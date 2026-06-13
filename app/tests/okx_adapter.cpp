#include "test.h"

#include <okx/adapter.h>
#include <okx/models.h>

namespace Okx = spp::App::Okx;
namespace QD  = spp::quant::data;

static f64 dec_to_f64(spp::Decimal<8> d) noexcept {
    return static_cast<f64>(d.raw()) /
           static_cast<f64>(spp::Decimal<8>::factor());
}

i32 main() {
    Test test{"empty"_v};

    Trace("candlestick_to_bar maps fields and converts strings to Decimal") {
        Okx::Candlestick c;
        c.open_time = 1700000000000LL;
        c.open  = "42000.5"_v.string<Mdefault>();
        c.high  = "42500.0"_v.string<Mdefault>();
        c.low   = "41800.25"_v.string<Mdefault>();
        c.close = "42200.0"_v.string<Mdefault>();
        c.vol   = "1.75"_v.string<Mdefault>();
        c.confirm = 1;

        auto bar = Okx::candlestick_to_bar(c);
        assert(bar.time.unix_ms() == 1700000000000LL);
        assert(dec_to_f64(bar.open)  == 42000.5);
        assert(dec_to_f64(bar.high)  == 42500.0);
        assert(dec_to_f64(bar.low)   == 41800.25);
        assert(dec_to_f64(bar.close) == 42200.0);
        assert(bar.volume > 1.749 && bar.volume < 1.751);
    }

    Trace("candles_to_ohlcv REVERSES OKX newest-first order to chronological") {
        // OKX returns candles newest-first. Adapter must flip.
        spp::Vec<Okx::Candlestick, Mdefault> raw;
        Okx::Candlestick c1, c2, c3;
        c1.open_time = 1700000180000LL;  // newest
        c1.open = "3"_v.string<Mdefault>(); c1.high = "3"_v.string<Mdefault>();
        c1.low = "3"_v.string<Mdefault>(); c1.close = "3"_v.string<Mdefault>();
        c1.vol = "1"_v.string<Mdefault>();
        raw.push(spp::move(c1));

        c2.open_time = 1700000120000LL;
        c2.open = "2"_v.string<Mdefault>(); c2.high = "2"_v.string<Mdefault>();
        c2.low = "2"_v.string<Mdefault>(); c2.close = "2"_v.string<Mdefault>();
        c2.vol = "1"_v.string<Mdefault>();
        raw.push(spp::move(c2));

        c3.open_time = 1700000060000LL;  // oldest
        c3.open = "1"_v.string<Mdefault>(); c3.high = "1"_v.string<Mdefault>();
        c3.low = "1"_v.string<Mdefault>(); c3.close = "1"_v.string<Mdefault>();
        c3.vol = "1"_v.string<Mdefault>();
        raw.push(spp::move(c3));

        auto ohlcv = Okx::candles_to_ohlcv<Mdefault>("BTC-USDT"_v, raw);
        assert(ohlcv.bars.length() == 3);
        // Chronological order — oldest first.
        assert(ohlcv.bars[0].bar.time.unix_ms() == 1700000060000LL);
        assert(ohlcv.bars[1].bar.time.unix_ms() == 1700000120000LL);
        assert(ohlcv.bars[2].bar.time.unix_ms() == 1700000180000LL);
        assert(ohlcv.bars[0].symbol == "BTC-USDT"_v);
        assert(dec_to_f64(ohlcv.bars[0].bar.close) == 1.0);
        assert(dec_to_f64(ohlcv.bars[2].bar.close) == 3.0);
    }

    Trace("Empty candles vector → empty Ohlcv_Data") {
        spp::Vec<Okx::Candlestick, Mdefault> empty;
        auto ohlcv = Okx::candles_to_ohlcv<Mdefault>("BTC-USDT"_v, empty);
        assert(ohlcv.bars.length() == 0);
    }

    return 0;
}
