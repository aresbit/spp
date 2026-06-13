#pragma once

#include <spp/core/base.h>

namespace spp::Net {

// Byte_Stream is the contract every transport above the syscall layer speaks:
// blocking send_all_result / recv_result / recv_exact_result / close. Plain
// TCP (Net::Tcp_Client), TLS-wrapped streams (companion library), test fakes
// (Net::Memory_Stream), and sidecar UDS wrappers all satisfy this concept,
// which lets the HTTP and WebSocket layers stay transport-agnostic and
// dependency-free.
template<typename S>
concept Byte_Stream = requires(S& s, Slice<const u8> in, Slice<u8> out) {
    { s.send_all_result(in) } -> Same<Result<u64, String_View>>;
    { s.recv_result(out) } -> Same<Result<u64, String_View>>;
    { s.recv_exact_result(out) } -> Same<Result<u64, String_View>>;
    { s.close() } -> Same<void>;
};

// Type-erased counterpart for when the transport must be chosen at runtime
// (e.g. config-driven plain TCP vs TLS). Costs one virtual call per send /
// recv; if you can pin the type at compile time prefer the concept directly.
struct Stream {
    virtual ~Stream() noexcept = default;
    [[nodiscard]] virtual Result<u64, String_View>
    send_all_result(Slice<const u8> in) noexcept = 0;
    [[nodiscard]] virtual Result<u64, String_View>
    recv_result(Slice<u8> out) noexcept = 0;
    [[nodiscard]] virtual Result<u64, String_View>
    recv_exact_result(Slice<u8> out) noexcept = 0;
    virtual void close() noexcept = 0;
};

// Adapts any Byte_Stream into the virtual Stream interface. Caller owns the
// underlying stream and must outlive the adapter.
template<typename S>
    requires Byte_Stream<S>
struct Stream_Adapter final : Stream {
    explicit Stream_Adapter(S& s) noexcept : inner_(&s) {
    }

    [[nodiscard]] Result<u64, String_View>
    send_all_result(Slice<const u8> in) noexcept override {
        return inner_->send_all_result(in);
    }
    [[nodiscard]] Result<u64, String_View> recv_result(Slice<u8> out) noexcept override {
        return inner_->recv_result(out);
    }
    [[nodiscard]] Result<u64, String_View> recv_exact_result(Slice<u8> out) noexcept override {
        return inner_->recv_exact_result(out);
    }
    void close() noexcept override {
        inner_->close();
    }

private:
    S* inner_ = null;
};

// In-memory byte stream. `inject()` preloads bytes the consumer will see via
// recv_*; `sent()` exposes whatever the producer wrote via send_all_result.
// Used for unit-testing HTTP/WS without touching sockets and as the canonical
// reference impl when teaching the Byte_Stream contract.
struct Memory_Stream {
    Memory_Stream() noexcept : outgoing_(64), incoming_(64) {
    }

    [[nodiscard]] Result<u64, String_View> send_all_result(Slice<const u8> in) noexcept {
        if(closed_) return Result<u64, String_View>::err("closed"_v);
        for(u64 i = 0; i < in.length(); i++) outgoing_.push(in[i]);
        return Result<u64, String_View>::ok(static_cast<u64>(in.length()));
    }

    [[nodiscard]] Result<u64, String_View> recv_result(Slice<u8> out) noexcept {
        u64 available = incoming_.length() - incoming_cursor_;
        u64 n = available < out.length() ? available : out.length();
        for(u64 i = 0; i < n; i++) out[i] = incoming_[incoming_cursor_ + i];
        incoming_cursor_ += n;
        // n == 0 here means EOF, mirroring the TCP convention.
        return Result<u64, String_View>::ok(spp::move(n));
    }

    [[nodiscard]] Result<u64, String_View> recv_exact_result(Slice<u8> out) noexcept {
        u64 available = incoming_.length() - incoming_cursor_;
        if(available < out.length()) {
            return Result<u64, String_View>::err("short_read"_v);
        }
        for(u64 i = 0; i < out.length(); i++) out[i] = incoming_[incoming_cursor_ + i];
        incoming_cursor_ += out.length();
        return Result<u64, String_View>::ok(static_cast<u64>(out.length()));
    }

    void close() noexcept {
        closed_ = true;
    }

    void inject(Slice<const u8> bytes) noexcept {
        for(u64 i = 0; i < bytes.length(); i++) incoming_.push(bytes[i]);
    }
    void inject(String_View bytes) noexcept {
        inject(Slice<const u8>{bytes.data(), bytes.length()});
    }

    [[nodiscard]] Slice<const u8> sent() const noexcept {
        return outgoing_.slice();
    }
    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

private:
    Vec<u8> outgoing_;
    Vec<u8> incoming_;
    u64 incoming_cursor_ = 0;
    bool closed_ = false;
};

} // namespace spp::Net
