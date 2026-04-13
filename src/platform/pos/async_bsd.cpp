
#include <spp/async/async.h>

#include <sys/event.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace spp::Async {

Event::Event() noexcept {
    int pipes[2];
    if(pipe(pipes) == -1) {
        die("Failed to create pipe: %", Log::sys_error());
    }
    fd = pipes[0];
    signal_fd = pipes[1];
    mask = EVFILT_READ;
}

Event::Event(Event&& other) noexcept {
    fd = other.fd;
    other.fd = -1;
    signal_fd = other.signal_fd;
    other.signal_fd = -1;
    mask = other.mask;
    other.mask = 0;
}

Event& Event::operator=(Event&& other) noexcept {
    this->~Event();
    fd = other.fd;
    other.fd = -1;
    signal_fd = other.signal_fd;
    other.signal_fd = -1;
    mask = other.mask;
    other.mask = 0;
    return *this;
}

Event::~Event() noexcept {
    if(fd != -1) close(fd);
    if(signal_fd != -1) close(signal_fd);
    fd = -1;
    signal_fd = -1;
    mask = 0;
}

[[nodiscard]] Event Event::of_sys(i32 fd, i16 mask) noexcept {
    return Event{fd, mask};
}

void Event::signal() const noexcept {
    u8 data = 1;
    assert(write(signal_fd, &data, sizeof(data)) == sizeof(data));
}

void Event::reset() const noexcept {
    u8 data = 0;
    assert(read(fd, &data, sizeof(data)) == sizeof(data));
    assert(data == 1);
}

[[nodiscard]] bool Event::try_wait() const noexcept {
    int kq = kqueue();
    if(kq == -1) {
        die("Failed to create kqueue: %", Log::sys_error());
    }

    struct kevent wait;
    EV_SET(&wait, fd, mask, EV_ADD | EV_ENABLE, 0, 0, null);
    struct kevent signaled;
    timespec timeout = {.tv_sec = 0, .tv_nsec = 0};
    int count = kevent(kq, &wait, 1, &signaled, 1, &timeout);
    close(kq);

    if(count < 0) {
        die("Failed to poll kevent readiness: %", Log::sys_error());
    }
    if(count == 0) return false;
    if(signaled.flags == EV_ERROR) {
        die("Failed kevent readiness with error: %", Log::sys_error());
    }
    return true;
}

[[nodiscard]] u64 Event::wait_any(Slice<Event> events) noexcept {

    int kq = kqueue();
    if(kq == -1) {
        die("Failed to create kqueue: %", Log::sys_error());
    }

    for(auto& e : events) {
        struct kevent change;
        EV_SET(&change, e.fd, e.mask, EV_ADD | EV_ENABLE, 0, 0, null);
        int add_count = kevent(kq, &change, 1, null, 0, null);
        if(add_count < 0) {
            die("Failed to add kevent: %", Log::sys_error());
        }
    }

    struct kevent signaled;
    int count = kevent(kq, null, 0, &signaled, 1, null);
    if((count < 0) || (signaled.flags == EV_ERROR)) {
        die("Failed to wait on kevents: %", Log::sys_error());
    }
    close(kq);

    if(count == 0) {
        return wait_any(events);
    }

    for(u64 i = 0; i < events.length(); ++i) {
        if(events[i].fd == static_cast<i32>(signaled.ident)) {
            return i;
        }
    }
    SPP_UNREACHABLE;
}

[[nodiscard]] Opt<u64> Event::wait_any_for(Slice<Event> events, u64 timeout_ms) noexcept {

    int kq = kqueue();
    if(kq == -1) {
        die("Failed to create kqueue: %", Log::sys_error());
    }

    for(auto& e : events) {
        struct kevent change;
        EV_SET(&change, e.fd, e.mask, EV_ADD | EV_ENABLE, 0, 0, null);
        int add_count = kevent(kq, &change, 1, null, 0, null);
        if(add_count < 0) {
            die("Failed to add kevent: %", Log::sys_error());
        }
    }

    timespec timeout = {};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    timeout.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000);

    struct kevent signaled;
    int count = kevent(kq, null, 0, &signaled, 1, &timeout);
    if(count < 0) {
        die("Failed to wait on kevents: %", Log::sys_error());
    }
    close(kq);

    if(count == 0) return Opt<u64>{};
    if(signaled.flags == EV_ERROR) {
        die("Failed kevent wait with error: %", Log::sys_error());
    }

    for(u64 i = 0; i < events.length(); ++i) {
        if(events[i].fd == static_cast<i32>(signaled.ident)) {
            return Opt<u64>{i};
        }
    }
    SPP_UNREACHABLE;
}

} // namespace spp::Async
