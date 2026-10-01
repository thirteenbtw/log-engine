#pragma once
// Zero-copy парсер строки лога. Формат:
//
//   2026-10-01T12:34:56.789Z INFO  auth-service  login ok user=42 took=12ms
//   └──────── 24 байта ────┘ level component     message (до конца строки)
//
// Все поля результата — string_view в исходный буфер, аллокаций нет.
#include <cstdint>
#include <cstring>
#include <string_view>

#include "time_util.hpp"

namespace le {

enum class Level : uint8_t { Trace, Debug, Info, Warn, Error, Fatal, Unknown };
inline constexpr int kLevelCount = 6;
inline constexpr const char* kLevelNames[kLevelCount] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};

struct LogRecord {
    int64_t ts_ms = 0;           // unix epoch, мс
    Level level = Level::Unknown;
    std::string_view component;
    std::string_view message;
    int32_t latency_ms = -1;     // из "took=NNNms", -1 если нет
};

namespace detail {

inline bool two_digits(const char* p, unsigned& v) noexcept {
    const unsigned a = static_cast<unsigned>(p[0] - '0');
    const unsigned b = static_cast<unsigned>(p[1] - '0');
    if (a > 9 || b > 9) return false;
    v = a * 10 + b;
    return true;
}

// "YYYY-MM-DDTHH:MM:SS.mmmZ" (ровно 24 символа)
inline bool parse_timestamp(const char* p, int64_t& out_ms) noexcept {
    if (p[4] != '-' || p[7] != '-' || p[10] != 'T' || p[13] != ':' || p[16] != ':' ||
        p[19] != '.' || p[23] != 'Z')
        return false;
    unsigned y1, y2, mo, d, h, mi, s;
    if (!two_digits(p, y1) || !two_digits(p + 2, y2) || !two_digits(p + 5, mo) ||
        !two_digits(p + 8, d) || !two_digits(p + 11, h) || !two_digits(p + 14, mi) ||
        !two_digits(p + 17, s))
        return false;
    const unsigned m0 = static_cast<unsigned>(p[20] - '0');
    const unsigned m1 = static_cast<unsigned>(p[21] - '0');
    const unsigned m2 = static_cast<unsigned>(p[22] - '0');
    if (m0 > 9 || m1 > 9 || m2 > 9) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return false;
    const int64_t days = days_from_civil(y1 * 100 + y2, mo, d);
    out_ms = ((days * 24 + h) * 60 + mi) * 60'000 + static_cast<int64_t>(s) * 1000 +
             (m0 * 100 + m1 * 10 + m2);
    return true;
}

inline Level parse_level(std::string_view t) noexcept {
    switch (t.size()) {
        case 4:
            if (t == "INFO") return Level::Info;
            if (t == "WARN") return Level::Warn;
            break;
        case 5:
            if (t == "TRACE") return Level::Trace;
            if (t == "DEBUG") return Level::Debug;
            if (t == "ERROR") return Level::Error;
            if (t == "FATAL") return Level::Fatal;
            break;
    }
    return Level::Unknown;
}

}  // namespace detail

// Возвращает false для некорректной строки. line — без '\n'.
inline bool parse_line(std::string_view line, LogRecord& r) noexcept {
    if (line.size() < 26 || line[24] != ' ') return false;
    const char* p = line.data();
    if (!detail::parse_timestamp(p, r.ts_ms)) return false;

    const char* cur = p + 25;
    const char* const end = p + line.size();
    auto skip_spaces = [&] {
        while (cur < end && *cur == ' ') ++cur;
    };
    auto token = [&]() -> std::string_view {
        const char* s = cur;
        while (cur < end && *cur != ' ') ++cur;
        return {s, static_cast<size_t>(cur - s)};
    };

    skip_spaces();
    const std::string_view lv = token();
    skip_spaces();
    r.component = token();
    skip_spaces();
    r.level = detail::parse_level(lv);
    if (r.level == Level::Unknown || r.component.empty()) return false;
    r.message = {cur, static_cast<size_t>(end - cur)};

    // took=NNNms
    r.latency_ms = -1;
    const size_t pos = r.message.find("took=");
    if (pos != std::string_view::npos) {
        const char* q = r.message.data() + pos + 5;
        const char* const e = r.message.data() + r.message.size();
        uint32_t v = 0;
        int nd = 0;
        while (q < e && nd < 9 && static_cast<unsigned>(*q - '0') <= 9) {
            v = v * 10 + static_cast<uint32_t>(*q - '0');
            ++q;
            ++nd;
        }
        if (nd) r.latency_ms = static_cast<int32_t>(v);
    }
    return true;
}

// Ключ группировки ошибок: сообщение до первой цифры или '=' (без хвостовых
// пробелов), чтобы "db timeout after 5000ms" и "... 4200ms" слились в один.
inline std::string_view error_key(std::string_view m) noexcept {
    const size_t lim = m.size() < 96 ? m.size() : 96;
    size_t n = 0;
    while (n < lim && static_cast<unsigned>(m[n] - '0') > 9 && m[n] != '=') ++n;
    while (n && m[n - 1] == ' ') --n;
    return m.substr(0, n);
}

}  // namespace le
