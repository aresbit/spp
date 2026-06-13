#pragma once
#include <spp/core/base.h>
#include <spp/quant/data/types.h>

namespace spp::quant::data {

struct Financial_Report {
    String<Mdefault> code;
    Deterministic_Time report_date;
    f64 eps;
    f64 bvps;
    f64 roe;
    f64 roa;
    f64 gross_margin;
    f64 net_margin;
    f64 debt_to_equity;
    f64 current_ratio;
    f64 quick_ratio;
    f64 revenue;
    f64 revenue_yoy;
    f64 net_profit;
    f64 net_profit_yoy;
    f64 total_assets;
    f64 total_equity;
    f64 total_liabilities;
    f64 operating_cashflow;
    f64 investing_cashflow;
    f64 financing_cashflow;
    f64 market_cap;
    f64 float_market_cap;
};

template <typename A = Mdefault>
struct Financial_Data {
    Vec<Financial_Report, A> reports;

    Financial_Data() noexcept = default;
    Financial_Data(Financial_Data&&) noexcept = default;
    Financial_Data& operator=(Financial_Data&&) noexcept = default;

    [[nodiscard]] u64 report_count() const noexcept {
        return reports.length();
    }

    [[nodiscard]] Vec<String<A>, A> symbols() const noexcept {
        Map<String<Mdefault>, u8, A> seen;
        Vec<String<A>, A> out;
        for(u64 i = 0; i < reports.length(); i++) {
            String_View sv = reports[i].code.view();
            if(!seen.contains(sv)) {
                seen.insert(reports[i].code.clone(), 1);
                out.push(sv.template string<A>());
            }
        }
        return out;
    }

    [[nodiscard]] Financial_Data<A> select_code(String_View code) const noexcept {
        Financial_Data<A> result;
        for(u64 i = 0; i < reports.length(); i++) {
            if(reports[i].code == code) {
                Financial_Report r;
                r.code = reports[i].code.clone();
                r.report_date = reports[i].report_date;
                r.eps = reports[i].eps;
                r.bvps = reports[i].bvps;
                r.roe = reports[i].roe;
                r.roa = reports[i].roa;
                r.gross_margin = reports[i].gross_margin;
                r.net_margin = reports[i].net_margin;
                r.debt_to_equity = reports[i].debt_to_equity;
                r.current_ratio = reports[i].current_ratio;
                r.quick_ratio = reports[i].quick_ratio;
                r.revenue = reports[i].revenue;
                r.revenue_yoy = reports[i].revenue_yoy;
                r.net_profit = reports[i].net_profit;
                r.net_profit_yoy = reports[i].net_profit_yoy;
                r.total_assets = reports[i].total_assets;
                r.total_equity = reports[i].total_equity;
                r.total_liabilities = reports[i].total_liabilities;
                r.operating_cashflow = reports[i].operating_cashflow;
                r.investing_cashflow = reports[i].investing_cashflow;
                r.financing_cashflow = reports[i].financing_cashflow;
                r.market_cap = reports[i].market_cap;
                r.float_market_cap = reports[i].float_market_cap;
                result.reports.push(spp::move(r));
            }
        }
        return result;
    }

    [[nodiscard]] Financial_Data<A> select_date(Deterministic_Time start,
                                                Deterministic_Time end) const noexcept {
        Financial_Data<A> result;
        for(u64 i = 0; i < reports.length(); i++) {
            auto t = reports[i].report_date;
            if(!(t < start) && t < end) {
                Financial_Report r;
                r.code = reports[i].code.clone();
                r.report_date = reports[i].report_date;
                r.eps = reports[i].eps;
                r.bvps = reports[i].bvps;
                r.roe = reports[i].roe;
                r.roa = reports[i].roa;
                r.gross_margin = reports[i].gross_margin;
                r.net_margin = reports[i].net_margin;
                r.debt_to_equity = reports[i].debt_to_equity;
                r.current_ratio = reports[i].current_ratio;
                r.quick_ratio = reports[i].quick_ratio;
                r.revenue = reports[i].revenue;
                r.revenue_yoy = reports[i].revenue_yoy;
                r.net_profit = reports[i].net_profit;
                r.net_profit_yoy = reports[i].net_profit_yoy;
                r.total_assets = reports[i].total_assets;
                r.total_equity = reports[i].total_equity;
                r.total_liabilities = reports[i].total_liabilities;
                r.operating_cashflow = reports[i].operating_cashflow;
                r.investing_cashflow = reports[i].investing_cashflow;
                r.financing_cashflow = reports[i].financing_cashflow;
                r.market_cap = reports[i].market_cap;
                r.float_market_cap = reports[i].float_market_cap;
                result.reports.push(spp::move(r));
            }
        }
        return result;
    }

    [[nodiscard]] Vec<f64, A> field_values(String_View field_name) const noexcept {
        Vec<f64, A> out;
        for(u64 i = 0; i < reports.length(); i++) {
            auto& r = reports[i];
            if(field_name == "eps"_v) out.push(r.eps);
            else if(field_name == "bvps"_v) out.push(r.bvps);
            else if(field_name == "roe"_v) out.push(r.roe);
            else if(field_name == "roa"_v) out.push(r.roa);
            else if(field_name == "gross_margin"_v) out.push(r.gross_margin);
            else if(field_name == "net_margin"_v) out.push(r.net_margin);
            else if(field_name == "debt_to_equity"_v) out.push(r.debt_to_equity);
            else if(field_name == "current_ratio"_v) out.push(r.current_ratio);
            else if(field_name == "quick_ratio"_v) out.push(r.quick_ratio);
            else if(field_name == "revenue"_v) out.push(r.revenue);
            else if(field_name == "revenue_yoy"_v) out.push(r.revenue_yoy);
            else if(field_name == "net_profit"_v) out.push(r.net_profit);
            else if(field_name == "net_profit_yoy"_v) out.push(r.net_profit_yoy);
            else if(field_name == "total_assets"_v) out.push(r.total_assets);
            else if(field_name == "total_equity"_v) out.push(r.total_equity);
            else if(field_name == "total_liabilities"_v) out.push(r.total_liabilities);
            else if(field_name == "operating_cashflow"_v) out.push(r.operating_cashflow);
            else if(field_name == "investing_cashflow"_v) out.push(r.investing_cashflow);
            else if(field_name == "financing_cashflow"_v) out.push(r.financing_cashflow);
            else if(field_name == "market_cap"_v) out.push(r.market_cap);
            else if(field_name == "float_market_cap"_v) out.push(r.float_market_cap);
        }
        return out;
    }
};

} // namespace spp::quant::data

SPP_NAMED_RECORD(spp::quant::data::Financial_Report, "Financial_Report",
    SPP_FIELD(code), SPP_FIELD(report_date), SPP_FIELD(eps),
    SPP_FIELD(bvps), SPP_FIELD(roe), SPP_FIELD(roa),
    SPP_FIELD(gross_margin), SPP_FIELD(net_margin),
    SPP_FIELD(debt_to_equity), SPP_FIELD(current_ratio),
    SPP_FIELD(quick_ratio), SPP_FIELD(revenue),
    SPP_FIELD(revenue_yoy), SPP_FIELD(net_profit),
    SPP_FIELD(net_profit_yoy), SPP_FIELD(total_assets),
    SPP_FIELD(total_equity), SPP_FIELD(total_liabilities),
    SPP_FIELD(operating_cashflow), SPP_FIELD(investing_cashflow),
    SPP_FIELD(financing_cashflow), SPP_FIELD(market_cap),
    SPP_FIELD(float_market_cap));
