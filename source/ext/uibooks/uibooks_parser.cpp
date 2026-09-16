#include "ext/uibooks/uibooks_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

namespace kraken::ext::uibooks {
    namespace {
        bool IsDirective(const std::string& value, const char* directive) {
            const size_t length = std::strlen(directive);
            if (value.size() != length)
                return false;
            for (size_t i = 0; i < length; ++i) {
                if (std::tolower((unsigned char) value[i])
                    != std::tolower((unsigned char) directive[i]))
                    return false;
            }
            return true;
        }

        bool ParseImageDimension(const std::string& value, ImageDimension* dimension,
                                 bool allowFill) {
            if (!dimension || value.empty())
                return false;

            std::string normalized;
            normalized.reserve(value.size());
            for (const char ch : value)
                normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            if (allowFill
                && (normalized == "fill" || normalized == "full" || normalized == "100%")) {
                dimension->specified = true;
                dimension->fill = true;
                dimension->pixels = 0.0f;
                return true;
            }
            char* end = nullptr;
            const float pixels = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || !std::isfinite(pixels) || pixels <= 0.0f)
                return false;
            dimension->specified = true;
            dimension->fill = false;
            dimension->pixels = pixels;
            return true;
        }

        bool ParseImageOptions(const std::string& options, ImageDimension* width,
                               ImageDimension* height) {
            if (!width || !height || options.size() < 2
                || options.front() != '{' || options.back() != '}')
                return false;

            size_t pos = 1;
            const size_t end = options.size() - 1;
            bool widthSeen = false;
            bool heightSeen = false;
            while (pos < end) {
                while (pos < end && (std::isspace(static_cast<unsigned char>(options[pos]))
                                     || options[pos] == ','))
                    ++pos;
                if (pos >= end)
                    break;

                const size_t keyStart = pos;
                while (pos < end && options[pos] != '='
                       && !std::isspace(static_cast<unsigned char>(options[pos])))
                    ++pos;
                if (pos == keyStart || pos >= end || options[pos] != '=')
                    return false;
                const std::string key = options.substr(keyStart, pos - keyStart);
                ++pos;
                const size_t valueStart = pos;
                while (pos < end && !std::isspace(static_cast<unsigned char>(options[pos]))
                       && options[pos] != ',')
                    ++pos;
                if (pos == valueStart)
                    return false;
                const std::string value = options.substr(valueStart, pos - valueStart);

                ImageDimension* target = nullptr;
                bool* seen = nullptr;
                if (key.size() == 5
                    && std::equal(key.begin(), key.end(), "width",
                                  [](char lhs, char rhs) {
                                      return std::tolower(static_cast<unsigned char>(lhs))
                                          == rhs;
                                  })) {
                    target = width;
                    seen = &widthSeen;
                }
                else if (key.size() == 6
                    && std::equal(key.begin(), key.end(), "height",
                                  [](char lhs, char rhs) {
                                      return std::tolower(static_cast<unsigned char>(lhs))
                                          == rhs;
                                  })) {
                    target = height;
                    seen = &heightSeen;
                }
                else {
                    return false;
                }
                if (*seen || !ParseImageDimension(value, target, target == width))
                    return false;
                *seen = true;
            }
            return widthSeen || heightSeen;
        }

        bool IsGameResourceId(const std::string& reference) {
            return !reference.empty()
                && reference.find_first_of("\\/:.") == std::string::npos;
        }

        bool ParseImageMarker(const std::string& line, std::string* alt, std::string* path,
                              ImageSource* source, ImageDimension* width,
                              ImageDimension* height) {
            if (!alt || !path || !source || !width || !height || line.size() < 6
                || line[0] != '!' || line[1] != '[')
                return false;

            const size_t separator = line.find("](", 2);
            if (separator == std::string::npos || separator == 2)
                return false;
            const size_t close = line.rfind(')');
            if (close == std::string::npos || close <= separator + 2)
                return false;
            size_t suffixStart = close + 1;
            while (suffixStart < line.size()
                   && std::isspace(static_cast<unsigned char>(line[suffixStart])))
                ++suffixStart;
            if (suffixStart < line.size()
                && !ParseImageOptions(line.substr(suffixStart), width, height))
                return false;

            const std::string imagePath = line.substr(separator + 2, close - separator - 2);
            if (imagePath.empty() || imagePath.find("..") != std::string::npos
                || imagePath.find(':') != std::string::npos
                || imagePath[0] == '/' || imagePath[0] == '\\')
                return false;

            *alt = line.substr(2, separator - 2);
            *path = imagePath;
            *source = IsGameResourceId(imagePath) ? ImageSource::GameResource
                                                  : ImageSource::Path;
            return true;
        }

        bool IsHex(char value) {
            return std::isxdigit(static_cast<unsigned char>(value)) != 0;
        }

        std::optional<uint32_t> ParseColorMarker(const std::string& text, size_t offset) {
            if (offset + 9 > text.size() || text[offset] != '@')
                return std::nullopt;
            uint32_t color = 0;
            for (size_t i = 1; i <= 8; ++i) {
                if (!IsHex(text[offset + i]))
                    return std::nullopt;
                color <<= 4;
                const char digit = text[offset + i];
                color |= static_cast<uint32_t>(digit >= '0' && digit <= '9'
                    ? digit - '0' : std::tolower(static_cast<unsigned char>(digit)) - 'a' + 10);
            }
            return color;
        }

        bool HasClosingStyleMarker(const std::string& text, size_t offset, char marker) {
            for (size_t i = offset + 1; i < text.size(); ++i) {
                if (text[i] == '%' && i + 1 < text.size()) {
                    ++i;
                    continue;
                }
                if (text[i] == marker)
                    return true;
            }
            return false;
        }
    }

    ParseResult ParseBookText(const char* src) {
        ParseResult res;
        if (!src)
            return res;

        std::vector<std::string> raw;
        std::string cur;
        const size_t n = std::strlen(src);
        for (size_t i = 0; i < n; ++i) {
            const char c = src[i];
            if (c == '%' && i + 1 < n) {
                cur += c;
                cur += src[++i];
                continue;
            }
            // XML attributes may contain physical line breaks. They are only
            // formatting of the source value, not an additional book line;
            // the pipe remains the explicit logical-line separator.
            if (c == '\r' || c == '\n') {
                if (!cur.empty() && cur.back() != ' ')
                    cur += ' ';
                continue;
            }
            if (c == '|') {
                raw.push_back(cur);
                cur.clear();
                continue;
            }
            cur += c;
        }
        raw.push_back(cur);

        int32_t currentAlign = res.align;
        for (size_t rawIndex = 0; rawIndex < raw.size(); ++rawIndex) {
            const std::string& line = raw[rawIndex];
            const auto isSpace = [](char value) {
                return std::isspace(static_cast<unsigned char>(value)) != 0;
            };
            size_t a = 0;
            while (a < line.size() && isSpace(line[a]))
                ++a;
            size_t b = line.size();
            while (b > a && isSpace(line[b - 1]))
                --b;
            const std::string core = line.substr(a, b - a);

            if (IsDirective(core, "#page")) {
                if (!res.lines.empty())
                    res.lineBreak.back() = true;
                continue;
            }

            if (IsDirective(core, "#left")
                || IsDirective(core, "#center")
                || IsDirective(core, "#right")) {
                currentAlign = IsDirective(core, "#left") ? hta::m3d::TF_LEFT
                            : IsDirective(core, "#center") ? hta::m3d::TF_CENTER
                            : hta::m3d::TF_RIGHT;
                if (res.lines.empty())
                    res.align = currentAlign;
                continue;
            }

            if (core.empty() && rawIndex + 1 == raw.size())
                continue;

            if (core.empty()) {
                ParsedLine pl;
                pl.align = currentAlign;
                res.lines.push_back(std::move(pl));
                res.lineBreak.push_back(false);
                continue;
            }

            std::string imageAlt;
            std::string imagePath;
            ImageSource imageSource = ImageSource::Path;
            ImageDimension imageWidth;
            ImageDimension imageHeight;
            if (ParseImageMarker(core, &imageAlt, &imagePath, &imageSource,
                                 &imageWidth, &imageHeight)) {
                ParsedLine pl;
                pl.align = currentAlign;
                pl.isImage = true;
                pl.imageAlt = std::move(imageAlt);
                pl.imageReference = std::move(imagePath);
                pl.imageSource = imageSource;
                pl.imageWidth = imageWidth;
                pl.imageHeight = imageHeight;
                res.lines.push_back(std::move(pl));
                res.lineBreak.push_back(false);
                ++res.imageCount;
                continue;
            }

            ParsedLine pl;
            pl.align = currentAlign;
            bool bold = false;
            bool ital = false;
            uint32_t style = 0;
            uint32_t color = 0;
            bool hasColor = false;
            std::string seg;
            auto flush = [&]() {
                if (seg.empty())
                    return;
                if (style) {
                    ++res.styledSegs;
                    res.styleMask |= style;
                }
                if (hasColor)
                    ++res.coloredSegs;
                pl.segs.push_back(Seg{ std::move(seg), style, color, hasColor });
                seg.clear();
            };
            for (size_t i = 0; i < core.size(); ++i) {
                const char ch = core[i];
                if (ch == '%' && i + 1 < core.size()
                    && (core[i + 1] == '%' || core[i + 1] == '*' || core[i + 1] == '_'
                        || core[i + 1] == '|')) {
                    seg += core[++i];
                    continue;
                }
                if (const std::optional<uint32_t> marker = ParseColorMarker(core, i)) {
                    flush();
                    color = *marker;
                    hasColor = true;
                    if (std::find(res.colors.begin(), res.colors.end(), color) == res.colors.end())
                        res.colors.push_back(color);
                    i += 8;
                    continue;
                }
                if (ch == '*' || ch == '_') {
                    bool& active = (ch == '*') ? bold : ital;
                    if (!active && !HasClosingStyleMarker(core, i, ch)) {
                        seg += ch;
                        continue;
                    }
                    flush();
                    active = !active;
                    style = (bold ? hta::m3d::FONT_BOLD : 0)
                          | (ital ? hta::m3d::FONT_ITALIC : 0);
                    continue;
                }
                seg += ch;
            }
            flush();
            if (pl.segs.empty())
                continue;
            res.lines.push_back(std::move(pl));
            res.lineBreak.push_back(false);
        }

        for (size_t i = 0; i < res.lines.size(); ++i) {
            if (i > 0)
                res.clean += "|";
            const ParsedLine& line = res.lines[i];
            if (line.isImage) {
                res.clean += line.imageAlt.empty() ? "[image]" : line.imageAlt;
                continue;
            }
            for (const Seg& segment : line.segs)
                res.clean += segment.text;
        }
        return res;
    }

}
