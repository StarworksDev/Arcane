#include <Arcane/Remote/Http.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Crypto/Crypto.hpp>

#include <cstddef>
#include <cstdint>

namespace Arcane::Remote
{

namespace
{

ARC_CONSTANT("crypto: 32 random bytes = 256 bits of bearer token entropy (64 hex chars)")
constexpr std::size_t kTokenBytes = 32;
ARC_CONSTANT("wire protocol: a Content-Length value longer than 10 digits exceeds any body bound")
constexpr std::size_t kMaxLengthDigits = 10;

char Lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string ToLower(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = Lower(c);
    return out;
}

bool EqualsNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

// RFC 9110 tchar: ALPHA / DIGIT / one of !#$%&'*+-.^_`|~
bool IsTokenChar(char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~': return true;
        default: return false;
    }
}

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::optional<HttpRequest> Fail(std::string* error, const char* why)
{
    if (error) *error = why;
    return std::nullopt;
}

} // namespace

std::string HttpResponse::Serialize() const
{
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    bool hasType = false;
    for (const auto& [name, value] : headers) {
        const std::string lower = ToLower(name);
        if (lower == "content-length" || lower == "x-remote-protocol") continue; // written below
        if (lower == "content-type") hasType = true;
        out += name + ": " + value + "\r\n";
    }
    if (!hasType) out += "Content-Type: application/json\r\n";
    out += "X-Remote-Protocol: " + std::to_string(kProtocol) + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    out += body;
    return out;
}

std::optional<HttpRequest> ParseHttpRequest(std::string_view bytes, std::string* error)
{
    const std::size_t end = bytes.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        return Fail(error, bytes.size() > kMaxHeaderBytes ? "bounds" : "partial");
    }
    if (end + 4 > kMaxHeaderBytes) return Fail(error, "bounds");

    const std::string_view head = bytes.substr(0, end);
    const std::size_t lineEnd = head.find("\r\n");
    const std::string_view requestLine = head.substr(0, lineEnd);

    const std::size_t sp1 = requestLine.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || sp1 == 0 || sp2 == sp1 + 1)
        return Fail(error, "malformed");

    HttpRequest req;
    req.method = std::string(requestLine.substr(0, sp1));
    req.target = std::string(requestLine.substr(sp1 + 1, sp2 - sp1 - 1));
    req.version = std::string(requestLine.substr(sp2 + 1));
    if (req.version != "HTTP/1.1") return Fail(error, "version");

    std::size_t pos = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        std::size_t next = head.find("\r\n", pos);
        if (next == std::string_view::npos) next = head.size();
        const std::string_view line = head.substr(pos, next - pos);
        pos = next + 2;
        // A line that starts with SP or HT is obs-fold (refused, RFC 9112 s5.2).
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) return Fail(error, "malformed");
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return Fail(error, "malformed");
        for (char c : line.substr(0, colon))
            if (!IsTokenChar(c)) return Fail(error, "malformed");
        req.headers.emplace_back(ToLower(line.substr(0, colon)), std::string(Trim(line.substr(colon + 1))));
    }

    // Any Transfer-Encoding is refused as "chunked": the remote protocol never frames a body that way, and a
    // Transfer-Encoding beside Content-Length is a request-smuggling shape.
    for (const auto& h : req.headers)
        if (h.first == "transfer-encoding") return Fail(error, "chunked");

    // Content-Length: digits only; duplicates must agree.
    std::uint64_t length = 0;
    bool haveLength = false;
    for (const auto& [name, value] : req.headers) {
        if (name != "content-length") continue;
        if (value.empty()) return Fail(error, "malformed");
        if (value.size() > kMaxLengthDigits) return Fail(error, "bounds");
        std::uint64_t n = 0;
        for (char c : value) {
            if (c < '0' || c > '9') return Fail(error, "malformed");
            n = n * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (haveLength && n != length) return Fail(error, "malformed");
        length = n;
        haveLength = true;
    }
    if (length > kMaxBodyBytes) return Fail(error, "bounds");

    const std::size_t bodyStart = end + 4;
    if (bytes.size() - bodyStart < length) return Fail(error, "partial");
    req.body = std::string(bytes.substr(bodyStart, static_cast<std::size_t>(length)));

    if (req.method == "POST" && !req.body.empty()) {
        const std::string typeValue = Header(req, "content-type");
        std::string_view type = typeValue;
        const std::size_t semi = type.find(';');
        if (semi != std::string_view::npos) type = type.substr(0, semi);
        if (!EqualsNoCase(Trim(type), "application/json")) return Fail(error, "content-type");
    }
    return req;
}

std::string Header(const HttpRequest& req, std::string_view nameLower)
{
    for (const auto& [name, value] : req.headers)
        if (name == nameLower) return value;
    return {};
}

TransportCheck CheckTransport(const HttpRequest& req, std::string_view portDecimal, std::string_view tokenHex)
{
    TransportCheck out;
    auto refuse = [&](TransportRefuse why, const char* message) {
        out.refuse = why;
        out.code = "auth.denied";
        out.message = message;
        return out;
    };

    for (const auto& h : req.headers)
        if (h.first == "origin") return refuse(TransportRefuse::Origin, "origin is not accepted");

    const std::string host = Header(req, "host");
    const std::string port(portDecimal);
    if (!EqualsNoCase(host, "127.0.0.1:" + port) && !EqualsNoCase(host, "localhost:" + port))
        return refuse(TransportRefuse::Host, "host is not accepted");

    const std::string expected = "Bearer " + std::string(tokenHex);
    const std::string got = Header(req, "authorization");
    if (tokenHex.empty() || !Crypto::HexEquals(got, expected))
        return refuse(TransportRefuse::Auth, "bearer token missing or invalid");
    return out;
}

std::string MakeToken()
{
    return Crypto::GenerateSecureToken(kTokenBytes);
}

} // namespace Arcane::Remote

#endif
