#include <spp/io/handle.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace spp::IO {

[[nodiscard]] static i32 as_fd(const Handle& handle) noexcept {
    if(!handle.valid()) return -1;
    return static_cast<i32>(handle.native);
}

[[nodiscard]] static short to_poll_mask(Poll_Event ev) noexcept {
    short out = 0;
    if(has_any(ev, Poll_Event::in)) out |= POLLIN;
    if(has_any(ev, Poll_Event::out)) out |= POLLOUT;
    if(has_any(ev, Poll_Event::err)) out |= POLLERR;
    if(has_any(ev, Poll_Event::hup)) out |= POLLHUP;
    return out ? out : POLLIN;
}

[[nodiscard]] static Poll_Event from_poll_mask(short revents) noexcept {
    Poll_Event out = Poll_Event::none;
    if(revents & POLLIN) out |= Poll_Event::in;
    if(revents & POLLOUT) out |= Poll_Event::out;
    if(revents & POLLERR) out |= Poll_Event::err;
    if(revents & POLLHUP) out |= Poll_Event::hup;
    return out;
}

[[nodiscard]] Capability_Matrix capability_matrix() noexcept {
#ifdef SPP_OS_LINUX
    return Capability_Matrix{
        .file_handles = true,
        .socket_handles = true,
        .pipe_handles = true,
        .poll_file = true,
        .poll_socket = true,
        .poll_pipe = true,
        .edge_triggered = false,
        .io_uring = true,
        .kqueue = false,
        .iocp = false,
    };
#else
    return Capability_Matrix{
        .file_handles = true,
        .socket_handles = true,
        .pipe_handles = true,
        .poll_file = true,
        .poll_socket = true,
        .poll_pipe = true,
        .edge_triggered = false,
        .io_uring = false,
        .kqueue = true,
        .iocp = false,
    };
#endif
}

[[nodiscard]] IO_Result<Pipe> pipe_result() noexcept {
    int fds[2] = {-1, -1};
#ifdef SPP_OS_LINUX
    if(pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        return IO_Result<Pipe>::err(Log::sys_error());
    }
#else
    if(pipe(fds) != 0) {
        return IO_Result<Pipe>::err(Log::sys_error());
    }
    static_cast<void>(fcntl(fds[0], F_SETFL, O_NONBLOCK));
    static_cast<void>(fcntl(fds[1], F_SETFL, O_NONBLOCK));
    static_cast<void>(fcntl(fds[0], F_SETFD, FD_CLOEXEC));
    static_cast<void>(fcntl(fds[1], F_SETFD, FD_CLOEXEC));
#endif

    return IO_Result<Pipe>::ok(
        Pipe{pipe_read_handle(static_cast<uptr>(fds[0])), pipe_write_handle(static_cast<uptr>(fds[1]))});
}

[[nodiscard]] IO_Result<u64> close_result(Handle& handle) noexcept {
    if(!handle.valid()) return IO_Result<u64>::ok(0);
    if(!handle.owned) {
        handle.invalidate();
        return IO_Result<u64>::ok(0);
    }
    i32 fd = as_fd(handle);
    if(fd < 0) return IO_Result<u64>::err("invalid_handle"_v);
    if(close(fd) != 0) {
        return IO_Result<u64>::err(Log::sys_error());
    }
    handle.invalidate();
    return IO_Result<u64>::ok(0);
}

[[nodiscard]] IO_Result<u64> read_some_result(const Handle& handle, Slice<u8> out) noexcept {
    if(!handle.valid()) return IO_Result<u64>::err("invalid_handle"_v);
    i32 fd = as_fd(handle);
    if(fd < 0) return IO_Result<u64>::err("invalid_handle"_v);

    i64 ret = read(fd, out.data(), static_cast<size_t>(out.length()));
    if(ret < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return IO_Result<u64>::err("would_block"_v);
        }
        return IO_Result<u64>::err(Log::sys_error());
    }
    return IO_Result<u64>::ok(static_cast<u64>(ret));
}

[[nodiscard]] IO_Result<u64> write_some_result(const Handle& handle, Slice<const u8> in) noexcept {
    if(!handle.valid()) return IO_Result<u64>::err("invalid_handle"_v);
    i32 fd = as_fd(handle);
    if(fd < 0) return IO_Result<u64>::err("invalid_handle"_v);

    i64 ret = write(fd, in.data(), static_cast<size_t>(in.length()));
    if(ret < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return IO_Result<u64>::err("would_block"_v);
        }
        return IO_Result<u64>::err(Log::sys_error());
    }
    return IO_Result<u64>::ok(static_cast<u64>(ret));
}

[[nodiscard]] IO_Result<Poll_Result> poll_any_result(Slice<Poll_Target> targets,
                                                     u64 timeout_ms) noexcept {
    if(targets.empty()) return IO_Result<Poll_Result>::err("empty_targets"_v);

    Vec<pollfd, Alloc> fds;
    fds.reserve(targets.length());
    for(u64 i = 0; i < targets.length(); i++) {
        targets[i].revents = Poll_Event::none;
        if(targets[i].handle == null || !targets[i].handle->valid()) {
            return IO_Result<Poll_Result>::err("invalid_handle"_v);
        }
        pollfd pfd{};
        pfd.fd = as_fd(*targets[i].handle);
        pfd.events = to_poll_mask(targets[i].events);
        pfd.revents = 0;
        fds.push(pfd);
    }

    int timeout = timeout_ms > 2147483647ULL ? 2147483647 : static_cast<int>(timeout_ms);
    int ret = -1;
    do {
        ret = poll(fds.data(), static_cast<nfds_t>(fds.length()), timeout);
    } while(ret < 0 && errno == EINTR);
    if(ret < 0) return IO_Result<Poll_Result>::err(Log::sys_error());
    if(ret == 0) return IO_Result<Poll_Result>::err("timeout"_v);

    for(u64 i = 0; i < fds.length(); i++) {
        auto ev = from_poll_mask(fds[i].revents);
        targets[i].revents = ev;
        if(ev != Poll_Event::none) {
            return IO_Result<Poll_Result>::ok(Poll_Result{i, ev});
        }
    }
    return IO_Result<Poll_Result>::err("no_events"_v);
}

[[nodiscard]] IO_Result<bool> poll_one_result(const Handle& handle, Poll_Event events,
                                              u64 timeout_ms) noexcept {
    Poll_Target t{const_cast<Handle*>(&handle), events, Poll_Event::none};
    auto got = poll_any_result(Slice<Poll_Target>{&t, 1}, timeout_ms);
    if(!got.ok()) {
        if(got.unwrap_err() == "timeout"_v) {
            return IO_Result<bool>::ok(false);
        }
        return IO_Result<bool>::err(spp::move(got.unwrap_err()));
    }
    return IO_Result<bool>::ok(true);
}

} // namespace spp::IO
