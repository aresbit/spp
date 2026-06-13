#pragma once

#include <spp/crypto/encode.h>
#include <spp/crypto/hmac.h>
#include <spp/protocol/http.h>

namespace spp::Exchange::Binance {

// Production REST endpoint. Connections must go through TLS — wire this
// library into a sidecar (stunnel / envoy / sshuttle) or plug your own TLS
// stream in front of the Tcp_Client until SPP grows native TLS. See
// docs/binance_integration.md for the recommended deployment shape.
inline const String_View k_spot_host = "api.binance.com"_v;
constexpr u16 k_spot_https_port = 443;

// Testnet endpoint. Same TLS requirement applies.
inline const String_View k_spot_testnet_host = "testnet.binance.vision"_v;

// A query-string builder that takes (key, value) pairs and emits the canonical
// `k1=v1&k2=v2` form expected by Binance Spot REST. Values are passed through
// `url_encode` so they are safe for both the query string and the signature
// payload (Binance signs the raw query string verbatim — including the
// percent-encoded form, which is what we feed back into the URL).
template<Allocator A = Mdefault>
struct Query_Builder {
    String<A> buffer;

    Query_Builder() noexcept : buffer{256} {
    }

    Query_Builder& add(String_View key, String_View value) noexcept {
        if(buffer.length() > 0) buffer = append_(spp::move(buffer), "&"_v);
        buffer = append_(spp::move(buffer), key);
        buffer = append_(spp::move(buffer), "="_v);
        auto encoded = Crypto::url_encode<A>(value);
        buffer = append_(spp::move(buffer), encoded.view());
        return *this;
    }

    [[nodiscard]] String_View view() const noexcept {
        return buffer.view();
    }

private:
    [[nodiscard]] static String<A> append_(String<A> lhs, String_View rhs) noexcept {
        return lhs.view().template append<A>(rhs);
    }
};

// HMAC-SHA256 signature over a query string. Returns lowercase hex per
// Binance's `signature=` parameter format.
template<Allocator A = Mdefault>
[[nodiscard]] inline String<A> sign(String_View api_secret, String_View query) noexcept {
    auto digest =
        Crypto::HMAC_SHA256::sign(Slice<const u8>{api_secret.data(), api_secret.length()},
                                  Slice<const u8>{query.data(), query.length()});
    return Crypto::hex_encode<A>(
        Slice<const u8>{digest.bytes, Crypto::HMAC_SHA256::digest_size});
}

// Convenience: append `&signature=<hex>` to a query string.
template<Allocator A = Mdefault>
[[nodiscard]] inline String<A> with_signature(String_View api_secret,
                                              String_View query) noexcept {
    auto sig = sign<A>(api_secret, query);
    auto joined = query.template append<A>("&signature="_v);
    return joined.view().template append<A>(sig.view());
}

// Self-contained signed request that owns its path buffer. `to_bytes()` on the
// returned `request` is safe as long as `owned_path` lives.
template<Allocator A = Mdefault>
struct Signed_Request {
    String<A> owned_path;
    Protocol::Http::Request<A> request;
};

template<Allocator A = Mdefault>
[[nodiscard]] inline Signed_Request<A>
signed_request_full(String_View method, String_View path, String_View query,
                    String_View host, String_View api_key, String_View api_secret) noexcept {
    Signed_Request<A> out;
    auto signed_query = with_signature<A>(api_secret, query);
    auto p1 = path.template append<A>("?"_v);
    out.owned_path = p1.view().template append<A>(signed_query.view());

    out.request.method = method;
    out.request.host = host;
    out.request.path = out.owned_path.view();
    Protocol::Http::Header api_hdr{"X-MBX-APIKEY"_v, api_key};
    out.request.headers.push(spp::move(api_hdr));
    return out;
}

} // namespace spp::Exchange::Binance
