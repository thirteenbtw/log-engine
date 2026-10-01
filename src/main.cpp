#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <utility>

#include "le/engine.hpp"
#include "le/mapped_file.hpp"

using namespace le;

static void usage() {
    std::puts(
        "usage: logengine <file> [options]\n"
        "  -t, --threads N       worker threads (default: all cores)\n"
        "      --chunk-mb N      chunk size in MiB (default: 4)\n"
        "      --level L         min level: TRACE|DEBUG|INFO|WARN|ERROR|FATAL\n"
        "      --component C     only this component\n"
        "      --grep S          only messages containing S\n"
        "      --from TS         only records >= TS  (YYYY-MM-DDTHH:MM:SS.mmmZ)\n"
        "      --to TS           only records <= TS\n"
        "      --top N           rows in top lists (default: 10)");
}

static bool parse_ts_arg(const char* s, int64_t& out) {
    return std::strlen(s) == 24 && detail::parse_timestamp(s, out);
}

static void format_ts(int64_t ms, char* buf, size_t n) {
    int64_t days = ms / 86'400'000;
    int64_t rem = ms % 86'400'000;
    if (rem < 0) { rem += 86'400'000; --days; }
    const Civil c = civil_from_days(days);
    std::snprintf(buf, n, "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ", static_cast<long long>(c.y), c.m, c.d,
                  static_cast<int>(rem / 3'600'000), static_cast<int>(rem / 60'000 % 60),
                  static_cast<int>(rem / 1000 % 60), static_cast<int>(rem % 1000));
}

int main(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help")) {
        usage();
        return argc < 2 ? 1 : 0;
    }

    Options opt;
    size_t top_n = 10;
    const char* path = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-t" || a == "--threads") opt.threads = static_cast<unsigned>(std::strtoul(need(), nullptr, 10));
        else if (a == "--chunk-mb") opt.chunk_bytes = static_cast<size_t>(std::strtoull(need(), nullptr, 10)) << 20;
        else if (a == "--top") top_n = static_cast<size_t>(std::strtoull(need(), nullptr, 10));
        else if (a == "--component") opt.filter.component = need();
        else if (a == "--grep") opt.filter.grep = need();
        else if (a == "--level") {
            const Level l = detail::parse_level(need());
            if (l == Level::Unknown) { std::fprintf(stderr, "bad --level\n"); return 2; }
            opt.filter.min_level = l;
        } else if (a == "--from") {
            if (!parse_ts_arg(need(), opt.filter.from_ms)) { std::fprintf(stderr, "bad --from\n"); return 2; }
        } else if (a == "--to") {
            if (!parse_ts_arg(need(), opt.filter.to_ms)) { std::fprintf(stderr, "bad --to\n"); return 2; }
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        } else path = argv[i];
    }
    if (!path) { usage(); return 1; }

    try {
        const MappedFile file = MappedFile::open(path);
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        const Stats st = run(file.view(), opt);
        const double sec = std::chrono::duration<double>(clock::now() - t0).count();

        std::printf("== Summary ==\n");
        std::printf("file size    : %.1f MiB\n", static_cast<double>(file.size()) / (1 << 20));
        std::printf("lines        : %llu (malformed %llu, matched %llu)\n", (unsigned long long)st.lines,
                    (unsigned long long)st.malformed, (unsigned long long)st.matched);
        if (st.matched) {
            char a[32], b[32];
            format_ts(st.ts_min, a, sizeof a);
            format_ts(st.ts_max, b, sizeof b);
            std::printf("time range   : %s .. %s\n", a, b);
        }

        std::printf("\n== Levels ==\n");
        for (int i = 0; i < kLevelCount; ++i)
            if (st.by_level[i])
                std::printf("%-6s %12llu\n", kLevelNames[i], (unsigned long long)st.by_level[i]);

        const uint64_t ls = st.latency_samples();
        if (ls) {
            std::printf("\n== Latency (took=Nms), %llu samples ==\n", (unsigned long long)ls);
            std::printf("p50 %u  p90 %u  p95 %u  p99 %u  p99.9 %u ms\n", st.latency_percentile(0.50),
                        st.latency_percentile(0.90), st.latency_percentile(0.95), st.latency_percentile(0.99),
                        st.latency_percentile(0.999));
        }

        using CompRow = std::pair<std::string_view, ComponentStat>;
        std::vector<CompRow> comps;
        comps.reserve(st.components.size());
        st.components.for_each([&](std::string_view k, const ComponentStat& v) { comps.emplace_back(k, v); });
        std::sort(comps.begin(), comps.end(), [](const CompRow& x, const CompRow& y) { return x.second.count > y.second.count; });

        std::printf("\n== Top components ==\n%-24s %12s %10s %10s %8s\n", "component", "count", "errors", "avg ms", "max ms");
        for (size_t i = 0; i < comps.size() && i < top_n; ++i) {
            const auto& [name, c] = comps[i];
            const double avg = c.lat_count ? static_cast<double>(c.lat_sum) / static_cast<double>(c.lat_count) : 0.0;
            std::printf("%-24.*s %12llu %10llu %10.1f %8u\n", (int)name.size(), name.data(),
                        (unsigned long long)c.count, (unsigned long long)c.errors, avg, c.lat_max);
        }

        using ErrRow = std::pair<std::string_view, uint64_t>;
        std::vector<ErrRow> errs;
        errs.reserve(st.errors.size());
        st.errors.for_each([&](std::string_view k, uint64_t v) { errs.emplace_back(k, v); });
        std::sort(errs.begin(), errs.end(), [](const ErrRow& x, const ErrRow& y) { return x.second > y.second; });
        if (!errs.empty()) {
            std::printf("\n== Top errors ==\n");
            for (size_t i = 0; i < errs.size() && i < top_n; ++i)
                std::printf("%10llu  %.*s\n", (unsigned long long)errs[i].second, (int)errs[i].first.size(),
                            errs[i].first.data());
        }

        std::printf("\n== Performance ==\n");
        std::printf("threads      : %u\n", opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency()));
        std::printf("elapsed      : %.3f s\n", sec);
        std::printf("throughput   : %.1f MiB/s, %.2f M lines/s\n", static_cast<double>(file.size()) / (1 << 20) / sec,
                    static_cast<double>(st.lines) / 1e6 / sec);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
