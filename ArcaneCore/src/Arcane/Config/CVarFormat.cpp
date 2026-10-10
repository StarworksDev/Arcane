#include <Arcane/Config/CVarFormat.hpp>
#include <Arcane/Util/CharConv.hpp>

#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

namespace Arcane
{
    namespace
    {
        // The sRGB transfer (IEC 61966-2-1): the curve the editor's colour
        // picker uses (ArcaneEditor/src/Widgets/EditorWidgets.cpp:1368-1383).
        float EncodeSrgb(float linear)
        {
            if (linear <= 0.0f) return 0.0f;
            if (linear >= 1.0f) return 1.0f;
            if (linear <= 0.0031308f) return linear * 12.92f;
            return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        }

        float DecodeSrgb(float srgb)
        {
            if (srgb <= 0.04045f) return srgb / 12.92f;
            return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
        }

        unsigned ToByte(float unit)
        {
            const float clamped = unit < 0.0f ? 0.0f : (unit > 1.0f ? 1.0f : unit);
            return static_cast<unsigned>(std::lround(clamped * 255.0f));
        }

        int HexDigit(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        std::string ShortestFloat(float v)
        {
            char buf[32];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v);
            return std::string(buf, r.ptr);
        }

        // Fields separated by spaces and/or commas, after one optional [ ].
        std::vector<std::string_view> Fields(std::string_view text)
        {
            while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
            while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
            if (text.size() >= 2 && text.front() == '[' && text.back() == ']')
                text = text.substr(1, text.size() - 2);
            std::vector<std::string_view> out;
            std::size_t i = 0;
            while (i < text.size())
            {
                while (i < text.size() && (text[i] == ' ' || text[i] == ',')) ++i;
                const std::size_t start = i;
                while (i < text.size() && text[i] != ' ' && text[i] != ',') ++i;
                if (i > start) out.push_back(text.substr(start, i - start));
            }
            return out;
        }

        // The whole of `text` as one finite float: "nan", "inf" and a numeric
        // prefix with trailing text ("1.5abc") are refused, because a
        // published NaN never compares equal (callbacks every Publish, and
        // Modified disagreeing with the reset arrow after a reload).
        template <class F>
        std::optional<F> ParseFinite(std::string_view text)
        {
            F v{};
            const auto r = Arcane::FromChars(text.data(), text.data() + text.size(), v);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || !std::isfinite(v)) return std::nullopt;
            return v;
        }

        // Exactly `count` whole, finite floats into out[0..count).
        bool ParseFloats(std::string_view text, float* out, std::size_t count)
        {
            const std::vector<std::string_view> fields = Fields(text);
            if (fields.size() != count) return false;
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::optional<float> v = ParseFinite<float>(fields[i]);
                if (!v) return false;
                out[i] = *v;
            }
            return true;
        }

        // v1's integer parse: from_chars, a numeric prefix is enough.
        template <class I>
        std::optional<I> ParseIntegral(std::string_view text)
        {
            I v{};
            const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (r.ec != std::errc{}) return std::nullopt;
            return v;
        }

        std::string Join(const std::vector<std::string>& names)
        {
            std::string out;
            for (const std::string& n : names)
            {
                if (!out.empty()) out += ", ";
                out += n;
            }
            return out;
        }

        bool Upper(char c) { return c >= 'A' && c <= 'Z'; }
        bool Lower(char c) { return c >= 'a' && c <= 'z'; }
        bool Digit(char c) { return c >= '0' && c <= '9'; }

        // camelCase / snake_case -> "Title Case Words". A word starts at a
        // capital after a lower-case letter ("fitMin" -> Fit Min), or at the
        // last capital of an acronym or digit run that a lower-case letter
        // follows ("HTTPPort" -> HTTP Port, "d3d12Debug" -> D3d12 Debug).
        // '_' and '-' separate words. A trailing acronym stays whole.
        std::string Words(std::string_view segment)
        {
            std::string out;
            bool wordStart = true;
            for (std::size_t i = 0; i < segment.size(); ++i)
            {
                const char c = segment[i];
                if (c == '_' || c == '-')
                {
                    wordStart = true;
                    continue;
                }
                if (i > 0 && Upper(c))
                {
                    const char prev = segment[i - 1];
                    const bool lowerNext = i + 1 < segment.size() && Lower(segment[i + 1]);
                    if (Lower(prev) || ((Upper(prev) || Digit(prev)) && lowerNext)) wordStart = true;
                }
                if (wordStart && !out.empty()) out += ' ';
                out += (wordStart && Lower(c)) ? static_cast<char>(c - 'a' + 'A') : c;
                wordStart = false;
            }
            return out;
        }
    }

    std::string CVarColorToHex(const CVarColor& c)
    {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        const unsigned bytes[4] = { ToByte(EncodeSrgb(c.r)), ToByte(EncodeSrgb(c.g)), ToByte(EncodeSrgb(c.b)), ToByte(c.a) };
        std::string out = "#";
        for (const unsigned b : bytes)
        {
            out += kDigits[b >> 4];
            out += kDigits[b & 15u];
        }
        return out;
    }

    std::optional<CVarColor> CVarColorFromHex(std::string_view text)
    {
        if ((text.size() != 7 && text.size() != 9) || text.front() != '#') return std::nullopt;
        unsigned bytes[4] = { 0, 0, 0, 255 };
        const std::size_t count = (text.size() - 1) / 2;
        for (std::size_t i = 0; i < count; ++i)
        {
            const int hi = HexDigit(text[1 + 2 * i]);
            const int lo = HexDigit(text[2 + 2 * i]);
            if (hi < 0 || lo < 0) return std::nullopt;
            bytes[i] = static_cast<unsigned>(hi * 16 + lo);
        }
        return CVarColor{ DecodeSrgb(static_cast<float>(bytes[0]) / 255.0f), DecodeSrgb(static_cast<float>(bytes[1]) / 255.0f),
                          DecodeSrgb(static_cast<float>(bytes[2]) / 255.0f), static_cast<float>(bytes[3]) / 255.0f };
    }

    bool CVarColorNearlyEqual(const CVarColor& a, const CVarColor& b)
    {
        const auto oneStep = [](unsigned x, unsigned y) { return (x > y ? x - y : y - x) <= 1u; };
        return oneStep(ToByte(EncodeSrgb(a.r)), ToByte(EncodeSrgb(b.r))) && oneStep(ToByte(EncodeSrgb(a.g)), ToByte(EncodeSrgb(b.g)))
            && oneStep(ToByte(EncodeSrgb(a.b)), ToByte(EncodeSrgb(b.b))) && oneStep(ToByte(a.a), ToByte(b.a));
    }

    std::string FormatCVarValue(const CVarValue& v, const std::vector<std::string>& enumNames)
    {
        switch (v.type)
        {
        case CVarType::Bool: return v.AsBool() ? "true" : "false";
        case CVarType::Int32: return std::to_string(v.AsInt32());
        case CVarType::UInt32: return std::to_string(v.AsUInt32());
        case CVarType::Int64: return std::to_string(v.AsInt64());
        case CVarType::UInt64: return std::to_string(v.AsUInt64());
        case CVarType::Float32: return std::to_string(v.AsFloat32());
        case CVarType::Float64: return std::to_string(v.AsFloat64());
        case CVarType::String: return v.AsString();
        case CVarType::Color: return CVarColorToHex(v.AsColor());
        case CVarType::Vec2: { const CVarVec2 x = v.AsVec2(); return ShortestFloat(x.x) + " " + ShortestFloat(x.y); }
        case CVarType::Vec3: { const CVarVec3 x = v.AsVec3(); return ShortestFloat(x.x) + " " + ShortestFloat(x.y) + " " + ShortestFloat(x.z); }
        case CVarType::Vec4:
        {
            const CVarVec4 x = v.AsVec4();
            return ShortestFloat(x.x) + " " + ShortestFloat(x.y) + " " + ShortestFloat(x.z) + " " + ShortestFloat(x.w);
        }
        case CVarType::Enum:
        {
            const std::int32_t ordinal = v.AsEnum();
            if (ordinal >= 0 && static_cast<std::size_t>(ordinal) < enumNames.size())
                return enumNames[static_cast<std::size_t>(ordinal)];
            return std::to_string(ordinal);
        }
        }
        return {};
    }

    std::optional<std::int32_t> CVarEnumOrdinal(const std::vector<std::string>& enumNames, std::string_view name)
    {
        for (std::size_t i = 0; i < enumNames.size(); ++i)
            if (enumNames[i] == name) return static_cast<std::int32_t>(i);
        return std::nullopt;
    }

    std::optional<CVarValue> ParseCVarText(std::string_view text, CVarType type,
                                           const std::vector<std::string>& enumNames, std::string& error)
    {
        const std::string token{ text };
        switch (type)
        {
        case CVarType::Bool:
            if (token == "1" || token == "true") return CVarValue::Bool(true);
            if (token == "0" || token == "false") return CVarValue::Bool(false);
            error = "expected true or false";
            return std::nullopt;
        case CVarType::Int32:
            if (const auto v = ParseIntegral<std::int32_t>(token)) return CVarValue::Int32(*v);
            error = "expected int32";
            return std::nullopt;
        case CVarType::UInt32:
            if (const auto v = ParseIntegral<std::uint32_t>(token)) return CVarValue::UInt32(*v);
            error = "expected uint32";
            return std::nullopt;
        case CVarType::Int64:
            if (const auto v = ParseIntegral<std::int64_t>(token)) return CVarValue::Int64(*v);
            error = "expected int64";
            return std::nullopt;
        case CVarType::UInt64:
            if (const auto v = ParseIntegral<std::uint64_t>(token)) return CVarValue::UInt64(*v);
            error = "expected uint64";
            return std::nullopt;
        case CVarType::Float32:
            if (const auto v = ParseFinite<float>(token)) return CVarValue::Float32(*v);
            error = "expected float";
            return std::nullopt;
        case CVarType::Float64:
            if (const auto v = ParseFinite<double>(token)) return CVarValue::Float64(*v);
            error = "expected double";
            return std::nullopt;
        case CVarType::String:
            return CVarValue::String(token);
        case CVarType::Color:
        {
            if (!token.empty() && token.front() == '#')
            {
                if (const auto c = CVarColorFromHex(token)) return CVarValue::Color(*c);
            }
            else
            {
                float f[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                if (ParseFloats(token, f, 4) || ParseFloats(token, f, 3))
                    return CVarValue::Color(CVarColor{ f[0], f[1], f[2], f[3] });
            }
            error = "expected #RRGGBB, #RRGGBBAA, or 3-4 linear floats";
            return std::nullopt;
        }
        case CVarType::Vec2:
        {
            float f[2] = {};
            if (ParseFloats(token, f, 2)) return CVarValue::Vec2(CVarVec2{ f[0], f[1] });
            error = "expected 2 floats";
            return std::nullopt;
        }
        case CVarType::Vec3:
        {
            float f[3] = {};
            if (ParseFloats(token, f, 3)) return CVarValue::Vec3(CVarVec3{ f[0], f[1], f[2] });
            error = "expected 3 floats";
            return std::nullopt;
        }
        case CVarType::Vec4:
        {
            float f[4] = {};
            if (ParseFloats(token, f, 4)) return CVarValue::Vec4(CVarVec4{ f[0], f[1], f[2], f[3] });
            error = "expected 4 floats";
            return std::nullopt;
        }
        case CVarType::Enum:
        {
            if (const auto ordinal = CVarEnumOrdinal(enumNames, token)) return CVarValue::Enum(*ordinal);
            std::int32_t n = -1;
            const auto r = std::from_chars(token.data(), token.data() + token.size(), n);
            if (r.ec == std::errc{} && r.ptr == token.data() + token.size() && n >= 0 &&
                static_cast<std::size_t>(n) < enumNames.size())
                return CVarValue::Enum(n);
            error = "expected one of: " + Join(enumNames);
            return std::nullopt;
        }
        }
        error = "type has no accessor";
        return std::nullopt;
    }

    const char* CVarTypeName(CVarType type)
    {
        switch (type)
        {
        case CVarType::Bool: return "bool";
        case CVarType::Int32: return "int32";
        case CVarType::UInt32: return "uint32";
        case CVarType::Int64: return "int64";
        case CVarType::UInt64: return "uint64";
        case CVarType::Float32: return "float";
        case CVarType::Float64: return "double";
        case CVarType::String: return "string";
        case CVarType::Color: return "color";
        case CVarType::Vec2: return "vec2";
        case CVarType::Vec3: return "vec3";
        case CVarType::Vec4: return "vec4";
        case CVarType::Enum: return "enum";
        }
        return "unknown";
    }

    std::string DeriveCVarDisplayName(std::string_view cvarName)
    {
        const auto dot = cvarName.rfind('.');
        return Words(dot == std::string_view::npos ? cvarName : cvarName.substr(dot + 1));
    }

    std::string DeriveCVarCategoryPath(std::string_view cvarName)
    {
        const auto last = cvarName.rfind('.');
        if (last == std::string_view::npos) return "General";
        std::string out;
        std::string_view rest = cvarName.substr(0, last);
        while (!rest.empty())
        {
            const auto dot = rest.find('.');
            if (!out.empty()) out += '/';
            out += Words(rest.substr(0, dot));
            if (dot == std::string_view::npos) break;
            rest.remove_prefix(dot + 1);
        }
        return out;
    }
}
