// Генератор тестовых логов: gen_logs <out> [lines=1000000] [seed=1]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "le/time_util.hpp"

namespace {
struct Rng {
    uint64_t s;
    uint64_t next() {  // xorshift64*
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    unsigned below(unsigned n) { return static_cast<unsigned>(next() % n); }
};

const char* kComponents[] = {"auth-service", "api-gateway", "payments", "orders", "inventory", "search",
                             "notifier", "db-proxy", "cache", "scheduler", "billing", "frontend"};
const char* kInfo[] = {"request handled", "user login ok", "cache hit", "order created", "job finished", "session refreshed"};
const char* kWarn[] = {"slow query", "retrying request", "cache miss storm", "queue depth high"};
const char* kErr[] = {"db timeout after", "connection refused to upstream", "payment declined code=", "null pointer in handler"};
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: gen_logs <out> [lines] [seed]\n"); return 1; }
    const unsigned long long lines = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1'000'000ull;
    Rng rng{argc > 3 ? std::strtoull(argv[3], nullptr, 10) | 1 : 1};

    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f) { std::perror("fopen"); return 1; }
    static char io[1 << 20];
    std::setvbuf(f, io, _IOFBF, sizeof io);

    int64_t ts = le::days_from_civil(2026, 10, 1) * 86'400'000ll;
    char line[512];
    for (unsigned long long i = 0; i < lines; ++i) {
        ts += rng.below(6);
        const le::Civil c = le::civil_from_days(ts / 86'400'000);
        const int64_t rem = ts % 86'400'000;
        const unsigned r = rng.below(1000);
        const char* lvl; const char* base;
        if (r < 700) { lvl = "INFO "; base = kInfo[rng.below(6)]; }
        else if (r < 850) { lvl = "DEBUG"; base = kInfo[rng.below(6)]; }
        else if (r < 940) { lvl = "WARN "; base = kWarn[rng.below(4)]; }
        else if (r < 995) { lvl = "ERROR"; base = kErr[rng.below(4)]; }
        else if (r < 998) { lvl = "FATAL"; base = kErr[rng.below(4)]; }
        else { lvl = "TRACE"; base = kInfo[rng.below(6)]; }

        const int lat = static_cast<int>(rng.below(50) + (rng.below(100) == 0 ? rng.below(3000) : 0));
        // ~2% намеренно битых строк
        if (rng.below(50) == 0) {
            std::fputs("garbage line without proper format\n", f);
            continue;
        }
        const int n = std::snprintf(line, sizeof line,
            "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ %s %-12s %s %u took=%dms\n",
            static_cast<long long>(c.y), c.m, c.d, static_cast<int>(rem / 3'600'000),
            static_cast<int>(rem / 60'000 % 60), static_cast<int>(rem / 1000 % 60), static_cast<int>(rem % 1000),
            lvl, kComponents[rng.below(12)], base, rng.below(100000), lat);
        std::fwrite(line, 1, static_cast<size_t>(n), f);
    }
    std::fclose(f);
    std::printf("wrote %llu lines to %s\n", lines, argv[1]);
    return 0;
}
