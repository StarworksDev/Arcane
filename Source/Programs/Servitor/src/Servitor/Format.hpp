#pragma once
// Servitor's output formats (spec s6.5): one envelope, printed as json, ndjson
// or human. Client program code (rev 8.3 V3/V4): no export macro, protocol
// types qualified.
#include <Arcane/Remote/Protocol.hpp>
#include <string>

#if !defined(ARC_BUILD_DIST)

namespace Servitor {

// --format github is not a Format (P3): ParseGlobals refuses it with exit 2.
enum class Format { Json, Human, Ndjson };

struct RenderOptions {
    Format format = Format::Json;
    bool tty = false;
};

struct Printed {
    std::string stdoutText;
    std::string stderrText;
};

// json: ToJson(env) plus "client":{exitCode, retried, descriptor}. One object.
// ndjson: one {"type":"progress",...} line per notification that carries detail.progress,
//         then {"type":"result","count":1,"envelope":<ToJson plus client>}.
// human: one summary line on stdout ("ok <command>" or "error <code>"), data keys as
//        "key: value", artifacts as paths. Diagnostics (errors, warnings) on stderr.
// json and ndjson leave stderrText empty: errors and warnings live in the envelope.
// Both streams pass through Arcane::Remote::RedactSecrets with client.token.
Printed Render(const Arcane::Remote::Envelope& env, const Arcane::Remote::ClientBlock& client, RenderOptions opt);

} // namespace Servitor

#endif
