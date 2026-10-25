#ifndef GUI_DEBUG_TEXT_HPP
#define GUI_DEBUG_TEXT_HPP

#include <touchgfx/Unicode.hpp>
#include <touchgfx/widgets/TextAreaWithWildcard.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace DebugText
{
// Designer owns widget geometry and typography. Only invalidate changed text.
template <std::size_t N>
void set(touchgfx::TextAreaWithOneWildcard& widget,
         touchgfx::Unicode::UnicodeChar (&buffer)[N], const char* text)
{
    std::size_t i = 0;
    while (i < N - 1 && text[i] &&
           buffer[i] == static_cast<touchgfx::Unicode::UnicodeChar>(text[i]))
    {
        ++i;
    }
    if (!text[i] && buffer[i] == 0) return;
    touchgfx::Unicode::strncpy(buffer, text, N);
    buffer[N - 1] = 0;
    widget.invalidate();
}

// newlib-nano does not require long-long printf support for these diagnostics.
inline void integer(char* out, std::size_t size, std::int64_t value)
{
    char digits[24];
    unsigned index = sizeof(digits) - 1;
    digits[index] = 0;
    std::uint64_t magnitude = value < 0 ?
        static_cast<std::uint64_t>(-(value + 1)) + 1U : static_cast<std::uint64_t>(value);
    do {
        digits[--index] = static_cast<char>('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    if (value < 0) digits[--index] = '-';
    std::snprintf(out, size, "%s", digits + index);
}

inline void seconds(char* out, std::size_t size, std::uint64_t ns)
{
    std::snprintf(out, size, "%lu.%09lu", static_cast<unsigned long>(ns / 1000000000ULL),
                  static_cast<unsigned long>(ns % 1000000000ULL));
}

inline const char* error(unsigned error)
{
    const char* names[] = {"PASS", "BAD PCM", "WEAK", "LOW Q", "AMBIG", "RANGE"};
    return error < sizeof(names) / sizeof(names[0]) ? names[error] : "UNKNOWN";
}
}

#endif
