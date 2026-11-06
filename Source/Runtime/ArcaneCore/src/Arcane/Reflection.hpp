#pragma once

// The Arcane:: reflection facade (input-seam spec 2026-10-02 s6.1). Game code
// reflects its components with ARC_REFLECT_* and never spells ASTRA_*:
//
//     ARC_REFLECT_TYPE(Health)
//         ARC_REFLECT_FIELD(Health, current)
//             ARC_REFLECT_ATTR(Range, 0.0f, 100.0f)
//     ARC_END_REFLECT_TYPE()
//
// Settings structs (`ARC_REFLECT_TYPE_ATTR(Settings, ...)` + `ARC_SETTINGS`): see Config/Settings.hpp.
// The macros forward to Astra's, so the registered metadata is identical.
// ATTRIBUTES live in Arcane::Attr, not Arcane:: -- Arcane::Hidden is already
// the Outliner-eye tag component (Scene/Components.hpp), serialized by name.
// ARC_REFLECT_ATTR qualifies for you: ARC_REFLECT_ATTR(Hidden). An
// AngleFormat argument is Arcane::Attr::AngleFormat::Unit::Degrees.

// ARC_INTERNAL_BEGIN: the facade's library side
#include <Astra/Reflection/Attribute.hpp>
#include <Astra/Reflection/Reflection.hpp>
#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Config/SettingsField.hpp>
#include <string_view>

namespace Arcane::Attr
{
    using ::Astra::Range;
    using ::Astra::Hidden;
    using ::Astra::ReadOnly;
    using ::Astra::DisplayName;
    using ::Astra::Tooltip;
    using ::Astra::Category;
    using ::Astra::Serializable;
    using ::Astra::ColorFormat;
    using ::Astra::AngleFormat;
    using ::Astra::Multiline;
    using ::Astra::FilePath;
    using ::Astra::DragSpeed;
    using ::Astra::Deprecated;
    using ::Astra::AliasName;
    using ::Astra::Precision;
    // FIELD attribute: the field may be omitted, so a JSON Schema leaves it out
    // of `required` (remote command args structs). Local until the vendored Astra declares
    // `struct Optional`; that sync replaces this with `using ::Astra::Optional;`
    // (Servitor P0 Task 4 fallback).
    struct Optional : ::Astra::AttributeBase<Optional>
    {
        constexpr Optional() noexcept = default;
    };
    // ---- settings attributes (settings arc S2, spec s4.3) -----------------
    // TYPE attribute: this reflected struct is a settings struct. Each field
    // becomes the cvar "<category>.<field>" (ARC_SETTINGS, Config/Settings.hpp),
    // with these defaults for scope, apply mode and audience.
    struct Settings : ::Astra::AttributeBase<Settings>
    {
        std::string_view category;
        SettingScope     scope;
        ApplyMode        apply;
        Audience         audience;
        std::string_view categoryPath{};   // display tree path; empty = derived from the dotted name

        constexpr Settings(std::string_view cat, SettingScope s, ApplyMode a, Audience au,
                           std::string_view tree = {}) noexcept
            : category(cat), scope(s), apply(a), audience(au), categoryPath(tree) {}
    };

    // Field attributes. Any of them on a field overrides the type default.
    struct Keywords : ::Astra::AttributeBase<Keywords>
    {
        std::string_view words;   // space-separated search terms
        constexpr explicit Keywords(std::string_view w) noexcept : words(w) {}
    };
    struct Widget : ::Astra::AttributeBase<Widget>
    {
        std::string_view hint;    // "", "asset:<kind>", "path:file", "path:dir", "keychord", "font", "slider"
        constexpr explicit Widget(std::string_view h) noexcept : hint(h) {}
    };
    struct Deterministic : ::Astra::AttributeBase<Deterministic>
    {
        constexpr Deterministic() noexcept = default;   // CVarFlags::Deterministic: changes replays and goldens
    };
    struct PlayerSafe : ::Astra::AttributeBase<PlayerSafe>
    {
        constexpr PlayerSafe() noexcept = default;      // Audience::PlayerSafe for this field
    };
    struct Apply : ::Astra::AttributeBase<Apply>
    {
        ApplyMode mode;
        constexpr explicit Apply(ApplyMode m) noexcept : mode(m) {}
    };
    struct Scope : ::Astra::AttributeBase<Scope>
    {
        SettingScope scope;
        constexpr explicit Scope(SettingScope s) noexcept : scope(s) {}
    };
    // CVarFlags (Dev, Cheat, Protected, ...). On the TYPE it applies to every
    // field; on a field it is OR-ed in. Hidden and Deterministic have their own
    // attributes; Archive is derived (Pref scopes persist).
    struct Flags : ::Astra::AttributeBase<Flags>
    {
        CVarFlags flags;
        constexpr explicit Flags(CVarFlags f) noexcept : flags(f) {}
    };
}

inline constexpr bool ArcaneReflectTypeAttr_Settings = false;

#define ARC_REFLECT_TYPE(Type)                    ASTRA_REFLECT_TYPE(Type)
#define ARC_REFLECT_FIELD(Type, FieldName) \
    ; ::Arcane::Detail::ReflectField<ArcaneReflectTypeAttr_Settings, Type, decltype(Type::FieldName), &Type::FieldName>(_astra_builder_, #FieldName)
// The field reflects under `NameLit` (a string literal) instead of its member
// name: for a frozen cvar name that cannot be the member's own -- a C++ keyword
// (debug.physics.color.static is the member staticBody), or a name an inheriting
// struct would see twice (debug.physics.trace.lineThickness is traceLineThickness).
#define ARC_REFLECT_FIELD_NAMED(Type, FieldName, NameLit) \
    ; ::Arcane::Detail::ReflectField<ArcaneReflectTypeAttr_Settings, Type, decltype(Type::FieldName), &Type::FieldName>(_astra_builder_, NameLit)
#define ARC_REFLECT_ATTR(AttrType, ...)           .Attr<::Arcane::Attr::AttrType>(__VA_ARGS__)
#define ARC_REFLECT_TYPE_ATTR(AttrType, ...) \
    ; _astra_builder_.TypeAttr<::Arcane::Attr::AttrType>(__VA_ARGS__) \
    ; [[maybe_unused]] constexpr bool ArcaneReflectTypeAttr_##AttrType = true
#define ARC_REFLECT_TYPE_END()                    ASTRA_REFLECT_TYPE_END()
#define ARC_END_REFLECT_TYPE()                    ASTRA_END_REFLECT_TYPE()
#define ARC_REFLECT_ENUM(EnumType)                ASTRA_REFLECT_ENUM(EnumType)
#define ARC_REFLECT_ENUM_VALUE(EnumType, Value)   ASTRA_REFLECT_ENUM_VALUE(EnumType, Value)
#define ARC_REFLECT_ENUM_VALUE_NAMED(EnumType, Value, DisplayName) \
    ASTRA_REFLECT_ENUM_VALUE_NAMED(EnumType, Value, DisplayName)
#define ARC_REFLECT_ENUM_VALUE_FULL(EnumType, Value, DisplayName, Description) \
    ASTRA_REFLECT_ENUM_VALUE_FULL(EnumType, Value, DisplayName, Description)
#define ARC_REFLECT_ENUM_FLAGS()                  ASTRA_REFLECT_ENUM_FLAGS()
#define ARC_REFLECT_ENUM_END()                    ASTRA_REFLECT_ENUM_END()
#define ARC_END_REFLECT_ENUM()                    ASTRA_END_REFLECT_ENUM()

// Opt a component into Astra's per-component change tracking (Changed<T>
// queries). Write it inside the struct body.
#define ARC_CHANGE_TRACKED static constexpr bool AstraChangeTracked = true;
// ARC_INTERNAL_END
