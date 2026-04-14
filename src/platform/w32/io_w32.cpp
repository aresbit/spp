#include <spp/io/handle.h>

#include <windows.h>
#include <winsock2.h>

namespace spp::IO {

[[nodiscard]] static SOCKET as_socket(const Handle& handle) noexcept {
    return static_cast<SOCKET>(handle.native);
}

[[nodiscard]] Capability_Matrix capability_matrix() noexcept {
    return Capability_Matrix{
        .file_handles = true,
        .socket_handles = true,
        .pipe_handles = true,
        .poll_file = false,
        .poll_socket = true,
        .poll_pipe = false,
        .edge_triggered = false,
        .io_uring = false,
        .kqueue = false,
        .iocp = true,
    };
}

[[nodiscard]] IO_Result<Pipe> pipe_result() noexcept {
    HANDLE read_h = INVALID_HANDLE_VALUE;
    HANDLE write_h = INVALID_HANDLE_VALUE;
    if(CreatePipe(&read_h, &write_h, null, 0) == 0) {
        return IO_Result<Pipe>::err(Log::sys_error());
    }
    return IO_Result<Pipe>::ok(Pipe{
        pipe_read_handle(reinterpret_cast<uptr>(read_h)),
        pipe_write_handle(reinterpret_cast<uptr>(write_h)),
    });
}

[[nodiscard]] IO_Result<u64> close_result(Handle& handle) noexcept {
    if(!handle.valid()) return IO_Result<u64>::ok(0);
    if(!handle.owned) {
        handle.invalidate();
        return IO_Result<u64>::ok(0);
    }

    bool ok = false;
    if(handle.kind == Handle_Kind::socket) {
        ok = closesocket(as_socket(handle)) == 0;
    } else {
        ok = CloseHandle(reinterpret_cast<HANDLE>(handle.native)) != 0;
    }
    if(!ok) return IO_Result<u64>::err(Log::sys_error());

    handle.invalidate();
    return IO_Result<u64>::ok(0);
}

[[nodiscard]] IO_Result<u64> read_some_result(const Handle& handle, Slice<u8> out) noexcept {
    if(!handle.valid()) return IO_Result<u64>::err("invalid_handle"_v);
    if(handle.kind == Handle_Kind::socket) {
        int ret = recv(as_socket(handle), reinterpret_cast<char*>(out.data()),
                       static_cast<int>(out.length()), 0);
        if(ret == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if(err == WSAEWOULDBLOCK) return IO_Result<u64>::err("would_block"_v);
            return IO_Result<u64>::err("recv_failed"_v);
        }
        return IO_Result<u64>::ok(static_cast<u64>(ret));
    }

    DWORD read_bytes = 0;
    if(ReadFile(reinterpret_cast<HANDLE>(handle.native), out.data(), static_cast<DWORD>(out.length()),
                &read_bytes, null) == 0) {
        return IO_Result<u64>::err(Log::sys_error());
    }
    return IO_Result<u64>::ok(static_cast<u64>(read_bytes));
}

[[nodiscard]] IO_Result<u64> write_some_result(const Handle& handle, Slice<const u8> in) noexcept {
    if(!handle.valid()) return IO_Result<u64>::err("invalid_handle"_v);
    if(handle.kind == Handle_Kind::socket) {
        int ret = send(as_socket(handle), reinterpret_cast<const char*>(in.data()),
                       static_cast<int>(in.length()), 0);
        if(ret == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if(err == WSAEWOULDBLOCK) return IO_Result<u64>::err("would_block"_v);
            return IO_Result<u64>::err("send_failed"_v);
        }
        return IO_Result<u64>::ok(static_cast<u64>(ret));
    }

    DWORD written = 0;
    if(WriteFile(reinterpret_cast<HANDLE>(handle.native), in.data(), static_cast<DWORD>(in.length()),
                 &written, null) == 0) {
        return IO_Result<u64>::err(Log::sys_error());
    }
    return IO_Result<u64>::ok(static_cast<u64>(written));
}

[[nodiscard]] IO_Result<Poll_Result> poll_any_result(Slice<Poll_Target> targets,
                                                     u64 timeout_ms) noexcept {
    if(targets.empty()) return IO_Result<Poll_Result>::err("empty_targets"_v);

    Vec<WSAPOLLFD, Alloc> fds;
    fds.reserve(targets.length());
    for(u64 i = 0; i < targets.length(); i++) {
        targets[i].revents = Poll_Event::none;
        if(targets[i].handle == null || !targets[i].handle->valid()) {
            return IO_Result<Poll_Result>::err("invalid_handle"_v);
        }
        if(targets[i].handle->kind != Handle_Kind::socket) {
            return IO_Result<Poll_Result>::err("unsupported_poll_handle"_v);
        }
        short ev = 0;
        if(has_any(targets[i].events, Poll_Event::in)) ev |= POLLRDNORM;
        if(has_any(targets[i].events, Poll_Event::out)) ev |= POLLWRNORM;
        if(has_any(targets[i].events, Poll_Event::err)) ev |= POLLERR;
        if(has_any(targets[i].events, Poll_Event::hup)) ev |= POLLHUP;
        WSAPOLLFD pfd{};
        pfd.fd = as_socket(*targets[i].handle);
        pfd.events = ev;
        pfd.revents = 0;
        fds.push(pfd);
    }

    int timeout = timeout_ms > 2147483647ULL ? 2147483647 : static_cast<int>(timeout_ms);
    int ret = WSAPoll(fds.data(), static_cast<ULONG>(fds.length()), timeout);
    if(ret == SOCKET_ERROR) return IO_Result<Poll_Result>::err("poll_failed"_v);
    if(ret == 0) return IO_Result<Poll_Result>::err("timeout"_v);

    for(u64 i = 0; i < fds.length(); i++) {
        Poll_Event out = Poll_Event::none;
        if(fds[i].revents & POLLRDNORM) out |= Poll_Event::in;
        if(fds[i].revents & POLLWRNORM) out |= Poll_Event::out;
        if(fds[i].revents & POLLERR) out |= Poll_Event::err;
        if(fds[i].revents & POLLHUP) out |= Poll_Event::hup;
        targets[i].revents = out;
        if(out != Poll_Event::none) {
            return IO_Result<Poll_Result>::ok(Poll_Result{i, out});
        }
    }
    return IO_Result<Poll_Result>::err("no_events"_v);
}

[[nodiscard]] IO_Result<bool> poll_one_result(const Handle& handle, Poll_Event events,
                                              u64 timeout_ms) noexcept {
    Poll_Target t{const_cast<Handle*>(&handle), events, Poll_Event::none};
    auto got = poll_any_result(Slice<Poll_Target>{&t, 1}, timeout_ms);
    if(!got.ok()) {
        if(got.unwrap_err() == "timeout"_v) return IO_Result<bool>::ok(false);
        return IO_Result<bool>::err(spp::move(got.unwrap_err()));
    }
    return IO_Result<bool>::ok(true);
}

} // namespace spp::IO
