#include "Project/ClassTemplates.hpp"

#include <Arcane/Project/GameDescriptor.hpp>

#include <cctype>
#include <string_view>

namespace Arcane::Editor::ClassTemplates
{
    const char* KindLabel(Kind kind)
    {
        switch (kind)
        {
            case Kind::Component:  return "Component";
            case Kind::System:     return "System";
            case Kind::PlainClass: return "Plain class";
            case Kind::Count:      break;
        }
        return "Class";
    }

    namespace
    {
        bool IsIdentStart(char c)
        {
            return c == '_' || std::isalpha(static_cast<unsigned char>(c)) != 0;
        }
        bool IsIdentChar(char c)
        {
            return c == '_' || std::isalnum(static_cast<unsigned char>(c)) != 0;
        }

        // The C++ keywords a class name could plausibly collide with. Not the
        // whole reserved list -- the identifier scan above already rules out
        // everything that is not [A-Za-z0-9_] -- just the words that ARE
        // identifiers lexically and would still fail to compile as a type name.
        bool IsKeyword(std::string_view s)
        {
            static constexpr std::string_view kKeywords[] = {
                "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool", "break",
                "case", "catch", "char", "class", "compl", "concept", "const", "consteval",
                "constexpr", "constinit", "continue", "decltype", "default", "delete", "do",
                "double", "else", "enum", "explicit", "export", "extern", "false", "float",
                "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
                "new", "noexcept", "not", "nullptr", "operator", "or", "private", "protected",
                "public", "register", "requires", "return", "short", "signed", "sizeof",
                "static", "struct", "switch", "template", "this", "throw", "true", "try",
                "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
                "void", "volatile", "wchar_t", "while", "xor",
            };
            for (std::string_view k : kKeywords)
                if (s == k)
                    return true;
            return false;
        }

        // Replace every "{{TOKEN}}" occurrence in `text` with `value`.
        void ReplaceAll(std::string& text, std::string_view token, std::string_view value)
        {
            std::size_t pos = 0;
            while ((pos = text.find(token, pos)) != std::string::npos)
            {
                text.replace(pos, token.size(), value);
                pos += value.size();
            }
        }

        std::string Fill(std::string_view tmpl,
                         std::string_view cls,
                         std::string_view ns,
                         std::string_view role = {},
                         std::string_view phase = {})
        {
            std::string out(tmpl);
            ReplaceAll(out, "{{CLASS}}", cls);
            ReplaceAll(out, "{{NS}}", ns);
            ReplaceAll(out, "{{ROLE}}", role);
            ReplaceAll(out, "{{PHASE}}", phase);
            return out;
        }

        // Render is RETIRED (F5 plan 1 Task 5): no host runs the render
        // scheduler, so a Render-phase system would register and never run.
        // The wizard never writes one; a typed Render normalizes like any
        // other unusable value.
        Arcane::SystemPhase NormalizePhase(Arcane::SystemPhase phase) noexcept
        {
            switch (phase)
            {
                case Arcane::SystemPhase::FixedUpdate:
                case Arcane::SystemPhase::Update:
                    return phase;
                case Arcane::SystemPhase::Render:
                    break;
            }
            return Arcane::SystemPhase::FixedUpdate;
        }

        Arcane::RoleMask NormalizeRole(Arcane::RoleMask role) noexcept
        {
            switch (role)
            {
                case Arcane::RoleMask::Both:
                case Arcane::RoleMask::Server:
                case Arcane::RoleMask::Client:
                    return role;
            }
            return Arcane::RoleMask::Both;
        }

        const char* PhaseSpelling(Arcane::SystemPhase phase) noexcept
        {
            switch (NormalizePhase(phase))
            {
                case Arcane::SystemPhase::FixedUpdate: return "Arcane::SystemPhase::FixedUpdate";
                case Arcane::SystemPhase::Update:      return "Arcane::SystemPhase::Update";
                case Arcane::SystemPhase::Render:      break;   // retired; NormalizePhase never yields it
            }
            return "Arcane::SystemPhase::FixedUpdate";
        }

        const char* RoleSpelling(Arcane::RoleMask role) noexcept
        {
            switch (NormalizeRole(role))
            {
                case Arcane::RoleMask::Both:   return "Arcane::RoleMask::Both";
                case Arcane::RoleMask::Server: return "Arcane::RoleMask::Server";
                case Arcane::RoleMask::Client: return "Arcane::RoleMask::Client";
            }
            return "Arcane::RoleMask::Both";
        }

        // ---- the templates ---------------------------------------------------
        // Kept as raw literals so what the wizard writes is readable HERE, at
        // the one place it is defined. Every literal ends in "\n".

        constexpr std::string_view kComponentHeader = R"(#pragma once

// {{CLASS}}: a component -- plain data on an entity. Reflected so the editor's
// Inspector can show and edit it, scenes can save it, and the Add Component
// catalog can offer it. Registered with the game module by the
// ARC_COMPONENT line in {{CLASS}}.cpp; nothing else to wire.

#include <Arcane/Reflection.hpp>

namespace {{NS}}
{
    struct {{CLASS}}
    {
        float value = 0.0f;
    };

    ARC_REFLECT_TYPE({{CLASS}})
        ARC_REFLECT_FIELD({{CLASS}}, value)
    ARC_END_REFLECT_TYPE()
}
)";

        constexpr std::string_view kComponentSource = R"(#include "{{CLASS}}.hpp"

#include <Arcane/Plugin/GameComponents.hpp>

// The one registration line: the ARC_GAME_MODULE prologue (Arcane/Plugin/
// GameModule.hpp) drains every ARC_COMPONENT of the module into its
// ComponentModule (Arcane::Game::RegisterComponents). One .cpp per type.
ARC_COMPONENT({{NS}}::{{CLASS}})
)";

        constexpr std::string_view kFixedUpdateSystemHeader = R"(#pragma once

// {{CLASS}}: a system -- a functor the scheduler runs each step. Its
// PARAMETERS say what it touches, so the scheduler can order and parallelise
// it: views over components and engine resources such as the sim clock.
//
//     void operator()(Arcane::View<Arcane::Transform>& view,
//                     Arcane::Res<Arcane::Time> time,
//                     Arcane::Res<Arcane::GameInput> input)   // #include <Arcane/Input/GameInput.hpp>
//
// Fixed-update systems run before transform propagation by default so gameplay
// can move local transforms first. Registrar discovery order is irrelevant:
// scheduler order is expressed only through Before<> and After<> traits.
// The ARC_SYSTEM declaration that selects phase and network role is in
// {{CLASS}}.cpp.

#include <Arcane/Ecs.hpp>
#include <Arcane/Scene/TransformSystems.hpp>   // the placement anchor

namespace {{NS}}
{
    struct {{CLASS}} : Arcane::SystemTraits<Arcane::Before<Arcane::TransformPropagationSystem>>
    {
        void operator()(Arcane::Res<Arcane::Time> time)
        {
            (void)time;
        }
    };
}
)";

        constexpr std::string_view kUnanchoredSystemHeader = R"(#pragma once

// {{CLASS}}: a system -- a functor the scheduler runs each step. Its
// PARAMETERS say what it touches, so the scheduler can order and parallelise
// it: views over components and engine resources such as the sim clock.
//
//     void operator()(Arcane::View<Arcane::Transform>& view,
//                     Arcane::Res<Arcane::Time> time)
//
// Fixed-step transform propagation is not installed in the Update scheduler,
// so this template invents no irrelevant edge. Registrar discovery
// order is irrelevant: derive Arcane::SystemTraits<Arcane::Before<...>> or
// After<...> whenever scheduler order matters. The ARC_SYSTEM declaration
// that selects phase and network role is in {{CLASS}}.cpp.

#include <Arcane/Ecs.hpp>

namespace {{NS}}
{
    struct {{CLASS}}
    {
        void operator()(Arcane::Res<Arcane::Time> time)
        {
            (void)time;
        }
    };
}
)";

        constexpr std::string_view kSystemSource = R"(#include "{{CLASS}}.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

// ARC_GAME_MODULE discovers this declaration while its DLL-owner bracket
// is open. Phase and role are explicit; scheduler order belongs in traits.
ARC_SYSTEM(
    {{NS}}::{{CLASS}},
    {{ROLE}},
    {{PHASE}})
)";

        constexpr std::string_view kPlainHeader = R"(#pragma once

namespace {{NS}}
{
    class {{CLASS}}
    {
    public:
        {{CLASS}}() = default;
    };
}
)";

        constexpr std::string_view kPlainSource = R"(#include "{{CLASS}}.hpp"

namespace {{NS}}
{
}
)";
    }

    const char* SystemPhaseChoiceLabel(int index) noexcept
    {
        switch (index)
        {
            case 0: return "Fixed Update";
            case 1: return "Update";
            default: return "Fixed Update";
        }
    }

    const char* SystemRoleChoiceLabel(int index) noexcept
    {
        switch (index)
        {
            case 0: return "Both";
            case 1: return "Server";
            case 2: return "Client";
            default: return "Both";
        }
    }

    SystemOptions SystemOptionsForChoiceIndices(int phaseIndex, int roleIndex) noexcept
    {
        SystemOptions options;
        switch (phaseIndex)
        {
            case 1: options.phase = Arcane::SystemPhase::Update; break;
            default: options.phase = Arcane::SystemPhase::FixedUpdate; break;
        }
        switch (roleIndex)
        {
            case 1: options.role = Arcane::RoleMask::Server; break;
            case 2: options.role = Arcane::RoleMask::Client; break;
            default: options.role = Arcane::RoleMask::Both; break;
        }
        return options;
    }

    std::optional<std::string> ValidateClassName(std::string_view name)
    {
        if (name.empty())
            return "Enter a class name.";
        if (!IsIdentStart(name.front()))
            return "A class name must start with a letter or underscore.";
        for (char c : name)
            if (!IsIdentChar(c))
                return "A class name may only contain letters, digits and underscores.";
        if (IsKeyword(name))
            return "That is a C++ keyword.";
        return std::nullopt;
    }

    std::string NamespaceForProject(std::string_view projectName)
    {
        std::string ns;
        ns.reserve(projectName.size() + 1);
        for (char c : projectName)
            ns.push_back(IsIdentChar(c) ? c : '_');
        if (!ns.empty() && !IsIdentStart(ns.front()))
            ns.insert(ns.begin(), '_');
        if (ns.empty())
            return "Game";
        return ns;
    }

    Rendered Render(Kind kind,
                    std::string_view className,
                    std::string_view projectName,
                    SystemOptions options)
    {
        const std::string ns = NamespaceForProject(projectName);
        const std::string cls(className);
        Rendered r;
        r.headerName = cls + ".hpp";
        switch (kind)
        {
            case Kind::Component:
                r.header     = Fill(kComponentHeader, cls, ns);
                r.sourceName = cls + ".cpp";
                r.source     = Fill(kComponentSource, cls, ns);
                break;
            case Kind::System:
            {
                const Arcane::SystemPhase phase = NormalizePhase(options.phase);
                const Arcane::RoleMask role = NormalizeRole(options.role);
                const std::string_view headerTemplate =
                    phase == Arcane::SystemPhase::FixedUpdate
                        ? kFixedUpdateSystemHeader
                        : kUnanchoredSystemHeader;
                r.header     = Fill(headerTemplate, cls, ns);
                r.sourceName = cls + ".cpp";
                r.source     = Fill(kSystemSource, cls, ns, RoleSpelling(role), PhaseSpelling(phase));
                break;
            }
            case Kind::PlainClass:
            case Kind::Count:
                r.header     = Fill(kPlainHeader, cls, ns);
                r.sourceName = cls + ".cpp";
                r.source     = Fill(kPlainSource, cls, ns);
                break;
        }
        return r;
    }

    DescriptorFile RenderGameDescriptor(std::string_view moduleName)
    {
        return DescriptorFile{ Arcane::GameDescriptorFileName(moduleName),
                               Arcane::RenderDefaultGameDescriptor(moduleName) };
    }

    std::string GameModuleNameFor(std::string_view gameModule, std::string_view projectName)
    {
        // The stem: strip any directory and the LAST extension (".dll" /
        // ".so" / ".dylib" -- the manifest may name any of them).
        std::string_view stem = gameModule;
        if (const auto slash = stem.find_last_of("/\\"); slash != std::string_view::npos)
            stem.remove_prefix(slash + 1);
        if (const auto dot = stem.find_last_of('.'); dot != std::string_view::npos && dot != 0)
            stem = stem.substr(0, dot);
        return std::string(stem.empty() ? projectName : stem);
    }
}
