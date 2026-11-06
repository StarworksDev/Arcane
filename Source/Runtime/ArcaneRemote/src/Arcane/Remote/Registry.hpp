#pragma once
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/Schema.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>

#if !defined(ARC_BUILD_DIST)

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Remote {

struct Call {
    std::string id;
    std::string command;
    nlohmann::json args = nlohmann::json::object();
    std::string world;          // options.world
    bool readOnly = false;      // options.readOnly
    std::uint32_t timeoutMs = 30000; // milliseconds, copied from the wire; the listener does not scale
    std::string callerId;
};

struct CommandResult {
    Envelope envelope;
    std::uint64_t completesJob = 0; // non-zero: dispatcher holds the waiter until that job finishes
};

using CommandFn = CommandResult (*)(const Call& call, void* user);

struct Desc {
    std::string name;           // dotted wire name, the value of Call::command
    std::string summary;
    Access access = Access::Observe;
    Mutates mutates = Mutates::None;
    PlayPolicy play = PlayPolicy::Any;
    ExecThread thread = ExecThread::Main;
    bool needsFrame = false;
    std::string module;         // empty = host
    const Astra::TypeMeta* argsMeta = nullptr;
    CommandFn fn = nullptr;
    void* user = nullptr;
    // Per-call access (ruling F19). When non-null, Task 8's state gate and its
    // audit use accessFor(call) instead of the static `access` (for example
    // capture.compare answers Drive when --bless is set). Last field, so the
    // ARC_REMOTE_COMMAND brace below still compiles.
    Access (*accessFor)(const Call& call) = nullptr;
    // Spec s4.6 Rule 3 (Task 20 fix round 1): true when the command reads
    // Call::world (it resolves --world itself, ResolveWorld). The dispatcher
    // refuses a non-empty world on every other command with arg.inapplicable
    // (exit 2) and never invokes it, so --world is never silently dropped.
    bool worldScoped = false;
};

// The process's remote command table (spec s5.1). One instance lives in ArcaneRemote,
// so a registration from any module's static initializer lands here. Commands
// run on the host main thread; the listener thread never touches the registry.
class ARC_REMOTE_API CommandRegistry {
public:
    static CommandRegistry& Get();

    // Refuses (false, reason in *error when non-null): an empty summary; an
    // empty name, an empty segment, or a segment outside [a-z0-9]+; a duplicate
    // name; a null argsMeta; a null fn; Drive with mutates None.
    bool Register(Desc desc, std::string* error);
    // Drops every command registered under `module`. "" names the host's own
    // commands, which are never dropped.
    void UnregisterModule(std::string_view module);
    // Increments on every change to the table (spec s5 item 8).
    std::uint32_t Generation() const;
    // The pointer stays valid until that command's module unregisters.
    const Desc* Find(std::string_view name) const;
    std::vector<const Desc*> List() const;      // sorted by name
    nlohmann::json SchemaOf(std::string_view name) const; // null when unregistered
    nlohmann::json Catalogue() const;           // {"generation": N, "commands": [...]} sorted by name
    // Runs the command's function. Does not apply the state gate, readOnly, or
    // accessFor (Task 8's dispatcher does).
    CommandResult Invoke(const Call& call);
    void SetHostKind(HostKind kind);   // HostSession::Start sets it (Task 11); default HostKind::Editor
    HostKind GetHostKind() const;

private:
    CommandRegistry() = default;

    mutable std::mutex m_mutex;
    std::map<std::string, Desc, std::less<>> m_commands;
    std::atomic<std::uint32_t> m_generation{0};
    std::atomic<HostKind> m_hostKind{HostKind::Editor};
};

#define ARC_REMOTE_COMMAND(ident, nameLit, summaryLit, access, mutates, play, thread, ArgsType, fn) \
    static const bool ident = ::Arcane::Remote::CommandRegistry::Get().Register( \
        ::Arcane::Remote::Desc{(nameLit), (summaryLit), (access), (mutates), (play), (thread), \
                            false, {}, ::Astra::GetMeta<ArgsType>(), (fn), nullptr}, nullptr)

} // namespace Arcane::Remote

#endif
