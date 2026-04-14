#pragma once

#include <spp/core/base.h>

namespace spp::IO {

using Alloc = Mallocator<"IO">;
template<typename T>
using IO_Result = Result<T, String_View>;

enum struct Handle_Kind : u8 {
    invalid = 0,
    file = 1,
    socket = 2,
    pipe_read = 3,
    pipe_write = 4,
};

enum struct Poll_Event : u16 {
    none = 0,
    in = 1 << 0,
    out = 1 << 1,
    err = 1 << 2,
    hup = 1 << 3,
};

[[nodiscard]] constexpr Poll_Event operator|(Poll_Event a, Poll_Event b) noexcept {
    return static_cast<Poll_Event>(static_cast<u16>(a) | static_cast<u16>(b));
}
[[nodiscard]] constexpr Poll_Event operator&(Poll_Event a, Poll_Event b) noexcept {
    return static_cast<Poll_Event>(static_cast<u16>(a) & static_cast<u16>(b));
}
constexpr Poll_Event& operator|=(Poll_Event& a, Poll_Event b) noexcept {
    a = a | b;
    return a;
}

[[nodiscard]] constexpr bool has_any(Poll_Event value, Poll_Event mask) noexcept {
    return (value & mask) != Poll_Event::none;
}

struct Handle {
    uptr native = ~uptr{0};
    Handle_Kind kind = Handle_Kind::invalid;
    bool owned = true;

    [[nodiscard]] bool valid() const noexcept {
        return native != ~uptr{0} && kind != Handle_Kind::invalid;
    }
    void invalidate() noexcept {
        native = ~uptr{0};
        kind = Handle_Kind::invalid;
        owned = false;
    }
};

struct Pipe {
    Handle read;
    Handle write;
};

struct Poll_Target {
    Handle* handle = null;
    Poll_Event events = Poll_Event::none;
    Poll_Event revents = Poll_Event::none;
};

struct Poll_Result {
    u64 ready_index = 0;
    Poll_Event events = Poll_Event::none;
};

struct Capability_Matrix {
    bool file_handles = false;
    bool socket_handles = false;
    bool pipe_handles = false;

    bool poll_file = false;
    bool poll_socket = false;
    bool poll_pipe = false;

    bool edge_triggered = false;
    bool io_uring = false;
    bool kqueue = false;
    bool iocp = false;
};

[[nodiscard]] inline Handle file_handle(uptr native, bool owned = true) noexcept {
    return Handle{native, Handle_Kind::file, owned};
}
[[nodiscard]] inline Handle socket_handle(uptr native, bool owned = true) noexcept {
    return Handle{native, Handle_Kind::socket, owned};
}
[[nodiscard]] inline Handle pipe_read_handle(uptr native, bool owned = true) noexcept {
    return Handle{native, Handle_Kind::pipe_read, owned};
}
[[nodiscard]] inline Handle pipe_write_handle(uptr native, bool owned = true) noexcept {
    return Handle{native, Handle_Kind::pipe_write, owned};
}

[[nodiscard]] Capability_Matrix capability_matrix() noexcept;

[[nodiscard]] IO_Result<Pipe> pipe_result() noexcept;
[[nodiscard]] IO_Result<u64> close_result(Handle& handle) noexcept;
[[nodiscard]] IO_Result<u64> read_some_result(const Handle& handle, Slice<u8> out) noexcept;
[[nodiscard]] IO_Result<u64> write_some_result(const Handle& handle, Slice<const u8> in) noexcept;
[[nodiscard]] IO_Result<Poll_Result> poll_any_result(Slice<Poll_Target> targets,
                                                     u64 timeout_ms) noexcept;
[[nodiscard]] IO_Result<bool> poll_one_result(const Handle& handle, Poll_Event events,
                                              u64 timeout_ms) noexcept;

} // namespace spp::IO
