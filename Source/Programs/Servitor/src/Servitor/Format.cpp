#include <Servitor/Format.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Redact.hpp>
#include <Json.hpp>
#include <string_view>
#include <utility>

namespace Servitor {

namespace {

// replace, not throw: a host string that is not UTF-8 never aborts printing.
std::string Dump(const nlohmann::json& j)
{
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

nlohmann::json WithClient(const Arcane::Remote::Envelope& env, const Arcane::Remote::ClientBlock& client)
{
    nlohmann::json j = Arcane::Remote::ToJson(env);
    j["client"] = {
        {"exitCode", client.exitCode},
        {"retried", client.retried},
        {"descriptor", client.descriptor},
    };
    return j;
}

std::string HumanValue(const nlohmann::json& v)
{
    return v.is_string() ? v.get<std::string>() : Dump(v);
}

void HumanDiagnostic(std::string& out, std::string_view label, const Arcane::Remote::Error& e)
{
    out += label;
    out += ' ';
    out += e.code;
    if (!e.message.empty()) {
        out += ": ";
        out += e.message;
    }
    out += '\n';
    if (!e.remediation.empty()) {
        out += "  remediation: ";
        out += e.remediation;
        out += '\n';
    }
    for (const auto& c : e.candidates) {
        out += "  candidate: ";
        out += c;
        out += '\n';
    }
}

Printed RenderHuman(const Arcane::Remote::Envelope& env)
{
    Printed p;
    if (env.ok && env.errors.empty())
        p.stdoutText = "ok " + env.command + "\n";
    else
        p.stdoutText = "error " + (env.errors.empty() ? std::string("unknown") : env.errors.front().code) + "\n";
    if (env.data.is_object()) {
        for (auto it = env.data.begin(); it != env.data.end(); ++it)
            p.stdoutText += it.key() + ": " + HumanValue(it.value()) + "\n";
    } else if (!env.data.is_null()) {
        p.stdoutText += "data: " + HumanValue(env.data) + "\n";
    }
    for (const auto& a : env.artifacts)
        p.stdoutText += a.path + "\n";
    for (const auto& e : env.errors)
        HumanDiagnostic(p.stderrText, "error", e);
    for (const auto& w : env.warnings)
        HumanDiagnostic(p.stderrText, "warning", w);
    return p;
}

Printed RenderNdjson(const Arcane::Remote::Envelope& env, const Arcane::Remote::ClientBlock& client)
{
    Printed p;
    for (const auto& n : env.notifications) {
        const auto progress = n.detail.find("progress");
        if (progress == n.detail.end())
            continue;
        const nlohmann::json frame = {
            {"type", "progress"},
            {"code", n.code},
            {"message", n.message},
            {"progress", *progress},
        };
        p.stdoutText += Dump(frame) + "\n";
    }
    const nlohmann::json result = {
        {"type", "result"},
        {"count", 1},
        {"envelope", WithClient(env, client)},
    };
    p.stdoutText += Dump(result) + "\n";
    return p;
}

} // namespace

Printed Render(const Arcane::Remote::Envelope& env, const Arcane::Remote::ClientBlock& client, RenderOptions opt)
{
    Printed p;
    switch (opt.format) {
        case Format::Json:
            p.stdoutText = Dump(WithClient(env, client)) + "\n";
            break;
        case Format::Ndjson:
            p = RenderNdjson(env, client);
            break;
        case Format::Human:
            p = RenderHuman(env);
            break;
    }
    // json and ndjson carry every error and warning inside the envelope on
    // stdout; only the human format writes diagnostics to stderr.
    if (opt.format != Format::Human)
        p.stderrText.clear();
    p.stdoutText = Arcane::Remote::RedactSecrets(p.stdoutText, client.token);
    p.stderrText = Arcane::Remote::RedactSecrets(p.stderrText, client.token);
    return p;
}

} // namespace Servitor

#endif
