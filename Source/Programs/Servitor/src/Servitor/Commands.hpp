#pragma once
// The self-description verbs (spec s4.6, s5.3; plan rev 8.3 V8): `commands`
// (the offline static manifest), `list`, `describe`, `schema`, and `call`, the
// generic access to any host command. Program code: no export macro (V4);
// protocol types stay qualified (no using in a header). Includes no
// ArcaneRemote host header (V3): the catalogue and schemas come from the host
// over the wire (GET /remote/v1/catalog, catalog.describe, schema.type).
#include <Servitor/Client.hpp>
#include <Servitor/Discover.hpp>
#include <Servitor/Format.hpp>
#include <Servitor/Verbs.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Json.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#if !defined(ARC_BUILD_DIST)

namespace Servitor {

// No socket, no descriptor: an OBJECT (rev 8.3 V8, spec AM8)
//   {version, protocolMajors, verbs:[{name, summary, access}],
//    exitCodes:[{code, meaning, launcherOnly}]}
// from the static tables in Commands.cpp. `access` is the catalogue spelling
// (observe, drive), per-command where the host command decides, or none for a
// verb that reaches no host. exitCodes lists 0-8, 9 (launcher only: servitor
// never returns it) and 130/143.
nlohmann::json StaticManifest();
// The exact text PrintStaticManifest writes (ruling F31): json and ndjson are
// the manifest on one line; human is the same rows as a table.
std::string RenderStaticManifest(const GlobalOptions& opt);
// Writes RenderStaticManifest(opt) to stdout; returns 0.
int PrintStaticManifest(const GlobalOptions& opt);

// call <command> [name=value ...] [--args-json <json> | --args-file <path>]
// (spec s4.6). The args object is --args-json or the --args-file contents
// (never both), then each name=value; a value that parses as JSON is that JSON
// (3, true, [1,2]), anything else is a string. A name given twice, a
// non-object JSON, unreadable JSON, a token without "name=" or an undeclared
// flag is false with *error set (usage, exit 2, nothing sent).
bool ParseCallTokens(std::span<const std::string> rest, CallRequest& out, std::string* error);
// The flags the call row declares (Run refuses every other one).
std::span<const VerbFlag> CallFlags();

// describe <command> -> {name}; schema component|resource <Name> -> {kind, name}.
bool DescribeArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
bool SchemaArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
// The call row's args hook: ParseCallTokens' result as {command, args}.
bool CallArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);

// Client-side verb rows (ruling F17).
// commands: PrintStaticManifest; no host, no project, no ScanProject.
int CommandsVerb(std::span<const std::string> rest, const GlobalOptions& opt);
// list: discovery (no host is target.none, exit 4), then ListCatalogue over
// LiveCatalog; prints what it renders and returns client.exitCode.
int ListVerb(std::span<const std::string> rest, const GlobalOptions& opt);

// Reads a host's catalogue for `list`: the seam ListCatalogue takes (tests
// substitute it). nullopt fills *error (when non-null) with the reason.
using CatalogFetchFn = std::optional<nlohmann::json> (*)(const Arcane::Remote::Descriptor& desc,
                                                         std::uint32_t timeoutMs, Arcane::Remote::Envelope* error,
                                                         void* user);
// The live fetch: GET /remote/v1/catalog through GetJson (Transport.hpp).
std::optional<nlohmann::json> LiveCatalog(const Arcane::Remote::Descriptor& desc, std::uint32_t timeoutMs,
                                          Arcane::Remote::Envelope* error, void* user);
// list for a selected host, rendered in opt's format; prints nothing. A fetch
// that fails is its envelope (host.unreachable exit 7, ...) as command "list";
// a catalogue that is not an object with a commands array is internal (exit
// 1); otherwise MakeOk("list", catalogue) with host.catalog = its generation.
// Human output of an ok answer is a name/access/summary table. *client gets
// exitCode, descriptor and the token (redaction only); *finalEnvelope (when
// non-null) the envelope.
Printed ListCatalogue(const LiveHost& host, const GlobalOptions& opt, CatalogFetchFn fetch, void* user,
                      Arcane::Remote::ClientBlock* client, Arcane::Remote::Envelope* finalEnvelope = nullptr);
// call: ParseCallTokens, then PostVerb (the same discovery, busy retry,
// options.readOnly and target watch as every posting verb; the host's gate
// decides read-only and drive access).
int CallVerb(std::span<const std::string> rest, const GlobalOptions& opt);

} // namespace Servitor

#endif
