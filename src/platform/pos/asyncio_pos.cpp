
#include <spp/async/asyncio.h>
#include <spp/io/files.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {
using spp::i32;
using spp::u32;
using spp::u64;
using spp::u8;

[[nodiscard]] static inline u32 load_acquire(const u32* ptr) noexcept {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

[[nodiscard]] static inline u32 load_relaxed(const u32* ptr) noexcept {
    return __atomic_load_n(ptr, __ATOMIC_RELAXED);
}

static inline void store_release(u32* ptr, u32 value) noexcept {
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

struct Io_Uring {
    int fd = -1;
    int event_fd = -1;

    io_uring_params params{};
    void* sq_ring_ptr = null;
    void* cq_ring_ptr = null;
    io_uring_sqe* sqes = null;

    u32* sq_head = null;
    u32* sq_tail = null;
    u32* sq_ring_mask = null;
    u32* sq_ring_entries = null;
    u32* sq_array = null;

    u32* cq_head = null;
    u32* cq_tail = null;
    u32* cq_ring_mask = null;
    io_uring_cqe* cqes = null;

    size_t sq_ring_size = 0;
    size_t cq_ring_size = 0;
    size_t sqes_size = 0;

    [[nodiscard]] bool init(u32 entries) noexcept {
        fd = static_cast<int>(syscall(SYS_io_uring_setup, entries, &params));
        if(fd < 0) {
            return false;
        }

        sq_ring_size = params.sq_off.array + params.sq_entries * sizeof(u32);
        cq_ring_size = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
        sqes_size = params.sq_entries * sizeof(io_uring_sqe);

        if(params.features & IORING_FEAT_SINGLE_MMAP) {
            size_t ring_size = sq_ring_size > cq_ring_size ? sq_ring_size : cq_ring_size;
            sq_ring_ptr = mmap(null, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                               fd, IORING_OFF_SQ_RING);
            cq_ring_ptr = sq_ring_ptr;
            sq_ring_size = ring_size;
            cq_ring_size = ring_size;
        } else {
            sq_ring_ptr = mmap(null, sq_ring_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
            cq_ring_ptr = mmap(null, cq_ring_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        }
        if(sq_ring_ptr == MAP_FAILED || cq_ring_ptr == MAP_FAILED) {
            cleanup();
            return false;
        }

        sqes = static_cast<io_uring_sqe*>(
            mmap(null, sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
                 IORING_OFF_SQES));
        if(sqes == MAP_FAILED) {
            sqes = null;
            cleanup();
            return false;
        }

        sq_head = reinterpret_cast<u32*>(static_cast<u8*>(sq_ring_ptr) + params.sq_off.head);
        sq_tail = reinterpret_cast<u32*>(static_cast<u8*>(sq_ring_ptr) + params.sq_off.tail);
        sq_ring_mask = reinterpret_cast<u32*>(static_cast<u8*>(sq_ring_ptr) + params.sq_off.ring_mask);
        sq_ring_entries =
            reinterpret_cast<u32*>(static_cast<u8*>(sq_ring_ptr) + params.sq_off.ring_entries);
        sq_array = reinterpret_cast<u32*>(static_cast<u8*>(sq_ring_ptr) + params.sq_off.array);

        cq_head = reinterpret_cast<u32*>(static_cast<u8*>(cq_ring_ptr) + params.cq_off.head);
        cq_tail = reinterpret_cast<u32*>(static_cast<u8*>(cq_ring_ptr) + params.cq_off.tail);
        cq_ring_mask = reinterpret_cast<u32*>(static_cast<u8*>(cq_ring_ptr) + params.cq_off.ring_mask);
        cqes = reinterpret_cast<io_uring_cqe*>(static_cast<u8*>(cq_ring_ptr) + params.cq_off.cqes);

        event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if(event_fd < 0) {
            cleanup();
            return false;
        }
        int reg = static_cast<int>(
            syscall(SYS_io_uring_register, fd, IORING_REGISTER_EVENTFD, &event_fd, 1));
        if(reg < 0) {
            cleanup();
            return false;
        }
        return true;
    }

    [[nodiscard]] io_uring_sqe* reserve_sqe() noexcept {
        u32 head = load_acquire(sq_head);
        u32 tail = load_relaxed(sq_tail);
        if(tail - head >= *sq_ring_entries) {
            return null;
        }
        u32 idx = tail & *sq_ring_mask;
        sq_array[idx] = idx;
        store_release(sq_tail, tail + 1);
        auto* sqe = &sqes[idx];
        spp::Libc::memset(sqe, 0, sizeof(*sqe));
        return sqe;
    }

    [[nodiscard]] int submit() noexcept {
        return static_cast<int>(syscall(SYS_io_uring_enter, fd, 1, 0, 0, null, 0));
    }

    [[nodiscard]] bool try_pop_cqe(u64& out_user_data, i32& out_res) noexcept {
        u32 head = load_acquire(cq_head);
        u32 tail = load_acquire(cq_tail);
        if(head == tail) {
            return false;
        }
        io_uring_cqe* cqe = &cqes[head & *cq_ring_mask];
        out_user_data = cqe->user_data;
        out_res = cqe->res;
        store_release(cq_head, head + 1);
        return true;
    }

    void drain_eventfd() const noexcept {
        u64 value = 0;
        int ret = read(event_fd, &value, sizeof(value));
        if(ret < 0 && errno != EAGAIN && errno != EINTR) {
            warn("Failed to drain io_uring eventfd: %", spp::Log::sys_error());
        }
    }

    void cleanup() noexcept {
        if(sqes) {
            munmap(sqes, sqes_size);
            sqes = null;
        }
        if(cq_ring_ptr && cq_ring_ptr != MAP_FAILED && cq_ring_ptr != sq_ring_ptr) {
            munmap(cq_ring_ptr, cq_ring_size);
        }
        if(sq_ring_ptr && sq_ring_ptr != MAP_FAILED) {
            munmap(sq_ring_ptr, sq_ring_size);
        }
        sq_ring_ptr = null;
        cq_ring_ptr = null;
        if(event_fd >= 0) {
            close(event_fd);
            event_fd = -1;
        }
        if(fd >= 0) {
            close(fd);
            fd = -1;
        }
    }

    ~Io_Uring() noexcept {
        cleanup();
    }
};

} // namespace

namespace spp::Async {

constexpr u64 DIRECT_IO_ALIGN = 4096;

[[nodiscard]] static Thread::Atomic& io_uring_disabled_flag() noexcept {
    static Thread::Atomic disabled;
    return disabled;
}

[[nodiscard]] static bool io_uring_globally_disabled_now() noexcept {
    return io_uring_disabled_flag().load() != 0;
}

static void disable_io_uring_globally_now() noexcept {
    io_uring_disabled_flag().exchange(1);
}

[[nodiscard]] static bool direct_io_aligned(const void* ptr, u64 offset, u64 length) noexcept {
    if(ptr == null) return false;
    uptr p = reinterpret_cast<uptr>(ptr);
    return (p % DIRECT_IO_ALIGN == 0) && (offset % DIRECT_IO_ALIGN == 0) &&
           (length % DIRECT_IO_ALIGN == 0);
}

struct Io_Uring_Cache {
    Thread::Mutex mut;
    bool attempted = false;
    bool available = false;
    Io_Uring ring;
    u64 next_user_data = 1;
    Vec<Pair<u64, i32>, Files::Alloc> completions;
};

[[nodiscard]] static Io_Uring_Cache& shared_ring_cache() noexcept {
    static Io_Uring_Cache cache;
    return cache;
}

[[nodiscard]] static bool ensure_ring_available(Io_Uring_Cache& cache) noexcept {
    if(io_uring_globally_disabled_now()) return false;
    if(!cache.attempted) {
        cache.attempted = true;
        cache.available = cache.ring.init(256);
        if(!cache.available) {
            disable_io_uring_globally_now();
            return false;
        }
    }
    return cache.available;
}

static void pump_completions_locked(Io_Uring_Cache& cache) noexcept {
    u64 token = 0;
    i32 res = 0;
    while(cache.ring.try_pop_cqe(token, res)) {
        cache.completions.emplace(token, res);
    }
}

[[nodiscard]] static bool take_completion_locked(Io_Uring_Cache& cache, u64 token,
                                                 i32& out_res) noexcept {
    for(u64 i = 0; i < cache.completions.length(); i++) {
        if(cache.completions[i].first == token) {
            out_res = cache.completions[i].second;
            if(i + 1 < cache.completions.length()) {
                cache.completions[i] = spp::move(cache.completions.back());
            }
            cache.completions.pop();
            return true;
        }
    }
    return false;
}

[[nodiscard]] static Task<Result<i32, String_View>> await_uring_token(Pool<>& pool,
                                                                      Io_Uring_Cache& cache,
                                                                      u64 token) noexcept {
    i32 res = 0;
    for(;;) {
        {
            Thread::Lock lock(cache.mut);
            cache.ring.drain_eventfd();
            pump_completions_locked(cache);
            if(take_completion_locked(cache, token, res)) {
                co_return Result<i32, String_View>::ok(spp::move(res));
            }
        }
        co_await pool.suspend();
    }
}

[[nodiscard]] Task<Result<Vec<u8, Files::Alloc>, String_View>> read_result(Pool<>& pool,
                                                                            String_View path_,
                                                                            File_IO_Mode mode) noexcept {
    int fd = -1;
    off_t full_size = 0;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_RDONLY);
    }
    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("open_failed"_v);
    }
    full_size = lseek(fd, 0, SEEK_END);
    close(fd);
    if(full_size < 0) {
        warn("Failed to seek file %: %", path_, Log::sys_error());
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("size_failed"_v);
    }

    Vec<u8, Files::Alloc> data(static_cast<u64>(full_size));
    data.resize(static_cast<u64>(full_size));
    if(full_size == 0) {
        co_return Result<Vec<u8, Files::Alloc>, String_View>::ok(spp::move(data));
    }

    auto got = co_await pread_result(pool, path_, 0, data.slice(), mode);
    if(!got.ok() || got.unwrap() != static_cast<u64>(full_size)) {
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("read_failed"_v);
    }
    co_return Result<Vec<u8, Files::Alloc>, String_View>::ok(spp::move(data));
}

[[nodiscard]] Task<Result<u64, String_View>> write_result(Pool<>& pool, String_View path_,
                                                          Slice<u8> data,
                                                          File_IO_Mode mode) noexcept {
    auto trunc = Files::truncate_result(path_, 0);
    if(!trunc.ok()) {
        warn("Failed to truncate file % before async write: %", path_, Log::sys_error());
        co_return Result<u64, String_View>::err("create_failed"_v);
    }

    auto wrote = co_await pwrite_result(pool, path_, 0, data, mode);
    if(!wrote.ok()) {
        co_return Result<u64, String_View>::err(spp::move(wrote.unwrap_err()));
    }
    co_return Result<u64, String_View>::ok(spp::move(wrote.unwrap()));
}

[[nodiscard]] Task<Result<u64, String_View>> pread_result(Pool<>& pool, String_View path,
                                                          u64 offset, Slice<u8> out,
                                                          File_IO_Mode mode) noexcept {
    if(mode == File_IO_Mode::direct && !direct_io_aligned(out.data(), offset, out.length())) {
        co_return Result<u64, String_View>::err("direct_alignment"_v);
    }

    if(io_uring_globally_disabled_now()) {
        auto ret = Files::pread_result(path, offset, out);
        if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
        co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
    }

    Io_Uring_Cache& cache = shared_ring_cache();
    if(!ensure_ring_available(cache)) {
        auto ret = Files::pread_result(path, offset, out);
        if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
        co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
    }

    int fd = -1;
    Region(R) {
        auto terminated = path.terminate<Mregion<R>>();
        int flags = O_RDONLY;
        if(mode == File_IO_Mode::direct) flags |= O_DIRECT;
        fd = open(reinterpret_cast<const char*>(terminated.data()), flags);
    }
    if(fd < 0) {
        warn("Failed to open file %: %", path, Log::sys_error());
        co_return Result<u64, String_View>::err("open_failed"_v);
    }

    u64 token = 0;
    {
        Thread::Lock lock(cache.mut);
        auto* sqe = cache.ring.reserve_sqe();
        if(!sqe) {
            close(fd);
            auto ret = Files::pread_result(path, offset, out);
            if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
            co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
        }
        token = cache.next_user_data++;
        if(token == 0) token = cache.next_user_data++;
        sqe->user_data = token;
        sqe->opcode = IORING_OP_READ;
        sqe->fd = fd;
        sqe->off = offset;
        sqe->addr = reinterpret_cast<u64>(out.data());
        sqe->len = static_cast<u32>(out.length());

        if(cache.ring.submit() < 0) {
            close(fd);
            auto ret = Files::pread_result(path, offset, out);
            if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
            co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
        }
    }

    auto cqe = co_await await_uring_token(pool, cache, token);
    close(fd);
    if(!cqe.ok()) {
        co_return Result<u64, String_View>::err("pread_failed"_v);
    }
    i32 res = cqe.unwrap();
    if(res < 0) {
        co_return Result<u64, String_View>::err("pread_failed"_v);
    }
    co_return Result<u64, String_View>::ok(u64{static_cast<u64>(res)});
}

[[nodiscard]] Task<Result<u64, String_View>> pwrite_result(Pool<>& pool, String_View path,
                                                           u64 offset,
                                                           Slice<const u8> data,
                                                           File_IO_Mode mode) noexcept {
    if(mode == File_IO_Mode::direct && !direct_io_aligned(data.data(), offset, data.length())) {
        co_return Result<u64, String_View>::err("direct_alignment"_v);
    }

    if(io_uring_globally_disabled_now()) {
        auto ret = Files::pwrite_result(path, offset, data);
        if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
        co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
    }

    Io_Uring_Cache& cache = shared_ring_cache();
    if(!ensure_ring_available(cache)) {
        auto ret = Files::pwrite_result(path, offset, data);
        if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
        co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
    }

    int fd = -1;
    Region(R) {
        auto terminated = path.terminate<Mregion<R>>();
        int flags = O_WRONLY | O_CREAT;
        if(mode == File_IO_Mode::direct) flags |= O_DIRECT;
        fd = open(reinterpret_cast<const char*>(terminated.data()), flags, 0644);
    }
    if(fd < 0) {
        warn("Failed to open file %: %", path, Log::sys_error());
        co_return Result<u64, String_View>::err("open_failed"_v);
    }

    u64 token = 0;
    {
        Thread::Lock lock(cache.mut);
        auto* sqe = cache.ring.reserve_sqe();
        if(!sqe) {
            close(fd);
            auto ret = Files::pwrite_result(path, offset, data);
            if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
            co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
        }
        token = cache.next_user_data++;
        if(token == 0) token = cache.next_user_data++;
        sqe->user_data = token;
        sqe->opcode = IORING_OP_WRITE;
        sqe->fd = fd;
        sqe->off = offset;
        sqe->addr = reinterpret_cast<u64>(data.data());
        sqe->len = static_cast<u32>(data.length());

        if(cache.ring.submit() < 0) {
            close(fd);
            auto ret = Files::pwrite_result(path, offset, data);
            if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
            co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
        }
    }

    auto cqe = co_await await_uring_token(pool, cache, token);
    close(fd);
    if(!cqe.ok()) {
        co_return Result<u64, String_View>::err("pwrite_failed"_v);
    }
    i32 res = cqe.unwrap();
    if(res < 0) {
        co_return Result<u64, String_View>::err("pwrite_failed"_v);
    }
    co_return Result<u64, String_View>::ok(u64{static_cast<u64>(res)});
}

[[nodiscard]] Task<Result<u64, String_View>> preadv_result(Pool<>& pool, String_View path,
                                                           u64 offset,
                                                           Slice<Files::Read_IO_Slice> outs,
                                                           File_IO_Mode mode) noexcept {
    if(mode == File_IO_Mode::direct) {
        if(offset % DIRECT_IO_ALIGN != 0) {
            co_return Result<u64, String_View>::err("direct_alignment"_v);
        }
        for(auto& out : outs) {
            if(!direct_io_aligned(out.data, 0, out.length)) {
                co_return Result<u64, String_View>::err("direct_alignment"_v);
            }
        }
    }

    if(io_uring_globally_disabled_now()) {
        auto ret = Files::preadv_result(path, offset, outs);
        if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
        co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
    }

    Vec<iovec, Files::Alloc> iovs;
    iovs.reserve(outs.length());
    for(auto& out : outs) {
        iovec v{};
        v.iov_base = out.data;
        v.iov_len = static_cast<size_t>(out.length);
        iovs.push(v);
    }

    Io_Uring_Cache& cache = shared_ring_cache();
    if(!ensure_ring_available(cache)) {
        auto ret = Files::preadv_result(path, offset, outs);
        if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
        co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
    }

    int fd = -1;
    Region(R) {
        auto terminated = path.terminate<Mregion<R>>();
        int flags = O_RDONLY;
        if(mode == File_IO_Mode::direct) flags |= O_DIRECT;
        fd = open(reinterpret_cast<const char*>(terminated.data()), flags);
    }
    if(fd < 0) {
        warn("Failed to open file %: %", path, Log::sys_error());
        co_return Result<u64, String_View>::err("open_failed"_v);
    }

    u64 token = 0;
    {
        Thread::Lock lock(cache.mut);
        auto* sqe = cache.ring.reserve_sqe();
        if(!sqe) {
            close(fd);
            auto ret = Files::preadv_result(path, offset, outs);
            if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
            co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
        }
        token = cache.next_user_data++;
        if(token == 0) token = cache.next_user_data++;
        sqe->user_data = token;
        sqe->opcode = IORING_OP_READV;
        sqe->fd = fd;
        sqe->off = offset;
        sqe->addr = reinterpret_cast<u64>(iovs.data());
        sqe->len = static_cast<u32>(iovs.length());

        if(cache.ring.submit() < 0) {
            close(fd);
            auto ret = Files::preadv_result(path, offset, outs);
            if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
            co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
        }
    }

    auto cqe = co_await await_uring_token(pool, cache, token);
    close(fd);
    if(!cqe.ok()) {
        co_return Result<u64, String_View>::err("preadv_failed"_v);
    }
    i32 res = cqe.unwrap();
    if(res < 0) {
        co_return Result<u64, String_View>::err("preadv_failed"_v);
    }
    co_return Result<u64, String_View>::ok(u64{static_cast<u64>(res)});
}

[[nodiscard]] Task<Result<u64, String_View>> pwritev_result(
    Pool<>& pool, String_View path, u64 offset, Slice<const Files::Write_IO_Slice> inputs,
    File_IO_Mode mode) noexcept {
    if(mode == File_IO_Mode::direct) {
        if(offset % DIRECT_IO_ALIGN != 0) {
            co_return Result<u64, String_View>::err("direct_alignment"_v);
        }
        for(auto& in : inputs) {
            if(!direct_io_aligned(in.data, 0, in.length)) {
                co_return Result<u64, String_View>::err("direct_alignment"_v);
            }
        }
    }

    if(io_uring_globally_disabled_now()) {
        auto ret = Files::pwritev_result(path, offset, inputs);
        if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
        co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
    }

    Vec<iovec, Files::Alloc> iovs;
    iovs.reserve(inputs.length());
    for(auto& in : inputs) {
        iovec v{};
        v.iov_base = const_cast<u8*>(in.data);
        v.iov_len = static_cast<size_t>(in.length);
        iovs.push(v);
    }

    Io_Uring_Cache& cache = shared_ring_cache();
    if(!ensure_ring_available(cache)) {
        auto ret = Files::pwritev_result(path, offset, inputs);
        if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
        co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
    }

    int fd = -1;
    Region(R) {
        auto terminated = path.terminate<Mregion<R>>();
        int flags = O_WRONLY | O_CREAT;
        if(mode == File_IO_Mode::direct) flags |= O_DIRECT;
        fd = open(reinterpret_cast<const char*>(terminated.data()), flags, 0644);
    }
    if(fd < 0) {
        warn("Failed to open file %: %", path, Log::sys_error());
        co_return Result<u64, String_View>::err("open_failed"_v);
    }

    u64 token = 0;
    {
        Thread::Lock lock(cache.mut);
        auto* sqe = cache.ring.reserve_sqe();
        if(!sqe) {
            close(fd);
            auto ret = Files::pwritev_result(path, offset, inputs);
            if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
            co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
        }
        token = cache.next_user_data++;
        if(token == 0) token = cache.next_user_data++;
        sqe->user_data = token;
        sqe->opcode = IORING_OP_WRITEV;
        sqe->fd = fd;
        sqe->off = offset;
        sqe->addr = reinterpret_cast<u64>(iovs.data());
        sqe->len = static_cast<u32>(iovs.length());

        if(cache.ring.submit() < 0) {
            close(fd);
            auto ret = Files::pwritev_result(path, offset, inputs);
            if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
            co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
        }
    }

    auto cqe = co_await await_uring_token(pool, cache, token);
    close(fd);
    if(!cqe.ok()) {
        co_return Result<u64, String_View>::err("pwritev_failed"_v);
    }
    i32 res = cqe.unwrap();
    if(res < 0) {
        co_return Result<u64, String_View>::err("pwritev_failed"_v);
    }
    co_return Result<u64, String_View>::ok(u64{static_cast<u64>(res)});
}

[[nodiscard]] Task<Result<u64, String_View>> fdatasync_result(Pool<>& pool,
                                                              String_View path,
                                                              File_IO_Mode mode) noexcept {
    if(io_uring_globally_disabled_now()) {
        auto ret = Files::fdatasync_result(path);
        if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
        co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
    }

    Io_Uring_Cache& cache = shared_ring_cache();
    if(!ensure_ring_available(cache)) {
        auto ret = Files::fdatasync_result(path);
        if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
        co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
    }

    int fd = -1;
    Region(R) {
        auto terminated = path.terminate<Mregion<R>>();
        int flags = O_WRONLY | O_CREAT;
        if(mode == File_IO_Mode::direct) flags |= O_DIRECT;
        fd = open(reinterpret_cast<const char*>(terminated.data()), flags, 0644);
    }
    if(fd < 0) {
        warn("Failed to open file %: %", path, Log::sys_error());
        co_return Result<u64, String_View>::err("open_failed"_v);
    }

    u64 token = 0;
    {
        Thread::Lock lock(cache.mut);
        auto* sqe = cache.ring.reserve_sqe();
        if(!sqe) {
            close(fd);
            auto ret = Files::fdatasync_result(path);
            if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
            co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
        }
        token = cache.next_user_data++;
        if(token == 0) token = cache.next_user_data++;
        sqe->user_data = token;
        sqe->opcode = IORING_OP_FSYNC;
        sqe->fd = fd;
        sqe->fsync_flags = IORING_FSYNC_DATASYNC;

        if(cache.ring.submit() < 0) {
            close(fd);
            auto ret = Files::fdatasync_result(path);
            if(!ret.ok()) co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
            co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
        }
    }

    auto cqe = co_await await_uring_token(pool, cache, token);
    close(fd);
    if(!cqe.ok()) {
        co_return Result<u64, String_View>::err("fdatasync_failed"_v);
    }
    i32 res = cqe.unwrap();
    if(res < 0) {
        co_return Result<u64, String_View>::err("fdatasync_failed"_v);
    }
    co_return Result<u64, String_View>::ok(u64{static_cast<u64>(res)});
}

[[nodiscard]] Task<Opt<Vec<u8, Files::Alloc>>> read(Pool<>& pool, String_View path,
                                                    File_IO_Mode mode) noexcept {
    auto result = co_await read_result(pool, path, mode);
    if(!result.ok()) co_return Opt<Vec<u8, Files::Alloc>>{};
    co_return Opt{move(result.unwrap())};
}

[[nodiscard]] Task<bool> write(Pool<>& pool, String_View path, Slice<u8> data,
                               File_IO_Mode mode) noexcept {
    auto result = co_await write_result(pool, path, data, mode);
    co_return result.ok();
}

[[nodiscard]] Task<Result<u64, Wait_Error>> wait_typed(Pool<>& pool, u64 ms) noexcept {

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if(fd == -1) {
        warn("Failed to create timerfd: %", Log::sys_error());
        co_return Result<u64, Wait_Error>::err(Wait_Error::timer_create_failed);
    }

    itimerspec spec = {};
    spec.it_value.tv_sec = ms / 1000;
    spec.it_value.tv_nsec = (ms % 1000) * 1000000;

    if(timerfd_settime(fd, 0, &spec, null) == -1) {
        warn("Failed to set timerfd: %", Log::sys_error());
        close(fd);
        co_return Result<u64, Wait_Error>::err(Wait_Error::timer_set_failed);
    }

    co_await pool.event(Async::Event::of_sys(fd, EPOLLIN));
    co_return Result<u64, Wait_Error>::ok(u64{ms});
}

[[nodiscard]] Task<void> wait(Pool<>& pool, u64 ms) noexcept {
    static_cast<void>(co_await wait_typed(pool, ms));
}

} // namespace spp::Async
