#pragma once

#include <spp/core/base.h>
#include <spp/quant/data/panel_data.h>

namespace spp::quant::factor {

struct Factor_Metadata {
    String<Mdefault> name;
    String<Mdefault> frequency;
    Deterministic_Time created_at;
    u32 version = 1;
};

template <typename A = Mdefault>
struct Factor_Store {
    Map<String<A>, Factor_Metadata, A> _metadata;
    Map<String<A>, Map<Deterministic_Time, Map<String<A>, f64, A>, A>, A> _data;

    Factor_Store() noexcept = default;

    auto register_factor(String_View name, String_View freq) -> Result<u64, String_View>;
    auto save(String_View name, Deterministic_Time date,
               const Map<String<A>, f64, A>& values) -> Result<u64, String_View>;
    auto load(String_View name, Deterministic_Time start, Deterministic_Time end)
        -> Result<data::Panel_Data<A>, String_View>;
    auto list_factors() -> Vec<Factor_Metadata, A>;
    auto unregister(String_View name) -> Result<u64, String_View>;
};

template <typename A>
auto Factor_Store<A>::register_factor(String_View name, String_View freq)
    -> Result<u64, String_View> {
    if (_metadata.contains(name))
        return Result<u64, String_View>::err("factor_already_registered"_v);
    Factor_Metadata meta;
    meta.name = name.template string<A>();
    meta.frequency = freq.template string<A>();
    meta.created_at = Deterministic_Time{};
    meta.version = 1;
    _metadata.insert(name.template string<A>(), spp::move(meta));
    Map<Deterministic_Time, Map<String<A>, f64, A>, A> empty_dm;
    _data.insert(name.template string<A>(), spp::move(empty_dm));
    return Result<u64, String_View>::ok(u64{_metadata.length()});
}

template <typename A>
auto Factor_Store<A>::save(String_View name, Deterministic_Time date,
                                const Map<String<A>, f64, A>& values)
    -> Result<u64, String_View> {
    if (!_metadata.contains(name))
        return Result<u64, String_View>::err("factor_not_registered"_v);
    auto& fd = _data.get(name);
    Map<String<A>, f64, A> dm;
    for (const auto& entry : values)
        dm.insert(entry.first.template clone<A>(), f64{entry.second});
    fd.insert(Deterministic_Time::from_unix_ns(date.unix_ns()), spp::move(dm));
    return Result<u64, String_View>::ok(u64{fd.length()});
}

template <typename A>
auto Factor_Store<A>::load(String_View name, Deterministic_Time start,
                                Deterministic_Time end)
    -> Result<data::Panel_Data<A>, String_View> {
    if (!_metadata.contains(name))
        return Result<data::Panel_Data<A>, String_View>::err("factor_not_registered"_v);
    data::Panel_Data<A> panel;
    auto& fd = _data.get(name);
    for (const auto& te : fd) {
        if (!(te.first < start) && te.first < end) {
            Map<String<A>, f64, A> copy;
            for (const auto& se : te.second)
                copy.insert(se.first.template clone<A>(), f64{se.second});
            panel.data.insert(
                Deterministic_Time::from_unix_ns(te.first.unix_ns()), spp::move(copy));
        }
    }
    return Result<data::Panel_Data<A>, String_View>::ok(spp::move(panel));
}

template <typename A>
auto Factor_Store<A>::list_factors() -> Vec<Factor_Metadata, A> {
    Vec<Factor_Metadata, A> result;
    for (const auto& entry : _metadata) {
        Factor_Metadata meta;
        meta.name = entry.second.name.template clone<A>();
        meta.frequency = entry.second.frequency.template clone<A>();
        meta.created_at
            = Deterministic_Time::from_unix_ns(entry.second.created_at.unix_ns());
        meta.version = entry.second.version;
        result.push(spp::move(meta));
    }
    return result;
}

template <typename A>
auto Factor_Store<A>::unregister(String_View name) -> Result<u64, String_View> {
    if (!_metadata.contains(name))
        return Result<u64, String_View>::err("factor_not_found"_v);
    (void)name;
    return Result<u64, String_View>::ok(u64{_metadata.length()});
}

} // namespace spp::quant::factor

SPP_NAMED_RECORD(spp::quant::factor::Factor_Metadata, "QF_Factor_Metadata",
    SPP_FIELD(name), SPP_FIELD(frequency), SPP_FIELD(created_at), SPP_FIELD(version));
