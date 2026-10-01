#pragma once
// Агрегаты. Каждый поток владеет своим Stats (без блокировок и атомиков на
// горячем пути); в конце частичные результаты сливаются через merge().
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "flat_map.hpp"
#include "parser.hpp"

namespace le {

inline constexpr uint32_t kMaxLatencyMs = 10'000;  // точные перцентили до 10 с, выше — одна корзина

struct ComponentStat {
    uint64_t count = 0;
    uint64_t errors = 0;  // ERROR + FATAL
    uint64_t lat_count = 0;
    uint64_t lat_sum = 0;
    uint32_t lat_max = 0;

    void merge(const ComponentStat& o) noexcept {
        count += o.count;
        errors += o.errors;
        lat_count += o.lat_count;
        lat_sum += o.lat_sum;
        lat_max = std::max(lat_max, o.lat_max);
    }
};

// alignas(64): соседние Stats в массиве не делят кэш-линию (нет false sharing).
struct alignas(64) Stats {
    uint64_t bytes = 0;
    uint64_t lines = 0;      // непустые строки
    uint64_t malformed = 0;  // не распарсились
    uint64_t matched = 0;    // прошли фильтр
    std::array<uint64_t, kLevelCount> by_level{};
    int64_t ts_min = std::numeric_limits<int64_t>::max();
    int64_t ts_max = std::numeric_limits<int64_t>::min();
    std::vector<uint32_t> lat_hist = std::vector<uint32_t>(kMaxLatencyMs + 1);
    FlatMap<ComponentStat> components{256};
    FlatMap<uint64_t> errors{256};

    void add_latency(uint32_t ms) { ++lat_hist[std::min(ms, kMaxLatencyMs)]; }

    void merge(const Stats& o) {
        bytes += o.bytes;
        lines += o.lines;
        malformed += o.malformed;
        matched += o.matched;
        for (int i = 0; i < kLevelCount; ++i) by_level[i] += o.by_level[i];
        ts_min = std::min(ts_min, o.ts_min);
        ts_max = std::max(ts_max, o.ts_max);
        for (size_t i = 0; i < lat_hist.size(); ++i) lat_hist[i] += o.lat_hist[i];
        o.components.for_each([&](std::string_view k, const ComponentStat& v) { components[k].merge(v); });
        o.errors.for_each([&](std::string_view k, uint64_t v) { errors[k] += v; });
    }

    uint64_t latency_samples() const noexcept {
        uint64_t n = 0;
        for (uint32_t c : lat_hist) n += c;
        return n;
    }

    // p в (0,1]; результат в мс (kMaxLatencyMs означает «>= 10 с»).
    uint32_t latency_percentile(double p) const noexcept {
        const uint64_t total = latency_samples();
        if (!total) return 0;
        const uint64_t target = std::max<uint64_t>(1, static_cast<uint64_t>(p * static_cast<double>(total) + 0.999999));
        uint64_t acc = 0;
        for (size_t i = 0; i < lat_hist.size(); ++i) {
            acc += lat_hist[i];
            if (acc >= target) return static_cast<uint32_t>(i);
        }
        return kMaxLatencyMs;
    }
};

}  // namespace le
