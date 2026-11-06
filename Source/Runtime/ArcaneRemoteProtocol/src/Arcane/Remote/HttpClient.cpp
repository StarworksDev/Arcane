#include <Arcane/Remote/HttpClient.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Protocol.hpp>

#include <cstddef>

namespace Arcane::Remote
{

namespace
{

ARC_CONSTANT("wire protocol: 19 decimal digits always fit a uint64 Content-Length")
constexpr std::size_t kMaxReplyLengthDigits = 19;

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

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::optional<HttpReply> Fail(std::string* error, const char* why)
{
    if (error) *error = why;
    return std::nullopt;
}

std::string RequestHead(std::string_view method, std::string_view target, std::uint16_t port, std::string_view token)
{
    std::string out;
    out.append(method).append(" ").append(target).append(" HTTP/1.1\r\n");
    out.append("Host: 127.0.0.1:").append(std::to_string(port)).append("\r\n");
    out.append("Authorization: Bearer ").append(token).append("\r\n");
    return out;
}

} // namespace

std::string BuildPost(std::string_view target, std::uint16_t port, std::string_view token, std::string_view jsonBody,
                      std::string_view client)
{
    std::string out = RequestHead("POST", target, port, token);
    if (!client.empty())
        out.append(kClientHeader).append(": ").append(client).append("\r\n");
    out.append("Content-Type: application/json\r\n");
    out.append("Content-Length: ").append(std::to_string(jsonBody.size())).append("\r\n");
    out.append("Connection: close\r\n\r\n");
    out.append(jsonBody);
    return out;
}

std::string BuildGet(std::string_view target, std::uint16_t port, std::string_view token)
{
    std::string out = RequestHead("GET", target, port, token);
    out.append("Content-Length: 0\r\n");
    out.append("Connection: close\r\n\r\n");
    return out;
}

std::optional<HttpReply> ParseHttpReply(std::string_view bytes, bool eof, std::string* error)
{
    const std::size_t end = bytes.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        if (bytes.size() > kMaxHeaderBytes) return Fail(error, "malformed");
        return Fail(error, eof ? "truncated" : "partial");
    }

    const std::string_view head = bytes.substr(0, end);
    const std::size_t lineEnd = head.find("\r\n");
    const std::string_view statusLine = head.substr(0, lineEnd);

    // "HTTP/1.1 200 OK": a version, one space, three digits, then the end of the line or a space.
    constexpr std::string_view kVersion11 = "HTTP/1.1 ";
    constexpr std::string_view kVersion10 = "HTTP/1.0 ";
    if (!statusLine.starts_with(kVersion11) && !statusLine.starts_with(kVersion10)) return Fail(error, "malformed");
    const std::string_view code = statusLine.substr(kVersion11.size());
    if (code.size() < 3 || (code.size() > 3 && code[3] != ' ')) return Fail(error, "malformed");
    HttpReply reply;
    for (std::size_t i = 0; i < 3; ++i) {
        if (code[i] < '0' || code[i] > '9') return Fail(error, "malformed");
        reply.status = reply.status * 10 + (code[i] - '0');
    }

    std::size_t pos = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        std::size_t next = head.find("\r\n", pos);
        if (next == std::string_view::npos) next = head.size();
        const std::string_view line = head.substr(pos, next - pos);
        pos = next + 2;
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) return Fail(error, "malformed");
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return Fail(error, "malformed");
        reply.headers.emplace_back(ToLower(line.substr(0, colon)), std::string(Trim(line.substr(colon + 1))));
    }

    // The host frames every body with Content-Length; chunked (any Transfer-Encoding) is not this protocol.
    std::uint64_t length = 0;
    bool haveLength = false;
    for (const auto& [name, value] : reply.headers) {
        if (name == "transfer-encoding") return Fail(error, "malformed");
        if (name != "content-length") continue;
        if (value.empty() || value.size() > kMaxReplyLengthDigits) return Fail(error, "malformed");
        std::uint64_t n = 0;
        for (char c : value) {
            if (c < '0' || c > '9') return Fail(error, "malformed");
            n = n * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (haveLength && n != length) return Fail(error, "malformed");
        length = n;
        haveLength = true;
    }
    if (!haveLength) return Fail(error, "malformed");

    const std::size_t bodyStart = end + 4;
    if (bytes.size() - bodyStart < length) return Fail(error, eof ? "truncated" : "partial");
    reply.body = std::string(bytes.substr(bodyStart, static_cast<std::size_t>(length)));
    return reply;
}

} // namespace Arcane::Remote

#endif
