#pragma once
#include <spp/core/base.h>
#include <spp/quant/data/types.h>

namespace spp::quant::data {

struct Price_Level {
    Decimal<8> price;
    f64 quantity;
    u32 order_count;
};

struct Level2_Snapshot {
    Deterministic_Time time;
    String<Mdefault> code;
    Decimal<8> pre_close;
    Decimal<8> open;
    Decimal<8> high;
    Decimal<8> low;
    Decimal<8> last;
    f64 total_volume;
    f64 total_value;
    Array<Price_Level, 10> bids;
    Array<Price_Level, 10> asks;
    u32 num_trades;
    f64 total_bid_qty;
    f64 total_offer_qty;
    Decimal<8> weighted_avg_bid;
    Decimal<8> weighted_avg_offer;
};

template <typename A = Mdefault>
struct Level2_Data {
    Vec<Level2_Snapshot, A> snapshots;

    Level2_Data() noexcept = default;
    Level2_Data(Level2_Data&&) noexcept = default;
    Level2_Data& operator=(Level2_Data&&) noexcept = default;

    [[nodiscard]] u64 snapshot_count() const noexcept {
        return snapshots.length();
    }

    [[nodiscard]] Vec<String<A>, A> symbols() const noexcept {
        Map<String<Mdefault>, u8, A> seen;
        Vec<String<A>, A> out;
        for(u64 i = 0; i < snapshots.length(); i++) {
            String_View sv = snapshots[i].code.view();
            if(!seen.contains(sv)) {
                seen.insert(snapshots[i].code.clone(), 1);
                out.push(sv.template string<A>());
            }
        }
        return out;
    }

    [[nodiscard]] Level2_Data<A> select_code(String_View code) const noexcept {
        Level2_Data<A> result;
        for(u64 i = 0; i < snapshots.length(); i++) {
            if(snapshots[i].code == code) {
                Level2_Snapshot snap;
                snap.time = snapshots[i].time;
                snap.code = snapshots[i].code.clone();
                snap.pre_close = snapshots[i].pre_close;
                snap.open = snapshots[i].open;
                snap.high = snapshots[i].high;
                snap.low = snapshots[i].low;
                snap.last = snapshots[i].last;
                snap.total_volume = snapshots[i].total_volume;
                snap.total_value = snapshots[i].total_value;
                snap.bids = snapshots[i].bids.clone();
                snap.asks = snapshots[i].asks.clone();
                snap.num_trades = snapshots[i].num_trades;
                snap.total_bid_qty = snapshots[i].total_bid_qty;
                snap.total_offer_qty = snapshots[i].total_offer_qty;
                snap.weighted_avg_bid = snapshots[i].weighted_avg_bid;
                snap.weighted_avg_offer = snapshots[i].weighted_avg_offer;
                result.snapshots.push(spp::move(snap));
            }
        }
        return result;
    }

    [[nodiscard]] Level2_Data<A> select_time(Deterministic_Time start,
                                             Deterministic_Time end) const noexcept {
        Level2_Data<A> result;
        for(u64 i = 0; i < snapshots.length(); i++) {
            auto t = snapshots[i].time;
            if(!(t < start) && t < end) {
                Level2_Snapshot snap;
                snap.time = snapshots[i].time;
                snap.code = snapshots[i].code.clone();
                snap.pre_close = snapshots[i].pre_close;
                snap.open = snapshots[i].open;
                snap.high = snapshots[i].high;
                snap.low = snapshots[i].low;
                snap.last = snapshots[i].last;
                snap.total_volume = snapshots[i].total_volume;
                snap.total_value = snapshots[i].total_value;
                snap.bids = snapshots[i].bids.clone();
                snap.asks = snapshots[i].asks.clone();
                snap.num_trades = snapshots[i].num_trades;
                snap.total_bid_qty = snapshots[i].total_bid_qty;
                snap.total_offer_qty = snapshots[i].total_offer_qty;
                snap.weighted_avg_bid = snapshots[i].weighted_avg_bid;
                snap.weighted_avg_offer = snapshots[i].weighted_avg_offer;
                result.snapshots.push(spp::move(snap));
            }
        }
        return result;
    }

    [[nodiscard]] Decimal<8> spread(i32 level = 0) const noexcept {
        if(snapshots.length() == 0) return Decimal<8>::from_int(0);
        auto& snap = snapshots[0];
        if(level < 0 || level >= 10) return Decimal<8>::from_int(0);
        auto bid = snap.bids[static_cast<u64>(level)].price;
        auto ask = snap.asks[static_cast<u64>(level)].price;
        return ask - bid;
    }

    [[nodiscard]] f64 order_book_imbalance() const noexcept {
        if(snapshots.length() == 0) return 0.0;
        auto& snap = snapshots[0];
        f64 total_bid = 0.0;
        f64 total_ask = 0.0;
        for(u64 i = 0; i < 10; i++) {
            total_bid += snap.bids[i].quantity;
            total_ask += snap.asks[i].quantity;
        }
        f64 total = total_bid + total_ask;
        if(total == 0.0) return 0.0;
        return (total_bid - total_ask) / total;
    }
};

} // namespace spp::quant::data

SPP_RECORD(spp::quant::data::Price_Level,
    SPP_FIELD(price), SPP_FIELD(quantity), SPP_FIELD(order_count));

SPP_NAMED_RECORD(spp::quant::data::Level2_Snapshot, "Level2_Snapshot",
    SPP_FIELD(time), SPP_FIELD(code), SPP_FIELD(pre_close),
    SPP_FIELD(open), SPP_FIELD(high), SPP_FIELD(low), SPP_FIELD(last),
    SPP_FIELD(total_volume), SPP_FIELD(total_value), SPP_FIELD(bids),
    SPP_FIELD(asks), SPP_FIELD(num_trades), SPP_FIELD(total_bid_qty),
    SPP_FIELD(total_offer_qty), SPP_FIELD(weighted_avg_bid),
    SPP_FIELD(weighted_avg_offer));
