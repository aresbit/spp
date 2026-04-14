
#include <spp/async/async.h>

#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace spp::Async {

[[nodiscard]] static short to_poll_mask(i32 mask) noexcept {
    short out = 0;
    if(mask & EPOLLIN) out |= POLLIN;
    if(mask & EPOLLOUT) out |= POLLOUT;
    if(mask & EPOLLERR) out |= POLLERR;
    if(mask & EPOLLHUP) out |= POLLHUP;
    return out ? out : POLLIN;
}

[[nodiscard]] static i32 wait_one_poll(i32 fd, i32 mask, i32 timeout_ms) noexcept {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = to_poll_mask(mask);

    for(;;) {
        int ret = poll(&pfd, 1, timeout_ms);
        if(ret < 0 && errno == EINTR) continue;
        return ret;
    }
}

Event::Event() noexcept {
    int event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if(event == -1) {
        die("Failed to create event: %", Log::sys_error());
    }
    fd = event;
    mask = EPOLLIN;
}

Event::Event(Event&& other) noexcept {
    fd = other.fd;
    other.fd = -1;
    mask = other.mask;
    other.mask = 0;
}

Event& Event::operator=(Event&& other) noexcept {
    this->~Event();
    fd = other.fd;
    other.fd = -1;
    mask = other.mask;
    other.mask = 0;
    return *this;
}

Event::~Event() noexcept {
    if(fd != -1) {
        int ret = close(fd);
        assert(ret == 0);
    }
    fd = -1;
    mask = 0;
}

[[nodiscard]] Event Event::of_sys(i32 fd, i32 mask) noexcept {
    return Event{fd, mask};
}

void Event::signal() const noexcept {
    u64 value = 1;
    int ret = write(fd, &value, sizeof(value));
    if(ret == -1) {
        die("Failed to signal event: %", Log::sys_error());
    }
}

void Event::reset() const noexcept {
    u64 value = 0;
    int ret = read(fd, &value, sizeof(value));
    if(ret == -1) {
        die("Failed to reset event: %", Log::sys_error());
    }
}

[[nodiscard]] bool Event::try_wait() const noexcept {
    int ret = wait_one_poll(fd, mask, 0);
    if(ret < 0) {
        die("Failed to poll event readiness: %", Log::sys_error());
    }
    return ret == 1;
}

[[nodiscard]] u64 Event::wait_any(Slice<Event> events) noexcept {
    assert(!events.empty());
    if(events.length() == 1) {
        int ret = wait_one_poll(events[0].fd, events[0].mask, -1);
        if(ret <= 0) {
            die("Failed to wait on single event: %", Log::sys_error());
        }
        return 0;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if(epfd == -1) {
        die("Failed to create epoll: %", Log::sys_error());
    }

    for(auto& event : events) {
        epoll_event ev;
        ev.events = event.mask;
        ev.data.fd = event.fd;
        int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, event.fd, &ev);
        if(ret == -1) {
            close(epfd);
            die("Failed to add event to epoll: %", Log::sys_error());
        }
    }

    epoll_event ev;
    int ret = -1;
    do {
        ret = epoll_wait(epfd, &ev, 1, -1);
    } while(ret == -1 && errno == EINTR);

    if(ret == -1) {
        close(epfd);
        die("Failed to wait on events: %", Log::sys_error());
    }

    ret = close(epfd);
    assert(ret == 0);

    for(u64 i = 0; i < events.length(); ++i) {
        if(events[i].fd == ev.data.fd) {
            return i;
        }
    }
    SPP_UNREACHABLE;
}

[[nodiscard]] Opt<u64> Event::wait_any_for(Slice<Event> events, u64 timeout_ms) noexcept {
    assert(!events.empty());
    if(events.length() == 1) {
        int wait_ret = wait_one_poll(events[0].fd, events[0].mask, static_cast<int>(timeout_ms));
        if(wait_ret < 0) {
            die("Failed to wait on single event: %", Log::sys_error());
        }
        if(wait_ret == 0) return Opt<u64>{};
        return Opt<u64>{u64{0}};
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if(epfd == -1) {
        die("Failed to create epoll: %", Log::sys_error());
    }

    for(auto& event : events) {
        epoll_event ev;
        ev.events = event.mask;
        ev.data.fd = event.fd;
        int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, event.fd, &ev);
        if(ret == -1) {
            close(epfd);
            die("Failed to add event to epoll: %", Log::sys_error());
        }
    }

    epoll_event ev;
    int wait_ret = -1;
    do {
        wait_ret = epoll_wait(epfd, &ev, 1, static_cast<int>(timeout_ms));
    } while(wait_ret == -1 && errno == EINTR);
    if(wait_ret == -1) {
        close(epfd);
        die("Failed to wait on events: %", Log::sys_error());
    }

    int close_ret = close(epfd);
    assert(close_ret == 0);

    if(wait_ret == 0) return Opt<u64>{};

    for(u64 i = 0; i < events.length(); ++i) {
        if(events[i].fd == ev.data.fd) {
            return Opt<u64>{i};
        }
    }
    SPP_UNREACHABLE;
}

} // namespace spp::Async
