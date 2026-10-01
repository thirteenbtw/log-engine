#pragma once
// Открытая адресация, линейное пробирование, ключ — string_view (без копирования:
// ключи указывают прямо в mmap-буфер). Один непрерывный массив слотов, рост
// удвоением — аллокации только при росте таблицы, а не на каждую запись.
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace le {

inline uint64_t hash_bytes(const char* p, size_t n) noexcept {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (n * 0xff51afd7ed558ccdull);
    while (n >= 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        h = (h ^ v) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
        p += 8;
        n -= 8;
    }
    uint64_t v = 0;
    if (n) std::memcpy(&v, p, n);
    h = (h ^ v) * 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 29;
    return h ? h : 1;  // 0 зарезервирован под «пустой слот»
}

template <class V>
class FlatMap {
public:
    explicit FlatMap(size_t initial_capacity = 1024)
        : slots_(std::bit_ceil(initial_capacity < 16 ? size_t{16} : initial_capacity)),
          mask_(slots_.size() - 1) {}

    // Найти или вставить значение по умолчанию.
    V& operator[](std::string_view key) {
        if ((size_ + 1) * 10 > slots_.size() * 7) grow();
        const uint64_t h = hash_bytes(key.data(), key.size());
        for (size_t i = h & mask_;; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.hash == 0) {
                s.hash = h;
                s.key = key;
                ++size_;
                return s.val;
            }
            if (s.hash == h && s.key == key) return s.val;
        }
    }

    template <class F>
    void for_each(F&& f) const {
        for (const Slot& s : slots_)
            if (s.hash) f(s.key, s.val);
    }

    size_t size() const noexcept { return size_; }

private:
    struct Slot {
        uint64_t hash = 0;
        std::string_view key;
        V val{};
    };

    void grow() {
        std::vector<Slot> old(slots_.size() * 2);
        old.swap(slots_);
        mask_ = slots_.size() - 1;
        for (Slot& s : old) {
            if (!s.hash) continue;
            size_t i = s.hash & mask_;
            while (slots_[i].hash) i = (i + 1) & mask_;
            slots_[i] = std::move(s);
        }
    }

    std::vector<Slot> slots_;
    size_t mask_;
    size_t size_ = 0;
};

}  // namespace le
