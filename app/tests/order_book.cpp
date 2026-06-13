#include "test.h"

#include <binance/order_book.h>

namespace Bnc = spp::App::Binance;

i32 main() {
    Test test{"empty"_v};

    Trace("Depth_Snapshot JSON parse") {
        String_View raw =
            "{\"lastUpdateId\":1027024,"
            "\"bids\":[[\"4.00000000\",\"431.00000000\"],"
            "[\"3.99000000\",\"100.00000000\"]],"
            "\"asks\":[[\"4.00000200\",\"12.00000000\"]]}"_v;
        auto parsed = Json::parse_result<Bnc::Depth_Snapshot>(raw);
        assert(parsed.ok());
        auto& s = parsed.unwrap();
        assert(s.lastUpdateId == 1027024);
        assert(s.bids.length() == 2);
        assert(s.bids[0].length() == 2);
        assert(s.bids[0][0] == "4.00000000"_v);
        assert(s.bids[0][1] == "431.00000000"_v);
        assert(s.asks.length() == 1);
    }

    Trace("Order_Book builds from snapshot with descending bids / ascending asks") {
        Bnc::Depth_Snapshot s;
        s.lastUpdateId = 100;
        auto push_level = [](Bnc::Depth_Snapshot& dst, bool bid,
                             String_View price, String_View qty) {
            Bnc::Wire_Level w;
            w.push(price.string<Mdefault>());
            w.push(qty.string<Mdefault>());
            if(bid) dst.bids.push(spp::move(w));
            else dst.asks.push(spp::move(w));
        };

        push_level(s, true, "99.5"_v, "1.0"_v);
        push_level(s, true, "100.0"_v, "2.0"_v); // intentionally out of order
        push_level(s, true, "99.0"_v, "3.0"_v);

        push_level(s, false, "101.0"_v, "1.5"_v);
        push_level(s, false, "100.5"_v, "0.5"_v);

        auto book = Bnc::Order_Book::from_snapshot(s);
        assert(book.last_update_id == 100);

        // Bids descending: 100.0 > 99.5 > 99.0
        assert(book.bids.length() == 3);
        assert(book.bids[0].price == "100.0"_v);
        assert(book.bids[1].price == "99.5"_v);
        assert(book.bids[2].price == "99.0"_v);

        // Asks ascending: 100.5 < 101.0
        assert(book.asks.length() == 2);
        assert(book.asks[0].price == "100.5"_v);
        assert(book.asks[1].price == "101.0"_v);

        auto bb = book.best_bid();
        assert(bb.ok());
        assert(bb->price == "100.0"_v);
        auto ba = book.best_ask();
        assert(ba.ok());
        assert(ba->price == "100.5"_v);
    }

    Trace("apply() respects Binance sequence number protocol") {
        Bnc::Depth_Snapshot s;
        s.lastUpdateId = 100;
        Bnc::Wire_Level w;
        w.push("100.0"_v.string<Mdefault>());
        w.push("1.0"_v.string<Mdefault>());
        s.bids.push(spp::move(w));
        auto book = Bnc::Order_Book::from_snapshot(s);

        // Stale: u <= last_update_id.
        Bnc::Depth_Update u_stale;
        u_stale.U = 50;
        u_stale.u = 100;
        assert(book.apply(u_stale) == Bnc::Order_Book::Merge_Status::stale_update);

        // Gap: U > last_update_id + 1.
        Bnc::Depth_Update u_gap;
        u_gap.U = 200;
        u_gap.u = 250;
        assert(book.apply(u_gap) == Bnc::Order_Book::Merge_Status::gap);
        assert(book.last_update_id == 100);

        // Clean: U == 101 (last + 1), u == 105.
        Bnc::Depth_Update u_ok;
        u_ok.U = 101;
        u_ok.u = 105;
        {
            Bnc::Wire_Level x;
            x.push("100.0"_v.string<Mdefault>());
            x.push("5.0"_v.string<Mdefault>()); // update qty
            u_ok.b.push(spp::move(x));
        }
        {
            Bnc::Wire_Level x;
            x.push("99.0"_v.string<Mdefault>());
            x.push("3.0"_v.string<Mdefault>()); // new bid level
            u_ok.b.push(spp::move(x));
        }
        assert(book.apply(u_ok) == Bnc::Order_Book::Merge_Status::ok);
        assert(book.last_update_id == 105);
        assert(book.bids.length() == 2);
        assert(book.bids[0].price == "100.0"_v);
        assert(book.bids[0].qty == "5.0"_v); // qty got updated
        assert(book.bids[1].price == "99.0"_v);
    }

    Trace("qty=0 removes level (Binance encoding)") {
        Bnc::Depth_Snapshot s;
        s.lastUpdateId = 100;
        for(String_View p : {"99.0"_v, "100.0"_v, "101.0"_v}) {
            Bnc::Wire_Level w;
            w.push(p.string<Mdefault>());
            w.push("1.0"_v.string<Mdefault>());
            s.bids.push(spp::move(w));
        }
        auto book = Bnc::Order_Book::from_snapshot(s);
        assert(book.bids.length() == 3);

        Bnc::Depth_Update u;
        u.U = 101;
        u.u = 102;
        // Remove the 100.0 level with qty=0.
        Bnc::Wire_Level w;
        w.push("100.0"_v.string<Mdefault>());
        w.push("0.00000000"_v.string<Mdefault>());
        u.b.push(spp::move(w));
        assert(book.apply(u) == Bnc::Order_Book::Merge_Status::ok);
        assert(book.bids.length() == 2);
        assert(book.bids[0].price == "101.0"_v);
        assert(book.bids[1].price == "99.0"_v);
    }

    Trace("Removing a non-existent level is a no-op (not an error)") {
        Bnc::Order_Book book; // empty book, last_update_id = 0
        Bnc::Depth_Update u;
        u.U = 1;
        u.u = 1;
        Bnc::Wire_Level w;
        w.push("100.0"_v.string<Mdefault>());
        w.push("0"_v.string<Mdefault>());
        u.b.push(spp::move(w));
        assert(book.apply(u) == Bnc::Order_Book::Merge_Status::ok);
        assert(book.bids.empty());
    }

    return 0;
}
