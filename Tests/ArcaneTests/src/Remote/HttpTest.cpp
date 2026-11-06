#if !defined(ARC_BUILD_DIST)
#include <catch2/catch_test_macros.hpp>
#include <rapidcheck/catch.h>
#include <Arcane/Remote/Http.hpp>

TEST_CASE("parser rejects HTTP/1.0, chunked, and an oversized body", "[remote][http]") {
    std::string err;
    auto v = Arcane::Remote::ParseHttpRequest("GET /remote/v1/status HTTP/1.0\r\nHost: 127.0.0.1:9\r\n\r\n", &err);
    CHECK_FALSE(v);
    CHECK(err == "version");
    auto c = Arcane::Remote::ParseHttpRequest(
        "POST /remote/v1/call HTTP/1.1\r\nHost: 127.0.0.1:9\r\nTransfer-Encoding: chunked\r\n\r\n", &err);
    CHECK_FALSE(c);
    CHECK(err == "chunked");
    std::string big = "POST /remote/v1/call HTTP/1.1\r\nHost: 127.0.0.1:9\r\nContent-Length: 5000000\r\n\r\n";
    auto b = Arcane::Remote::ParseHttpRequest(big, &err);
    CHECK_FALSE(b);
    CHECK(err == "bounds");
}

TEST_CASE("parser reports partial reads, bounds on headers, and content type", "[remote][http]") {
    std::string err;
    CHECK_FALSE(Arcane::Remote::ParseHttpRequest("GET /remote/v1/status HTTP/1.1\r\nHost: x", &err));
    CHECK(err == "partial");
    CHECK_FALSE(Arcane::Remote::ParseHttpRequest(
        "POST /remote/v1/call HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\n12345", &err));
    CHECK(err == "partial");

    const std::string huge = "GET / HTTP/1.1\r\nX: " + std::string(20000, 'a') + "\r\n\r\n";
    CHECK_FALSE(Arcane::Remote::ParseHttpRequest(huge, &err));
    CHECK(err == "bounds");
    const std::string hugeNoEnd = "GET / HTTP/1.1\r\nX: " + std::string(20000, 'a');
    CHECK_FALSE(Arcane::Remote::ParseHttpRequest(hugeNoEnd, &err));
    CHECK(err == "bounds");

    auto ok = Arcane::Remote::ParseHttpRequest(
        "POST /remote/v1/call HTTP/1.1\r\nHOST: h\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: 2\r\n\r\n{}", &err);
    REQUIRE(ok);
    CHECK(ok->method == "POST");
    CHECK(ok->target == "/remote/v1/call");
    CHECK(ok->body == "{}");
    CHECK(Arcane::Remote::Header(*ok, "host") == "h");

    CHECK_FALSE(Arcane::Remote::ParseHttpRequest(
        "POST /remote/v1/call HTTP/1.1\r\nHost: h\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\n{}", &err));
    CHECK(err == "content-type");
}

TEST_CASE("parser refuses malformed and smuggling shapes", "[remote][http]") {
    std::string err;
    auto refused = [&](const std::string& raw, const char* why) {
        err.clear();
        CHECK_FALSE(Arcane::Remote::ParseHttpRequest(raw, &err));
        CHECK(err == why);
    };
    const std::string pre = "POST /remote/v1/call HTTP/1.1\r\nHost: h\r\nContent-Type: application/json\r\n";

    // whitespace before the colon / obs-fold
    refused("GET / HTTP/1.1\r\nHost: h\r\nOrigin : x\r\n\r\n", "malformed");
    refused("GET / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding : chunked\r\n\r\n", "malformed");
    refused(pre + "Content-Length : 2\r\n\r\n{}", "malformed");
    refused("GET / HTTP/1.1\r\nHost: h\r\n foo: bar\r\n\r\n", "malformed");
    refused("GET / HTTP/1.1\r\nHost: h\r\nX-A: b\r\n\tfolded\r\n\r\n", "malformed");
    refused("GET / HTTP/1.1\r\nHost: h\r\nBad Name: x\r\n\r\n", "malformed");

    // Content-Length
    refused(pre + "Content-Length: 2\r\nContent-Length: 3\r\n\r\n{}x", "malformed");
    auto agree = Arcane::Remote::ParseHttpRequest(pre + "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", &err);
    REQUIRE(agree);
    CHECK(agree->body == "{}");
    refused(pre + "Content-Length: 2a\r\n\r\n{}", "malformed");
    refused(pre + "Content-Length: -2\r\n\r\n{}", "malformed");
    refused(pre + "Content-Length:\r\n\r\n{}", "malformed");
    refused(pre + "Content-Length: 12345678901\r\n\r\n", "bounds");

    // Transfer-Encoding of any value
    refused(pre + "Transfer-Encoding: gzip\r\nContent-Length: 2\r\n\r\n{}", "chunked");

    // request line and header line shape
    refused("GET /x\r\nHost: h\r\n\r\n", "malformed");
    refused("GET  HTTP/1.1\r\nHost: h\r\n\r\n", "malformed");
    refused(" / HTTP/1.1\r\nHost: h\r\n\r\n", "malformed");
    refused("GET / HTTP/1.1\r\nHost: h\r\nNoColonHere\r\n\r\n", "malformed");
    refused("GET / HTTP/1.1\r\nHost: h\r\n: empty\r\n\r\n", "malformed");
}

TEST_CASE("a good token does not survive Origin or a foreign Host", "[remote][http]") {
    const std::string token = Arcane::Remote::MakeToken();
    REQUIRE(token.size() == 64);
    std::string err;
    const std::string okRaw =
        "GET /remote/v1/status HTTP/1.1\r\nHost: 127.0.0.1:9\r\nAuthorization: Bearer " + token + "\r\n\r\n";
    auto ok = Arcane::Remote::ParseHttpRequest(okRaw, &err);
    REQUIRE(ok);
    CHECK(Arcane::Remote::CheckTransport(*ok, "9", token).refuse == Arcane::Remote::TransportRefuse::None);

    const std::string originRaw = okRaw.substr(0, okRaw.size() - 2) + "Origin: http://evil\r\n\r\n";
    auto withOrigin = Arcane::Remote::ParseHttpRequest(originRaw, &err);
    REQUIRE(withOrigin);
    auto origin = Arcane::Remote::CheckTransport(*withOrigin, "9", token);
    CHECK(origin.refuse == Arcane::Remote::TransportRefuse::Origin);
    CHECK(origin.code == "auth.denied");
    CHECK(origin.message.find(token) == std::string::npos);

    const std::string foreign =
        "GET /remote/v1/status HTTP/1.1\r\nHost: 10.0.0.8:9\r\nAuthorization: Bearer " + token + "\r\n\r\n";
    auto fr = Arcane::Remote::ParseHttpRequest(foreign, &err);
    REQUIRE(fr);
    CHECK(Arcane::Remote::CheckTransport(*fr, "9", token).refuse == Arcane::Remote::TransportRefuse::Host);

    const std::string local =
        "GET /remote/v1/status HTTP/1.1\r\nHost: LocalHost:9\r\nAuthorization: Bearer " + token + "\r\n\r\n";
    auto lh = Arcane::Remote::ParseHttpRequest(local, &err);
    REQUIRE(lh);
    CHECK(Arcane::Remote::CheckTransport(*lh, "9", token).refuse == Arcane::Remote::TransportRefuse::None);

    const std::string bad =
        "GET /remote/v1/status HTTP/1.1\r\nHost: 127.0.0.1:9\r\nAuthorization: Bearer " + std::string(64, 'a') + "\r\n\r\n";
    auto denied = Arcane::Remote::ParseHttpRequest(bad, &err);
    REQUIRE(denied);
    auto d = Arcane::Remote::CheckTransport(*denied, "9", token);
    CHECK(d.refuse == Arcane::Remote::TransportRefuse::Auth);
    CHECK(d.code == "auth.denied");
    CHECK(d.message.find(std::string(64, 'a')) == std::string::npos);

    const std::string none = "GET /remote/v1/status HTTP/1.1\r\nHost: 127.0.0.1:9\r\n\r\n";
    auto na = Arcane::Remote::ParseHttpRequest(none, &err);
    REQUIRE(na);
    CHECK(Arcane::Remote::CheckTransport(*na, "9", token).refuse == Arcane::Remote::TransportRefuse::Auth);
}

TEST_CASE("response serialization carries the protocol header and length", "[remote][http]") {
    Arcane::Remote::HttpResponse r;
    r.body = "{}";
    const std::string s = r.Serialize();
    CHECK(s.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(s.find("Content-Type: application/json\r\n") != std::string::npos);
    CHECK(s.find("X-Remote-Protocol: 1\r\n") != std::string::npos);
    CHECK(s.find("Content-Length: 2\r\n") != std::string::npos);
    CHECK(s.substr(s.size() - 4) == "\r\n{}");
}

TEST_CASE("redaction removes the token and not the surrounding text", "[remote][http]") {
    const std::string token = Arcane::Remote::MakeToken();
    const std::string line = "Authorization: Bearer " + token + " host=127.0.0.1";
    const std::string out = Arcane::Remote::RedactSecrets(line, token);
    CHECK(out.find(token) == std::string::npos);
    CHECK(out.find("127.0.0.1") != std::string::npos);
    CHECK(out.find("<redacted>") != std::string::npos);

    const std::string other = "Bearer " + std::string(64, 'b') + " tail";
    CHECK(Arcane::Remote::RedactSecrets(other, token) == "Bearer <redacted> tail");
}

TEST_CASE("property: header order does not admit Origin", "[remote][http]") {
    rc::prop("origin anywhere is refused", [] {
        const std::string token = Arcane::Remote::MakeToken();
        const auto noise = *rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z'));
        const std::string raw =
            "GET /remote/v1/status HTTP/1.1\r\nX-Noise: " + noise +
            "\r\nOrigin: http://x\r\nHost: 127.0.0.1:9\r\nAuthorization: Bearer " + token + "\r\n\r\n";
        std::string err;
        auto req = Arcane::Remote::ParseHttpRequest(raw, &err);
        RC_ASSERT(req.has_value());
        RC_ASSERT(Arcane::Remote::CheckTransport(*req, "9", token).refuse == Arcane::Remote::TransportRefuse::Origin);
    });
}
#endif
