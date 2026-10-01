#pragma once
// Многопоточный движок: файл режется на чанки фиксированного размера, потоки
// забирают чанки через один atomic-счётчик (work-stealing без очередей и
// мьютексов). Границы чанков выравниваются по '\n', так что каждая строка
// принадлежит ровно одному чанку: строка принадлежит чанку, в котором она НАЧИНАЕТСЯ.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include <string_view>
#include <thread>
#include <vector>

#include "parser.hpp"
#include "stats.hpp"

namespace le {

struct Filter {
    Level min_level = Level::Trace;
    std::string_view component;  // пусто = любой
    std::string_view grep;       // подстрока в сообщении, пусто = любое
    int64_t from_ms = std::numeric_limits<int64_t>::min();
    int64_t to_ms = std::numeric_limits<int64_t>::max();

    bool accepts(const LogRecord& r) const noexcept {
        if (r.level < min_level) return false;
        if (r.ts_ms < from_ms || r.ts_ms > to_ms) return false;
        if (!component.empty() && r.component != component) return false;
        if (!grep.empty() && r.message.find(grep) == std::string_view::npos) return false;
        return true;
    }
};

struct Options {
    unsigned threads = 0;                 // 0 = hardware_concurrency
    size_t chunk_bytes = size_t{4} << 20; // 4 MiB
    Filter filter;
};

namespace detail {

inline void process_range(std::string_view data, size_t begin, size_t end, const Filter& flt, Stats& st) {
    st.bytes += end - begin;
    const char* base = data.data();
    size_t pos = begin;
    while (pos < end) {
        const void* nl = std::memchr(base + pos, '\n', end - pos);
        const size_t line_end = nl ? static_cast<size_t>(static_cast<const char*>(nl) - base) : end;
        size_t len = line_end - pos;
        if (len && base[pos + len - 1] == '\r') --len;
        const std::string_view line{base + pos, len};
        pos = line_end + 1;
        if (line.empty()) continue;

        ++st.lines;
        LogRecord r;
        if (!parse_line(line, r)) {
            ++st.malformed;
            continue;
        }
        if (!flt.accepts(r)) continue;

        ++st.matched;
        ++st.by_level[static_cast<size_t>(r.level)];
        st.ts_min = std::min(st.ts_min, r.ts_ms);
        st.ts_max = std::max(st.ts_max, r.ts_ms);

        const bool is_error = r.level >= Level::Error;
        ComponentStat& cs = st.components[r.component];
        ++cs.count;
        if (is_error) {
            ++cs.errors;
            ++st.errors[error_key(r.message)];
        }
        if (r.latency_ms >= 0) {
            const auto ms = static_cast<uint32_t>(r.latency_ms);
            ++cs.lat_count;
            cs.lat_sum += ms;
            cs.lat_max = std::max(cs.lat_max, ms);
            st.add_latency(ms);
        }
    }
}

}  // namespace detail

// data должен жить до тех пор, пока используется результат (ключи — view в data).
inline Stats run(std::string_view data, const Options& opt) {
    const size_t chunk = std::max<size_t>(opt.chunk_bytes, 64 * 1024);
    const size_t nchunks = (data.size() + chunk - 1) / chunk;
    unsigned nthreads = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
    nthreads = static_cast<unsigned>(std::min<size_t>(nthreads, std::max<size_t>(1, nchunks)));

    // Начало первой строки, которая начинается в позиции >= off.
    auto align = [&](size_t off) -> size_t {
        if (off == 0) return 0;
        if (off >= data.size()) return data.size();
        const void* nl = std::memchr(data.data() + off - 1, '\n', data.size() - (off - 1));
        return nl ? static_cast<size_t>(static_cast<const char*>(nl) - data.data()) + 1 : data.size();
    };

    std::vector<Stats> parts(nthreads);
    std::atomic<size_t> next{0};
    auto worker = [&](Stats& st) {
        for (;;) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= nchunks) break;
            const size_t b = align(i * chunk);
            const size_t e = (i + 1 == nchunks) ? data.size() : align((i + 1) * chunk);
            detail::process_range(data, b, e, opt.filter, st);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(nthreads - 1);
    for (unsigned t = 1; t < nthreads; ++t) pool.emplace_back(worker, std::ref(parts[t]));
    worker(parts[0]);  // главный поток тоже работает
    for (auto& th : pool) th.join();

    for (unsigned t = 1; t < nthreads; ++t) parts[0].merge(parts[t]);
    return std::move(parts[0]);
}

}  // namespace le
