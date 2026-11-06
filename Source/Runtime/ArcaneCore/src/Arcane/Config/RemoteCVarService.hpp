#pragma once

// The server surface (settings spec s9): get / set / list / explain for an
// admin, in the ServerAdmin context (s3.2), transport-agnostic. ArcaneServer
// wires it to its stdin console; Aphelyon's services to their HMAC-signed
// internal RPC.
//
// - Protected values are NEVER readable through this service, on any
//   transport: get/explain refuse, list prints "<protected>", a set is
//   allowed but its audit record carries "<protected>" for old and new, and
//   the cvar_explain command is refused (use the explain op) so history
//   cannot leak through the command path.
// - Commands run only via op set, and only when they carry ServerCanExecute.
//   get/explain on a command is refused without executing it.
// - list applies ServerAdmin visibility to every entry (Editor-audience
//   names, including Protected ones, are absent).
// - Hidden names answer "unknown", exactly like names that do not exist.
// - Every set and every command run, allowed or refused, is handed to the
//   injected sink (who, old, new, outcome, when). A set that names a known
//   cvar but omits the value is a refused set and is audited as Deny.
//   Unknown names are not audited. This sink is the SERVICE's audit; a host
//   installs it OR the registry-wide SetAuditSink for the same destination,
//   not both, or Server sets are recorded twice.
//
// THREADING: Handle runs on the registry's writer thread (ArcaneServer's tick
// thread; Aphelyon serializes callers under a mutex). It publishes after a
// successful set, so the reply shows the value now in effect.

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Core/Api.hpp>

#include <string>
#include <string_view>

namespace Arcane
{
    struct RemoteCVarRequest
    {
        std::string op;          // get | set | list | explain
        std::string name;        // the cvar or command; list: an optional dotted prefix
        std::string value;       // set: the new value as console text; a command: its arguments
        std::string callerId;    // who is asking, as the transport authenticated it
        CVarContext context = CVarContext::ServerAdmin;   // LAST: {op, name, value, callerId} still compiles
    };

    struct RemoteCVarResponse
    {
        bool ok = false;
        std::string text;
        // Structured form (Servitor P0). valueJson is the JSON text "null" when
        // there is no value; the other strings are empty when not filled.
        std::string valueJson = "null";
        std::string type;
        std::string setBy;
        std::string audience;
        std::string scope;
        std::string apply;
        std::string flags;
        std::string publishedJson;
        std::string pendingJson;
        std::string historyJson;   // JSON array of {by, value, module}, oldest first
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif
    class ARC_CORE_API RemoteCVarService
    {
    public:
        explicit RemoteCVarService(CVarRegistry& registry, CVarAuditFn sink = nullptr, void* sinkUser = nullptr) noexcept;

        RemoteCVarResponse Handle(const RemoteCVarRequest& request);

    private:
        enum class Kind : std::uint8_t { Unknown, CVar, Command };
        struct Target { Kind kind = Kind::Unknown; CVarFlags flags = CVarFlags::None; };

        [[nodiscard]] Target Resolve(std::string_view name) const;
        void FillStructured(RemoteCVarResponse& res, const std::string& name) const;
        [[nodiscard]] std::string ReadValue(std::string_view name, const CVarCaller& caller, CVarContext context);
        RemoteCVarResponse List(const std::string& prefix, const CVarCaller& caller, CVarContext context);
        RemoteCVarResponse RunCommand(const std::string& op, const std::string& name, const std::string& args,
                                      const std::string& callerId, const CVarCaller& caller, CVarFlags flags,
                                      CVarContext context);
        void Audit(std::string_view name, std::string_view callerId, std::string before, std::string after,
                   PolicyVerdict verdict, CVarContext context);

        CVarRegistry* m_registry;
        CVarAuditFn m_sink;
        void* m_sinkUser;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
