#include <Arcane/Remote/Registry.hpp>

#if !defined(ARC_BUILD_DIST)

#include <cstddef>
#include <utility>

namespace Arcane::Remote
{

namespace
{

ARC_CONSTANT("wire protocol: s6.4 command.unknown lists at most 8 candidates")
constexpr std::size_t kMaxCandidates = 8;

// Dotted name, every segment [a-z0-9]+ (spec s5.1).
bool ValidName(std::string_view name)
{
    if (name.empty())
        return false;
    std::size_t segment = 0;
    for (const char c : name)
    {
        if (c == '.')
        {
            if (segment == 0)
                return false;
            segment = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return false;
        ++segment;
    }
    return segment != 0;
}

std::string_view FirstSegment(std::string_view name)
{
    return name.substr(0, name.find('.'));
}

// The catalogue spells each enumerator in lower case (`observe`, `drive`).
const char* ToCatalogue(Access a)
{
    switch (a) {
        case Access::Observe: return "observe";
        case Access::Drive: return "drive";
    }
    return "observe";
}

const char* ToCatalogue(Mutates m)
{
    switch (m) {
        case Mutates::None: return "none";
        case Mutates::Scene: return "scene";
        case Mutates::Project: return "project";
        case Mutates::Preferences: return "preferences";
        case Mutates::PlayMode: return "playmode";
    }
    return "none";
}

const char* ToCatalogue(PlayPolicy p)
{
    switch (p) {
        case PlayPolicy::Any: return "any";
        case PlayPolicy::EditOnly: return "editonly";
        case PlayPolicy::PlayingOnly: return "playingonly";
    }
    return "any";
}

const char* ToCatalogue(ExecThread t)
{
    switch (t) {
        case ExecThread::Main: return "main";
        case ExecThread::Service: return "service";
        case ExecThread::Job: return "job";
    }
    return "main";
}

bool Refuse(std::string* error, std::string reason)
{
    if (error)
        *error = std::move(reason);
    return false;
}

} // namespace

CommandRegistry& CommandRegistry::Get()
{
    static CommandRegistry instance;
    return instance;
}

bool CommandRegistry::Register(Desc desc, std::string* error)
{
    if (desc.summary.empty())
        return Refuse(error, "command '" + desc.name + "' has an empty summary");
    if (!ValidName(desc.name))
        return Refuse(error, "command name '" + desc.name + "' is not dotted [a-z0-9]+ segments");
    if (desc.argsMeta == nullptr)
        return Refuse(error, "command '" + desc.name + "' has no reflected args type");
    if (desc.fn == nullptr)
        return Refuse(error, "command '" + desc.name + "' has no function");
    if (desc.access == Access::Drive && desc.mutates == Mutates::None)
        return Refuse(error, "command '" + desc.name + "' is Drive with mutates None; declare it Observe");

    std::lock_guard lock(m_mutex);
    if (m_commands.contains(desc.name))
        return Refuse(error, "command '" + desc.name + "' is already registered");
    std::string key = desc.name;
    m_commands.emplace(std::move(key), std::move(desc));
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void CommandRegistry::UnregisterModule(std::string_view module)
{
    if (module.empty())
        return;
    std::lock_guard lock(m_mutex);
    const std::size_t dropped = std::erase_if(m_commands, [module](const auto& entry) {
        return entry.second.module == module;
    });
    if (dropped != 0)
        m_generation.fetch_add(1, std::memory_order_acq_rel);
}

std::uint32_t CommandRegistry::Generation() const
{
    return m_generation.load(std::memory_order_acquire);
}

const Desc* CommandRegistry::Find(std::string_view name) const
{
    std::lock_guard lock(m_mutex);
    const auto it = m_commands.find(name);
    return it == m_commands.end() ? nullptr : &it->second;
}

std::vector<const Desc*> CommandRegistry::List() const
{
    std::lock_guard lock(m_mutex);
    std::vector<const Desc*> out;
    out.reserve(m_commands.size());
    for (const auto& entry : m_commands)
        out.push_back(&entry.second);
    return out;
}

nlohmann::json CommandRegistry::SchemaOf(std::string_view name) const
{
    const Desc* desc = Find(name);
    if (desc == nullptr || desc->argsMeta == nullptr)
        return nullptr;
    return SchemaFor(*desc->argsMeta);
}

nlohmann::json CommandRegistry::Catalogue() const
{
    std::lock_guard lock(m_mutex);
    nlohmann::json commands = nlohmann::json::array();
    for (const auto& entry : m_commands)
    {
        const Desc& desc = entry.second;
        commands.push_back({
            { "name", desc.name },
            { "summary", desc.summary },
            { "access", ToCatalogue(desc.access) },
            { "mutates", ToCatalogue(desc.mutates) },
            { "play", ToCatalogue(desc.play) },
            { "thread", ToCatalogue(desc.thread) },
            { "needsFrame", desc.needsFrame },
            { "module", desc.module },
        });
    }
    return nlohmann::json{ { "generation", Generation() }, { "commands", std::move(commands) } };
}

CommandResult CommandRegistry::Invoke(const Call& call)
{
    CommandFn fn = nullptr;
    void* user = nullptr;
    std::vector<std::string> candidates;
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_commands.find(call.command);
        if (it != m_commands.end())
        {
            fn = it->second.fn;
            user = it->second.user;
        }
        else
        {
            const std::string_view first = FirstSegment(call.command);
            for (const auto& entry : m_commands)
            {
                if (candidates.size() == kMaxCandidates)
                    break;
                if (FirstSegment(entry.first) == first)
                    candidates.push_back(entry.first);
            }
        }
    }
    if (fn != nullptr)
        return fn(call, user);

    CommandResult result;
    const HostKind kind = GetHostKind();
    if (call.command.starts_with("editor.") && kind != HostKind::Editor)
    {
        // Ruling F-1: editor commands register from the editor program only,
        // so a runtime or server never carries the name.
        result.envelope = MakeError(call.command, "command.host-kind",
                                    call.command + " is an editor command; this host is " + ToString(kind),
                                    "attach to an editor host");
        return result;
    }
    result.envelope = MakeError(call.command, "command.unknown",
                                "no command named '" + call.command + "' on this host", "servitor list");
    result.envelope.errors[0].candidates = std::move(candidates);
    return result;
}

void CommandRegistry::SetHostKind(HostKind kind)
{
    m_hostKind.store(kind, std::memory_order_release);
}

HostKind CommandRegistry::GetHostKind() const
{
    return m_hostKind.load(std::memory_order_acquire);
}

} // namespace Arcane::Remote

#endif
