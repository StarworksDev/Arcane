#pragma once
#include <Arcane/Modules/ArcaneRemoteProtocolApi.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(ARC_BUILD_DIST)

// The client half of the wire (rev 8.3 V3): building requests and reading replies. Pure string code;
// the host half (request parsing, transport checks) is ArcaneRemote's Http.hpp, which a client never includes.
namespace Arcane::Remote
{

struct HttpReply {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers; // names lowercased
    std::string body;
};

// "POST <target> HTTP/1.1\r\nHost: 127.0.0.1:<port>\r\nAuthorization: Bearer <token>\r\n
//  Content-Type: application/json\r\nContent-Length: <n>\r\nConnection: close\r\n\r\n<body>"
// A non-empty `client` adds "X-Remote-Client: <client>\r\n" after the token
// (spec s4.5, the audit log's caller); the caller passes an IsValidClientId value.
ARC_REMOTE_PROTOCOL_API std::string BuildPost(std::string_view target, std::uint16_t port,
                                              std::string_view token, std::string_view jsonBody,
                                              std::string_view client = {});
// The same shape for GET (no body, Content-Length: 0), used for /remote/v1/status and /catalog.
ARC_REMOTE_PROTOCOL_API std::string BuildGet(std::string_view target, std::uint16_t port, std::string_view token);
// Parses one complete response: status line, headers, a Content-Length body. nullopt sets *error
// to "partial" (need more bytes; the host closes after each response, so EOF before the body is
// complete is "truncated"), "truncated", or "malformed". Chunked bodies are "malformed".
ARC_REMOTE_PROTOCOL_API std::optional<HttpReply> ParseHttpReply(std::string_view bytes, bool eof, std::string* error);

} // namespace Arcane::Remote

#endif
