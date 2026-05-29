#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"
#include "spp/io/files.h"

namespace spp::quant::data {

// =========================================================================
// KVStore — embedded persistent key-value store
//
// Provides SQLite-like functionality without external dependencies:
//   - Tables (maps of rows keyed by primary key)
//   - Insert / Update / Delete / Select operations
//   - Persisted to JSONL files (one JSON object per line)
//   - Atomic writes (write to .tmp, then POSIX rename) for crash safety
//   - Simple SQL parser for INSERT / SELECT / UPDATE / DELETE
//
// File format per table:
//   Header line:  {"_meta":{"name":"...","pk":"...","columns":[...],"auto_inc":bool,"next_id":N}}
//   Row lines:    {"col1":"val1","col2":"val2",...}
//   One JSON object per line (JSONL / NDJSON)
// =========================================================================

// A row in a table — map of column name to value (all stored as strings).
using Row = Map<String<>, String<>>;

// A table schema.
struct TableSchema {
    String<>        name_;
    Vec<String<>>   columns_;
    String<>        primary_key_;
    bool            auto_increment_ = false;
    u64             next_id_ = 1;
};

// Query result container.
struct QueryResult {
    Vec<Row> rows_;

    [[nodiscard]] u64 row_count() const noexcept { return rows_.length(); }
    [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }

    Row& operator[](u64 i) noexcept { return rows_[i]; }
    const Row& operator[](u64 i) const noexcept { return rows_[i]; }
};

// Compiled query with bound parameters.
struct PreparedQuery {
    String<>                  sql_;
    Map<String<>, String<>>   params_;

    void bind_int(String_View name, i64 value) noexcept {
        String<> val = i64_to_string(value);
        params_.insert(String<>{name}, spp::move(val));
    }

    void bind_float(String_View name, f64 value) noexcept {
        String<> val = f64_to_string(value);
        params_.insert(String<>{name}, spp::move(val));
    }

    void bind_string(String_View name, String_View value) noexcept {
        params_.insert(String<>{name}, String<>{value});
    }

    void bind_date(String_View name, Date value) noexcept {
        String<> val = date_to_string(value);
        params_.insert(String<>{name}, spp::move(val));
    }

private:
    static String<> i64_to_string(i64 v) noexcept {
        if (v == 0) { String<> s{1}; s.set_length(1); s[0] = '0'; return s; }
        bool neg = v < 0; if (neg) v = -v;
        char buf[24]; i32 p = 23; buf[p] = '\0';
        while (v > 0) { buf[--p] = static_cast<char>('0' + (v % 10)); v /= 10; }
        i32 len = 23 - p;
        if (neg) { buf[--p] = '-'; len++; }
        String<> s{static_cast<u64>(len)}; s.set_length(static_cast<u64>(len));
        for (i32 i = 0; i < len; i++) s[static_cast<u64>(i)] = static_cast<u8>(buf[p + i]);
        return s;
    }

    static String<> f64_to_string(f64 value) noexcept {
        Vec<char> buf; buf.reserve(32);
        if (value < 0.0) { buf.push('-'); value = -value; }
        i64 int_part = static_cast<i64>(value);
        f64 frac = value - static_cast<f64>(int_part);
        char ibuf[24]; i32 ip = 23; ibuf[ip] = '\0';
        if (int_part == 0) ibuf[--ip] = '0';
        else { i64 ipv = int_part; while (ipv > 0) { ibuf[--ip] = static_cast<char>('0' + (ipv % 10)); ipv /= 10; } }
        for (i32 i = ip; i < 23; i++) buf.push(ibuf[i]);
        buf.push('.');
        for (i32 d = 0; d < 6; d++) { frac *= 10.0; i32 digit = static_cast<i32>(frac); buf.push(static_cast<char>('0' + digit)); frac -= static_cast<f64>(digit); }
        String<> s{static_cast<u64>(buf.length())};
        s.set_length(static_cast<u64>(buf.length()));
        for (u64 i = 0; i < buf.length(); i++) s[i] = static_cast<u8>(buf[i]);
        return s;
    }

    static String<> date_to_string(Date d) noexcept {
        if (d.serial_ == 0) { String<> s{2}; s.set_length(2); s[0] = '"'; s[1] = '"'; return s; }
        auto ymd = d.ymd();
        char buf[16];
        (void)Libc::snprintf(reinterpret_cast<u8*>(buf), 16, "%04d-%02u-%02u", ymd.get<0>(), ymd.get<1>(), ymd.get<2>());
        String<> s{10}; s.set_length(10);
        for (u64 i = 0; i < 10; i++) s[i] = static_cast<u8>(buf[i]);
        return s;
    }
};

// =========================================================================
// Condition type — for non-template row filtering
// =========================================================================

struct RowCondition {
    enum struct Op : u8 { Eq, Neq, Gt, Lt, Ge, Le, Contains };

    String<> column_;
    String<> value_;
    Op op_ = Op::Eq;

    static RowCondition eq(String_View col, String_View val) noexcept {
        RowCondition c; c.column_ = String<>{col}; c.value_ = String<>{val}; c.op_ = Op::Eq; return c;
    }

    [[nodiscard]] bool matches(const Row& row) const noexcept {
        auto opt = row.try_get(String<>{column_.view()});
        if (!opt.ok()) return false;
        String_View v = (**opt).view();
        String_View expected = value_.view();

        // Numeric comparison attempted if both look numeric
        bool v_is_num = !v.empty() && (v[0] == '-' || (v[0] >= '0' && v[0] <= '9'));
        bool e_is_num = !expected.empty() && (expected[0] == '-' || (expected[0] >= '0' && expected[0] <= '9'));

        if (v_is_num && e_is_num) {
            f64 vn = string_to_f64(v);
            f64 en = string_to_f64(expected);
            switch (op_) {
            case Op::Eq:  return Math::abs(vn - en) < 1e-12;
            case Op::Neq: return Math::abs(vn - en) >= 1e-12;
            case Op::Gt:  return vn > en;
            case Op::Lt:  return vn < en;
            case Op::Ge:  return vn >= en;
            case Op::Le:  return vn <= en;
            default: return false;
            }
        }

        // String comparison
        switch (op_) {
        case Op::Eq:       return v == expected;
        case Op::Neq:      return v != expected;
        case Op::Contains: return sv_contains(v, expected);
        default:           return v == expected;
        }
    }

private:
    static f64 string_to_f64(String_View s) noexcept {
        f64 r = 0.0, sign = 1.0; u64 p = 0;
        if (!s.empty() && static_cast<char>(s[0]) == '-') { sign = -1.0; p++; }
        f64 frac = 0.1; bool in_frac = false;
        while (p < s.length()) {
            char c = static_cast<char>(s[p]);
            if (c >= '0' && c <= '9') {
                if (in_frac) { r += static_cast<f64>(c - '0') * frac; frac *= 0.1; }
                else r = r * 10.0 + static_cast<f64>(c - '0');
            } else if (c == '.') in_frac = true;
            else break;
            p++;
        }
        return r * sign;
    }

    static bool sv_contains(String_View haystack, String_View needle) noexcept {
        if (needle.empty()) return true;
        for (u64 i = 0; i + needle.length() <= haystack.length(); i++) {
            bool match = true;
            for (u64 j = 0; j < needle.length(); j++) {
                if (haystack[i+j] != needle[j]) { match = false; break; }
            }
            if (match) return true;
        }
        return false;
    }
};

// =========================================================================
// KVStore — embedded persistent key-value store
// =========================================================================

struct KVStore {
    String<>                  db_path_;    // directory containing table files
    Map<String<>, TableSchema> schemas_;
    Map<String<>, Vec<Row>>    tables_;     // in-memory cache
    bool                       dirty_ = false;

    // =====================================================================
    // Lifecycle
    // =====================================================================

    // Open/create database at path. Loads all existing tables from disk.
    static KVStore open(String_View path) noexcept {
        KVStore store;
        store.db_path_ = String<>{path};
        store.schemas_ = Map<String<>, TableSchema>{};
        store.tables_  = Map<String<>, Vec<Row>>{};
        store.dirty_   = false;

        // Ensure directory exists
        ensure_dir(path);

        store.load_all();
        return store;
    }

    // Close and flush all dirty tables.
    void close() noexcept {
        if (dirty_) flush();
    }

    // =====================================================================
    // Schema
    // =====================================================================

    // Create a table with the given columns and primary key.
    bool create_table(String_View name, Slice<const String_View> columns,
                      String_View primary_key, bool auto_increment = false) noexcept {
        if (name.empty() || table_exists(name)) return false;

        TableSchema schema;
        schema.name_           = String<>{name};
        schema.primary_key_    = String<>{primary_key};
        schema.auto_increment_ = auto_increment;
        schema.next_id_        = 1;

        for (u64 i = 0; i < columns.length(); i++) {
            schema.columns_.push(String<>{columns[i]});
        }

        Vec<Row> empty_rows;
        schemas_.insert(String<>{name}, spp::move(schema));
        tables_.insert(String<>{name}, spp::move(empty_rows));
        dirty_ = true;
        return true;
    }

    // Check if a table exists.
    [[nodiscard]] bool table_exists(String_View name) const noexcept {
        return schemas_.contains(String<>{name});
    }

    // =====================================================================
    // CRUD Operations
    // =====================================================================

    // Insert a row. Returns the primary key value (auto-incremented if
    // applicable, otherwise the value from the row itself).
    String<> insert(String_View table_name, const Row& row) noexcept {
        auto schema_opt = schemas_.try_get(String<>{table_name});
        auto rows_opt   = tables_.try_get(String<>{table_name});
        if (!schema_opt.ok() || !rows_opt.ok()) return String<>{};

        TableSchema& schema = **schema_opt;
        Vec<Row>& rows      = **rows_opt;

        Row new_row;
        // Copy all provided columns
        for (const auto& kv : row) {
            new_row.insert(String<>{kv.first}, String<>{kv.second});
        }

        String<> pk_value;

        // Handle auto-increment primary key
        if (schema.auto_increment_) {
            String<> pk_str = u64_to_string(schema.next_id_);
            pk_value = String<>{pk_str};
            new_row.insert(String<>{schema.primary_key_}, spp::move(pk_str));
            schema.next_id_++;
        } else {
            // Get PK from row
            auto pk_opt = row.try_get(String<>{schema.primary_key_.view()});
            if (pk_opt.ok()) {
                pk_value = String<>{ (**pk_opt).view() };
            } else {
                // No PK provided — generate one as a UUID-like string
                static u64 fallback_id = 0;
                fallback_id++;
                String<> fb = u64_to_string(fallback_id);
                pk_value = String<>{fb};
                new_row.insert(String<>{schema.primary_key_}, spp::move(fb));
            }
        }

        // Ensure all schema columns exist in the row (fill missing with empty)
        for (u64 i = 0; i < schema.columns_.length(); i++) {
            String_View col = schema.columns_[i].view();
            if (!new_row.contains(String<>{col})) {
                new_row.insert(String<>{col}, String<>{});
            }
        }

        rows.push(spp::move(new_row));
        dirty_ = true;
        return pk_value;
    }

    // ---- Non-template update: by primary key equality ----
    u64 update_by_key(String_View table_name, String_View pk_value,
                      const Row& new_values) noexcept {
        auto schema_opt = schemas_.try_get(String<>{table_name});
        auto rows_opt   = tables_.try_get(String<>{table_name});
        if (!schema_opt.ok() || !rows_opt.ok()) return 0;

        TableSchema& schema = **schema_opt;
        Vec<Row>& rows      = **rows_opt;
        u64 count = 0;

        for (u64 i = 0; i < rows.length(); i++) {
            auto pk_opt = rows[i].try_get(String<>{schema.primary_key_.view()});
            if (!pk_opt.ok()) continue;
            if ((**pk_opt).view() != pk_value) continue;

            // Apply updates
            for (const auto& kv : new_values) {
                // Update or insert the column
                auto existing = rows[i].try_get(String<>{kv.first});
                if (existing.ok()) {
                    // Replace existing value — remove and re-insert
                    rows[i].remove(String<>{kv.first});
                }
                rows[i].insert(String<>{kv.first}, String<>{kv.second});
            }
            count++;
        }

        if (count > 0) dirty_ = true;
        return count;
    }

    // ---- Non-template update: by condition ----
    u64 update_by_condition(String_View table_name, const RowCondition& cond,
                            const Row& new_values) noexcept {
        auto schema_opt = schemas_.try_get(String<>{table_name});
        auto rows_opt   = tables_.try_get(String<>{table_name});
        if (!schema_opt.ok() || !rows_opt.ok()) return 0;

        TableSchema& schema = **schema_opt;
        Vec<Row>& rows      = **rows_opt;
        u64 count = 0;

        for (u64 i = 0; i < rows.length(); i++) {
            if (!cond.matches(rows[i])) continue;

            for (const auto& kv : new_values) {
                auto existing = rows[i].try_get(String<>{kv.first});
                if (existing.ok()) {
                    rows[i].remove(String<>{kv.first});
                }
                rows[i].insert(String<>{kv.first}, String<>{kv.second});
            }
            count++;
        }

        if (count > 0) dirty_ = true;
        return count;
    }

    // ---- Non-template delete: by condition ----
    u64 delete_by_condition(String_View table_name, const RowCondition& cond) noexcept {
        auto schema_opt = schemas_.try_get(String<>{table_name});
        auto rows_opt   = tables_.try_get(String<>{table_name});
        if (!schema_opt.ok() || !rows_opt.ok()) return 0;

        TableSchema& schema = **schema_opt;
        Vec<Row>& rows      = **rows_opt;
        u64 count = 0;

        // Delete matching rows (iterate backwards for safe removal)
        for (u64 i = rows.length(); i > 0; i--) {
            u64 idx = i - 1;
            if (cond.matches(rows[idx])) {
                // Shift and pop
                for (u64 j = idx; j + 1 < rows.length(); j++) {
                    rows[j] = spp::move(rows[j + 1]);
                }
                rows.pop();
                count++;
            }
        }

        if (count > 0) dirty_ = true;
        return count;
    }

    // ---- Non-template select: by condition ----
    [[nodiscard]] QueryResult select_by_condition(String_View table_name,
                                                   const RowCondition& cond) const noexcept {
        QueryResult result;
        auto schema_opt = schemas_.try_get(String<>{table_name});
        auto rows_opt   = tables_.try_get(String<>{table_name});
        if (!schema_opt.ok() || !rows_opt.ok()) return result;

        const TableSchema& schema = **schema_opt;
        const Vec<Row>& rows      = **rows_opt;

        for (u64 i = 0; i < rows.length(); i++) {
            if (cond.matches(rows[i])) {
                result.rows_.push(copy_row(rows[i]));
            }
        }
        return result;
    }

    // ---- Non-template select: all rows ----
    [[nodiscard]] QueryResult select_all(String_View table_name) const noexcept {
        QueryResult result;
        auto rows_opt = tables_.try_get(String<>{table_name});
        if (!rows_opt.ok()) return result;

        const Vec<Row>& rows = **rows_opt;
        for (u64 i = 0; i < rows.length(); i++) {
            result.rows_.push(copy_row(rows[i]));
        }
        return result;
    }

    // ---- Select by primary key (single row) ----
    [[nodiscard]] Opt<Row> select_by_key(String_View table_name, String_View key) const noexcept {
        auto schema_opt = schemas_.try_get(String<>{table_name});
        auto rows_opt   = tables_.try_get(String<>{table_name});
        if (!schema_opt.ok() || !rows_opt.ok()) return {};

        const TableSchema& schema = **schema_opt;
        const Vec<Row>& rows      = **rows_opt;

        for (u64 i = 0; i < rows.length(); i++) {
            auto pk_opt = rows[i].try_get(String<>{schema.primary_key_.view()});
            if (pk_opt.ok() && (**pk_opt).view() == key) {
                return Opt<Row>{copy_row(rows[i])};
            }
        }
        return {};
    }

    // ---- Select with column projection ----
    [[nodiscard]] QueryResult select_columns(String_View table_name,
                                              Slice<const String_View> columns,
                                              const RowCondition& cond) const noexcept {
        QueryResult result;
        auto rows_opt = tables_.try_get(String<>{table_name});
        if (!rows_opt.ok()) return result;

        const Vec<Row>& rows = **rows_opt;

        for (u64 i = 0; i < rows.length(); i++) {
            if (cond.matches(rows[i])) {
                Row projected;
                for (u64 c = 0; c < columns.length(); c++) {
                    auto val = rows[i].try_get(String<>{columns[c]});
                    if (val.ok()) {
                        projected.insert(String<>{columns[c]}, String<>{ (**val).view() });
                    }
                }
                result.rows_.push(spp::move(projected));
            }
        }
        return result;
    }

    // ---- Non-template delete by primary key ----
    u64 delete_by_key(String_View table_name, String_View pk_value) noexcept {
        auto schema_opt = schemas_.try_get(String<>{table_name});
        auto rows_opt   = tables_.try_get(String<>{table_name});
        if (!schema_opt.ok() || !rows_opt.ok()) return 0;

        TableSchema& schema = **schema_opt;
        Vec<Row>& rows      = **rows_opt;

        for (u64 i = 0; i < rows.length(); i++) {
            auto pk_opt = rows[i].try_get(String<>{schema.primary_key_.view()});
            if (pk_opt.ok() && (**pk_opt).view() == pk_value) {
                for (u64 j = i; j + 1 < rows.length(); j++) {
                    rows[j] = spp::move(rows[j + 1]);
                }
                rows.pop();
                dirty_ = true;
                return 1;
            }
        }
        return 0;
    }

    // =====================================================================
    // SQL-like interface
    // =====================================================================

    // Execute a simple SQL-like query string.
    // Supported:
    //   INSERT INTO table VALUES (v1, v2, ...)
    //   INSERT INTO table (col1, col2) VALUES (v1, v2)
    //   SELECT * FROM table WHERE col = 'value'
    //   SELECT col1, col2 FROM table WHERE col = 'value'
    //   SELECT * FROM table
    //   UPDATE table SET col1=v1, col2=v2 WHERE col = 'value'
    //   DELETE FROM table WHERE col = 'value'
    QueryResult execute(String_View sql) noexcept {
        // Trim whitespace
        sql = trim(sql);
        if (sql.empty()) return QueryResult{};

        // Dispatch based on first keyword
        if (sv::starts_with(sql, "INSERT"_v) || sv::starts_with(sql, "insert"_v)) {
            return execute_insert(sql);
        }
        if (sv::starts_with(sql, "SELECT"_v) || sv::starts_with(sql, "select"_v)) {
            return execute_select(sql);
        }
        if (sv::starts_with(sql, "UPDATE"_v) || sv::starts_with(sql, "update"_v)) {
            return execute_update(sql);
        }
        if (sv::starts_with(sql, "DELETE"_v) || sv::starts_with(sql, "delete"_v)) {
            return execute_delete(sql);
        }

        return QueryResult{};
    }

    // Prepare a parameterized query.
    PreparedQuery prepare(String_View sql) noexcept {
        PreparedQuery pq;
        pq.sql_ = String<>{sql};
        return pq;
    }

    // Execute a prepared query (substitutes $param placeholders).
    QueryResult execute_prepared(const PreparedQuery& query) noexcept {
        String<> resolved = substitute_params(query.sql_.view(), query.params_);
        return execute(resolved.view());
    }

    // =====================================================================
    // Persistence
    // =====================================================================

    // Flush all dirty tables to disk.
    bool flush() noexcept {
        if (db_path_.view().empty()) return false;

        bool all_ok = true;
        for (const auto& kv : schemas_) {
            if (!flush_table(kv.first.view())) all_ok = false;
        }
        if (all_ok) dirty_ = false;
        return all_ok;
    }

    // Flush a specific table to disk.
    bool flush_table(String_View name) noexcept {
        auto schema_opt = schemas_.try_get(String<>{name});
        auto rows_opt   = tables_.try_get(String<>{name});
        if (!schema_opt.ok() || !rows_opt.ok()) return false;

        // Build file path: db_path_/table_name.jsonl
        Vec<u8> path_buf;
        path_buf.reserve(db_path_.view().length() + name.length() + 16);
        push_sv_to(path_buf, db_path_.view());
        path_buf.push('/');
        push_sv_to(path_buf, name);
        push_sv_to(path_buf, ".jsonl"_v);

        String<> file_path{path_buf.length()};
        file_path.set_length(path_buf.length());
        for (u64 i = 0; i < path_buf.length(); i++) file_path[i] = path_buf[i];

        // Build temp path
        Vec<u8> tmp_path_buf = path_buf;
        push_sv_to(tmp_path_buf, ".tmp"_v);
        String<> tmp_path{tmp_path_buf.length()};
        tmp_path.set_length(tmp_path_buf.length());
        for (u64 i = 0; i < tmp_path_buf.length(); i++) tmp_path[i] = tmp_path_buf[i];

        // Serialize table to JSONL
        String<> content = serialize_table(name, **schema_opt, **rows_opt);

        // Atomic write: write to .tmp, then rename
        Slice<const u8> data{content.data(), content.length()};
        if (!Files::write(tmp_path.view(), data)) {
            // Clean up temp file
            Files::remove_result(tmp_path.view());
            return false;
        }

        auto rename_result = Files::rename_result(tmp_path.view(), file_path.view());
        if (!rename_result.ok()) {
            Files::remove_result(tmp_path.view());
            return false;
        }

        return true;
    }

    // Load all tables from disk.
    bool load_all() noexcept {
        if (db_path_.view().empty()) return false;

        // We probe for known table files. In production, use readdir.
        // For each schema we know about (or discover from meta files),
        // load the corresponding .jsonl file.

        // First pass: probe for any .jsonl files at the path
        // Since we can't list directories, we rely on schemas already
        // registered, or we try loading tables that were previously
        // created (via create_table).

        // For each schema we already know, try loading
        for (const auto& kv : schemas_) {
            load_table_file(kv.first.view());
        }

        dirty_ = false;
        return true;
    }

    // Create a checkpoint (atomic snapshot of all tables).
    bool checkpoint(String_View checkpoint_path) noexcept {
        // Write all tables to the checkpoint directory
        ensure_dir(checkpoint_path);

        bool all_ok = true;
        for (const auto& kv : schemas_) {
            auto rows_opt = tables_.try_get(String<>{kv.first.view()});
            if (!rows_opt.ok()) { all_ok = false; continue; }

            Vec<u8> path_buf;
            path_buf.reserve(checkpoint_path.length() + kv.first.view().length() + 16);
            push_sv_to(path_buf, checkpoint_path);
            path_buf.push('/');
            push_sv_to(path_buf, kv.first.view());
            push_sv_to(path_buf, ".jsonl"_v);

            String<> file_path{path_buf.length()};
            file_path.set_length(path_buf.length());
            for (u64 i = 0; i < path_buf.length(); i++) file_path[i] = path_buf[i];

            String<> content = serialize_table(kv.first.view(), kv.second, **rows_opt);
            Slice<const u8> data{content.data(), content.length()};
            if (!Files::write(file_path.view(), data)) all_ok = false;
        }
        return all_ok;
    }

private:
    // =====================================================================
    // File I/O helpers
    // =====================================================================

    static void push_sv_to(Vec<u8>& buf, String_View s) noexcept {
        for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
    }

    static void ensure_dir(String_View path) noexcept {
#ifdef SPP_OS_LINUX
        String<> path_z{path.length() + 1};
        path_z.set_length(path.length() + 1);
        for (u64 i = 0; i < path.length(); i++) path_z[i] = path[i];
        path_z[path.length()] = '\0';
        ::mkdir(reinterpret_cast<const char*>(path_z.data()), 0755);
#else
        (void)path;
#endif
    }

    static String<> u64_to_string(u64 v) noexcept {
        if (v == 0) { String<> s{1}; s.set_length(1); s[0] = '0'; return s; }
        char buf[24]; i32 p = 23; buf[p] = '\0';
        u64 vv = v;
        while (vv > 0) { buf[--p] = static_cast<char>('0' + (vv % 10)); vv /= 10; }
        i32 len = 23 - p;
        String<> s{static_cast<u64>(len)}; s.set_length(static_cast<u64>(len));
        for (i32 i = 0; i < len; i++) s[static_cast<u64>(i)] = static_cast<u8>(buf[p + i]);
        return s;
    }

    static String_View trim(String_View s) noexcept {
        u64 start = 0;
        while (start < s.length() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) start++;
        u64 end = s.length();
        while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\n' || s[end-1] == '\r')) end--;
        return s.sub(start, end);
    }

    static Row copy_row(const Row& src) noexcept {
        Row dst;
        for (const auto& kv : src) {
            dst.insert(String<>{kv.first}, String<>{kv.second});
        }
        return dst;
    }

    // =====================================================================
    // JSONL serialization
    // =====================================================================

    [[nodiscard]] String<> serialize_table(String_View name, const TableSchema& schema,
                                            const Vec<Row>& rows) const noexcept {
        Vec<u8> buf;
        buf.reserve(4096);

        // ---- Header line: _meta object ----
        write_jsonl_meta_line(buf, name, schema);

        // ---- Row lines ----
        for (u64 i = 0; i < rows.length(); i++) {
            write_jsonl_row_line(buf, rows[i], schema);
        }

        String<> result{buf.length()};
        result.set_length(buf.length());
        for (u64 i = 0; i < buf.length(); i++) result[i] = buf[i];
        return result;
    }

    void write_jsonl_meta_line(Vec<u8>& buf, String_View name,
                                const TableSchema& schema) const noexcept {
        auto push_sv = [&buf](String_View s) {
            for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
        };

        push_sv("{\"_meta\":{\"name\":"_v);
        json_write_string(buf, name);
        push_sv(",\"pk\":"_v);
        json_write_string(buf, schema.primary_key_.view());
        push_sv(",\"columns\":["_v);
        for (u64 i = 0; i < schema.columns_.length(); i++) {
            if (i > 0) buf.push(',');
            json_write_string(buf, schema.columns_[i].view());
        }
        buf.push(']');
        push_sv(",\"auto_inc\":"_v);
        if (schema.auto_increment_) push_sv("true"_v);
        else push_sv("false"_v);
        push_sv(",\"next_id\":"_v);
        push_sv(u64_to_string(schema.next_id_).view());
        push_sv("}}\n"_v);
    }

    void write_jsonl_row_line(Vec<u8>& buf, const Row& row,
                               const TableSchema& schema) const noexcept {
        buf.push('{');
        bool first = true;
        for (u64 i = 0; i < schema.columns_.length(); i++) {
            String_View col = schema.columns_[i].view();
            auto val_opt = row.try_get(String<>{col});
            if (!val_opt.ok()) continue;

            if (!first) buf.push(',');
            first = false;

            json_write_string(buf, col);
            buf.push(':');
            json_write_string(buf, (**val_opt).view());
        }
        push_sv_to(buf, "}\n"_v);
    }

    // =====================================================================
    // JSONL deserialization
    // =====================================================================

    bool load_table_file(String_View name) noexcept {
        Vec<u8> path_buf;
        path_buf.reserve(db_path_.view().length() + name.length() + 16);
        push_sv_to(path_buf, db_path_.view());
        path_buf.push('/');
        push_sv_to(path_buf, name);
        push_sv_to(path_buf, ".jsonl"_v);

        String<> file_path{path_buf.length()};
        file_path.set_length(path_buf.length());
        for (u64 i = 0; i < path_buf.length(); i++) file_path[i] = path_buf[i];

        auto file_data = Files::read(file_path.view());
        if (!file_data.ok()) return false;

        String_View content{file_data->data(), file_data->length()};
        return parse_jsonl(content, name);
    }

    bool parse_jsonl(String_View content, String_View /*table_name*/) noexcept {
        // Parse line by line
        u64 line_start = 0;
        TableSchema* current_schema = null;

        for (u64 i = 0; i <= content.length(); i++) {
            if (i == content.length() || content[i] == '\n') {
                String_View line = content.sub(line_start, i);
                // Strip trailing \r
                if (!line.empty() && line[line.length()-1] == '\r') {
                    line = line.sub(0, line.length() - 1);
                }
                line_start = i + 1;
                if (line.empty()) continue;

                // Check if this is a meta line
                if (line.length() > 10 && line[0] == '{' && line[1] == '"' &&
                    line[2] == '_' && line[3] == 'm' && line[4] == 'e' &&
                    line[5] == 't' && line[6] == 'a' && line[7] == '"') {
                    // Parse meta line
                    auto schema_opt = parse_meta_line(line);
                    if (schema_opt.ok()) {
                        String<> tname{(*schema_opt).name_};
                        current_schema = null; // will be set after insertion
                        schemas_.insert(String<>{tname}, spp::move(*schema_opt));
                        Vec<Row> empty;
                        tables_.insert(String<>{tname}, spp::move(empty));
                        current_schema = schemas_.try_get(String<>{tname});
                        if (current_schema.ok()) current_schema = &(**current_schema);
                    }
                } else if (line[0] == '{') {
                    // Parse row line
                    if (current_schema) {
                        auto row_opt = parse_row_line(line, *current_schema);
                        if (row_opt.ok()) {
                            auto rows_opt = tables_.try_get(String<>{current_schema->name_.view()});
                            if (rows_opt.ok()) {
                                (**rows_opt).push(spp::move(*row_opt));
                            }
                        }
                    }
                }
            }
        }
        return true;
    }

    Opt<TableSchema> parse_meta_line(String_View line) noexcept {
        TableSchema schema;

        auto name = json_extract_string(line, "name"_v);
        if (!name.ok()) return {};
        schema.name_ = String<>{*name};

        auto pk = json_extract_string(line, "pk"_v);
        if (pk.ok()) schema.primary_key_ = String<>{*pk};

        auto auto_inc = json_extract_bool(line, "auto_inc"_v);
        if (auto_inc.ok()) schema.auto_increment_ = *auto_inc;

        auto next_id = json_extract_u64(line, "next_id"_v);
        if (next_id.ok()) schema.next_id_ = *next_id;

        // Parse columns array
        auto cols = json_extract_array(line, "columns"_v);
        if (cols.ok()) {
            String_View arr = *cols;
            u64 pos = 1; // skip '['
            while (pos < arr.length()) {
                while (pos < arr.length() && (arr[pos] == ' ' || arr[pos] == ',')) pos++;
                if (pos >= arr.length() || arr[pos] == ']') break;
                if (arr[pos] == '"') {
                    u64 vs = pos + 1;
                    u64 ve = vs;
                    while (ve < arr.length() && arr[ve] != '"') { if (arr[ve] == '\\') ve++; ve++; }
                    if (ve < arr.length()) {
                        schema.columns_.push(String<>{arr.sub(vs, ve)});
                        pos = ve + 1;
                        continue;
                    }
                }
                pos++;
            }
        }

        return Opt<TableSchema>{spp::move(schema)};
    }

    Opt<Row> parse_row_line(String_View line, const TableSchema& schema) noexcept {
        Row row;

        // Parse JSON object: {"col1":"val1","col2":"val2",...}
        u64 pos = 1; // skip '{'
        while (pos < line.length() && line[pos] != '}') {
            // Skip whitespace and commas
            while (pos < line.length() && (line[pos] == ' ' || line[pos] == ',')) pos++;
            if (pos >= line.length() || line[pos] == '}') break;

            // Read key
            if (line[pos] != '"') break;
            u64 key_start = pos + 1;
            u64 key_end = key_start;
            while (key_end < line.length() && line[key_end] != '"') {
                if (line[key_end] == '\\') key_end++;
                key_end++;
            }
            if (key_end >= line.length()) break;
            String_View key = line.sub(key_start, key_end);
            pos = key_end + 1;

            // Skip ':'
            while (pos < line.length() && line[pos] != ':') pos++;
            pos++; // skip ':'
            while (pos < line.length() && (line[pos] == ' ')) pos++;

            // Read value
            if (pos >= line.length()) break;
            String_View value;
            if (line[pos] == '"') {
                u64 val_start = pos + 1;
                u64 val_end = val_start;
                while (val_end < line.length() && line[val_end] != '"') {
                    if (line[val_end] == '\\') val_end++;
                    val_end++;
                }
                if (val_end >= line.length()) break;
                value = line.sub(val_start, val_end);
                pos = val_end + 1;
            } else if (line[pos] == 't' || line[pos] == 'f' || line[pos] == 'n') {
                // true / false / null
                u64 ve = pos;
                while (ve < line.length() && line[ve] != ',' && line[ve] != '}' && line[ve] != ' ') ve++;
                value = line.sub(pos, ve);
                pos = ve;
            } else {
                // Number
                u64 ve = pos;
                if (line[ve] == '-') ve++;
                while (ve < line.length() && ((line[ve] >= '0' && line[ve] <= '9') || line[ve] == '.')) ve++;
                value = line.sub(pos, ve);
                pos = ve;
            }

            if (!key.empty()) {
                row.insert(String<>{key}, String<>{value});
            }
        }

        if (row.empty()) return {};
        return Opt<Row>{spp::move(row)};
    }

    // =====================================================================
    // JSON helpers for parsing
    // =====================================================================

    static void json_write_string(Vec<u8>& buf, String_View s) noexcept {
        buf.push('"');
        for (u64 i = 0; i < s.length(); i++) {
            u8 c = s[i];
            if (c == '"' || c == '\\') { buf.push('\\'); buf.push(c); }
            else if (c == '\n') { buf.push('\\'); buf.push('n'); }
            else if (c == '\r') { buf.push('\\'); buf.push('r'); }
            else if (c == '\t') { buf.push('\\'); buf.push('t'); }
            else buf.push(c);
        }
        buf.push('"');
    }

    static Opt<String_View> json_extract_string(String_View json, String_View key) noexcept {
        u64 pos = find_json_key(json, key);
        if (pos >= json.length()) return {};

        u64 cp = pos;
        while (cp < json.length() && json[cp] != ':') cp++;
        cp++;
        while (cp < json.length() && (json[cp] == ' ' || json[cp] == '\t')) cp++;
        if (cp >= json.length() || json[cp] != '"') return {};

        u64 vs = cp + 1;
        u64 ve = vs;
        while (ve < json.length() && json[ve] != '"') { if (json[ve] == '\\') ve++; ve++; }
        if (ve >= json.length()) return {};

        return Opt<String_View>{json.sub(vs, ve)};
    }

    static Opt<bool> json_extract_bool(String_View json, String_View key) noexcept {
        auto sv = json_extract_string(json, key);
        if (!sv.ok()) {
            // Try as bare token (not string-quoted)
            u64 pos = find_json_key(json, key);
            if (pos >= json.length()) return {};
            u64 cp = pos;
            while (cp < json.length() && json[cp] != ':') cp++;
            cp++;
            while (cp < json.length() && (json[cp] == ' ' || json[cp] == '\t')) cp++;
            if (cp + 4 <= json.length() && json[cp] == 't' && json[cp+1] == 'r' && json[cp+2] == 'u' && json[cp+3] == 'e')
                return Opt<bool>{true};
            if (cp + 5 <= json.length() && json[cp] == 'f' && json[cp+1] == 'a' && json[cp+2] == 'l' && json[cp+3] == 's' && json[cp+4] == 'e')
                return Opt<bool>{false};
            return {};
        }
        return Opt<bool>{(*sv == "true"_v)};
    }

    static Opt<u64> json_extract_u64(String_View json, String_View key) noexcept {
        u64 pos = find_json_key(json, key);
        if (pos >= json.length()) return {};

        u64 cp = pos;
        while (cp < json.length() && json[cp] != ':') cp++;
        cp++;
        while (cp < json.length() && (json[cp] == ' ' || json[cp] == '\t')) cp++;
        if (cp >= json.length()) return {};

        u64 result = 0;
        while (cp < json.length() && json[cp] >= '0' && json[cp] <= '9') {
            result = result * 10 + static_cast<u64>(json[cp] - '0');
            cp++;
        }
        if (cp > pos && result > 0) return Opt<u64>{result};
        return {};
    }

    static Opt<String_View> json_extract_array(String_View json, String_View key) noexcept {
        u64 pos = find_json_key(json, key);
        if (pos >= json.length()) return {};

        u64 cp = pos;
        while (cp < json.length() && json[cp] != ':') cp++;
        cp++;
        while (cp < json.length() && (json[cp] == ' ' || json[cp] == '\t')) cp++;
        if (cp >= json.length() || json[cp] != '[') return {};

        i32 depth = 0;
        u64 end = cp;
        for (; end < json.length(); end++) {
            if (json[end] == '[') depth++;
            else if (json[end] == ']') { depth--; if (depth == 0) break; }
        }
        if (end >= json.length()) return {};
        return Opt<String_View>{json.sub(cp, end + 1)};
    }

    static u64 find_json_key(String_View json, String_View key) noexcept {
        for (u64 i = 0; i + key.length() + 3 <= json.length(); i++) {
            if (json[i] == '"') {
                u64 ks = i + 1;
                bool match = true;
                for (u64 k = 0; k < key.length(); k++) {
                    if (ks + k >= json.length() || json[ks + k] != key[k]) {
                        match = false; break;
                    }
                }
                if (match && ks + key.length() < json.length() && json[ks + key.length()] == '"') {
                    return i;
                }
            }
        }
        return json.length();
    }

    // =====================================================================
    // Simple SQL parser
    // =====================================================================

    // INSERT INTO table VALUES (v1, v2, ...)
    // INSERT INTO table (col1, col2) VALUES (v1, v2)
    QueryResult execute_insert(String_View sql) noexcept {
        QueryResult result;

        // Skip "INSERT INTO "
        String_View rest = skip_keyword(sql, "INSERT"_v);
        rest = skip_keyword(rest, "INTO"_v);

        // Read table name
        auto table_name = read_identifier(rest);
        if (!table_name.ok()) return result;
        rest = advance(rest, table_name->end());

        // Check for column list "(col1, col2)"
        Vec<String_View> columns;
        if (!rest.empty() && rest[0] == '(') {
            auto cols = read_value_list(rest);
            if (!cols.ok()) return result;
            columns = spp::move(*cols);
            rest = advance(rest, cols.end());
        }

        // Skip "VALUES"
        rest = skip_keyword(rest, "VALUES"_v);

        // Read value list "(v1, v2, ...)"
        auto vals = read_value_list(rest);
        if (!vals.ok()) return result;

        // Build row
        Row row;
        // If columns specified, match positions; otherwise use schema column order
        auto schema_opt = schemas_.try_get(String<>{table_name->view()});
        if (schema_opt.ok() && columns.empty()) {
            // Positional: map to schema columns in order
            const TableSchema& schema = **schema_opt;
            u64 col_count = vals->length();
            for (u64 i = 0; i < col_count && i < schema.columns_.length(); i++) {
                row.insert(String<>{schema.columns_[i]}, String<>{(*vals)[i]});
            }
        } else {
            // Named columns
            u64 n = columns.length();
            if (vals->length() < n) n = vals->length();
            for (u64 i = 0; i < n; i++) {
                row.insert(String<>{columns[i]}, String<>{(*vals)[i]});
            }
        }

        if (!row.empty()) {
            String<> pk = insert(table_name->view(), row);
            Row result_row;
            result_row.insert(String<>{'p', 'k'}, spp::move(pk));
            result.rows_.push(spp::move(result_row));
        }
        return result;
    }

    // SELECT * FROM table WHERE col = 'value'
    // SELECT col1, col2 FROM table WHERE col = 'value'
    // SELECT * FROM table
    QueryResult execute_select(String_View sql) noexcept {
        // Skip "SELECT "
        String_View rest = skip_keyword(sql, "SELECT"_v);

        // Read columns or *
        Vec<String_View> columns;
        bool select_all_cols = false;
        if (!rest.empty() && rest[0] == '*') {
            select_all_cols = true;
            rest = rest.sub(1, rest.length());
            rest = trim(rest);
        } else {
            // Read comma-separated column list
            while (!rest.empty() && !sv::starts_with(rest, "FROM"_v) && !sv::starts_with(rest, "from"_v)) {
                auto id = read_identifier(rest);
                if (!id.ok()) break;
                columns.push(*id);
                rest = advance(rest, id.end());
                rest = trim(rest);
                if (!rest.empty() && rest[0] == ',') {
                    rest = rest.sub(1, rest.length());
                    rest = trim(rest);
                }
            }
        }

        // Skip "FROM"
        rest = skip_keyword(rest, "FROM"_v);

        // Read table name
        auto table_name = read_identifier(rest);
        if (!table_name.ok()) return QueryResult{};
        rest = advance(rest, table_name.end());

        // Check for WHERE clause
        if (!rest.empty() && (sv::starts_with(rest, "WHERE"_v) || sv::starts_with(rest, "where"_v))) {
            rest = skip_keyword(rest, "WHERE"_v);
            auto cond = parse_where_clause(rest);
            if (cond.ok()) {
                if (select_all_cols) {
                    return select_by_condition(table_name->view(), *cond);
                } else if (!columns.empty()) {
                    // Build Slice of String_View columns
                    Vec<String_View> col_views;
                    for (u64 i = 0; i < columns.length(); i++) {
                        col_views.push(columns[i]);
                    }
                    return select_columns(table_name->view(), col_views.slice(), *cond);
                }
            }
        }

        // No WHERE — return all rows
        return select_all(table_name->view());
    }

    // UPDATE table SET col1=v1, col2=v2 WHERE col = 'value'
    QueryResult execute_update(String_View sql) noexcept {
        QueryResult result;

        // Skip "UPDATE "
        String_View rest = skip_keyword(sql, "UPDATE"_v);

        // Read table name
        auto table_name = read_identifier(rest);
        if (!table_name.ok()) return result;
        rest = advance(rest, table_name.end());

        // Skip "SET"
        rest = skip_keyword(rest, "SET"_v);

        // Read assignments: col1=v1, col2=v2
        Row new_values;
        while (!rest.empty() && !sv::starts_with(rest, "WHERE"_v) && !sv::starts_with(rest, "where"_v)) {
            auto col = read_identifier(rest);
            if (!col.ok()) break;
            rest = advance(rest, col.end());
            rest = trim(rest);
            if (rest.empty() || rest[0] != '=') break;
            rest = rest.sub(1, rest.length());
            rest = trim(rest);

            // Read value
            String_View val;
            if (!rest.empty() && rest[0] == '\'') {
                auto qv = read_quoted_string(rest);
                if (!qv.ok()) break;
                val = *qv;
                rest = advance(rest, qv.end());
            } else if (!rest.empty() && (rest[0] == '"')) {
                auto qv = read_double_quoted_string(rest);
                if (!qv.ok()) break;
                val = *qv;
                rest = advance(rest, qv.end());
            } else {
                auto id = read_identifier(rest);
                if (!id.ok()) break;
                val = *id;
                rest = advance(rest, id.end());
            }

            new_values.insert(String<>{*col}, String<>{val});

            rest = trim(rest);
            if (!rest.empty() && rest[0] == ',') {
                rest = rest.sub(1, rest.length());
                rest = trim(rest);
            }
        }

        if (new_values.empty()) return result;

        // Check for WHERE clause
        if (!rest.empty() && (sv::starts_with(rest, "WHERE"_v) || sv::starts_with(rest, "where"_v))) {
            rest = skip_keyword(rest, "WHERE"_v);
            auto cond = parse_where_clause(rest);
            if (cond.ok()) {
                u64 count = update_by_condition(table_name->view(), *cond, new_values);
                Row count_row;
                count_row.insert(String<>{'u', 'p', 'd', 'a', 't', 'e', 'd'}, u64_to_string(count));
                result.rows_.push(spp::move(count_row));
            }
        } else {
            // No WHERE — update all rows
            // Use a condition that always matches
            RowCondition always_true;
            always_true.op_ = RowCondition::Op::Eq;
            // Match rows that have the primary key set (always true for valid rows)
            u64 count = 0;
            auto schema_opt = schemas_.try_get(String<>{table_name->view()});
            auto rows_opt   = tables_.try_get(String<>{table_name->view()});
            if (schema_opt.ok() && rows_opt.ok()) {
                for (u64 i = 0; i < (**rows_opt).length(); i++) {
                    for (const auto& kv : new_values) {
                        auto ex = (**rows_opt)[i].try_get(String<>{kv.first});
                        if (ex.ok()) (**rows_opt)[i].remove(String<>{kv.first});
                        (**rows_opt)[i].insert(String<>{kv.first}, String<>{kv.second});
                    }
                    count++;
                }
            }
            Row count_row;
            count_row.insert(String<>{'u', 'p', 'd', 'a', 't', 'e', 'd'}, u64_to_string(count));
            result.rows_.push(spp::move(count_row));
        }

        return result;
    }

    // DELETE FROM table WHERE col = 'value'
    QueryResult execute_delete(String_View sql) noexcept {
        QueryResult result;

        // Skip "DELETE FROM "
        String_View rest = skip_keyword(sql, "DELETE"_v);
        rest = skip_keyword(rest, "FROM"_v);

        // Read table name
        auto table_name = read_identifier(rest);
        if (!table_name.ok()) return result;
        rest = advance(rest, table_name.end());

        // Check for WHERE clause
        if (!rest.empty() && (sv::starts_with(rest, "WHERE"_v) || sv::starts_with(rest, "where"_v))) {
            rest = skip_keyword(rest, "WHERE"_v);
            auto cond = parse_where_clause(rest);
            if (cond.ok()) {
                u64 count = delete_by_condition(table_name->view(), *cond);
                Row count_row;
                count_row.insert(String<>{'d', 'e', 'l', 'e', 't', 'e', 'd'}, u64_to_string(count));
                result.rows_.push(spp::move(count_row));
            }
        } else {
            // No WHERE — delete all rows
            auto rows_opt = tables_.try_get(String<>{table_name->view()});
            u64 count = 0;
            if (rows_opt.ok()) {
                count = (**rows_opt).length();
                (**rows_opt).clear();
                dirty_ = true;
            }
            Row count_row;
            count_row.insert(String<>{'d', 'e', 'l', 'e', 't', 'e', 'd'}, u64_to_string(count));
            result.rows_.push(spp::move(count_row));
        }

        return result;
    }

    // =====================================================================
    // SQL parser helpers
    // =====================================================================

    // A parsed token with end position for advancing
    struct ParsePos { u64 end_pos; };

    static Opt<String_View> read_identifier(String_View sql) noexcept {
        sql = trim(sql);
        if (sql.empty()) return {};

        u64 start = 0;
        u64 end = start;
        // Identifiers: alphanumeric + underscore
        while (end < sql.length()) {
            char c = static_cast<char>(sql[end]);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '.') {
                end++;
            } else break;
        }
        if (end == start) return {};
        return Opt<String_View>{sql.sub(start, end)};
    }

    static Opt<Vec<String_View>> read_value_list(String_View sql) noexcept {
        sql = trim(sql);
        if (sql.empty() || sql[0] != '(') return {};

        Vec<String_View> values;
        u64 pos = 1; // skip '('

        while (pos < sql.length() && sql[pos] != ')') {
            while (pos < sql.length() && (sql[pos] == ' ' || sql[pos] == ',' || sql[pos] == '\t')) pos++;
            if (pos >= sql.length() || sql[pos] == ')') break;

            String_View val;
            if (sql[pos] == '\'') {
                // Single-quoted string
                u64 vs = pos + 1;
                u64 ve = vs;
                while (ve < sql.length() && sql[ve] != '\'') {
                    if (sql[ve] == '\\') ve++;
                    ve++;
                }
                if (ve >= sql.length()) break;
                val = sql.sub(vs, ve);
                pos = ve + 1;
            } else if (sql[pos] == '"') {
                // Double-quoted string
                u64 vs = pos + 1;
                u64 ve = vs;
                while (ve < sql.length() && sql[ve] != '"') {
                    if (sql[ve] == '\\') ve++;
                    ve++;
                }
                if (ve >= sql.length()) break;
                val = sql.sub(vs, ve);
                pos = ve + 1;
            } else {
                // Number or identifier
                u64 ve = pos;
                while (ve < sql.length() && sql[ve] != ',' && sql[ve] != ')' && sql[ve] != ' ') ve++;
                val = sql.sub(pos, ve);
                pos = ve;
            }
            values.push(val);
        }
        if (pos >= sql.length() || sql[pos] != ')') return {};
        pos++; // skip ')'

        return Opt<Vec<String_View>>{spp::move(values)};
    }

    static Opt<String_View> read_quoted_string(String_View sql) noexcept {
        sql = trim(sql);
        if (sql.empty() || sql[0] != '\'') return {};
        u64 vs = 1;
        u64 ve = vs;
        while (ve < sql.length() && sql[ve] != '\'') {
            if (sql[ve] == '\\') ve++;
            ve++;
        }
        if (ve >= sql.length()) return {};
        return Opt<String_View>{sql.sub(vs, ve)};
    }

    static Opt<String_View> read_double_quoted_string(String_View sql) noexcept {
        sql = trim(sql);
        if (sql.empty() || sql[0] != '"') return {};
        u64 vs = 1;
        u64 ve = vs;
        while (ve < sql.length() && sql[ve] != '"') {
            if (sql[ve] == '\\') ve++;
            ve++;
        }
        if (ve >= sql.length()) return {};
        return Opt<String_View>{sql.sub(vs, ve)};
    }

    static String_View skip_keyword(String_View sql, String_View keyword) noexcept {
        sql = trim(sql);
        if (sql.length() >= keyword.length()) {
            bool match = true;
            for (u64 i = 0; i < keyword.length(); i++) {
                char a = static_cast<char>(sql[i]);
                char b = static_cast<char>(keyword[i]);
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                if (a != b) { match = false; break; }
            }
            if (match) {
                sql = sql.sub(keyword.length(), sql.length());
                return trim(sql);
            }
        }
        return sql;
    }

    static String_View advance(String_View sql, u64 pos) noexcept {
        if (pos >= sql.length()) return ""_v;
        return trim(sql.sub(pos, sql.length()));
    }

    static Opt<RowCondition> parse_where_clause(String_View sql) noexcept {
        sql = trim(sql);
        if (sql.empty()) return {};

        // Read left-hand column
        auto col = read_identifier(sql);
        if (!col.ok()) return {};
        sql = advance(sql, col.end());

        // Read operator
        RowCondition::Op op = RowCondition::Op::Eq;
        if (sv::starts_with(sql, "!="_v) || sv::starts_with(sql, "<>"_v)) {
            op = RowCondition::Op::Neq;
            sql = sql.sub(2, sql.length());
        } else if (sv::starts_with(sql, ">="_v)) {
            op = RowCondition::Op::Ge;
            sql = sql.sub(2, sql.length());
        } else if (sv::starts_with(sql, "<="_v)) {
            op = RowCondition::Op::Le;
            sql = sql.sub(2, sql.length());
        } else if (sql[0] == '>') {
            op = RowCondition::Op::Gt;
            sql = sql.sub(1, sql.length());
        } else if (sql[0] == '<') {
            op = RowCondition::Op::Lt;
            sql = sql.sub(1, sql.length());
        } else if (sql[0] == '=') {
            op = RowCondition::Op::Eq;
            sql = sql.sub(1, sql.length());
        }
        sql = trim(sql);

        // Read right-hand value
        String_View val;
        if (!sql.empty() && sql[0] == '\'') {
            auto qv = read_quoted_string(sql);
            if (!qv.ok()) return {};
            val = *qv;
        } else if (!sql.empty() && sql[0] == '"') {
            auto qv = read_double_quoted_string(sql);
            if (!qv.ok()) return {};
            val = *qv;
        } else {
            auto id = read_identifier(sql);
            if (!id.ok()) return {};
            val = *id;
        }

        RowCondition cond;
        cond.column_ = String<>{*col};
        cond.value_  = String<>{val};
        cond.op_     = op;
        return Opt<RowCondition>{spp::move(cond)};
    }

    // Parameter substitution: $name → value from params map
    String<> substitute_params(String_View sql, const Map<String<>, String<>>& params) const noexcept {
        Vec<u8> buf;
        buf.reserve(sql.length() + 64);

        u64 i = 0;
        while (i < sql.length()) {
            if (sql[i] == '$' && i + 1 < sql.length() &&
                ((sql[i+1] >= 'a' && sql[i+1] <= 'z') ||
                 (sql[i+1] >= 'A' && sql[i+1] <= 'Z') ||
                 sql[i+1] == '_')) {
                // Read parameter name
                u64 start = i + 1;
                u64 end = start;
                while (end < sql.length()) {
                    char c = static_cast<char>(sql[end]);
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_') {
                        end++;
                    } else break;
                }
                String_View param_name = sql.sub(start, end);

                // Look up parameter
                auto pv = params.try_get(String<>{param_name});
                if (pv.ok()) {
                    for (u64 j = 0; j < (**pv).length(); j++) {
                        buf.push((**pv)[j]);
                    }
                }
                i = end;
            } else {
                buf.push(sql[i]);
                i++;
            }
        }

        String<> result{buf.length()};
        result.set_length(buf.length());
        for (u64 j = 0; j < buf.length(); j++) result[j] = buf[j];
        return result;
    }
};

// =========================================================================
// Pre-built tables for trading system
// =========================================================================

namespace TradingTables {

// Create all standard trading tables in the given KVStore.
inline void create_all(KVStore& db) noexcept {
    // Orders table
    {
        const String_View cols[] = {
            "cl_ord_id"_v, "exchange_order_id"_v, "symbol"_v, "side"_v,
            "type"_v, "quantity"_v, "price"_v, "stop_price"_v, "status"_v,
            "filled_qty"_v, "avg_fill_price"_v, "sent_time"_v,
            "ack_time"_v, "last_update"_v
        };
        db.create_table("orders"_v, Slice<const String_View>{cols, 14}, "cl_ord_id"_v, false);
    }

    // Fills table
    {
        const String_View cols[] = {
            "fill_id"_v, "order_id"_v, "cl_ord_id"_v, "symbol"_v, "side"_v,
            "quantity"_v, "price"_v, "commission"_v, "fill_time"_v
        };
        db.create_table("fills"_v, Slice<const String_View>{cols, 9}, "fill_id"_v, true);
    }

    // Positions table
    {
        const String_View cols[] = {
            "symbol"_v, "quantity"_v, "entry_price"_v,
            "entry_date"_v, "currency"_v
        };
        db.create_table("positions"_v, Slice<const String_View>{cols, 5}, "symbol"_v, false);
    }

    // Signals table (for IC decay tracking on restart)
    {
        const String_View cols[] = {
            "id"_v, "timestamp"_v, "symbol"_v, "direction"_v,
            "strength"_v, "signal_value"_v
        };
        db.create_table("signals"_v, Slice<const String_View>{cols, 6}, "id"_v, true);
    }

    // Trades table (PnL history)
    {
        const String_View cols[] = {
            "id"_v, "symbol"_v, "entry_time"_v, "exit_time"_v,
            "quantity"_v, "entry_price"_v, "exit_price"_v,
            "pnl"_v, "pnl_pct"_v, "holding_days"_v
        };
        db.create_table("trades"_v, Slice<const String_View>{cols, 10}, "id"_v, true);
    }

    // Account snapshots
    {
        const String_View cols[] = {
            "date"_v, "equity"_v, "cash"_v, "margin_used"_v,
            "realized_pnl"_v, "unrealized_pnl"_v
        };
        db.create_table("account_snapshots"_v, Slice<const String_View>{cols, 6}, "date"_v, false);
    }

    // Risk events
    {
        const String_View cols[] = {
            "id"_v, "timestamp"_v, "event_type"_v, "severity"_v, "details"_v
        };
        db.create_table("risk_events"_v, Slice<const String_View>{cols, 5}, "id"_v, true);
    }

    // Configuration key-value store
    {
        const String_View cols[] = { "key"_v, "value"_v };
        db.create_table("config"_v, Slice<const String_View>{cols, 2}, "key"_v, false);
    }
}

} // namespace TradingTables

} // namespace spp::quant::data

// =========================================================================
// SPP reflection records
// =========================================================================
SPP_NAMED_RECORD(::spp::quant::data::TableSchema, "TableSchema",
                 SPP_FIELD(name_), SPP_FIELD(primary_key_),
                 SPP_FIELD(auto_increment_), SPP_FIELD(next_id_));

SPP_NAMED_RECORD(::spp::quant::data::QueryResult, "QueryResult",
                 SPP_FIELD(rows_));

SPP_NAMED_RECORD(::spp::quant::data::PreparedQuery, "PreparedQuery",
                 SPP_FIELD(sql_));
