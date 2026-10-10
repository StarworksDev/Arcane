#pragma once

// Arcane/Util/CharConv.hpp -- std::from_chars, everywhere (macOS port,
// 2026-10-07).
//
// Apple's libc++ (Xcode 16) has the integer overloads of std::from_chars but
// not the floating-point ones (libc++ gained those in LLVM 20; the library
// advertises the gap by leaving __cpp_lib_to_chars undefined). Arcane::FromChars
// is std::from_chars where the standard library is complete -- MSVC and
// libstdc++ take that branch, so Windows and Linux are byte-for-byte the
// standard call -- and on a library without it parses floating point with
// strtod_l/strtof_l in the "C" locale under std::from_chars' contract:
// no leading whitespace, no '+', no "0x" prefix, the value untouched on
// failure, result_out_of_range on overflow.

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>
#include <type_traits>

#if !(defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L)
    #define ARC_FROM_CHARS_FLOAT_FALLBACK 1
    #include <clocale>
    #include <cstdlib>
    #if defined(__APPLE__)
        #include <xlocale.h>
    #else
        #include <locale.h>
    #endif
#endif

namespace Arcane
{
    template <class T>
    std::from_chars_result FromChars(const char* first, const char* last, T& value) noexcept
    {
#if defined(ARC_FROM_CHARS_FLOAT_FALLBACK)
        if constexpr (std::is_floating_point_v<T>)
        {
            std::from_chars_result r{ first, std::errc::invalid_argument };
            const char* p = first;
            if (p != last && *p == '-') ++p;
            if (p == last || *p == '+' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v')
                return r;

            // from_chars' general format has no hex: "0x1p3" parses as "0".
            std::size_t len = static_cast<std::size_t>(last - first);
            if (last - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
                len = static_cast<std::size_t>(p + 1 - first);

            std::string text(first, len);   // strto* needs a terminator
            static const locale_t s_c = ::newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
            char* end = nullptr;
            errno = 0;
            T parsed;
            if constexpr (std::is_same_v<T, float>)
                parsed = ::strtof_l(text.c_str(), &end, s_c);
            else if constexpr (std::is_same_v<T, double>)
                parsed = ::strtod_l(text.c_str(), &end, s_c);
            else
                parsed = ::strtold_l(text.c_str(), &end, s_c);
            if (end == text.c_str())
                return r;
            r.ptr = first + (end - text.c_str());
            if (errno == ERANGE)
            {
                r.ec = std::errc::result_out_of_range;
                return r;
            }
            r.ec = std::errc{};
            value = parsed;
            return r;
        }
        else
#endif
        {
            return std::from_chars(first, last, value);
        }
    }
}
