#include "test.h"

enum class Side : u8 {
    buy,
    sell,
};

struct TradeV1 {
    i64 id = 0;
    Decimal_4 px{};
    i64 qty = 0;
    Side side = Side::buy;
};

struct TradeV2 {
    i64 id = 0;
    Decimal_4 px{};
    i64 qty = 0;
    Side side = Side::buy;
    i32 venue = 0;
};

SPP_ENUM(Side, buy, SPP_CASE(buy), SPP_CASE(sell));
SPP_NAMED_RECORD(TradeV1, "Trade", SPP_FIELD(id), SPP_FIELD(px), SPP_FIELD(qty), SPP_FIELD(side));
SPP_NAMED_RECORD(TradeV2, "Trade", SPP_FIELD(id), SPP_FIELD(px), SPP_FIELD(qty), SPP_FIELD(side),
                 SPP_FIELD(venue));

i32 main() {
    Test test{"empty"_v};

    Trace("Binary encode/decode basic") {
        TradeV1 t1{};
        t1.id = 42;
        t1.px = Decimal_4::from_raw(123456);
        t1.qty = 9;
        t1.side = Side::sell;

        auto encoded = Binary::encode_result(t1, 1);
        assert(encoded.ok());

        auto decoded = Binary::decode_result<TradeV1>(encoded.unwrap().slice());
        assert(decoded.ok());
        auto t2 = decoded.unwrap();
        assert(t2.id == 42);
        assert(t2.px.raw() == 123456);
        assert(t2.qty == 9);
        assert(t2.side == Side::sell);
    }

    Trace("Binary schema evolution old->new") {
        TradeV1 old{};
        old.id = 7;
        old.px = Decimal_4::from_raw(654321);
        old.qty = 88;
        old.side = Side::buy;

        auto encoded = Binary::encode_result(old, 1);
        assert(encoded.ok());

        auto new_read = Binary::decode_with_header_result<TradeV2>(encoded.unwrap().slice());
        assert(new_read.ok());
        auto got = new_read.unwrap().first;
        auto hdr = new_read.unwrap().second;

        assert(hdr.schema == 1);
        assert(got.id == old.id);
        assert(got.px.raw() == old.px.raw());
        assert(got.qty == old.qty);
        assert(got.side == old.side);
        assert(got.venue == 0); // missing field keeps default
    }

    Trace("Binary schema evolution new->old") {
        TradeV2 now{};
        now.id = 1001;
        now.px = Decimal_4::from_raw(1000000);
        now.qty = 12;
        now.side = Side::sell;
        now.venue = 3;

        auto encoded = Binary::encode_result(now, 2);
        assert(encoded.ok());

        auto old_read = Binary::decode_with_header_result<TradeV1>(encoded.unwrap().slice());
        assert(old_read.ok());
        auto got = old_read.unwrap().first;
        auto hdr = old_read.unwrap().second;

        assert(hdr.schema == 2);
        assert(got.id == now.id);
        assert(got.px.raw() == now.px.raw());
        assert(got.qty == now.qty);
        assert(got.side == now.side);
    }

    Trace("Binary persist/load") {
        String_View path = "tmp_trade.bin"_v;
        TradeV2 in{};
        in.id = 9;
        in.px = Decimal_4::from_raw(424242);
        in.qty = 100;
        in.side = Side::buy;
        in.venue = 11;

        auto wrote = Binary::persist_result(path, in, 3);
        assert(wrote.ok());
        assert(wrote.unwrap() > 0);

        auto loaded = Binary::load_with_header_result<TradeV2>(path);
        assert(loaded.ok());
        auto out = loaded.unwrap().first;
        assert(loaded.unwrap().second.schema == 3);
        assert(out.id == in.id);
        assert(out.px.raw() == in.px.raw());
        assert(out.qty == in.qty);
        assert(out.side == in.side);
        assert(out.venue == in.venue);

        auto removed = Files::remove_result(path);
        assert(removed.ok());
    }

    return 0;
}
