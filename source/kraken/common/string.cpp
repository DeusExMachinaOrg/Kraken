#include <common/string.hpp>

#include <cstdio>
#include <cstdlib>

namespace kraken {
    String::String() {
        size = 0;
        data[0] = '\0';
    };

    String::String(String&& other) {
        std::memcpy(this, &other, sizeof(String));
        other.size = 0;
        other.data[0] = '\0';
    };

    String::String(const String& other) {
        size = other.size;
        if (size < SSO) {
            std::memcpy(data, other.data, size + 1);
        } else {
            heap = (char*)std::malloc(size + 1);
            std::memcpy(heap, other.heap, size + 1);
        }
    };

    String::String(const char* other) {
        size = std::strlen(other);
        if (size < SSO) {
            std::memcpy(data, other, size + 1);
        } else {
            heap = (char*)std::malloc(size + 1);
            std::memcpy(heap, other, size + 1);
        }
    };

    String::String(const char* fmt, va_list args) {
        va_list copy;
        va_copy(copy, args);
        int len = std::vsnprintf(nullptr, 0, fmt, copy);
        va_end(copy);

        size = (len > 0) ? (size_t)len : 0;
        if (size < SSO) {
            std::vsnprintf(data, SSO, fmt, args);
        } else {
            heap = (char*)std::malloc(size + 1);
            std::vsnprintf(heap, size + 1, fmt, args);
        }
    };

    String String::Format(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        String result(fmt, args);
        va_end(args);
        return result;
    };

    String::~String() {
        if (size >= SSO)
            std::free(heap);
    };

    size_t String::Hash() const {
        if (hash == 0) {
            const char* str = (size < SSO) ? data : heap;
            hash = 0x811C9DC5u;
            for (size_t i = 0; i < size; i++) {
                hash ^= (uint8_t)str[i];
                hash *= 0x01000193u;
            }
        }
        return hash;
    };

    String& String::operator = (String&& other) {
        if (this != &other) {
            if (size >= SSO) std::free(heap);
            std::memcpy(this, &other, sizeof(String));
            other.size = 0;
            other.data[0] = '\0';
        }
        return *this;
    };

    String& String::operator = (const String& other) {
        if (this != &other) {
            if (size >= SSO) std::free(heap);
            size = other.size;
            if (size < SSO) {
                std::memcpy(data, other.data, size + 1);
            } else {
                heap = (char*)std::malloc(size + 1);
                std::memcpy(heap, other.heap, size + 1);
            }
        }
        return *this;
    };

    String& String::operator = (const char* other) {
        if (size >= SSO) std::free(heap);
        size = std::strlen(other);
        if (size < SSO) {
            std::memcpy(data, other, size + 1);
        } else {
            heap = (char*)std::malloc(size + 1);
            std::memcpy(heap, other, size + 1);
        }
        return *this;
    };

    bool String::operator == (const char* other) const {
        const char* self = (size < SSO) ? data : heap;
        if (self == other) return true;
        return std::strcmp(self, other) == 0;
    };

    bool String::operator == (const String& other) const {
        if (this == &other) return true;
        if (size != other.size) return false;
        const char* a = (size < SSO) ? data : heap;
        const char* b = (other.size < SSO) ? other.data : other.heap;
        return std::memcmp(a, b, size) == 0;
    };

    bool String::operator != (const char* other) const {
        return !(*this == other);
    };

    bool String::operator != (const String& other) const {
        return !(*this == other);
    };

    String::operator char*() {
        return (size < SSO) ? data : heap;
    };

    String::operator const char*() const {
        return (size < SSO) ? data : heap;
    };
};
