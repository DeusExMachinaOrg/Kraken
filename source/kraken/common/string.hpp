#ifndef KRAKEN_COMMON_STRING_HPP
#define KRAKEN_COMMON_STRING_HPP

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdarg>

namespace kraken {
    union String {
    private:
        static constexpr size_t SSO = 24;
    private:
        struct { size_t size; mutable size_t hash; char  data[SSO]; };
        struct { size_t size; mutable size_t hash; char* heap;      };
    public:
        String();
        String(String&& other);
        String(const String& other);
        String(const char* other);
        String(const char* fmt, va_list args);
       ~String();
    public:
        static String Format(const char* fmt, ...);
    public:
        size_t Hash() const;
    public:
        String& operator =  (String&& other);
        String& operator =  (const String& other);
        String& operator =  (const char* other);
        bool    operator == (const char* other) const;
        bool    operator == (const String& other) const;
        bool    operator != (const char* other) const;
        bool    operator != (const String& other) const;
    public:
        operator       char*();
        operator const char*() const;
    };
};

namespace std {
    template<> struct hash<kraken::String> {
        size_t operator()(const kraken::String& ref) const { return ref.Hash(); }
    };
};

#endif