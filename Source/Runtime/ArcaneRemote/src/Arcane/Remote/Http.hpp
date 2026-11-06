#pragma once
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Redact.hpp> // RedactSecrets moved to ArcaneRemoteProtocol (spec AM9)
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

struct HttpRequest {
    std::string method;
    std::string target;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers; // names lowercased
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    ARC_REMOTE_API std::string Serialize() const;
};

enum class TransportRefuse { None, Origin, Host, Auth, Bounds, Chunked, Version, Throttled };

struct TransportCheck {
    TransportRefuse refuse = TransportRefuse::None;
    std::string code;       // auth.denied or auth.throttled
    std::string message;    // never contains the token
};

// Parses one complete HTTP/1.1 request. On failure returns nullopt and sets *error to one of
// "partial" (need more bytes), "version", "chunked", "bounds", "content-type", "malformed".
ARC_REMOTE_API std::optional<HttpRequest> ParseHttpRequest(std::string_view bytes, std::string* error);
ARC_REMOTE_API TransportCheck CheckTransport(const HttpRequest& req, std::string_view portDecimal,
                                          std::string_view tokenHex);
ARC_REMOTE_API std::string Header(const HttpRequest& req, std::string_view nameLower);
ARC_REMOTE_API std::string MakeToken(); // Arcane::Crypto::GenerateSecureToken(32); 64 hex chars

} // namespace Arcane::Remote

#endif
