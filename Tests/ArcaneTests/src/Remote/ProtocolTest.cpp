#if !defined(ARC_BUILD_DIST)
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/HttpClient.hpp>
#include <Arcane/Remote/Http.hpp> // ParseHttpRequest, CheckTransport, HttpResponse (the host half)
#include "../Helpers/ConstantScan.hpp"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace Arcane::Remote;

static Envelope Err(const char* code) {
    return MakeError("cvar.get", code, "m", "r");
}

TEST_CASE("exit table matches s6.6, AM1, and AM6", "[remote][protocol]") {
    CHECK(ExitFromHost(Err("internal")) == 1);
    CHECK(ExitFromHost(Err("capture.no-renderer")) == 2);
    CHECK(ExitFromHost(Err("arg.unknown")) == 2);
    CHECK(ExitFromHost(Err("command.unknown")) == 2);
    CHECK(ExitFromHost(Err("target.ambiguous")) == 2);
    CHECK(ExitFromHost(Err("auth.denied")) == 3);
    CHECK(ExitFromHost(Err("auth.throttled")) == 3);
    CHECK(ExitFromHost(Err("permission.denied")) == 3);
    CHECK(ExitFromHost(Err("permission.read-only")) == 3);
    CHECK(ExitFromHost(Err("cvar.protected")) == 3);
    CHECK(ExitFromHost(Err("protocol.mismatch")) == 4);
    CHECK(ExitFromHost(Err("target.none")) == 4);
    CHECK(ExitFromHost(Err("engine.unresolved")) == 4);   // rev 8.3 V7
    CHECK(ExitFromHost(Err("host.busy")) == 5);
    auto minimized = Err("host.busy");
    minimized.errors[0].detail = nlohmann::json{{"state", "minimized"}};
    CHECK(ExitFromHost(minimized) == 5);
    auto full = Err("host.busy");
    full.errors[0].detail = nlohmann::json{{"state", "queue-full"}};
    CHECK(ExitFromHost(full) == 5);
    auto pending = Err("host.busy");
    pending.errors[0].detail = nlohmann::json{{"state", "capture-pending"}};
    CHECK(ExitFromHost(pending) == 5);
    CHECK(ExitFromHost(Err("capture.minimized")) == 5);
    CHECK(ExitFromHost(Err("host.unavailable")) == 5);
    CHECK(ExitFromHost(Err("entity.not-found")) == 6);
    CHECK(ExitFromHost(Err("compare.missing-reference")) == 6);
    CHECK(ExitFromHost(Err("capture.no-frame")) == 6);
    CHECK(ExitFromHost(Err("scene.artifact-failed")) == 6);   // listed explicitly (Task 25 fix round 1)
    CHECK(ExitFromHost(Err("compare.reference-unreadable")) == 6);   // listed explicitly (Task 28)
    CHECK(ExitFromHost(Err("compare.bless-failed")) == 6);
    CHECK(ExitFromHost(Err("compare.artifact-failed")) == 6);
    CHECK(ExitFromHost(Err("job.failed")) == 6);
    CHECK(ExitFromHost(Err("play.edit-only")) == 6);
    CHECK(ExitFromHost(Err("play.not-playing")) == 6);   // listed explicitly (Task 29)
    CHECK(ExitFromHost(Err("play.refused")) == 6);
    CHECK(ExitFromHost(Err("play.stopped")) == 6);
    CHECK(ExitFromHost(Err("module.reload-failed")) == 6);   // Task 30: wait reload on a failed reload
    CHECK(ExitFromHost(Err("quit.force-failed")) == 6);      // Task 30 s6.4 table pass
    CHECK(ExitFromHost(Err("some.future-code")) == 6); // any other non-empty code (ruling F6)
    CHECK(ExitFromHost(Err("")) == 2);                 // empty code on a non-ok envelope
    CHECK(ExitFromHost(Err("host.died")) == 7);
    CHECK(ExitFromHost(Err("host.unreachable")) == 7);
    // Final fix pass: a client read deadline on a live host (C-I2), a launch
    // whose spawned host never reached ready (C-I6), a log.read dump whose
    // artifact failed (C-M10).
    CHECK(ExitFromHost(Err("call.timeout")) == 8);
    CHECK(ExitFromHost(Err("launch.timeout")) == 8);
    CHECK(ExitFromHost(Err("log.artifact-failed")) == 6);
    HostBlock host;
    host.kind = "runtime";
    host.frame = 61;
    host.state = "ready";
    auto compare = MakeOk("capture.compare", nlohmann::json{{"verdict", "Failed"}, {"diffPixels", 4}}, host);
    CHECK(compare.ok);
    CHECK(compare.errors.empty());
    CHECK(ExitFromHost(compare) == 8);
    auto timed = MakeOk("host.wait", nlohmann::json{{"timedOut", true}}, host);
    CHECK(timed.ok);
    CHECK(ExitFromHost(timed) == 8);
}

TEST_CASE("ToJson is the s6.3 object and omits client and exitCode", "[remote][protocol]") {
    HostBlock host;
    host.kind = "editor";
    host.pid = 23180;
    host.world = "client";
    host.frame = 81234;
    host.state = "ready";
    host.catalog = 7;
    host.pluginGeneration = 3;
    Envelope env = MakeOk("entity.get", nlohmann::json{{"entity", "MeshCube"}}, host);
    env.id = "r-17";
    env.timing.queuedMs = 3;
    env.timing.ranMs = 0.4;
    const auto j = ToJson(env);
    CHECK(j["ok"] == true);
    CHECK(j["protocol"] == 1);
    CHECK(j["id"] == "r-17");
    CHECK(j["command"] == "entity.get");
    CHECK(j["host"]["kind"] == "editor");
    CHECK(j["host"]["frame"] == 81234);
    CHECK(j["host"]["catalog"] == 7);
    CHECK(j["data"]["entity"] == "MeshCube");
    CHECK(j["errors"].is_array());
    CHECK(j["warnings"].is_array());
    CHECK(j["notifications"].is_array());
    CHECK(j["artifacts"].is_array());
    CHECK(j["undo"].is_null());
    CHECK(j["timing"]["queuedMs"] == 3);
    CHECK_FALSE(j.contains("result"));
    CHECK_FALSE(j.contains("hostFrame"));
    CHECK_FALSE(j.contains("exitCode"));
    CHECK_FALSE(j.contains("client"));
    const Envelope back = ParseEnvelope(j);
    CHECK(back.command == "entity.get");
    CHECK(back.host.frame == 81234);
    CHECK(back.data["entity"] == "MeshCube");

    UndoBlock refused;
    refused.label = nullptr;
    refused.undoable = false;
    refused.reason = "not-undoable-command";
    refused.transaction = 0;
    env.undo = refused;
    const auto ju = ToJson(env);
    CHECK(ju["undo"]["label"].is_null());
    CHECK(ju["undo"]["undoable"] == false);
    CHECK(ju["undo"]["transaction"] == 0);
    CHECK(ju["undo"]["reason"] == "not-undoable-command");

    UndoBlock recorded;
    recorded.label = "remote: cvar.set";
    recorded.undoable = true;
    recorded.transaction = 412;
    env.undo = recorded;
    const auto jb = ToJson(env);
    CHECK(jb["undo"]["label"] == "remote: cvar.set");
    CHECK(jb["undo"]["undoable"] == true);
    CHECK(jb["undo"]["transaction"] == 412);
    CHECK_FALSE(jb["undo"].contains("reason"));
}

TEST_CASE("WorstExit ranks a dead host above a busy host", "[remote][protocol]") {
    CHECK(WorstExit(std::vector<int>{0, 5, 7}) == 7);
    CHECK(WorstExit(std::vector<int>{8, 6}) == 6);
    CHECK(WorstExit(std::vector<int>{5, 8, 4}) == 8);
    CHECK(WorstExit(std::vector<int>{3, 2}) == 3);
    CHECK(WorstExit(std::vector<int>{}) == 0);
}

TEST_CASE("MakeOk never carries errors", "[remote][protocol]") {
    const Envelope env = MakeOk("status", nlohmann::json::object(), HostBlock{});
    CHECK(env.ok);
    CHECK(env.errors.empty());
    CHECK(env.protocol == 1);
    CHECK(kIdleConnectionSeconds == 30);
}

TEST_CASE("Indeterminate verdict exits 8", "[remote][protocol]") {
    auto env = MakeOk("capture.compare", nlohmann::json{{"verdict", "Indeterminate"}}, HostBlock{});
    CHECK(ExitFromHost(env) == 8);
}

TEST_CASE("TryParseEnvelope rejects malformed replies", "[remote][protocol]") {
    const auto good = ToJson(MakeOk("status", nlohmann::json::object(), HostBlock{}));
    const auto parsed = TryParseEnvelope(good);
    REQUIRE(parsed.has_value());
    CHECK(ToJson(*parsed) == ToJson(ParseEnvelope(good)));

    CHECK_FALSE(TryParseEnvelope(nlohmann::json::array()).has_value());
    auto noOk = good; noOk.erase("ok");
    CHECK_FALSE(TryParseEnvelope(noOk).has_value());
    auto badOk = good; badOk["ok"] = "yes";
    CHECK_FALSE(TryParseEnvelope(badOk).has_value());
    auto noProto = good; noProto.erase("protocol");
    CHECK_FALSE(TryParseEnvelope(noProto).has_value());
    auto badProto = good; badProto["protocol"] = -1;
    CHECK_FALSE(TryParseEnvelope(badProto).has_value());
    auto badErrors = good; badErrors["errors"] = "x";
    CHECK_FALSE(TryParseEnvelope(badErrors).has_value());
    auto noCode = good; noCode["errors"] = nlohmann::json::array({nlohmann::json::object()});
    CHECK_FALSE(TryParseEnvelope(noCode).has_value());
    auto badHost = good; badHost["host"] = 3;
    CHECK_FALSE(TryParseEnvelope(badHost).has_value());
    // Final fix pass (C-M11): ok:false says the operation failed, so it must
    // name a failure. Without one the reply is malformed (EnvelopeOf: internal,
    // exit 1), never a usage error.
    auto failedNoErrors = good; failedNoErrors["ok"] = false;
    CHECK_FALSE(TryParseEnvelope(failedNoErrors).has_value());
    auto failedNoKey = failedNoErrors; failedNoKey.erase("errors");
    CHECK_FALSE(TryParseEnvelope(failedNoKey).has_value());
    auto failedNamed = failedNoErrors; failedNamed["errors"] = nlohmann::json::array({{{"code", "entity.not-found"}}});
    CHECK(TryParseEnvelope(failedNamed).has_value());
}

TEST_CASE("X-Remote-Client is printable ASCII up to kMaxClientIdBytes, and BuildPost sends it", "[remote][protocol]") {
    CHECK(kClientHeader == "X-Remote-Client");
    CHECK(IsValidClientId("servitor/0.1.0 pid 4242"));
    CHECK(IsValidClientId("claude"));
    CHECK(IsValidClientId(std::string(kMaxClientIdBytes, 'a')));
    CHECK(IsValidClientId("~ !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}"));
    CHECK_FALSE(IsValidClientId(""));
    CHECK_FALSE(IsValidClientId(std::string(kMaxClientIdBytes + 1, 'a')));
    CHECK_FALSE(IsValidClientId("tab\there"));
    CHECK_FALSE(IsValidClientId("line\r\nX-Evil: 1"));
    CHECK_FALSE(IsValidClientId("caf\xc3\xa9"));
    CHECK_FALSE(IsValidClientId(std::string("nul\0byte", 8)));
    CHECK_FALSE(IsValidClientId("del\x7f"));

    const std::string token(64, 'a');
    const std::string named = BuildPost("/remote/v1/call", 51234, token, "{}", "agent-7 (ci)");
    CHECK(named.find("\r\nX-Remote-Client: agent-7 (ci)\r\n") != std::string::npos);
    std::string err;
    const auto req = ParseHttpRequest(named, &err);
    REQUIRE(req.has_value());
    CHECK(Header(*req, "x-remote-client") == "agent-7 (ci)");
    CHECK(CheckTransport(*req, "51234", token).refuse == TransportRefuse::None);
    CHECK(BuildPost("/remote/v1/call", 51234, token, "{}").find("X-Remote-Client") == std::string::npos);
}

TEST_CASE("HostStatus ToJson carries protocolMinor", "[remote][protocol]") {
    HostStatus status;
    status.hostKind = HostKind::Runtime;
    status.state = "ready";
    CHECK(status.protocolMinor == kProtocolMinor);
    const auto j = ToJson(status);
    CHECK(j.at("protocol") == kProtocol);
    CHECK(Arcane::Remote::ToJson(status).at("protocolMinor") == Arcane::Remote::kProtocolMinor);
}

TEST_CASE("remote-protocol.json matches the protocol constants", "[remote][protocol]") {
    std::ifstream in(Arcane::Test::RepoRoot() / "Source" / "Runtime" / "ArcaneRemoteProtocol" / "remote-protocol.json");
    REQUIRE(in.good());
    const auto j = nlohmann::json::parse(in);
    CHECK(j.at("major").get<std::uint32_t>() == Arcane::Remote::kProtocol);
    CHECK(j.at("minor").get<std::uint32_t>() == Arcane::Remote::kProtocolMinor);
}

TEST_CASE("ArcaneRemoteProtocol depends on ArcaneCore only", "[remote][protocol]") {
    std::ifstream in(Arcane::Test::RepoRoot() / "Source" / "Runtime" / "ArcaneRemoteProtocol" / "ArcaneRemoteProtocol.arcmodule");
    REQUIRE(in.good());
    const auto j = nlohmann::json::parse(in);
    CHECK(j.at("dependencies").at("public") == nlohmann::json::array({"ArcaneCore"}));
    CHECK(j.at("dependencies").at("private").empty());
    CHECK(j.at("thirdParty").at("public") == nlohmann::json::array({"nlohmann"}));
    CHECK(j.at("thirdParty").at("private").empty());
}

TEST_CASE("envelopes carry protocolMinor and tolerate unknown fields", "[remote][protocol]") {
    auto env = Arcane::Remote::MakeOk("x.y", nlohmann::json{{"a", 1}}, {});
    auto j = Arcane::Remote::ToJson(env);
    CHECK(j.at("protocolMinor") == Arcane::Remote::kProtocolMinor);
    // A newer host: a later minor, plus fields this build has never heard of.
    j["protocolMinor"] = 7;
    j["futureTopLevel"] = "x";
    j["host"]["futureHostField"] = 3;
    j["data"]["futureDataField"] = true;
    const auto parsed = Arcane::Remote::TryParseEnvelope(j);
    REQUIRE(parsed.has_value());
    CHECK(parsed->ok);
    CHECK(parsed->protocolMinor == 7);
    CHECK(parsed->data.at("a") == 1);
    // An older host: no protocolMinor at all reads 0.
    j.erase("protocolMinor");
    const auto older = Arcane::Remote::TryParseEnvelope(j);
    REQUIRE(older.has_value());
    CHECK(older->protocolMinor == 0);
    // A present non-integer is malformed.
    j["protocolMinor"] = "seven";
    CHECK_FALSE(Arcane::Remote::TryParseEnvelope(j).has_value());
}

TEST_CASE("BuildPost passes the host's transport checks", "[remote][protocol]") {
    const std::string token(64, 'a');
    const std::string body = R"({"id":"r-1","command":"status","args":{}})";
    const std::string wire = BuildPost("/remote/v1/call", 51234, token, body);
    std::string err;
    const auto req = ParseHttpRequest(wire, &err);
    REQUIRE(req.has_value());
    CHECK(req->method == "POST");
    CHECK(req->target == "/remote/v1/call");
    CHECK(Header(*req, "content-type") == "application/json");
    CHECK(Header(*req, "connection") == "close");
    CHECK(req->body == body);
    CHECK(CheckTransport(*req, "51234", token).refuse == TransportRefuse::None);
    CHECK(CheckTransport(*req, "51235", token).refuse == TransportRefuse::Host);
    CHECK(CheckTransport(*req, "51234", std::string(64, 'b')).refuse == TransportRefuse::Auth);

    const std::string get = BuildGet("/remote/v1/status", 51234, token);
    const auto greq = ParseHttpRequest(get, &err);
    REQUIRE(greq.has_value());
    CHECK(greq->method == "GET");
    CHECK(greq->target == "/remote/v1/status");
    CHECK(Header(*greq, "content-length") == "0");
    CHECK(greq->body.empty());
    CHECK(CheckTransport(*greq, "51234", token).refuse == TransportRefuse::None);
}

TEST_CASE("ParseHttpReply reads a serialized HttpResponse back", "[remote][protocol]") {
    HttpResponse resp;
    resp.status = 503;
    resp.reason = "Service Unavailable";
    resp.headers.emplace_back("X-Extra", "1");
    resp.body = R"({"ok":false,"protocol":1})";
    const std::string wire = resp.Serialize();

    std::string err;
    const auto reply = ParseHttpReply(wire, false, &err);
    REQUIRE(reply.has_value());
    CHECK(reply->status == 503);
    CHECK(reply->body == resp.body);
    bool sawExtra = false;
    for (const auto& [name, value] : reply->headers)
        if (name == "x-extra" && value == "1") sawExtra = true;
    CHECK(sawExtra);
    CHECK(ParseHttpReply(wire, true, &err).has_value());

    // A prefix that stops inside the body: more bytes may come, unless the host already closed.
    const std::string prefix = wire.substr(0, wire.size() - 3);
    err.clear();
    CHECK_FALSE(ParseHttpReply(prefix, false, &err).has_value());
    CHECK(err == "partial");
    err.clear();
    CHECK_FALSE(ParseHttpReply(prefix, true, &err).has_value());
    CHECK(err == "truncated");
    // The same inside the header block.
    const std::string head = wire.substr(0, 10);
    err.clear();
    CHECK_FALSE(ParseHttpReply(head, false, &err).has_value());
    CHECK(err == "partial");
    err.clear();
    CHECK_FALSE(ParseHttpReply(head, true, &err).has_value());
    CHECK(err == "truncated");
}

TEST_CASE("ParseHttpReply refuses chunked and malformed replies", "[remote][protocol]") {
    std::string err;
    CHECK_FALSE(ParseHttpReply("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n", true, &err).has_value());
    CHECK(err == "malformed");
    err.clear();
    CHECK_FALSE(ParseHttpReply("garbage\r\n\r\n", true, &err).has_value());
    CHECK(err == "malformed");
    err.clear();
    CHECK_FALSE(ParseHttpReply("HTTP/1.1 2x0 OK\r\nContent-Length: 0\r\n\r\n", true, &err).has_value());
    CHECK(err == "malformed");
    err.clear();
    CHECK_FALSE(ParseHttpReply("HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\n", true, &err).has_value());
    CHECK(err == "malformed");
    err.clear();
    CHECK_FALSE(ParseHttpReply("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n{}", true, &err).has_value());
    CHECK(err == "malformed");
    err.clear();
    CHECK_FALSE(ParseHttpReply("HTTP/1.1 200 OK\r\nNoColon\r\nContent-Length: 0\r\n\r\n", true, &err).has_value());
    CHECK(err == "malformed");
    const auto empty = ParseHttpReply("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n", false, &err);
    REQUIRE(empty.has_value());
    CHECK(empty->status == 204);
    CHECK(empty->body.empty());
}

// Task 30 fix round 1 (M6): the exit CHECKs above also pass when an entry
// whose exit is 6 is deleted (ruling F6's default is 6). ExitCodeIsListed
// fails here instead, for every code ExitForCode names and the spec's s6.4
// table lists.
TEST_CASE("every code the s6.4 table names is listed in ExitForCode, not left to the F6 default", "[remote][protocol]") {
    for (const char* code : {"internal", "arg.invalid", "arg.unknown", "arg.missing", "arg.inapplicable",
                             "command.unknown", "command.unavailable", "command.host-kind", "target.ambiguous",
                             "component.ambiguous", "entity.ambiguous", "resource.ambiguous", "capture.no-renderer",
                             "auth.denied", "auth.throttled", "permission.denied", "permission.read-only",
                             "cvar.protected", "protocol.mismatch", "target.none", "engine.unresolved", "host.busy",
                             "capture.minimized", "host.unavailable", "entity.not-found", "component.unknown",
                             "component.absent", "compare.missing-reference", "capture.no-frame", "cvar.unknown",
                             "cvar.type-mismatch", "cvar.refused-weaker", "cvar.refused", "reflect.unreadable",
                             "ecs.artifact-failed", "resource.artifact-failed", "scene.artifact-failed",
                             "capture.artifact-failed", "compare.reference-unreadable", "compare.bless-failed",
                             "compare.artifact-failed", "play.not-playing", "play.refused", "play.stopped",
                             "module.reload-failed", "quit.force-failed", "resource.not-found", "host.unreachable",
                             "host.died", "host.restarting", "host.hung", "compare.failed", "wait.timeout",
                             "call.timeout", "launch.timeout", "log.artifact-failed"}) {
        INFO(code);
        CHECK(ExitCodeIsListed(code));
    }
    CHECK_FALSE(ExitCodeIsListed("some.future-code"));   // the F6 default, not a listing
    CHECK_FALSE(ExitCodeIsListed("job.failed"));         // a P3 code the table names but no P0 code path emits
}

// Task 30 (C2, fix round 1 I1): the one bounded parse both sides use, with
// the cap named at every call. Exactly the cap parses; one more is refused as
// too deep, and a megabyte of '[' (inside kMaxBodyBytes, which bounds size,
// not depth) is refused without building a level past the cap.
TEST_CASE("ParseJsonBounded refuses nesting past either cap and nothing shallower", "[remote][protocol]") {
    const auto nest = [](std::uint32_t levels) {
        return std::string(levels, '[') + std::string(levels, ']');
    };
    CHECK(kMaxRequestDepth < kMaxReplyDepth);   // a host accepts less than it may send
    for (const std::uint32_t cap : {kMaxRequestDepth, kMaxReplyDepth}) {
        bool deep = true;
        const auto atLimit = ParseJsonBounded(nest(cap), cap, &deep);
        CHECK_FALSE(atLimit.is_discarded());
        CHECK_FALSE(deep);
        const auto past = ParseJsonBounded(nest(cap + 1), cap, &deep);
        CHECK(past.is_discarded());
        CHECK(deep);
        deep = false;
        CHECK(ParseJsonBounded(std::string(1024u * 1024u, '['), cap, &deep).is_discarded());
        CHECK(deep);
    }
    // Objects count the same as arrays; the outermost container is level 1.
    CHECK_FALSE(ParseJsonBounded(R"({"a":{"b":[1]}})", 3).is_discarded());
    CHECK(ParseJsonBounded(R"({"a":{"b":[1]}})", 2).is_discarded());
    // Malformed text is discarded without the depth flag; scalars are depth 0.
    bool deep = true;
    CHECK(ParseJsonBounded("{not json", kMaxRequestDepth, &deep).is_discarded());
    CHECK_FALSE(deep);
    CHECK(ParseJsonBounded("42", kMaxRequestDepth).get<int>() == 42);
    // The envelope a host builds parses back whole under the reply cap.
    const auto env = ToJson(MakeOk("x.y", nlohmann::json{{"a", nlohmann::json::array({1, 2})}}, HostBlock{}));
    const auto round = ParseJsonBounded(env.dump(), kMaxReplyDepth);
    REQUIRE_FALSE(round.is_discarded());
    CHECK(round == env);
}
#endif
