#include "test.h"

#include <binance/models.h>

namespace Bnc = spp::App::Binance;

i32 main() {
    Test test{"empty"_v};

    Trace("Server_Time JSON round-trip") {
        // Binance returns {"serverTime":<int>}.
        auto parsed = Json::parse_result<Bnc::Server_Time>("{\"serverTime\":1499827319559}"_v);
        assert(parsed.ok());
        assert(parsed.unwrap().serverTime == 1499827319559LL);

        auto encoded = Json::stringify(parsed.unwrap());
        assert(encoded == "{\"serverTime\":1499827319559}"_v);
    }

    Trace("Ticker_Price keeps price as String (precision-preserving)") {
        auto parsed = Json::parse_result<Bnc::Ticker_Price>(
            "{\"symbol\":\"BTCUSDT\",\"price\":\"42500.12345678\"}"_v);
        assert(parsed.ok());
        assert(parsed.unwrap().symbol == "BTCUSDT"_v);
        assert(parsed.unwrap().price == "42500.12345678"_v);
    }

    Trace("Account_Info with balances Vec") {
        String_View raw =
            "{\"makerCommission\":10,\"takerCommission\":10,\"updateTime\":1700000000000,"
            "\"canTrade\":true,\"canWithdraw\":true,\"canDeposit\":true,"
            "\"balances\":["
            "{\"asset\":\"BTC\",\"free\":\"0.5\",\"locked\":\"0.0\"},"
            "{\"asset\":\"USDT\",\"free\":\"1000.0\",\"locked\":\"50.0\"}"
            "]}"_v;
        auto parsed = Json::parse_result<Bnc::Account_Info>(raw);
        assert(parsed.ok());
        auto& a = parsed.unwrap();
        assert(a.canTrade);
        assert(a.updateTime == 1700000000000LL);
        assert(a.balances.length() == 2);
        assert(a.balances[0].asset == "BTC"_v);
        assert(a.balances[0].free == "0.5"_v);
        assert(a.balances[1].asset == "USDT"_v);
        assert(a.balances[1].locked == "50.0"_v);
    }

    Trace("Order_Response parse") {
        String_View raw =
            "{\"symbol\":\"BTCUSDT\",\"orderId\":28,\"orderListId\":-1,"
            "\"clientOrderId\":\"6gCrw2kRUAF9CvJDGP16IP\",\"transactTime\":1507725176595,"
            "\"price\":\"0.00000000\",\"origQty\":\"10.00000000\",\"executedQty\":\"10.00000000\","
            "\"cummulativeQuoteQty\":\"10.00000000\",\"status\":\"FILLED\","
            "\"timeInForce\":\"GTC\",\"type\":\"MARKET\",\"side\":\"SELL\"}"_v;
        auto parsed = Json::parse_result<Bnc::Order_Response>(raw);
        assert(parsed.ok());
        auto& o = parsed.unwrap();
        assert(o.symbol == "BTCUSDT"_v);
        assert(o.orderId == 28);
        assert(o.status == "FILLED"_v);
        assert(o.side == "SELL"_v);
    }

    Trace("Enum wire-name helpers match Binance canonical casing") {
        assert(wire_name(Bnc::Side::BUY) == "BUY"_v);
        assert(wire_name(Bnc::Side::SELL) == "SELL"_v);
        assert(wire_name(Bnc::Order_Type::LIMIT) == "LIMIT"_v);
        assert(wire_name(Bnc::Order_Type::STOP_LOSS_LIMIT) == "STOP_LOSS_LIMIT"_v);
        assert(wire_name(Bnc::Time_In_Force::GTC) == "GTC"_v);
        assert(wire_name(Bnc::Time_In_Force::IOC) == "IOC"_v);
    }

    return 0;
}
