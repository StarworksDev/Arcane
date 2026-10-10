#include <Arcane/Config/CVarRegistry.hpp>

#include <Arcane/Config/CVarFormat.hpp>
#include <Arcane/Config/CVarModule.hpp>
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarRef.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace Arcane
{
    namespace
    {
        const char* SetByName(SetBy by)
        {
            switch (by)
            {
            case SetBy::Default: return "Default";
            case SetBy::EngineConfig: return "EngineConfig";
            case SetBy::Plugin: return "Plugin";
            case SetBy::Project: return "Project";
            case SetBy::EditorUser: return "EditorUser";
            case SetBy::User: return "User";
            case SetBy::CommandLine: return "CommandLine";
            case SetBy::Code: return "Code";
            case SetBy::Console: return "Console";
            }
            return "?";
        }

        bool TakesRange(CVarType t)
        {
            return (t >= CVarType::Int32 && t <= CVarType::Float64) || (t >= CVarType::Color && t <= CVarType::Vec4);
        }

        // True when some component of `lo` exceeds `hi` (same type, checked by the caller).
        bool RangeEmpty(const CVarValue& lo, const CVarValue& hi)
        {
            switch (lo.type)
            {
            case CVarType::Int32: return lo.AsInt32() > hi.AsInt32();
            case CVarType::UInt32: return lo.AsUInt32() > hi.AsUInt32();
            case CVarType::Int64: return lo.AsInt64() > hi.AsInt64();
            case CVarType::UInt64: return lo.AsUInt64() > hi.AsUInt64();
            case CVarType::Float32: return lo.AsFloat32() > hi.AsFloat32();
            case CVarType::Float64: return lo.AsFloat64() > hi.AsFloat64();
            case CVarType::Color:
            {
                const CVarColor a = lo.AsColor();
                const CVarColor b = hi.AsColor();
                return a.r > b.r || a.g > b.g || a.b > b.b || a.a > b.a;
            }
            case CVarType::Vec2: { const CVarVec2 a = lo.AsVec2(); const CVarVec2 b = hi.AsVec2(); return a.x > b.x || a.y > b.y; }
            case CVarType::Vec3: { const CVarVec3 a = lo.AsVec3(); const CVarVec3 b = hi.AsVec3(); return a.x > b.x || a.y > b.y || a.z > b.z; }
            case CVarType::Vec4:
            {
                const CVarVec4 a = lo.AsVec4();
                const CVarVec4 b = hi.AsVec4();
                return a.x > b.x || a.y > b.y || a.z > b.z || a.w > b.w;
            }
            default: return false;
            }
        }

        // settings spec s4.1: the hint a String (or, for "slider", a number)
        // carries to the settings window.
        bool WidgetFits(std::string_view widget, CVarType type)
        {
            if (widget.empty()) return true;
            if (widget == "slider") return type >= CVarType::Int32 && type <= CVarType::Float64;
            if (widget == "path:file" || widget == "path:dir" || widget == "keychord" || widget == "font")
                return type == CVarType::String;
            if (widget.starts_with("asset:")) return widget.size() > 6 && type == CVarType::String;
            return false;
        }

        // Spec s3.2's default table, before any game policy. `absent`: the
        // setting does not exist for this caller (an Editor setting, or a
        // Hidden one, outside the editor), so it is reported as unknown rather
        // than denied.
        struct AccessRule
        {
            bool absent = false;
            bool allowed = false;
        };

        AccessRule DefaultAccess(Audience audience, CVarFlags flags, CVarContext ctx, bool write,
                                 bool cheatsOn, bool clientMaySetServer)
        {
            if (ctx == CVarContext::Editor) return { false, true };
            if (audience == Audience::Editor || HasFlag(flags, CVarFlags::Hidden)) return { true, false };
            if (!write)
                return { false, !HasFlag(flags, CVarFlags::Protected) || ctx == CVarContext::ServerAdmin };
            const bool cheat = HasFlag(flags, CVarFlags::Cheat);
            switch (audience)
            {
            case Audience::Game:       return { false, cheat && cheatsOn };
            case Audience::PlayerSafe: return { false, !cheat || cheatsOn };
            case Audience::Server:
                if (ctx == CVarContext::Client) return { false, clientMaySetServer && (!cheat || cheatsOn) };
                return { false, !cheat || cheatsOn };
            case Audience::Editor:     break;
            }
            return { true, false };
        }

        bool PublishedBool(const CVarRegistry& registry, std::string_view name, bool fallback)
        {
            const auto v = registry.Get(registry.Find(name));
            return v && v->type == CVarType::Bool ? v->AsBool() : fallback;
        }

        // The context and caller of the Execute that is dispatching the current
        // command. CommandFn carries neither, and the registry's own cvarlist /
        // cvar_explain must list and print only what that context, for that
        // caller, may read (s3.2: the table, then the game's policy).
        thread_local CVarContext tExecutingContext = CVarContext::Editor;
        thread_local const CVarCaller* tExecutingCaller = nullptr;

        struct ExecutingContextScope
        {
            CVarContext outerContext;
            const CVarCaller* outerCaller;
            ExecutingContextScope(CVarContext ctx, const CVarCaller* caller)
                : outerContext(tExecutingContext), outerCaller(tExecutingCaller)
            {
                tExecutingContext = ctx;
                tExecutingCaller = caller;
            }
            ~ExecutingContextScope()
            {
                tExecutingContext = outerContext;
                tExecutingCaller = outerCaller;
            }
        };

        // "maxFrameDeltaSeconds" -> "Max Frame Delta Seconds": the last dotted
        // segment, a space before each lower->upper boundary, the first letter capitalised.
        std::string DeriveDisplayName(std::string_view name)
        {
            const std::size_t dot = name.rfind('.');
            const std::string_view leaf = dot == std::string_view::npos ? name : name.substr(dot + 1);
            std::string out;
            out.reserve(leaf.size() + 4);
            for (std::size_t i = 0; i < leaf.size(); ++i)
            {
                const char c = leaf[i];
                const bool upper = c >= 'A' && c <= 'Z';
                const bool prevUpper = i > 0 && leaf[i - 1] >= 'A' && leaf[i - 1] <= 'Z';
                if (i > 0 && upper && !prevUpper) out.push_back(' ');
                out.push_back(i == 0 && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
            }
            return out;
        }

        CommandResult ListCommand(std::string_view, void* user);
        CommandResult ExplainCommand(std::string_view args, void* user);
    }

    struct CVarRegistry::Slot
    {
        std::uint32_t generation = 1;
        bool alive = false;
        CVarType type = CVarType::Bool;
        CVarFlags flags = CVarFlags::None;
        std::string name;
        std::string help;
        std::string declaredBy;
        const void* source = nullptr;   // declaring code address (RegisterDeclaredCVar caller, or Register)
        std::optional<CVarValue> min;
        std::optional<CVarValue> max;
        CVarValue defaultValue = CVarValue::Bool(false);
        std::string displayName;
        std::string keywords;
        std::string widget;
        Audience audience = Audience::Game;
        SettingScope scope = SettingScope::Project;
        ApplyMode apply = ApplyMode::Live;
        std::int32_t order = 0;
        std::string categoryPath;
        std::vector<std::string> enumNames;
        std::string group;
        CVarValue published = CVarValue::Bool(false);
        std::vector<CVarHistoryRecord> history;
        bool dirty = false;
        std::uint64_t settingsType = 0;   // the settings struct this cvar is a field of; 0 = none
        struct Callback { ChangeFn fn = nullptr; void* user = nullptr; std::string module; };   // module: the ScopedModule at AddCallback
        std::vector<Callback> callbacks;
    };

    struct CVarRegistry::Command
    {
        std::string name;
        std::string help;
        std::string module;
        CVarFlags flags = CVarFlags::None;
        CommandFn fn = nullptr;
        LegacyCommandFn legacy = nullptr;   // exactly one of fn / legacy is set
        void* user = nullptr;
        bool alive = true;
        bool builtin = false;               // the registry's own (cvarlist, cvar_explain): survives every UnregisterModule
    };

    struct CVarRegistry::SettingsBinding
    {
        std::uint64_t                typeHash = 0;
        std::string                  typeName;
        std::string                  module;
        std::shared_ptr<void>      (*make)() = nullptr;
        std::vector<CVarHandle>      handles;   // per field; stale = refused (Dev in Dist, or logged error)
        std::vector<SettingsWriteFn> writers;   // per field
    };

    struct CVarRegistry::Impl
    {
        bool devCvars = true;
        std::vector<Slot> slots;
        std::vector<std::uint32_t> freeSlots;
        std::unordered_map<std::string, std::uint32_t> byName;
        std::vector<Command> commands;
        std::unordered_map<std::string, std::uint32_t> commandByName;
        std::map<std::string, std::string> aliases;     // old -> new
        std::unordered_set<std::string> warnedAliases;  // old names already warned about
        std::unordered_set<std::string> compiledOut;    // Dev names refused because devCvars=false (spec s12: not unknown keys)
        bool publishing = false;
        std::string lastError;

        // The published snapshot (settings spec s4.6, O5). Readers load it
        // atomically from any thread; only the main thread stores, and never
        // mutates one it has stored. Xcode 16 libc++ needs the shared_ptr
        // atomic free functions; other toolchains have atomic<shared_ptr>.
#if defined(__APPLE__)
        std::shared_ptr<const CVarSnapshot> snapshot;
#else
        std::atomic<std::shared_ptr<const CVarSnapshot>> snapshot;
#endif
        std::uint64_t                                    serial = 0;
        std::uint64_t                                    revision = 0;
        std::thread::id                                  mainThread = std::this_thread::get_id();

        std::vector<SettingsBinding> settings;
        // The two snapshots before the current one, kept alive so a
        // Settings<T>() reference survives two publishes (settings arc S2).
        std::array<std::shared_ptr<const CVarSnapshot>, 2> retired;

        struct ModuleImage
        {
            std::string module;
            const void* base = nullptr;
            std::size_t size = 0;
        };
        std::vector<ModuleImage> images;

        std::string_view NameForAddress(const void* p) const noexcept
        {
            if (!p) return {};
            const auto* addr = static_cast<const unsigned char*>(p);
            for (const ModuleImage& img : images)
            {
                if (!img.base || img.size == 0) continue;
                const auto* base = static_cast<const unsigned char*>(img.base);
                if (addr >= base && addr < base + img.size) return img.module;
            }
            return {};
        }

        void RebuildSnapshot(CVarRegistry& registry)
        {
            auto next = std::make_shared<CVarSnapshot>();
            next->serial = ++serial;
            next->entries.reserve(slots.size());
            for (const Slot& s : slots)
                next->entries.push_back(CVarSnapshot::Entry{ s.generation, s.alive, s.published });
            registry.FillSettingsBlocks(*next, {});
            registry.StoreSnapshot(std::move(next));
        }

        // Both RegisterCommand overloads land here; exactly one of fn / legacy is set.
        bool AddCommand(std::string name, CVarFlags flags, std::string help, std::string module,
                        CommandFn fn, LegacyCommandFn legacy, void* user)
        {
            if (name.empty() || (!fn && !legacy)) return false;
            if (byName.contains(name) || commandByName.contains(name) || aliases.contains(name)) return false;
            if (HasFlag(flags, CVarFlags::Dev) && !devCvars) return false;
            if (module.empty())
            {
                const void* source = fn ? reinterpret_cast<const void*>(fn) : reinterpret_cast<const void*>(legacy);
                if (const std::string_view fromImage = NameForAddress(source); !fromImage.empty())
                    module = std::string(fromImage);
            }
            const std::uint32_t index = static_cast<std::uint32_t>(commands.size());
            Command command;
            command.name = std::move(name);
            command.help = std::move(help);
            command.module = std::move(module);
            command.flags = flags;
            command.fn = fn;
            command.legacy = legacy;
            command.user = user;
            commands.push_back(std::move(command));
            commandByName.emplace(commands.back().name, index);
            return true;
        }

        // The table plus server.cheatsAllowed: outside the editor, the cheats
        // gate can be turned ON only where the host allows cheats at all.
        AccessRule Access(const CVarRegistry& self, const Slot& slot, CVarContext ctx, bool write,
                          const CVarValue* value) const
        {
            AccessRule rule = DefaultAccess(slot.audience, slot.flags, ctx, write, self.CheatsEnabled(),
                                            PublishedBool(self, "server.allowClientSetServer", false));
            if (rule.allowed && write && ctx != CVarContext::Editor && slot.name == "server.cheats"
                && value && value->type == CVarType::Bool && value->AsBool()
                && !PublishedBool(self, "server.cheatsAllowed", true))
                rule.allowed = false;
            return rule;
        }

        CVarPolicyFn policy = nullptr;
        void*        policyUser = nullptr;
        std::string  policyModule;
        CVarAuditFn  audit = nullptr;
        void*        auditUser = nullptr;

        struct Decision
        {
            bool          absent = false;
            bool          allowed = false;
            PolicyVerdict verdict = PolicyVerdict::Default;
        };

        // The table, then the game's policy (settings spec s3.2). The policy
        // is never asked in the Editor context, about an absent setting (it
        // cannot reach Editor or Hidden), or about a Protected read outside
        // ServerAdmin (it cannot make one readable).
        Decision Decide(const CVarRegistry& self, const Slot& slot, CVarContext ctx, const CVarCaller* caller,
                        bool write, const CVarValue* value) const
        {
            if (ctx == CVarContext::Editor) return { false, true, PolicyVerdict::Default };
            const AccessRule rule = Access(self, slot, ctx, write, value);
            if (rule.absent) return { true, false, PolicyVerdict::Default };   // the policy cannot reach it
            const bool protectedRead = !write && HasFlag(slot.flags, CVarFlags::Protected) && ctx != CVarContext::ServerAdmin;
            if (!policy || protectedRead) return { false, rule.allowed, PolicyVerdict::Default };
            const CVarInfo info{ slot.name, slot.type, slot.flags, slot.audience };
            const CVarRequest request{ ctx, caller, write };
            const PolicyVerdict verdict = policy(info, request, policyUser);
            if (verdict == PolicyVerdict::Allow) return { false, true, verdict };
            if (verdict == PolicyVerdict::Deny) return { false, false, verdict };
            return { false, rule.allowed, PolicyVerdict::Default };
        }

        void Audit(const std::string& name, const CVarCaller* caller, CVarContext ctx, PolicyVerdict verdict,
                   std::string oldValue, std::string newValue) const
        {
            if (!audit) return;
            CVarAuditRecord record;
            record.name = name;
            record.callerId = caller ? std::string(caller->id) : std::string();
            record.oldValue = std::move(oldValue);
            record.newValue = std::move(newValue);
            record.context = ctx;
            record.verdict = verdict;
            record.unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            audit(record, auditUser);
        }

        // Move each listed dirty slot's winning record into `published`.
        // Returns the slots whose value changed.
        std::vector<std::uint32_t> Promote(const std::vector<std::uint32_t>& indices)
        {
            std::vector<std::uint32_t> changed;
            for (const std::uint32_t i : indices)
            {
                if (i >= slots.size()) continue;
                Slot& slot = slots[i];
                if (!slot.alive || !slot.dirty) continue;
                slot.dirty = false;
                const CVarValue& next = slot.history.back().value;
                if (!(next == slot.published))
                {
                    slot.published = next;
                    changed.push_back(i);
                }
            }
            return changed;
        }

        // Fire each changed slot's callbacks. Each list is COPIED first (spec
        // s4.5, O4): a callback may add a callback, which first fires on the
        // next change, or register a cvar, which can move the slot array.
        void Dispatch(const std::vector<std::uint32_t>& changed)
        {
            for (const std::uint32_t i : changed)
            {
                if (i >= slots.size() || !slots[i].alive) continue;
                const CVarHandle handle{ i, slots[i].generation };
                const std::vector<Slot::Callback> callbacks = slots[i].callbacks;
                for (const Slot::Callback& cb : callbacks)
                    if (cb.fn) cb.fn(handle, cb.user);
            }
        }

        // Drop the Console and Code records of every Cheat cvar. Returns the
        // slots whose history changed; they are now dirty.
        std::vector<std::uint32_t> DropCheatHistory()
        {
            std::vector<std::uint32_t> touched;
            for (std::uint32_t i = 0; i < slots.size(); ++i)
            {
                Slot& slot = slots[i];
                if (!slot.alive || !HasFlag(slot.flags, CVarFlags::Cheat)) continue;
                const auto before = slot.history.size();
                std::erase_if(slot.history, [](const CVarHistoryRecord& h) {
                    return h.by == SetBy::Console || h.by == SetBy::Code;
                });
                if (slot.history.empty())
                    slot.history.push_back(CVarHistoryRecord{ SetBy::Default, slot.published, {} });
                if (slot.history.size() != before)
                {
                    slot.dirty = true;
                    touched.push_back(i);
                }
            }
            return touched;
        }
    };

    namespace
    {
        // The CVarModuleScope stack of this thread (settings spec s4.3). A
        // deque, so pushing an inner scope never moves an outer scope's string.
        thread_local std::deque<std::string> t_moduleScopes;
        // Set by RegisterDeclaredCVar so Register records the PLUGIN's
        // instantiating frame, not RegisterDeclaredCVar in ArcaneCore.
        thread_local const void* t_registerSource = nullptr;

#if defined(_MSC_VER)
#define ARC_CVAR_RETURN_ADDRESS() _ReturnAddress()
#elif defined(__GNUC__)
#define ARC_CVAR_RETURN_ADDRESS() __builtin_return_address(0)
#else
#define ARC_CVAR_RETURN_ADDRESS() nullptr
#endif

        bool AddressInRange(const void* p, const void* base, std::size_t size) noexcept
        {
            if (!p || !base || size == 0) return false;
            const auto* addr = static_cast<const unsigned char*>(p);
            const auto* b = static_cast<const unsigned char*>(base);
            return addr >= b && addr < b + size;
        }
    }

    CVarModuleScope::CVarModuleScope(std::string_view module) { t_moduleScopes.emplace_back(module); }
    CVarModuleScope::~CVarModuleScope() { if (!t_moduleScopes.empty()) t_moduleScopes.pop_back(); }

    std::string_view CVarRegistry::ScopedModule() noexcept
    {
        return t_moduleScopes.empty() ? std::string_view{} : std::string_view(t_moduleScopes.back());
    }

    std::string_view CVarRegistry::CurrentModule() noexcept
    {
        const std::string_view scoped = ScopedModule();
        return scoped.empty() ? std::string_view(ARC_MODULE_NAME_STRING) : scoped;
    }

    std::string CVarRegistry::ModuleOf(CVarHandle handle) const
    {
        if (handle.index >= m->slots.size()) return {};
        const Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return {};
        return slot.declaredBy;
    }

    CVarRegistry::CVarRegistry() : CVarRegistry(true) {}

    CVarRegistry::CVarRegistry(bool devCvars) : m(new Impl)
    {
        m->devCvars = devCvars;
        m->RebuildSnapshot(*this);   // readers never see a null snapshot
        // The registry's own: declared by the module it lives in (ArcaneCore)
        // and, as built-ins, kept through every UnregisterModule.
        const bool listed = RegisterCommand("cvarlist", CVarFlags::ServerCanExecute, "List registered cvars.",
                                            std::string(CurrentModule()), &ListCommand, this);
        const bool explained = RegisterCommand("cvar_explain", CVarFlags::ServerCanExecute, "Show who set a cvar and the history under it.",
                                               std::string(CurrentModule()), &ExplainCommand, this);
        (void)listed;
        (void)explained;
        m->commands[m->commandByName.at("cvarlist")].builtin = true;
        m->commands[m->commandByName.at("cvar_explain")].builtin = true;
        // node-page phase s8.2: the command line's history depth. Registered on
        // EVERY registry (test registries included), so through CVarDesc rather
        // than ARC_CVAR (which targets Get() only). ConsoleModel reads it.
        // Game / Pref-P per the inventory's R1: it serves both consoles. The
        // module is left empty here and below: Register fills CurrentModule().
        const CVarHandle history = Register(CVarDesc{
            .name = "console.historySize", .type = CVarType::Int32, .defaultValue = CVarValue::Int32(64),
            .min = CVarValue::Int32(1), .max = CVarValue::Int32(1024), .flags = CVarFlags::Archive,
            .help = "Command-line history depth.",
            .audience = Audience::Game, .scope = SettingScope::PreferencesProject });
        (void)history;
        // Settings sweep S6-41: the console's output line cap, beside the
        // history depth for the same reason (every registry; ConsoleModel reads
        // the registry it is handed). 0 = unbounded, the pre-sweep behaviour.
        const CVarHandle maxLines = Register(CVarDesc{
            .name = "console.maxLines", .type = CVarType::Int32, .defaultValue = CVarValue::Int32(0),
            .min = CVarValue::Int32(0), .max = CVarValue::Int32(1000000), .flags = CVarFlags::Archive,
            .help = "Console output lines kept; the oldest drop past it. 0 keeps every line.",
            .audience = Audience::Game, .scope = SettingScope::PreferencesProject });
        (void)maxLines;
        // Settings spec s3.2: the cheats gate and the two engine knobs, on EVERY
        // registry (test registries included), like console.historySize. They
        // are Server audience: a LocalHost or ServerAdmin console sets them, and
        // a Client only reads them.
        const auto serverBool = [this](std::string_view name, bool def, CVarFlags flags, std::string_view help)
        {
            CVarDesc desc;
            desc.name = name;
            desc.type = CVarType::Bool;
            desc.defaultValue = CVarValue::Bool(def);
            desc.flags = flags;
            desc.help = help;
            desc.audience = Audience::Server;
            desc.scope = SettingScope::Project;
            desc.apply = ApplyMode::Live;
            (void)Register(desc);
        };
        serverBool("server.cheats", false, CVarFlags::Replicated,
                   "Allow Cheat settings to change (Source's sv_cheats). Turning it off reverts every Cheat setting.");
        serverBool("server.allowClientSetServer", false, CVarFlags::None,
                   "Let a connected client change Server settings when the game's policy leaves the decision to the engine.");
        serverBool("server.cheatsAllowed", true, CVarFlags::None,
                   "Whether server.cheats may be turned on outside the editor. A dedicated server sets its Project default to false.");
        (void)RegisterAlias("cheats", "server.cheats");
    }

    CVarRegistry::~CVarRegistry() { delete m; }

    const std::string& CVarRegistry::LastError() const { return m->lastError; }

    CVarRegistry& CVarRegistry::Get()
    {
#if defined(ARC_BUILD_DIST)
        static CVarRegistry registry{ false };
#else
        static CVarRegistry registry{ true };
#endif
        return registry;
    }

    namespace
    {
        CommandResult ListCommand(std::string_view, void* user)
        {
            auto* self = static_cast<CVarRegistry*>(user);
            std::string out;
            for (const CVarListEntry& e : self->List(tExecutingContext))
            {
                out += e.name;
                out += " (";
                out += CVarTypeName(e.type);
                out += ") ";
                out += e.help;
                out += '\n';
            }
            return { true, std::move(out) };
        }

        CommandResult ExplainCommand(std::string_view args, void* user)
        {
            auto* self = static_cast<CVarRegistry*>(user);
            while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
            const auto explained = self->Explain(args);
            const CVarHandle handle = explained ? self->Find(explained->name) : CVarHandle{};
            const auto meta = self->Metadata(handle);
            // The same read rule as a plain `name` (s3.2): absent outside the
            // editor reads as unknown; a Protected value never prints where the
            // plain read is refused; the game's policy may refuse the caller of
            // the Execute that dispatched this command.
            const AccessRule table = meta ? DefaultAccess(meta->audience, meta->flags, tExecutingContext, false, false, false)
                                          : AccessRule{ true, false };
            if (table.absent)
                return { false, "unknown cvar '" + std::string(args) + "'" };
            if (!self->CanRead(handle, tExecutingContext, tExecutingCaller))
            {
                // The table refuses a read for one reason only (Protected outside
                // ServerAdmin), and the policy is never asked about that one; any
                // other refusal is the policy's Deny.
                return { false, "denied: '" + explained->name + (table.allowed ? "' by the game's policy" : "' is protected") };
            }
            const std::vector<std::string>& enumNames = meta->enumNames;
            std::string out = explained->name;
            out += " = ";
            out += FormatCVarValue(explained->published, enumNames);
            out += " [";
            out += SetByName(explained->setBy);
            out += "]\n";
            for (const CVarHistoryRecord& h : explained->history)
            {
                out += "  ";
                out += SetByName(h.by);
                if (!h.module.empty()) { out += " ("; out += h.module; out += ")"; }
                out += " = ";
                out += FormatCVarValue(h.value, enumNames);
                out += '\n';
            }
            return { true, std::move(out) };
        }
    }

    CVarHandle CVarRegistry::Register(const CVarDesc& desc)
    {
        m->lastError.clear();
        const void* source = t_registerSource ? t_registerSource : ARC_CVAR_RETURN_ADDRESS();
        std::string module;
        if (!desc.module.empty())
            module = std::string(desc.module);
        else if (!ScopedModule().empty())
            module = std::string(ScopedModule());
        else if (const std::string_view fromImage = m->NameForAddress(source); !fromImage.empty())
            module = std::string(fromImage);
        else
            module = std::string(CurrentModule());
        const std::string name{ desc.name };
        const auto refuse = [&](const std::string& why) {
            m->lastError = "cvar '" + name + "' " + why;
            return CVarHandle{};
        };
        if (name.empty())
        {
            m->lastError = "a cvar needs a name";
            return {};
        }
        if (desc.type > CVarType::Enum)
            return refuse("has an unknown type");
        // settings spec s4.2: every row shows its help as the tooltip.
        if (desc.help.empty() && !HasFlag(desc.flags, CVarFlags::Hidden))
            return refuse("has no help text (every setting that is not Hidden needs one)");
        // O4: a default of another type would publish a value its readers cannot take.
        if (desc.defaultValue.type != desc.type)
            return refuse(std::string("has a ") + CVarTypeName(desc.defaultValue.type) + " default but is declared " +
                          CVarTypeName(desc.type));
        if (desc.min || desc.max)
        {
            if (!TakesRange(desc.type))
                return refuse(std::string("is ") + CVarTypeName(desc.type) + ", which takes no min/max");
            for (const std::optional<CVarValue>* bound : { &desc.min, &desc.max })
                if (*bound && (*bound)->type != desc.type)
                    return refuse(std::string("has a ") + CVarTypeName((*bound)->type) + " bound but is declared " +
                                  CVarTypeName(desc.type));
            if (desc.min && desc.max && RangeEmpty(*desc.min, *desc.max))
                return refuse("has an empty range (min > max)");
        }
        if (desc.type == CVarType::Enum)
        {
            if (desc.enumNames.empty())
                return refuse("is an Enum with no names (a reflected enum needs ARC_REFLECT_ENUM, not FLAGS, "
                              "visible ahead of its ARC_CVAR)");
            for (std::size_t i = 0; i < desc.enumNames.size(); ++i)
            {
                if (desc.enumNames[i].empty())
                    return refuse("has an empty Enum name");
                for (std::size_t j = 0; j < i; ++j)
                    if (desc.enumNames[j] == desc.enumNames[i])
                        return refuse("names the Enum value '" + desc.enumNames[i] + "' twice");
            }
            const std::int32_t ordinal = desc.defaultValue.AsEnum();
            if (ordinal < 0 || static_cast<std::size_t>(ordinal) >= desc.enumNames.size())
                return refuse("has an Enum default outside its " + std::to_string(desc.enumNames.size()) + " names");
        }
        else if (!desc.enumNames.empty())
            return refuse(std::string("declares Enum names but is ") + CVarTypeName(desc.type));
        if (!WidgetFits(desc.widget, desc.type))
            return refuse("has widget '" + std::string(desc.widget) + "', which does not fit its type (" +
                          CVarTypeName(desc.type) + ")");
        if (HasFlag(desc.flags, CVarFlags::Dev) && !m->devCvars)
        {
            m->compiledOut.insert(name);
            return refuse("is Dev and this build compiled it out");
        }
        if (const auto existing = m->byName.find(name); existing != m->byName.end())
            return refuse("already registered by module '" + m->slots[existing->second].declaredBy +
                          "', refused from '" + module + "'");
        if (m->commandByName.contains(name))
            return refuse("collides with a command");
        if (const auto alias = m->aliases.find(name); alias != m->aliases.end())
            return refuse("collides with an alias of '" + alias->second + "'");

        std::uint32_t index = 0;
        if (!m->freeSlots.empty())
        {
            index = m->freeSlots.back();
            m->freeSlots.pop_back();
        }
        else
        {
            index = static_cast<std::uint32_t>(m->slots.size());
            m->slots.emplace_back();
        }

        Slot& slot = m->slots[index];
        const std::uint32_t generation = slot.generation == 0 ? 1 : slot.generation;
        slot = Slot{};
        slot.generation = generation;
        slot.alive = true;
        slot.type = desc.type;
        slot.flags = desc.flags;
        slot.audience = desc.audience;
        // UserSettable is DERIVED from the audience (settings contract; spec
        // s3.2): a declaration's own bit is ignored, and Archive never implies it.
        slot.flags = static_cast<CVarFlags>(static_cast<std::uint32_t>(slot.flags)
                                            & ~static_cast<std::uint32_t>(CVarFlags::UserSettable));
        if (slot.audience == Audience::PlayerSafe || slot.audience == Audience::Server)
            slot.flags = slot.flags | CVarFlags::UserSettable;
        slot.name = desc.name;
        slot.help = desc.help;
        slot.declaredBy = module;
        slot.source = source;
        slot.min = desc.min;
        slot.max = desc.max;
        slot.published = Clamp(desc.defaultValue, slot.min, slot.max);
        slot.defaultValue = slot.published;
        slot.displayName = desc.displayName.empty() ? DeriveCVarDisplayName(desc.name) : std::string(desc.displayName);
        slot.keywords = desc.keywords;
        slot.widget = desc.widget;
        slot.scope = desc.scope;
        slot.apply = desc.apply;
        slot.order = desc.order;
        slot.categoryPath = desc.categoryPath.empty() ? DeriveCVarCategoryPath(desc.name) : std::string(desc.categoryPath);
        slot.enumNames = desc.enumNames;
        slot.group = desc.group;
        slot.history.push_back(CVarHistoryRecord{ SetBy::Default, slot.published, {} });
        slot.dirty = false;
        m->byName.emplace(slot.name, index);
        m->RebuildSnapshot(*this);
        ++m->revision;
        return CVarHandle{ index, generation };
    }

    bool CVarRegistry::RegisterCommand(std::string name, CVarFlags flags, std::string help,
                                       std::string module, CommandFn fn, void* user)
    {
        return m->AddCommand(std::move(name), flags, std::move(help), std::move(module), fn, nullptr, user);
    }

    bool CVarRegistry::RegisterCommand(std::string name, CVarFlags flags, std::string help,
                                       std::string module, LegacyCommandFn fn, void* user)
    {
        return m->AddCommand(std::move(name), flags, std::move(help), std::move(module), nullptr, fn, user);
    }

    CVarHandle CVarRegistry::Find(std::string_view name) const
    {
        const auto it = m->byName.find(std::string(name));
        if (it == m->byName.end()) return {};
        const Slot& slot = m->slots[it->second];
        if (!slot.alive) return {};
        return CVarHandle{ it->second, slot.generation };
    }

    bool CVarRegistry::IsCompiledOut(std::string_view name) const
    {
        return m->compiledOut.contains(std::string(name));
    }

    std::optional<CVarValue> CVarRegistry::Get(CVarHandle handle) const
    {
        if (handle.index >= m->slots.size()) return std::nullopt;
        const Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return std::nullopt;
        return slot.published;
    }

    std::optional<CVarMetadata> CVarRegistry::Metadata(CVarHandle handle) const
    {
        if (handle.index >= m->slots.size()) return std::nullopt;
        const Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return std::nullopt;
        CVarMetadata out;
        out.name = slot.name;
        out.help = slot.help;
        out.module = slot.declaredBy;
        out.type = slot.type;
        out.flags = slot.flags;
        out.defaultValue = slot.defaultValue;
        out.min = slot.min;
        out.max = slot.max;
        out.displayName = slot.displayName;
        out.keywords = slot.keywords;
        out.widget = slot.widget;
        out.audience = slot.audience;
        out.scope = slot.scope;
        out.apply = slot.apply;
        out.order = slot.order;
        out.categoryPath = slot.categoryPath;
        out.enumNames = slot.enumNames;
        out.group = slot.group;
        return out;
    }

    bool CVarRegistry::CanRead(CVarHandle handle, CVarContext ctx, const CVarCaller* caller) const
    {
        if (handle.index >= m->slots.size()) return false;
        const Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return false;
        if (ctx == CVarContext::Editor) return true;
        // Decide runs the policy (game code), which may move the slot array:
        // `slot` is not read after it.
        const Impl::Decision read = m->Decide(*this, slot, ctx, caller, false, nullptr);
        return !read.absent && read.allowed;
    }

    SetResult CVarRegistry::Set(CVarHandle handle, CVarValue value, SetBy by,
                                std::string_view sourceModule, CVarContext ctx, const CVarCaller* caller)
    {
        if (handle.index >= m->slots.size()) return SetResult::Stale;
        if (!m->slots[handle.index].alive || m->slots[handle.index].generation != handle.generation) return SetResult::Stale;
        if (value.type != m->slots[handle.index].type) return SetResult::TypeMismatch;
        if (m->slots[handle.index].type == CVarType::Enum &&
            (value.AsEnum() < 0 || static_cast<std::size_t>(value.AsEnum()) >= m->slots[handle.index].enumNames.size()))
            return SetResult::TypeMismatch;   // an ordinal outside the declared names
        // Inventory R2: an evidence-capture switch is `--set` only. No file
        // rung, console, settings window or policy may turn it off.
        if (HasFlag(m->slots[handle.index].flags, CVarFlags::CommandLineOnly)
            && by != SetBy::Default && by != SetBy::CommandLine && by != SetBy::Code)
            return SetResult::Denied;
        // Settings S7-SEC: a rung that lives in a project never names a
        // program nor sets a machine-wide preference; and a program is never
        // chosen from a console outside the editor (a remote admin, a player).
        // The config walk reports the refusal; this is the backstop for every
        // other caller.
        if (RungRefusal(m->slots[handle.index].flags, m->slots[handle.index].scope, by) != CVarRungRefusal::None)
            return SetResult::Denied;
        if (HasFlag(m->slots[handle.index].flags, CVarFlags::LaunchesProgram) && by == SetBy::Console
            && ctx != CVarContext::Editor)
            return SetResult::Denied;

        const Impl::Decision decision = m->Decide(*this, m->slots[handle.index], ctx, caller, true, &value);
        // The policy is game code. It may register a cvar and move the slot
        // array, so the slot is looked up again only after it has run.
        Slot& slot = m->slots[handle.index];
        // Every non-Editor change of a Server setting, and every policy denial
        // of one, reaches the audit sink (s3.2, s9). A table denial is neither.
        const bool audited = ctx != CVarContext::Editor && slot.audience == Audience::Server;
        if (!decision.allowed)
        {
            if (audited && decision.verdict == PolicyVerdict::Deny)
                m->Audit(slot.name, caller, ctx, decision.verdict,
                         FormatCVarValue(slot.history.back().value, slot.enumNames), FormatCVarValue(value, slot.enumNames));
            return SetResult::Denied;
        }

        value = Clamp(std::move(value), slot.min, slot.max);
        const std::string before = audited ? FormatCVarValue(slot.history.back().value, slot.enumNames) : std::string();
        const std::string after = audited ? FormatCVarValue(value, slot.enumNames) : std::string();
        // ONE record per (rung, source) (settings spec s4.5, O3): a repeat
        // replaces its own record, so re-opening a project or re-applying a
        // layer never grows the history. Records stay ordered weakest rung
        // first (newest last within a rung), so the back is always the winner.
        // A set beneath a stronger rung is RECORDED there: it is what that rung
        // holds once the stronger one is reverted. It reports RefusedWeaker
        // because it does not win now.
        std::erase_if(slot.history, [&](const CVarHistoryRecord& h) { return h.by == by && h.module == sourceModule; });
        const auto at = std::find_if(slot.history.begin(), slot.history.end(),
                                     [by](const CVarHistoryRecord& h) { return h.by > by; });
        const bool wins = at == slot.history.end();
        slot.history.insert(at, CVarHistoryRecord{ by, std::move(value), std::string(sourceModule) });
        slot.dirty = true;
        if (audited)
            m->Audit(slot.name, caller, ctx, decision.verdict, before, after);
        return wins ? SetResult::Applied : SetResult::RefusedWeaker;
    }

    std::size_t CVarRegistry::DropMatching(std::string_view module, const void* base, std::size_t size)
    {
        const auto matchesName = [module](std::string_view m) {
            return !module.empty() && m == module;
        };
        const auto matchesAddr = [base, size](const void* p) {
            return AddressInRange(p, base, size);
        };

        const auto keepSettingsHash = [&](std::uint64_t typeHash) {
            for (const SettingsBinding& b : m->settings)
                if (b.typeHash == typeHash) return true;
            return false;
        };

        const std::size_t settingsBefore = m->settings.size();
        std::erase_if(m->settings, [&](const SettingsBinding& b) {
            return matchesName(b.module) || matchesAddr(reinterpret_cast<const void*>(b.make));
        });
        const bool droppedSettings = m->settings.size() != settingsBefore;
        std::size_t dropped = settingsBefore - m->settings.size();

        // Pin dropped blocks BEFORE RebuildSnapshot. StoreSnapshot evicts the
        // oldest retired snapshot; a SettingsShared holder of that snapshot's
        // unique block would otherwise escape the Debug guard.
        std::vector<std::shared_ptr<const void>> heldBlocks;
        if (droppedSettings)
        {
            std::unordered_set<const void*> seen;
            const auto takeDropped = [&](const std::shared_ptr<const CVarSnapshot>& snap) {
                if (!snap) return;
                for (const CVarSettingsBlock& block : snap->settings)
                {
                    if (keepSettingsHash(block.typeHash) || !block.data) continue;
                    if (seen.insert(block.data.get()).second)
                        heldBlocks.push_back(block.data);
                }
            };
            takeDropped(m->retired[0]);
            takeDropped(m->retired[1]);
            takeDropped(Snapshot());
        }

        std::vector<std::uint32_t> kill;
        for (std::uint32_t i = 0; i < m->slots.size(); ++i)
        {
            Slot& slot = m->slots[i];
            if (!slot.alive) continue;
            if (matchesName(slot.declaredBy) || matchesAddr(slot.source))
            {
                kill.push_back(i);
                continue;
            }
            const auto cbBefore = slot.callbacks.size();
            std::erase_if(slot.callbacks, [&](const Slot::Callback& c) {
                return matchesName(c.module) || matchesAddr(reinterpret_cast<const void*>(c.fn));
            });
            dropped += cbBefore - slot.callbacks.size();
            const auto before = slot.history.size();
            std::erase_if(slot.history, [&](const CVarHistoryRecord& h) { return matchesName(h.module); });
            if (slot.history.size() != before) slot.dirty = true;
            if (slot.history.empty())
                slot.history.push_back(CVarHistoryRecord{ SetBy::Default, slot.published, {} });
        }
        dropped += kill.size();
        for (std::uint32_t index : kill)
        {
            Slot& slot = m->slots[index];
            m->byName.erase(slot.name);
            const std::uint32_t nextGen = slot.generation + 1;
            slot = Slot{};
            slot.generation = nextGen == 0 ? 1 : nextGen;
            slot.alive = false;
            m->freeSlots.push_back(index);
        }
        if (!kill.empty()) ++m->revision;
        for (Command& command : m->commands)
        {
            if (!command.alive || command.builtin) continue;
            const void* fn = command.fn ? reinterpret_cast<const void*>(command.fn)
                                        : reinterpret_cast<const void*>(command.legacy);
            if (matchesName(command.module) || matchesAddr(fn))
            {
                m->commandByName.erase(command.name);
                command.alive = false;
                ++dropped;
            }
        }
        if (m->policy && (matchesName(m->policyModule) || matchesAddr(reinterpret_cast<const void*>(m->policy))))
            SetPolicy(nullptr, nullptr, {});

        m->RebuildSnapshot(*this);
        if (droppedSettings)
        {
            // Dropped blocks' deleters are the unloading module's code. Strip
            // them from the retire ring while the image is still mapped, and
            // keep other modules' blocks so a Settings<T>() reference survives
            // its two-publish lifetime. An outside SettingsShared<T> holder
            // keeps its copy; holding one across a hot reload is the holder's
            // bug.
            const auto stripDropped = [&](std::shared_ptr<const CVarSnapshot>& snap) {
                if (!snap) return;
                bool hasDropped = false;
                for (const CVarSettingsBlock& block : snap->settings)
                    if (!keepSettingsHash(block.typeHash)) { hasDropped = true; break; }
                if (!hasDropped) return;
                auto clone = std::make_shared<CVarSnapshot>(*snap);
                std::erase_if(clone->settings, [&](const CVarSettingsBlock& block) {
                    return !keepSettingsHash(block.typeHash);
                });
                snap = std::move(clone);
            };
            stripDropped(m->retired[0]);
            stripDropped(m->retired[1]);
#if !defined(NDEBUG)
            for (const std::shared_ptr<const void>& data : heldBlocks)
            {
                const long uses = data.use_count();
                if (uses > 1)
                {
                    ARC_WARN("cvar: module '{}': a SettingsShared holder still owns a dropped "
                             "settings block (use_count={}); releasing it after unmap calls the "
                             "module deleter",
                             module.empty() ? std::string_view("<image>") : module, uses);
                    ARC_ENSURE(uses <= 1, "SettingsShared holder outlived UnregisterModule");
                }
            }
#endif
        }
        return dropped;
    }

    void CVarRegistry::UnregisterModule(std::string_view module)
    {
        (void)DropMatching(module, nullptr, 0);
    }

    std::size_t CVarRegistry::UnregisterModuleRange(const void* base, std::size_t size)
    {
        // Address only: two images can share a stem, and name-matching would
        // drop the other image's registrations (Review Focus 2, Plugin::Load).
        const std::size_t dropped = DropMatching({}, base, size);
        Log::UnregisterMosaicLevelTargetsInRange(base, size);
        UnregisterModuleImage(base, size);
        return dropped;
    }

    void CVarRegistry::RegisterModuleImage(std::string_view module, const void* base, std::size_t size)
    {
        if (!base || size == 0) return;
        for (Impl::ModuleImage& img : m->images)
        {
            if (img.base == base)
            {
                img.module = std::string(module);
                img.size = size;
                return;
            }
        }
        m->images.push_back(Impl::ModuleImage{ std::string(module), base, size });
    }

    void CVarRegistry::UnregisterModuleImage(const void* base, std::size_t size)
    {
        (void)size;
        std::erase_if(m->images, [base](const Impl::ModuleImage& img) { return img.base == base; });
    }

    void CVarRegistry::RevertCheats()
    {
        (void)m->DropCheatHistory();
    }

    void CVarRegistry::RevertLayer(SetBy by)
    {
        for (Slot& slot : m->slots)
        {
            if (!slot.alive) continue;
            const auto before = slot.history.size();
            std::erase_if(slot.history, [by](const CVarHistoryRecord& h) { return h.by == by; });
            if (slot.history.size() != before) slot.dirty = true;
            if (slot.history.empty())
                slot.history.push_back(CVarHistoryRecord{ SetBy::Default, slot.published, {} });
        }
    }

    bool CVarRegistry::ClearRung(CVarHandle handle, SetBy rung)
    {
        if (rung == SetBy::Default || handle.index >= m->slots.size()) return false;
        Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return false;
        const auto before = slot.history.size();
        std::erase_if(slot.history, [rung](const CVarHistoryRecord& h) { return h.by == rung; });
        if (slot.history.size() == before) return false;
        if (slot.history.empty())
            slot.history.push_back(CVarHistoryRecord{ SetBy::Default, slot.published, {} });
        slot.dirty = true;
        return true;
    }

    void CVarRegistry::SetPolicy(CVarPolicyFn fn, void* user, std::string_view module)
    {
        m->policy = fn;
        m->policyUser = fn ? user : nullptr;
        m->policyModule = fn ? std::string(module) : std::string();
    }

    void CVarRegistry::SetAuditSink(CVarAuditFn fn, void* user)
    {
        m->audit = fn;
        m->auditUser = fn ? user : nullptr;
    }

    void CVarRegistry::Publish()
    {
        if (m->publishing) return;
        m->publishing = true;
        // Cleared however Publish exits (S6-GATE): a callback that throws
        // propagates to the caller, but must not leave `publishing` latched,
        // or every later Publish would be a silent no-op for the process.
        struct PublishingReset
        {
            bool& flag;
            ~PublishingReset() { flag = false; }
        } publishingReset{ m->publishing };
        const bool cheatsWere = CheatsEnabled();
        std::vector<std::uint32_t> dirty;
        for (std::uint32_t i = 0; i < m->slots.size(); ++i)
            if (m->slots[i].alive && m->slots[i].dirty) dirty.push_back(i);
        // The snapshot is swapped between the promote and the dispatch, so the
        // callbacks and the CVarRef readers inside them already see the new values.
        const std::vector<std::uint32_t> changed = m->Promote(dirty);
        if (!changed.empty())
        {
            auto next = std::make_shared<CVarSnapshot>();
            next->serial = ++m->serial;
            next->entries.reserve(m->slots.size());
            for (const auto& s : m->slots)
                next->entries.push_back(CVarSnapshot::Entry{ s.generation, s.alive, s.published });
            FillSettingsBlocks(*next, changed);   // settings arc S2: typed blocks, BEFORE the swap, so callbacks read them
            StoreSnapshot(std::move(next));
        }
        m->Dispatch(changed);
        // server.cheats went off in this publish (spec s4.5, O4). The Cheat
        // settings revert through the SAME promote-and-dispatch path, so their
        // callbacks fire. Only the reverted slots are promoted here, so a set
        // made by a callback above still waits for the next Publish.
        if (cheatsWere && !CheatsEnabled())
        {
            const std::vector<std::uint32_t> reverted = m->Promote(m->DropCheatHistory());
            if (!reverted.empty())
            {
                auto next = std::make_shared<CVarSnapshot>();
                next->serial = ++m->serial;
                next->entries.reserve(m->slots.size());
                for (const auto& s : m->slots)
                    next->entries.push_back(CVarSnapshot::Entry{ s.generation, s.alive, s.published });
                FillSettingsBlocks(*next, reverted);
                StoreSnapshot(std::move(next));
            }
            m->Dispatch(reverted);
        }
    }

    std::shared_ptr<const CVarSnapshot> CVarRegistry::Snapshot() const
    {
#if defined(__APPLE__)
        return std::atomic_load_explicit(&m->snapshot, std::memory_order_acquire);
#else
        return m->snapshot.load(std::memory_order_acquire);
#endif
    }

    void CVarRegistry::PublishImmediate()
    {
        ARC_ASSERT(std::this_thread::get_id() == m->mainThread,
                   "CVarRegistry::PublishImmediate: main thread only (settings spec s4.6)");
        Publish();   // inside a running Publish (a callback), this is the documented no-op
    }

    std::optional<CVarExplain> CVarRegistry::Explain(std::string_view name) const
    {
        CVarHandle handle = Find(name);
        if (handle.IsStale())
            if (const auto alias = m->aliases.find(std::string(name)); alias != m->aliases.end())
                handle = Find(alias->second);   // a read: no rename warning
        if (handle.IsStale()) return std::nullopt;
        const Slot& slot = m->slots[handle.index];
        CVarExplain out;
        out.name = slot.name;
        out.help = slot.help;
        out.type = slot.type;
        out.flags = slot.flags;
        out.published = slot.published;
        out.pending = slot.history.back().value;
        out.setBy = slot.history.back().by;
        out.history = slot.history;
        out.audience = slot.audience;
        out.scope = slot.scope;
        out.apply = slot.apply;
        return out;
    }

    std::vector<CVarListEntry> CVarRegistry::List(CVarContext ctx) const
    {
        std::vector<CVarListEntry> out;
        for (const Slot& slot : m->slots)
        {
            if (!slot.alive) continue;
            if (HasFlag(slot.flags, CVarFlags::Hidden)) continue;
            if (HasFlag(slot.flags, CVarFlags::Dev) && !m->devCvars) continue;
            // A read, so the cheats gate and the client knob play no part.
            const AccessRule read = DefaultAccess(slot.audience, slot.flags, ctx, false, false, false);
            if (read.absent || !read.allowed) continue;
            out.push_back(CVarListEntry{ slot.name, slot.help, slot.type, slot.flags });
        }
        return out;
    }

    std::vector<CVarListEntryEx> CVarRegistry::ListEx() const
    {
        std::vector<CVarListEntryEx> out;
        for (const Slot& slot : m->slots)
        {
            if (!slot.alive) continue;
            if (HasFlag(slot.flags, CVarFlags::Hidden)) continue;
            if (HasFlag(slot.flags, CVarFlags::Dev) && !m->devCvars) continue;
            CVarListEntryEx e;
            e.name = slot.name;
            e.displayName = slot.displayName.empty() ? DeriveDisplayName(slot.name) : slot.displayName;
            e.help = slot.help;
            e.categoryPath = slot.categoryPath;
            e.widget = slot.widget;
            e.type = slot.type;
            e.flags = slot.flags;
            e.audience = slot.audience;
            e.scope = slot.scope;
            e.apply = slot.apply;
            e.order = slot.order;
            e.min = slot.min;
            e.max = slot.max;
            e.enumNames = slot.enumNames;
            e.value = slot.published;
            e.defaultValue = slot.published;
            for (const CVarHistoryRecord& h : slot.history)
            {
                if (h.by == SetBy::Default) { e.defaultValue = h.value; break; }
            }
            out.push_back(std::move(e));
        }
        return out;
    }

    std::vector<CVarListEntry> CVarRegistry::ListCommands() const
    {
        std::vector<CVarListEntry> out;
        for (const Command& command : m->commands)
        {
            if (!command.alive) continue;
            if (HasFlag(command.flags, CVarFlags::Hidden)) continue;
            if (HasFlag(command.flags, CVarFlags::Dev) && !m->devCvars) continue;
            out.push_back(CVarListEntry{ command.name, command.help, CVarType::Bool, command.flags });
        }
        return out;
    }

    std::optional<CVarDescInfo> CVarRegistry::Describe(std::string_view name) const
    {
        const CVarHandle handle = Find(name);
        if (handle.IsStale()) return std::nullopt;
        const Slot& slot = m->slots[handle.index];
        CVarDescInfo out;
        out.name = slot.name;
        out.help = slot.help;
        out.displayName = slot.displayName;
        out.keywords = slot.keywords;
        out.widget = slot.widget;
        out.categoryPath = slot.categoryPath;
        out.module = slot.declaredBy;
        out.type = slot.type;
        out.flags = slot.flags;
        out.min = slot.min;
        out.max = slot.max;
        out.audience = slot.audience;
        out.scope = slot.scope;
        out.apply = slot.apply;
        out.order = slot.order;
        out.enumNames = slot.enumNames;
        out.group = slot.group;
        out.defaultValue = slot.defaultValue;
        return out;
    }

    std::vector<std::string> CVarRegistry::Names(bool includeHidden) const
    {
        std::vector<std::string> out;
        for (const Slot& slot : m->slots)
        {
            if (!slot.alive) continue;
            if (!includeHidden && HasFlag(slot.flags, CVarFlags::Hidden)) continue;
            if (HasFlag(slot.flags, CVarFlags::Dev) && !m->devCvars) continue;
            out.push_back(slot.name);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::optional<CVarValue> CVarRegistry::RungValue(std::string_view name, SetBy by) const
    {
        const CVarHandle handle = Find(name);
        if (handle.IsStale()) return std::nullopt;
        const Slot& slot = m->slots[handle.index];
        for (auto it = slot.history.rbegin(); it != slot.history.rend(); ++it)
            if (it->by == by) return it->value;
        return std::nullopt;
    }

    bool CVarRegistry::SetRung(std::string_view name, SetBy by, CVarValue value, std::string_view sourceModule)
    {
        if (by == SetBy::Default) return false;
        const CVarHandle handle = Find(name);
        if (handle.IsStale()) return false;
        Slot& slot = m->slots[handle.index];
        if (value.type != slot.type) return false;
        if (slot.type == CVarType::Enum &&
            (value.AsEnum() < 0 || static_cast<std::size_t>(value.AsEnum()) >= slot.enumNames.size()))
            return false;
        if (RungRefusal(slot.flags, slot.scope, by) != CVarRungRefusal::None) return false;   // as Set (S7-SEC)
        value = Clamp(std::move(value), slot.min, slot.max);
        std::erase_if(slot.history, [by](const CVarHistoryRecord& h) { return h.by == by; });
        // History is in rung order (Set refuses a weaker rung, S1 replaces in place):
        // insert before the first stronger record, so the winner stays the winner.
        const auto at = std::find_if(slot.history.begin(), slot.history.end(),
                                     [by](const CVarHistoryRecord& h) { return h.by > by; });
        slot.history.insert(at, CVarHistoryRecord{ by, std::move(value), std::string(sourceModule) });
        slot.dirty = true;
        return true;
    }

    std::uint64_t CVarRegistry::Revision() const noexcept { return m->revision; }

    ExecResult CVarRegistry::Execute(std::string_view line, CVarContext ctx, SetBy by, const CVarCaller* caller)
    {
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        if (line.empty()) return { false, "empty" };
        const auto gap = line.find(' ');
        const std::string name{ line.substr(0, gap) };
        const std::string_view args = gap == std::string_view::npos ? std::string_view{} : line.substr(gap + 1);

        if (const auto cit = m->commandByName.find(name); cit != m->commandByName.end())
        {
            Command& command = m->commands[cit->second];
            if (!command.alive) return { false, "unknown command '" + name + "'" };
            if (ctx != CVarContext::Editor && HasFlag(command.flags, CVarFlags::Cheat) && !CheatsEnabled())
                return { false, "denied: '" + name + "' needs server.cheats" };
            // Copied out first: a command may register another and move the vector.
            const CommandFn fn = command.fn;
            const LegacyCommandFn legacy = command.legacy;
            void* const user = command.user;
            const ExecutingContextScope scope{ ctx, caller };
            if (fn)
            {
                CommandResult result = fn(args, user);
                return { result.ok, std::move(result.text) };
            }
            std::string text;
            legacy(args, text, user);
            return { true, std::move(text) };   // the old form cannot report failure (spec s4.5)
        }

        const CVarHandle handle = Resolve(name);
        if (handle.IsStale()) return { false, "unknown '" + name + "'" };
        if (ctx != CVarContext::Editor)
        {
            if (m->Access(*this, m->slots[handle.index], ctx, false, nullptr).absent)
                return { false, "unknown '" + name + "'" };
            // A plain read asks the policy here; a write line is ONE write,
            // asked once, by Set. A read is refused for exactly two reasons:
            // the table (Protected outside ServerAdmin) or the policy's Deny.
            if (args.empty())
            {
                const Impl::Decision read = m->Decide(*this, m->slots[handle.index], ctx, caller, false, nullptr);
                if (!read.allowed)
                    return { false, read.verdict == PolicyVerdict::Deny ? "denied: '" + name + "' by the game's policy"
                                                                        : "denied: '" + name + "' is protected" };
            }
        }
        // The policy is game code: it may Register, and Register may grow the
        // slot vector. So a Slot reference is taken only AFTER the read-path
        // policy above has run, and lives in a block that ends BEFORE Set runs
        // the write-path policy. The echo after Set uses a copy of the name.
        // (handle.index itself is stable: Register appends or reuses a DEAD
        // slot, never moves a live one.)
        std::optional<CVarValue> parsed;
        std::string canonical;
        {
            const Slot& slot = m->slots[handle.index];
            if (args.empty())
                return { true, slot.name + " = " + FormatCVarValue(slot.published, slot.enumNames) };
            // The console and --set (ApplyCVarCommandLine) parse every type the same way.
            std::string token{ args };
            while (!token.empty() && token.back() == ' ') token.pop_back();
            std::string error;
            parsed = ParseCVarText(token, slot.type, slot.enumNames, error);
            if (!parsed) return { false, error };
            canonical = slot.name;
        }
        const SetResult result = Set(handle, std::move(*parsed), by, {}, ctx, caller);
        if (result == SetResult::Denied) return { false, "denied" };
        if (result == SetResult::RefusedWeaker) return { false, "refused: a stronger source holds " + canonical };
        if (result != SetResult::Applied) return { false, "rejected" };
        return { true, canonical + " set (pending publish)" };
    }

    void CVarRegistry::AddCallback(CVarHandle handle, ChangeFn fn, void* user)
    {
        if (handle.index >= m->slots.size()) return;
        Slot& slot = m->slots[handle.index];
        if (!slot.alive || slot.generation != handle.generation || !fn) return;
        std::string module = std::string(ScopedModule());
        if (module.empty())
        {
            if (const std::string_view fromImage = m->NameForAddress(reinterpret_cast<const void*>(fn)); !fromImage.empty())
            {
                module = std::string(fromImage);
#if !defined(NDEBUG)
                ARC_WARN("cvar: AddCallback from module '{}' had no CVarModuleScope; attributed by image address",
                         module);
#endif
            }
        }
        for (const Slot::Callback& c : slot.callbacks)
            if (c.fn == fn && c.user == user && c.module == module)
                return;
        slot.callbacks.push_back(Slot::Callback{ fn, user, std::move(module) });
    }

    bool CVarRegistry::CheatsEnabled() const
    {
        const CVarHandle handle = Find("server.cheats");
        const auto value = Get(handle);
        return value && value->type == CVarType::Bool && value->AsBool();
    }

    bool CVarRegistry::RegisterAlias(std::string_view oldName, std::string_view newName)
    {
        if (oldName.empty() || newName.empty() || oldName == newName) return false;
        const std::string from{ oldName };
        const std::string to{ newName };
        if (m->byName.contains(from) || m->commandByName.contains(from)) return false;
        if (m->aliases.contains(to)) return false;                     // the target is itself an old name
        if (const auto it = m->aliases.find(from); it != m->aliases.end()) return it->second == to;
        for (auto& [old, target] : m->aliases)
            if (target == from) target = to;                           // a -> b, then b -> c: a -> c
        m->aliases.emplace(from, to);
        return true;
    }

    CVarHandle CVarRegistry::Resolve(std::string_view name)
    {
        if (const CVarHandle direct = Find(name); !direct.IsStale()) return direct;
        const auto it = m->aliases.find(std::string(name));
        if (it == m->aliases.end()) return {};
        if (m->warnedAliases.insert(it->first).second)
            ARC_WARN("cvar: '{}' is renamed '{}' -- update the file, --set or script that names it (warned once)",
                     it->first, it->second);
        return Find(it->second);
    }

    std::string CVarRegistry::AliasTarget(std::string_view oldName) const
    {
        const auto it = m->aliases.find(std::string(oldName));
        return it == m->aliases.end() ? std::string{} : it->second;
    }

    std::vector<std::pair<std::string, std::string>> CVarRegistry::Aliases() const
    {
        return std::vector<std::pair<std::string, std::string>>(m->aliases.begin(), m->aliases.end());
    }

    void CVarRegistry::StoreSnapshot(std::shared_ptr<const CVarSnapshot> next)
    {
#if defined(__APPLE__)
        std::shared_ptr<const CVarSnapshot> outgoing = std::atomic_exchange_explicit(&m->snapshot, std::move(next), std::memory_order_acq_rel);
#else
        std::shared_ptr<const CVarSnapshot> outgoing = m->snapshot.exchange(std::move(next), std::memory_order_acq_rel);
#endif
        m->retired[1] = std::move(m->retired[0]);
        m->retired[0] = std::move(outgoing);
    }

    std::shared_ptr<const void> CVarRegistry::BuildSettingsBlock(const SettingsBinding& binding) const
    {
        std::shared_ptr<void> block = binding.make();
        for (std::size_t i = 0; i < binding.handles.size(); ++i)
        {
            const CVarHandle h = binding.handles[i];
            if (h.IsStale() || h.index >= m->slots.size()) continue;
            const Slot& slot = m->slots[h.index];
            if (!slot.alive || slot.generation != h.generation) continue;
            binding.writers[i](block.get(), slot.published);
        }
        return block;
    }

    void CVarRegistry::FillSettingsBlocks(CVarSnapshot& next, const std::vector<std::uint32_t>& changedSlots) const
    {
        const std::shared_ptr<const CVarSnapshot> previous = Snapshot();
        std::vector<std::uint64_t> touched;
        for (std::uint32_t index : changedSlots)
        {
            const std::uint64_t type = m->slots[index].settingsType;
            if (type != 0 && std::find(touched.begin(), touched.end(), type) == touched.end())
                touched.push_back(type);
        }
        next.settings.clear();
        next.settings.reserve(m->settings.size());
        for (const SettingsBinding& b : m->settings)
        {
            const CVarSettingsBlock* old = previous ? previous->FindSettingsEntry(b.typeHash) : nullptr;
            const bool rebuild = !old || std::find(touched.begin(), touched.end(), b.typeHash) != touched.end();
            next.settings.push_back(rebuild ? CVarSettingsBlock{ b.typeHash, BuildSettingsBlock(b) } : *old);
        }
        std::sort(next.settings.begin(), next.settings.end(),
                  [](const CVarSettingsBlock& a, const CVarSettingsBlock& b) { return a.typeHash < b.typeHash; });
    }

    void CVarRegistry::ReplaceSnapshotSettings(std::vector<CVarSettingsBlock> blocks)
    {
        const std::shared_ptr<const CVarSnapshot> current = Snapshot();
        if (!current) return;   // no snapshot yet: the first Publish's FillSettingsBlocks builds every block
        auto next = std::make_shared<CVarSnapshot>(*current);
        std::sort(blocks.begin(), blocks.end(),
                  [](const CVarSettingsBlock& a, const CVarSettingsBlock& b) { return a.typeHash < b.typeHash; });
        next->settings = std::move(blocks);
        StoreSnapshot(std::move(next));
    }

    bool CVarRegistry::RegisterSettings(const SettingsTypeDesc& desc)
    {
        m->lastError.clear();
        if (!desc.error.empty() || !desc.make)
        {
            m->lastError = "settings '" + desc.typeName + "': " + (desc.error.empty() ? std::string("no factory") : desc.error);
            ARC_ERROR("cvar: {}", m->lastError);
            return false;
        }
        for (const SettingsBinding& b : m->settings)
            if (b.typeHash == desc.typeHash)
            {
                m->lastError = "settings '" + desc.typeName + "' already registered by module '" + b.module + "'";
                return false;
            }

        SettingsBinding binding;
        binding.typeHash = desc.typeHash;
        binding.typeName = desc.typeName;
        binding.module   = desc.module;
        if (binding.module.empty())
        {
            if (!ScopedModule().empty())
                binding.module = std::string(ScopedModule());
            else if (const std::string_view fromImage = m->NameForAddress(reinterpret_cast<const void*>(desc.make));
                     !fromImage.empty())
                binding.module = std::string(fromImage);
        }
        binding.make     = desc.make;
        for (std::size_t i = 0; i < desc.fields.size(); ++i)
        {
            const SettingsFieldDesc& f = desc.fields[i];
            CVarDesc cv;
            cv.name         = f.name;
            cv.type         = f.type;
            cv.defaultValue = f.defaultValue;
            cv.min          = f.min;
            cv.max          = f.max;
            cv.flags        = f.flags;
            cv.help         = f.help;
            cv.module       = binding.module;
            cv.displayName  = f.displayName;
            cv.keywords     = f.keywords;
            cv.widget       = f.widget;
            cv.audience     = f.audience;
            cv.scope        = f.scope;
            cv.apply        = f.apply;
            cv.order        = static_cast<std::int32_t>(i);
            cv.enumNames    = f.enumNames;
            cv.group        = f.group;
            cv.categoryPath = f.categoryPath;
            const CVarHandle h = Register(cv);
            if (!h.IsStale())
            {
                m->slots[h.index].settingsType = desc.typeHash;
                for (const std::string& alias : f.aliases)
                    (void)RegisterAlias(alias, f.name);
            }
            else if (!(HasFlag(f.flags, CVarFlags::Dev) && !m->devCvars))
                ARC_ERROR("cvar: settings '{}' field '{}' not registered: {}", desc.typeName, f.name, m->lastError);
            binding.handles.push_back(h);
            binding.writers.push_back(f.write);
        }
        m->settings.push_back(std::move(binding));
        // Publish the block NOW, so Settings<T>() answers before the next frame's Publish.
        if (const std::shared_ptr<const CVarSnapshot> current = Snapshot())
        {
            std::vector<CVarSettingsBlock> blocks = current->settings;
            blocks.push_back(CVarSettingsBlock{ desc.typeHash, BuildSettingsBlock(m->settings.back()) });
            ReplaceSnapshotSettings(std::move(blocks));
        }
        m->lastError.clear();
        return true;
    }

    const void* CVarRegistry::SettingsBlock(std::uint64_t typeHash) const noexcept
    {
        const std::shared_ptr<const CVarSnapshot> snap = Snapshot();
        return snap ? snap->FindSettings(typeHash) : nullptr;
    }

    bool Detail::RegisterDeclaredAlias(std::string_view oldName, std::string_view newName)
    {
        const bool ok = CVarRegistry::Get().RegisterAlias(oldName, newName);
        if (!ok)
            ARC_ERROR("cvar: the alias '{}' -> '{}' was refused (an empty or equal name, a live cvar or command "
                      "under the old name, or a chain)", oldName, newName);
        return ok;
    }

    CVarHandle Detail::RegisterDeclaredCVar(const CVarDesc& desc)
    {
        const void* previous = t_registerSource;
        t_registerSource = ARC_CVAR_RETURN_ADDRESS();
        CVarRegistry& registry = CVarRegistry::Get();
        const CVarHandle handle = registry.Register(desc);
        t_registerSource = previous;
#if defined(ARC_BUILD_DIST)
        const bool compiledOut = HasFlag(desc.flags, CVarFlags::Dev);   // Get()'s registry is built without Dev cvars
#else
        const bool compiledOut = false;
#endif
        if (handle.IsStale() && !compiledOut)
            ARC_ERROR("cvar: the declaration of '{}' (module '{}') was refused: {}", desc.name, desc.module,
                      registry.LastError());
        return handle;
    }

    void Detail::WarnUnlistedSettingsEnumValue(std::string_view enumType, std::int64_t raw)
    {
        static std::mutex lock;
        static std::set<std::pair<std::string, std::int64_t>> warned;
        {
            const std::lock_guard guard(lock);
            if (!warned.emplace(std::string(enumType), raw).second) return;
        }
        ARC_WARN("settings: {} value {} is not in its reflected enum table -- the cvar reads it as index 0; "
                 "reflect the enumerator (ARC_REFLECT_ENUM_VALUE) or change the value (warned once)", enumType, raw);
    }
}
