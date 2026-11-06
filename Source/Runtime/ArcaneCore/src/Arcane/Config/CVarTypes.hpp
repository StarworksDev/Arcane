#pragma once

// Cvar vocabulary (docs/specs/2026-09-02-cvar-system-design.md s3, decision
// 18; settings spec 2026-10-03 s4.1). The enum is the contract games declare
// against: new types are APPENDED, never inserted, so no ordinal moves.

#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace Arcane
{
    enum class CVarType : std::uint8_t
    {
        Bool = 0,
        Int32,
        UInt32,
        Int64,
        UInt64,
        Float32,
        Float64,
        String,
        Color,    // CVarColor: linear RGBA floats
        Vec2,     // CVarVec2
        Vec3,     // CVarVec3
        Vec4,     // CVarVec4
        Enum,     // an Int32 ORDINAL into the cvar's declared names (= 12)
    };

    struct CVarVec2 { float x = 0, y = 0; bool operator==(const CVarVec2&) const = default; };
    struct CVarVec3 { float x = 0, y = 0, z = 0; bool operator==(const CVarVec3&) const = default; };
    struct CVarVec4 { float x = 0, y = 0, z = 0, w = 0; bool operator==(const CVarVec4&) const = default; };
    // Linear floats. Files and the console spell it "#RRGGBBAA", sRGB-encoded
    // (CVarFormat.hpp); the JSON read also takes [r,g,b(,a)] linear.
    struct CVarColor { float r = 0, g = 0, b = 0, a = 1; bool operator==(const CVarColor&) const = default; };

    enum class CVarFlags : std::uint32_t
    {
        None                = 0,
        Dev                 = 1u << 0,   // compiled out in Dist
        Hidden              = 1u << 1,   // never compiled out; omitted from find
        UserSettable        = 1u << 2,   // the default-deny opt-in
        Archive             = 1u << 3,   // implies UserSettable at registration
        Cheat               = 1u << 4,
        Replicated          = 1u << 5,   // reserved until networking enforces it
        ServerOnly          = 1u << 6,
        NotConnected        = 1u << 7,
        Protected           = 1u << 8,
        ServerCanExecute    = 1u << 9,
        ClientCmdCanExecute = 1u << 10,
        ReloadShaders       = 1u << 11,
        ReloadMaterials     = 1u << 12,
        Deterministic       = 1u << 13,
        CommandLineOnly     = 1u << 14,  // only the Default, CommandLine and Code rungs may set it (inventory R2: evidence-capture switches)
        LaunchesProgram     = 1u << 15,  // its value names a program Arcane runs: no rung inside a project may set it (RungRefusal; settings S7-SEC)
    };

    constexpr CVarFlags operator|(CVarFlags a, CVarFlags b) noexcept
    {
        return static_cast<CVarFlags>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
    }
    constexpr CVarFlags operator&(CVarFlags a, CVarFlags b) noexcept
    {
        return static_cast<CVarFlags>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
    }
    constexpr bool HasFlag(CVarFlags set, CVarFlags bit) noexcept
    {
        return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit)) != 0;
    }

    // Who last won the value. Not serialized: rebuilt from the layers on boot.
    // Numbered with gaps so a rung can be inserted later without renumbering.
    enum class SetBy : std::uint8_t
    {
        Default      = 0,
        EngineConfig = 10,
        Plugin       = 20,
        Project      = 30,
        EditorUser   = 35,
        User         = 40,
        CommandLine  = 50,
        Code         = 60,
        Console      = 70,
    };

    // The rung's enumerator name ("Default", ..., "Console"); "?" outside the enum.
    [[nodiscard]] ARC_CORE_API const char* ToString(SetBy by) noexcept;

    constexpr bool operator<(SetBy a, SetBy b) noexcept
    {
        return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
    }
    constexpr bool operator>(SetBy a, SetBy b) noexcept { return b < a; }
    constexpr bool operator<=(SetBy a, SetBy b) noexcept { return !(b < a); }
    constexpr bool operator>=(SetBy a, SetBy b) noexcept { return !(a < b); }

    // Who is asking right now (settings spec s3.2): the session role, not which
    // console was used. It replaces the v1 Permission:
    //   - the editor, its console and its --set are Editor;
    //   - a single-player game or a listen-server host is LocalHost;
    //   - a dedicated server's own console or an authenticated remote admin is ServerAdmin;
    //   - a player connected to someone else's server is Client.
    enum class CVarContext : std::uint8_t { Editor, LocalHost, ServerAdmin, Client };

    // Who a setting is for (settings spec 2026-10-03 s3.2), declared once per
    // setting. Editor settings live in the editor DLL, so a game never has them.
    enum class Audience : std::uint8_t
    {
        Editor,
        Game,
        PlayerSafe,   // a Game refinement any player may change
        Server,
    };

    // Where a setting's DEFAULT is edited (s3.3; inventory Pref-M / Pref-P /
    // Project): the Preferences window, machine-wide or per project, or the
    // shared Project Settings window.
    enum class SettingScope : std::uint8_t
    {
        PreferencesMachine,
        PreferencesProject,
        Project,
    };

    // When a change reaches its consumer (s3.4).
    enum class ApplyMode : std::uint8_t
    {
        Live,        // the next Publish
        NextWorld,   // read when a world, registry or physics world is created
        Restart,     // read once at boot
    };

    // Why a rung may not set a cvar at all (settings S7-SEC, amending spec
    // s3.3/s11.1). Project config is shared and committed: a cloned repository
    // must never choose a program Arcane runs, nor the user's machine-wide
    // preferences.
    //   - LaunchesProgram: honoured only from the default, the machine-wide
    //     EditorUser rung, --set, code and an Editor-context console. The
    //     EngineConfig, Plugin, Project and User rungs refuse it. A config
    //     folder inside a project or plugin root refuses it whatever its rung
    //     (CVarConfig's walk checks the folder).
    //   - MachinePreference: a PreferencesMachine setting is the user's; the
    //     Plugin and Project rungs refuse it. Its per-project override is the
    //     User rung ("This project", s3.3), which keeps it.
    // PreferencesProject keeps "the project may suggest": a project default,
    // overridden by the user's rungs.
    enum class CVarRungRefusal : std::uint8_t { None, LaunchesProgram, MachinePreference };

    constexpr CVarRungRefusal RungRefusal(CVarFlags flags, SettingScope scope, SetBy by) noexcept
    {
        const bool projectFile = by == SetBy::Plugin || by == SetBy::Project;
        if (HasFlag(flags, CVarFlags::LaunchesProgram) && (projectFile || by == SetBy::EngineConfig || by == SetBy::User))
            return CVarRungRefusal::LaunchesProgram;
        if (scope == SettingScope::PreferencesMachine && projectFile)
            return CVarRungRefusal::MachinePreference;
        return CVarRungRefusal::None;
    }

    // The reason a refusal warning gives, after the file and the key.
    constexpr std::string_view RungRefusalReason(CVarRungRefusal why) noexcept
    {
        switch (why)
        {
        case CVarRungRefusal::LaunchesProgram:   return "names a program; set it in Preferences (machine) or with --set";
        case CVarRungRefusal::MachinePreference: return "is a machine-wide preference; set it in Preferences, not in project config";
        case CVarRungRefusal::None:              break;
        }
        return {};
    }

    // The thirteen value types. Enum shares Int32's storage; the type tag
    // keeps them apart, so an Enum never equals an Int32.
    class CVarValue
    {
    public:
        static CVarValue Bool(bool v)        { return CVarValue(CVarType::Bool, v); }
        static CVarValue Int32(std::int32_t v)   { return CVarValue(CVarType::Int32, v); }
        static CVarValue UInt32(std::uint32_t v) { return CVarValue(CVarType::UInt32, v); }
        static CVarValue Int64(std::int64_t v)   { return CVarValue(CVarType::Int64, v); }
        static CVarValue UInt64(std::uint64_t v) { return CVarValue(CVarType::UInt64, v); }
        static CVarValue Float32(float v)    { return CVarValue(CVarType::Float32, v); }
        static CVarValue Float64(double v)   { return CVarValue(CVarType::Float64, v); }
        static CVarValue String(std::string v) { return CVarValue(CVarType::String, std::move(v)); }
        static CVarValue Color(CVarColor v)  { return CVarValue(CVarType::Color, v); }
        static CVarValue Vec2(CVarVec2 v)    { return CVarValue(CVarType::Vec2, v); }
        static CVarValue Vec3(CVarVec3 v)    { return CVarValue(CVarType::Vec3, v); }
        static CVarValue Vec4(CVarVec4 v)    { return CVarValue(CVarType::Vec4, v); }
        static CVarValue Enum(std::int32_t ordinal) { return CVarValue(CVarType::Enum, ordinal); }

        CVarType type = CVarType::Bool;

        bool AsBool() const { return std::get<bool>(m_storage); }
        std::int32_t AsInt32() const { return std::get<std::int32_t>(m_storage); }
        std::uint32_t AsUInt32() const { return std::get<std::uint32_t>(m_storage); }
        std::int64_t AsInt64() const { return std::get<std::int64_t>(m_storage); }
        std::uint64_t AsUInt64() const { return std::get<std::uint64_t>(m_storage); }
        float AsFloat32() const { return std::get<float>(m_storage); }
        double AsFloat64() const { return std::get<double>(m_storage); }
        const std::string& AsString() const { return std::get<std::string>(m_storage); }
        CVarColor AsColor() const { return std::get<CVarColor>(m_storage); }
        CVarVec2 AsVec2() const { return std::get<CVarVec2>(m_storage); }
        CVarVec3 AsVec3() const { return std::get<CVarVec3>(m_storage); }
        CVarVec4 AsVec4() const { return std::get<CVarVec4>(m_storage); }
        std::int32_t AsEnum() const { return std::get<std::int32_t>(m_storage); }

        bool operator==(const CVarValue& o) const
        {
            return type == o.type && m_storage == o.m_storage;
        }

    private:
        using Storage = std::variant<bool, std::int32_t, std::uint32_t, std::int64_t,
                                      std::uint64_t, float, double, std::string,
                                      CVarColor, CVarVec2, CVarVec3, CVarVec4>;

        template <typename T>
        CVarValue(CVarType t, T v) : type(t), m_storage(std::move(v)) {}

        Storage m_storage{ false };
    };

    namespace Detail
    {
        constexpr float ClampLow(float v, float lo) noexcept { return v < lo ? lo : v; }
        constexpr float ClampHigh(float v, float hi) noexcept { return v > hi ? hi : v; }
    }

    // Clamp a value into [min, max] when a bound is present and the same
    // type: numbers whole, Color and Vec* per component. Strings, bools and
    // Enums are returned unchanged. A mismatched bound is ignored rather
    // than applied as a different type.
    inline CVarValue Clamp(CVarValue value, const std::optional<CVarValue>& min,
                           const std::optional<CVarValue>& max)
    {
        switch (value.type)
        {
        case CVarType::Int32:
        {
            auto v = value.AsInt32();
            if (min && min->type == CVarType::Int32 && v < min->AsInt32()) v = min->AsInt32();
            if (max && max->type == CVarType::Int32 && v > max->AsInt32()) v = max->AsInt32();
            return CVarValue::Int32(v);
        }
        case CVarType::UInt32:
        {
            auto v = value.AsUInt32();
            if (min && min->type == CVarType::UInt32 && v < min->AsUInt32()) v = min->AsUInt32();
            if (max && max->type == CVarType::UInt32 && v > max->AsUInt32()) v = max->AsUInt32();
            return CVarValue::UInt32(v);
        }
        case CVarType::Int64:
        {
            auto v = value.AsInt64();
            if (min && min->type == CVarType::Int64 && v < min->AsInt64()) v = min->AsInt64();
            if (max && max->type == CVarType::Int64 && v > max->AsInt64()) v = max->AsInt64();
            return CVarValue::Int64(v);
        }
        case CVarType::UInt64:
        {
            auto v = value.AsUInt64();
            if (min && min->type == CVarType::UInt64 && v < min->AsUInt64()) v = min->AsUInt64();
            if (max && max->type == CVarType::UInt64 && v > max->AsUInt64()) v = max->AsUInt64();
            return CVarValue::UInt64(v);
        }
        case CVarType::Float32:
        {
            auto v = value.AsFloat32();
            if (min && min->type == CVarType::Float32 && v < min->AsFloat32()) v = min->AsFloat32();
            if (max && max->type == CVarType::Float32 && v > max->AsFloat32()) v = max->AsFloat32();
            return CVarValue::Float32(v);
        }
        case CVarType::Float64:
        {
            auto v = value.AsFloat64();
            if (min && min->type == CVarType::Float64 && v < min->AsFloat64()) v = min->AsFloat64();
            if (max && max->type == CVarType::Float64 && v > max->AsFloat64()) v = max->AsFloat64();
            return CVarValue::Float64(v);
        }
        case CVarType::Color:
        {
            CVarColor v = value.AsColor();
            if (min && min->type == CVarType::Color)
            {
                const CVarColor lo = min->AsColor();
                v = { Detail::ClampLow(v.r, lo.r), Detail::ClampLow(v.g, lo.g), Detail::ClampLow(v.b, lo.b), Detail::ClampLow(v.a, lo.a) };
            }
            if (max && max->type == CVarType::Color)
            {
                const CVarColor hi = max->AsColor();
                v = { Detail::ClampHigh(v.r, hi.r), Detail::ClampHigh(v.g, hi.g), Detail::ClampHigh(v.b, hi.b), Detail::ClampHigh(v.a, hi.a) };
            }
            return CVarValue::Color(v);
        }
        case CVarType::Vec2:
        {
            CVarVec2 v = value.AsVec2();
            if (min && min->type == CVarType::Vec2)
            {
                const CVarVec2 lo = min->AsVec2();
                v = { Detail::ClampLow(v.x, lo.x), Detail::ClampLow(v.y, lo.y) };
            }
            if (max && max->type == CVarType::Vec2)
            {
                const CVarVec2 hi = max->AsVec2();
                v = { Detail::ClampHigh(v.x, hi.x), Detail::ClampHigh(v.y, hi.y) };
            }
            return CVarValue::Vec2(v);
        }
        case CVarType::Vec3:
        {
            CVarVec3 v = value.AsVec3();
            if (min && min->type == CVarType::Vec3)
            {
                const CVarVec3 lo = min->AsVec3();
                v = { Detail::ClampLow(v.x, lo.x), Detail::ClampLow(v.y, lo.y), Detail::ClampLow(v.z, lo.z) };
            }
            if (max && max->type == CVarType::Vec3)
            {
                const CVarVec3 hi = max->AsVec3();
                v = { Detail::ClampHigh(v.x, hi.x), Detail::ClampHigh(v.y, hi.y), Detail::ClampHigh(v.z, hi.z) };
            }
            return CVarValue::Vec3(v);
        }
        case CVarType::Vec4:
        {
            CVarVec4 v = value.AsVec4();
            if (min && min->type == CVarType::Vec4)
            {
                const CVarVec4 lo = min->AsVec4();
                v = { Detail::ClampLow(v.x, lo.x), Detail::ClampLow(v.y, lo.y), Detail::ClampLow(v.z, lo.z), Detail::ClampLow(v.w, lo.w) };
            }
            if (max && max->type == CVarType::Vec4)
            {
                const CVarVec4 hi = max->AsVec4();
                v = { Detail::ClampHigh(v.x, hi.x), Detail::ClampHigh(v.y, hi.y), Detail::ClampHigh(v.z, hi.z), Detail::ClampHigh(v.w, hi.w) };
            }
            return CVarValue::Vec4(v);
        }
        default:
            return value;
        }
    }
}
