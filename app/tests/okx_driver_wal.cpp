#include "test.h"

#include <spp/io/files.h>

#include <okx/driver_wal.h>

namespace Okx = spp::App::Okx;

// Each trace writes to a unique filename — a per-run counter in the
// path avoids cross-trace truncation issues.
static u32 gSeq = 1;

i32 main() {
    Test test{"empty"_v};

    Trace("WAL round-trip: confirm + cancel entries survive replay") {
        char fbuf[40];
        i32 fn = Libc::snprintf((u8*)fbuf, sizeof(fbuf),
                                "okx_wal_%u.bin", gSeq++);
        String_View path(reinterpret_cast<const u8*>(fbuf), (u64)fn);

        WAL::Writer wal;
        assert(wal.open_result(path, 0, 0).ok());

        assert(Okx::wal_append_confirm(wal, "local1"_v, "312269865356374016"_v).ok());
        assert(Okx::wal_append_confirm(wal, "local2"_v, "99"_v).ok());
        assert(Okx::wal_append_cancel(wal, "local1"_v).ok());
        assert(wal.flush_result().ok());

        auto raw_res = Files::read_result(path);
        assert(raw_res.ok());
        auto& raw = raw_res.unwrap();
        assert(raw.length() > 0);

        auto state = Okx::replay_wal<Mdefault>(raw.slice());
        assert(state.exchange_ord_id.length() == 2);
        assert(state.exchange_ord_id.get("local1"_v) == "312269865356374016"_v);
        assert(state.exchange_ord_id.get("local2"_v) == "99"_v);
        assert(state.cancel_done.length() == 1);
        assert(state.cancel_done.contains("local1"_v));
        assert(state.pending_at_crash == 0);

        static_cast<void>(Files::remove_result(path));
    }

    Trace("WAL: pending_at_crash counts type-0x01 entries") {
        char fbuf[40];
        i32 fn = Libc::snprintf((u8*)fbuf, sizeof(fbuf),
                                "okx_wal_%u.bin", gSeq++);
        String_View path(reinterpret_cast<const u8*>(fbuf), (u64)fn);

        WAL::Writer wal;
        assert(wal.open_result(path, 0, 0).ok());

        using Dir = spp::quant::strategy::Order_Direction;
        using Off = spp::quant::strategy::Order_Offset;
        assert(Okx::wal_append_order(wal, "oid1"_v, "BTC-USDT"_v, Dir::buy,
                                      Off::open_, 4200000000LL, 0.001).ok());
        assert(Okx::wal_append_order(wal, "oid2"_v, "ETH-USDT"_v, Dir::sell,
                                      Off::close_, 250000000000LL, 0.1).ok());
        assert(Okx::wal_append_confirm(wal, "oid2"_v, "eth_ord_abc"_v).ok());
        assert(Okx::wal_append_order(wal, "oid3"_v, "BTC-USDT"_v, Dir::buy,
                                      Off::open_, 4200100000LL, 0.002).ok());
        assert(wal.flush_result().ok());

        auto raw_res = Files::read_result(path);
        assert(raw_res.ok());
        auto state = Okx::replay_wal<Mdefault>(raw_res.unwrap().slice());
        assert(state.pending_at_crash == 3);
        assert(state.exchange_ord_id.length() == 1);
        assert(state.exchange_ord_id.get("oid2"_v) == "eth_ord_abc"_v);

        static_cast<void>(Files::remove_result(path));
    }

    Trace("WAL: empty file replays to empty state") {
        char fbuf[40];
        i32 fn = Libc::snprintf((u8*)fbuf, sizeof(fbuf),
                                "okx_wal_%u.bin", gSeq++);
        String_View path(reinterpret_cast<const u8*>(fbuf), (u64)fn);
        assert(Files::write_result(path, Slice<const u8>{null, 0}).ok());

        auto raw_res = Files::read_result(path);
        assert(raw_res.ok());
        auto state = Okx::replay_wal<Mdefault>(raw_res.unwrap().slice());
        assert(state.exchange_ord_id.length() == 0);
        assert(state.cancel_done.length() == 0);
        assert(state.pending_at_crash == 0);

        static_cast<void>(Files::remove_result(path));
    }

    Trace("WAL: overwritten confirm replaces the same order_id mapping") {
        char fbuf[40];
        i32 fn = Libc::snprintf((u8*)fbuf, sizeof(fbuf),
                                "okx_wal_%u.bin", gSeq++);
        String_View path(reinterpret_cast<const u8*>(fbuf), (u64)fn);

        WAL::Writer wal;
        assert(wal.open_result(path, 0, 0).ok());
        assert(Okx::wal_append_confirm(wal, "local1"_v, "ord_old"_v).ok());
        assert(Okx::wal_append_confirm(wal, "local1"_v, "ord_new"_v).ok());
        assert(wal.flush_result().ok());

        auto raw_res = Files::read_result(path);
        assert(raw_res.ok());
        auto state = Okx::replay_wal<Mdefault>(raw_res.unwrap().slice());
        // Deduplication must keep the later overwrite, not duplicate keys.
        assert(state.exchange_ord_id.length() == 1);
        assert(state.exchange_ord_id.get("local1"_v) == "ord_new"_v);

        static_cast<void>(Files::remove_result(path));
    }

    return 0;
}
