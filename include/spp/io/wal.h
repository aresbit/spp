#pragma once

#include <spp/core/base.h>
#include <spp/io/files.h>

namespace spp::WAL {

enum struct Page_Cache_Policy : u32 {
    normal = 0,
    append_sequential = 1,
    random_access = 2,
    preload = 3,
    drop_soon = 4,
};

[[nodiscard]] inline Files::File_Result<u64> apply_page_cache_policy_result(
    const Files::Mapped_File& mapped, Page_Cache_Policy policy) noexcept {
    switch(policy) {
        case Page_Cache_Policy::normal:
            return Files::madvise_result(mapped, Files::Madvise_Hint::normal);
        case Page_Cache_Policy::append_sequential:
            return Files::madvise_result(mapped, Files::Madvise_Hint::sequential);
        case Page_Cache_Policy::random_access:
            return Files::madvise_result(mapped, Files::Madvise_Hint::random);
        case Page_Cache_Policy::preload:
            return Files::madvise_result(mapped, Files::Madvise_Hint::willneed);
        case Page_Cache_Policy::drop_soon:
            return Files::madvise_result(mapped, Files::Madvise_Hint::dontneed);
    }
    return Files::File_Result<u64>::err("bad_policy"_v);
}

struct Writer {
    [[nodiscard]] Files::File_Result<u64> open_result(String_View path, u64 preallocate_bytes = 0,
                                                      u64 sync_every_bytes = 0) noexcept {
        path_ = path;
        sync_every_bytes_ = sync_every_bytes;
        dirty_bytes_ = 0;
        write_offset_ = 0;
        reserved_bytes_ = 0;

        if(preallocate_bytes > 0) {
            auto falloc = Files::fallocate_result(path_, preallocate_bytes);
            if(!falloc.ok()) return falloc;
            reserved_bytes_ = preallocate_bytes;
        }
        return Files::File_Result<u64>::ok(0);
    }

    [[nodiscard]] Files::File_Result<u64> append_result(Slice<const u8> data) noexcept {
        if(data.length() == 0) return Files::File_Result<u64>::ok(0);
        auto reserve = ensure_capacity_result(write_offset_ + data.length());
        if(!reserve.ok()) return reserve;

        auto write = Files::pwrite_result(path_, write_offset_, data);
        if(!write.ok()) return write;

        u64 written = write.unwrap();
        write_offset_ += written;
        dirty_bytes_ += written;
        return maybe_sync_result(spp::move(written));
    }

    [[nodiscard]] Files::File_Result<u64> appendv_result(
        Slice<const Files::Write_IO_Slice> data) noexcept {
        u64 total = 0;
        for(u64 i = 0; i < data.length(); i++) total += data[i].length;
        if(total == 0) return Files::File_Result<u64>::ok(0);

        auto reserve = ensure_capacity_result(write_offset_ + total);
        if(!reserve.ok()) return reserve;

        auto write = Files::pwritev_result(path_, write_offset_, data);
        if(!write.ok()) return write;

        u64 written = write.unwrap();
        write_offset_ += written;
        dirty_bytes_ += written;
        return maybe_sync_result(spp::move(written));
    }

    [[nodiscard]] Files::File_Result<u64> flush_result() noexcept {
        auto sync = Files::fdatasync_result(path_);
        if(!sync.ok()) return sync;
        dirty_bytes_ = 0;
        return Files::File_Result<u64>::ok(0);
    }

    [[nodiscard]] u64 write_offset() const noexcept {
        return write_offset_;
    }

private:
    [[nodiscard]] Files::File_Result<u64> maybe_sync_result(u64 written) noexcept {
        if(sync_every_bytes_ == 0 || dirty_bytes_ < sync_every_bytes_) {
            return Files::File_Result<u64>::ok(spp::move(written));
        }
        auto sync = Files::fdatasync_result(path_);
        if(!sync.ok()) return sync;
        dirty_bytes_ = 0;
        return Files::File_Result<u64>::ok(spp::move(written));
    }

    [[nodiscard]] Files::File_Result<u64> ensure_capacity_result(u64 need) noexcept {
        if(need <= reserved_bytes_ || reserved_bytes_ == 0) {
            if(reserved_bytes_ == 0) return Files::File_Result<u64>::ok(0);
        } else {
            u64 target = reserved_bytes_;
            while(target < need) target *= 2;
            auto falloc = Files::fallocate_result(path_, target);
            if(!falloc.ok()) return falloc;
            reserved_bytes_ = target;
        }
        return Files::File_Result<u64>::ok(0);
    }

    String_View path_;
    u64 write_offset_ = 0;
    u64 reserved_bytes_ = 0;
    u64 dirty_bytes_ = 0;
    u64 sync_every_bytes_ = 0;
};

} // namespace spp::WAL
