#include <Arcane/Remote/Redact.hpp>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

namespace
{

bool IsHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

std::string RedactSecrets(std::string_view text, std::string_view tokenHex)
{
    std::string stage;
    stage.reserve(text.size());
    if (tokenHex.empty()) {
        stage = std::string(text);
    } else {
        std::size_t i = 0;
        while (i < text.size()) {
            const std::size_t hit = text.find(tokenHex, i);
            if (hit == std::string_view::npos) { stage.append(text.substr(i)); break; }
            stage.append(text.substr(i, hit - i));
            stage += "<redacted>";
            i = hit + tokenHex.size();
        }
    }

    constexpr std::string_view kBearer = "Bearer ";
    std::string out;
    out.reserve(stage.size());
    std::size_t i = 0;
    while (i < stage.size()) {
        const std::size_t hit = stage.find(kBearer, i);
        if (hit == std::string::npos) { out.append(stage, i, std::string::npos); break; }
        out.append(stage, i, hit - i);
        std::size_t run = hit + kBearer.size();
        const std::size_t runStart = run;
        while (run < stage.size() && IsHex(stage[run])) ++run;
        if (run - runStart >= kTokenHexChars) {
            out += "Bearer <redacted>";
            i = run;
        } else {
            out.append(kBearer);
            i = hit + kBearer.size();
        }
    }
    return out;
}

} // namespace Arcane::Remote

#endif
