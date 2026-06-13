#pragma once

#include <spp/core/base.h>
#include <spp/quant/data/ohlcv_data.h>

namespace spp::quant::broker {

struct Exchange_Config {
    String<Mdefault> api_key;
    String<Mdefault> secret_key;
    String<Mdefault> host;
    u16 port = 443;
};

struct Account_Info {
    f64 total_equity = 0.0;
    f64 available_balance = 0.0;
    f64 frozen_balance = 0.0;
    f64 margin_used = 0.0;
    f64 unrealized_pnl = 0.0;
    f64 realized_pnl = 0.0;
};

struct Order {
    String<Mdefault> order_id;
    String<Mdefault> symbol;
    f64 price;
    f64 quantity;
    u8 side;
    Deterministic_Time timestamp;
};

struct Quote {
    f64 bid;
    f64 ask;
    f64 bid_size;
    f64 ask_size;
    Deterministic_Time timestamp;
};

template <typename A = Mdefault>
struct Exchange_Adapter {

    Exchange_Adapter() noexcept = default;
    virtual ~Exchange_Adapter() = default;

    Exchange_Adapter(const Exchange_Adapter&) = delete;
    Exchange_Adapter& operator=(const Exchange_Adapter&) = delete;
    Exchange_Adapter(Exchange_Adapter&&) = default;
    Exchange_Adapter& operator=(Exchange_Adapter&&) = default;

    virtual auto connect(const Exchange_Config&) -> Result<u8, String_View> {
        return Result<u8, String_View>::ok(u8{0});
    }
    virtual auto disconnect() -> void {}
    virtual auto place_order(const Order&)
        -> Result<Order, String_View> {
        return Result<Order, String_View>::err("not_implemented"_v);
    }
    virtual auto cancel_order(String_View)
        -> Result<Order, String_View> {
        return Result<Order, String_View>::err("not_implemented"_v);
    }
    virtual auto query_order(String_View)
        -> Result<Order, String_View> {
        return Result<Order, String_View>::err("not_implemented"_v);
    }
    virtual auto query_position(String_View)
        -> Result<f64, String_View> {
        return Result<f64, String_View>::err("not_implemented"_v);
    }
    virtual auto query_account()
        -> Result<Account_Info, String_View> {
        return Result<Account_Info, String_View>::err("not_implemented"_v);
    }
    virtual auto fetch_bars(String_View, String_View, Deterministic_Time,
                             Deterministic_Time)
        -> Result<data::Ohlcv_Data<A>, String_View> {
        return Result<data::Ohlcv_Data<A>, String_View>::err("not_implemented"_v);
    }
    virtual auto fetch_ticker(String_View)
        -> Result<Quote, String_View> {
        return Result<Quote, String_View>::err("not_implemented"_v);
    }
    virtual auto is_connected() -> bool { return false; }
};

} // namespace spp::quant::broker

SPP_NAMED_RECORD(spp::quant::broker::Exchange_Config, "QB_Exchange_Config",
    SPP_FIELD(api_key), SPP_FIELD(secret_key), SPP_FIELD(host), SPP_FIELD(port));

SPP_NAMED_RECORD(spp::quant::broker::Account_Info, "QB_Account_Info",
    SPP_FIELD(total_equity), SPP_FIELD(available_balance), SPP_FIELD(frozen_balance),
    SPP_FIELD(margin_used), SPP_FIELD(unrealized_pnl), SPP_FIELD(realized_pnl));

SPP_RECORD(spp::quant::broker::Quote,
    SPP_FIELD(bid), SPP_FIELD(ask), SPP_FIELD(bid_size), SPP_FIELD(ask_size),
    SPP_FIELD(timestamp));
