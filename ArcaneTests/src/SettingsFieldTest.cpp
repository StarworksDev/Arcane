// The settings-field map (settings arc S2, spec s4.3): the compile-time type
// map, the codec ARC_REFLECT_FIELD attaches inside a Settings block (and
// ONLY there), and the Arcane::Attr settings attributes. The compile-FAIL half
// (an unmappable field stops the build) is scripts/settings-compile-fail.ps1.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

#include "Helpers/SettingsProbe.hpp"

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace Arcane;

namespace
{
    static_assert(Detail::kIsSettingsFieldType<bool>);
    static_assert(Detail::kIsSettingsFieldType<std::int32_t>);
    static_assert(Detail::kIsSettingsFieldType<std::uint32_t>);
    static_assert(Detail::kIsSettingsFieldType<std::int64_t>);
    static_assert(Detail::kIsSettingsFieldType<std::uint64_t>);
    static_assert(Detail::kIsSettingsFieldType<std::size_t>);   // == std::uint64_t on x64 MSVC
    static_assert(Detail::kIsSettingsFieldType<float>);
    static_assert(Detail::kIsSettingsFieldType<double>);
    static_assert(Detail::kIsSettingsFieldType<std::string>);
    static_assert(Detail::kIsSettingsFieldType<CVarColor>);
    static_assert(Detail::kIsSettingsFieldType<CVarVec2>);
    static_assert(Detail::kIsSettingsFieldType<CVarVec3>);
    static_assert(Detail::kIsSettingsFieldType<CVarVec4>);
    static_assert(Detail::kIsSettingsFieldType<SettingsProbe::Quality>);
    static_assert(!Detail::kIsSettingsFieldType<std::vector<int>>);
    // long is distinct from int32_t on MSVC, but aliases int64_t on LP64.
    static_assert(Detail::kIsSettingsFieldType<long> ==
                  (std::is_same_v<long, std::int32_t> || std::is_same_v<long, std::int64_t>));
    // The real intent: the trait matches the exact fixed-width types, never an
    // integer that merely has the same width. Every platform has one: long on
    // MSVC (32-bit, not int32_t) and macOS (64-bit, not int64_t), long long on
    // LP64 Linux (64-bit, not int64_t).
    using DistinctSameWidthInt = std::conditional_t<
        std::is_same_v<long, std::int32_t> || std::is_same_v<long, std::int64_t>, long long, long>;
    static_assert(sizeof(DistinctSameWidthInt) == sizeof(std::int32_t) || sizeof(DistinctSameWidthInt) == sizeof(std::int64_t));
    static_assert(!std::is_same_v<DistinctSameWidthInt, std::int32_t> && !std::is_same_v<DistinctSameWidthInt, std::int64_t>);
    static_assert(!Detail::kIsSettingsFieldType<DistinctSameWidthInt>);
    static_assert(!Detail::kIsSettingsFieldType<std::uint8_t>);
    static_assert(!Detail::kIsSettingsFieldType<std::int16_t>);
    static_assert(!Detail::kIsSettingsFieldType<const char*>);
    static_assert(Detail::SettingsCVarType<std::uint64_t>() == CVarType::UInt64);
    static_assert(Detail::SettingsCVarType<CVarVec3>() == CVarType::Vec3);
    static_assert(Detail::SettingsCVarType<SettingsProbe::Quality>() == CVarType::Enum);

    // A plain reflected struct: an unmappable field is fine here, and no field
    // may carry a codec -- ARC_REFLECT_FIELD is Astra's Field() outside a
    // Settings block.
    struct PlainComponent
    {
        float            speed = 1.0f;
        std::vector<int> path;
    };

    ARC_REFLECT_TYPE(PlainComponent)
        ARC_REFLECT_FIELD(PlainComponent, speed)
        ARC_REFLECT_FIELD(PlainComponent, path)
    ARC_END_REFLECT_TYPE()

    template <class T>
    Astra::TypeMeta BuildMeta()
    {
        REQUIRE(static_cast<bool>(Astra::Detail::MetaFactory<T>::fn));
        return Astra::Detail::MetaFactory<T>::fn();
    }

    const Detail::SettingsCodec* CodecOf(const Astra::TypeMeta& meta, std::string_view field)
    {
        const Astra::FieldInfo* f = meta.GetField(field);
        REQUIRE(f != nullptr);
        return f->GetAttribute<Detail::SettingsCodec>();
    }
}

TEST_CASE("Settings attributes: the type attribute and every field attribute land on the reflected metadata", "[settings]")
{
    const Astra::TypeMeta meta = BuildMeta<SettingsProbe::ProbeSettings>();
    const auto* type = meta.GetAttribute<Attr::Settings>();
    REQUIRE(type != nullptr);
    CHECK(type->category == "tests.settingsProbe");
    CHECK(type->scope == SettingScope::PreferencesProject);
    CHECK(type->apply == ApplyMode::Live);
    CHECK(type->audience == Audience::Editor);

    const Astra::FieldInfo* count = meta.GetField("count");
    REQUIRE(count != nullptr);
    REQUIRE(count->GetAttribute<Attr::Keywords>() != nullptr);
    CHECK(count->GetAttribute<Attr::Keywords>()->words == "items amount");
    CHECK(count->GetDisplayName() == "Item count");
    CHECK(meta.GetField("ratio")->HasAttribute<Attr::Deterministic>());
    CHECK(meta.GetField("ratio")->GetAttribute<Attr::Apply>()->mode == ApplyMode::NextWorld);
    CHECK(meta.GetField("seconds")->GetAttribute<Attr::Scope>()->scope == SettingScope::Project);
    CHECK(meta.GetField("label")->GetAttribute<Attr::Widget>()->hint == "path:file");
    CHECK(meta.GetField("tint")->HasAttribute<Attr::PlayerSafe>());
    CHECK(meta.GetField("bytes")->GetAttribute<Attr::Flags>()->flags == CVarFlags::Dev);
}

TEST_CASE("ARC_REFLECT_FIELD attaches a codec inside a Settings block and nothing anywhere else", "[settings]")
{
    const Astra::TypeMeta probe = BuildMeta<SettingsProbe::ProbeSettings>();
    REQUIRE(probe.fields.size() == 13);
    for (const Astra::FieldInfo& f : probe.fields)
    {
        INFO(f.name);
        CHECK(f.GetAttribute<Detail::SettingsCodec>() != nullptr);
    }
    CHECK(CodecOf(probe, "toggle")->type == CVarType::Bool);
    CHECK(CodecOf(probe, "count")->type == CVarType::Int32);
    CHECK(CodecOf(probe, "mask")->type == CVarType::UInt32);
    CHECK(CodecOf(probe, "offset")->type == CVarType::Int64);
    CHECK(CodecOf(probe, "bytes")->type == CVarType::UInt64);
    CHECK(CodecOf(probe, "ratio")->type == CVarType::Float32);
    CHECK(CodecOf(probe, "seconds")->type == CVarType::Float64);
    CHECK(CodecOf(probe, "label")->type == CVarType::String);
    CHECK(CodecOf(probe, "tint")->type == CVarType::Color);
    CHECK(CodecOf(probe, "size")->type == CVarType::Vec2);
    CHECK(CodecOf(probe, "axis")->type == CVarType::Vec3);
    CHECK(CodecOf(probe, "rect")->type == CVarType::Vec4);
    CHECK(CodecOf(probe, "quality")->type == CVarType::Enum);

    const Astra::TypeMeta plain = BuildMeta<PlainComponent>();
    REQUIRE(plain.fields.size() == 2);
    for (const Astra::FieldInfo& f : plain.fields)
    {
        INFO(f.name);
        CHECK(f.GetAttribute<Detail::SettingsCodec>() == nullptr);
    }
}

TEST_CASE("Settings codecs round-trip every mapped type through a CVarValue", "[settings]")
{
    const Astra::TypeMeta meta = BuildMeta<SettingsProbe::ProbeSettings>();
    SettingsProbe::ProbeSettings source;
    source.toggle  = false;
    source.count   = 42;
    source.mask    = 0xF0u;
    source.offset  = -9000000000LL;
    source.bytes   = 1ull << 40;
    source.ratio   = 0.125f;
    source.seconds = 1.5;
    source.label   = "renamed";
    source.tint    = { 0.1f, 0.2f, 0.3f, 0.4f };
    source.size    = { 5.0f, 6.0f };
    source.axis    = { 1.0f, 0.0f, 0.0f };
    source.rect    = { 1.0f, 2.0f, 3.0f, 4.0f };
    source.quality = SettingsProbe::Quality::Medium;

    SettingsProbe::ProbeSettings target;
    for (const Astra::FieldInfo& f : meta.fields)
    {
        INFO(f.name);
        const auto* codec = f.GetAttribute<Detail::SettingsCodec>();
        REQUIRE(codec != nullptr);
        const CVarValue v = codec->read(&source);
        CHECK(v.type == codec->type);
        codec->write(&target, v);
    }
    CHECK(target.toggle == false);
    CHECK(target.count == 42);
    CHECK(target.mask == 0xF0u);
    CHECK(target.offset == -9000000000LL);
    CHECK(target.bytes == (1ull << 40));
    CHECK(target.ratio == 0.125f);
    CHECK(target.seconds == 1.5);
    CHECK(target.label == "renamed");
    CHECK(target.tint.r == 0.1f);
    CHECK(target.tint.a == 0.4f);
    CHECK(target.size.y == 6.0f);
    CHECK(target.axis.x == 1.0f);
    CHECK(target.rect.w == 4.0f);
    CHECK(target.quality == SettingsProbe::Quality::Medium);
}

TEST_CASE("Enum codecs store the declared INDEX, not the value, and keep the field on a bad index or type", "[settings]")
{
    const Astra::TypeMeta meta = BuildMeta<SettingsProbe::ProbeSettings>();
    const auto* codec = CodecOf(meta, "quality");
    REQUIRE(codec != nullptr);
    SettingsProbe::ProbeSettings s;                              // quality = High (= 4)
    CHECK(codec->read(&s).AsEnum() == 2);
    CHECK(codec->enumNames() == std::vector<std::string>{ "Low", "Medium", "High" });
    codec->write(&s, CVarValue::Enum(0));
    CHECK(s.quality == SettingsProbe::Quality::Low);
    codec->write(&s, CVarValue::Enum(9));                       // out of range: kept
    CHECK(s.quality == SettingsProbe::Quality::Low);
    codec->write(&s, CVarValue::Int32(1));                      // wrong type: kept
    CHECK(s.quality == SettingsProbe::Quality::Low);
    CHECK(CodecOf(meta, "count")->enumNames().empty());
}

TEST_CASE("Range bounds convert to the field's cvar type; bool, string and enum have none", "[settings]")
{
    const Astra::TypeMeta meta = BuildMeta<SettingsProbe::ProbeSettings>();
    CHECK(CodecOf(meta, "count")->bound(100.0)->AsInt32() == 100);
    CHECK(CodecOf(meta, "mask")->bound(-3.0)->AsUInt32() == 0u);  // a negative bound clamps to 0, never wraps
    CHECK(CodecOf(meta, "bytes")->bound(1e7)->AsUInt64() == 10000000u);
    CHECK(CodecOf(meta, "seconds")->bound(0.5)->AsFloat64() == 0.5);
    CHECK(CodecOf(meta, "tint")->bound(0.5)->AsColor().g == 0.5f);
    CHECK(CodecOf(meta, "rect")->bound(2.0)->AsVec4().w == 2.0f);
    CHECK_FALSE(CodecOf(meta, "toggle")->bound(1.0).has_value());
    CHECK_FALSE(CodecOf(meta, "label")->bound(1.0).has_value());
    CHECK_FALSE(CodecOf(meta, "quality")->bound(1.0).has_value());
}

namespace
{
    // S2-H item 4: an enumerator the reflection block does not list. Its
    // value (2) is outside the reflected table, so the codec falls back to
    // index 0 -- and says so.
    enum class PartlyReflected : std::uint8_t { Listed = 0, AlsoListed = 1, Unlisted = 2 };

    ARC_REFLECT_ENUM(PartlyReflected)
        ARC_REFLECT_ENUM_VALUE(PartlyReflected, Listed)
        ARC_REFLECT_ENUM_VALUE(PartlyReflected, AlsoListed)
    ARC_END_REFLECT_ENUM()

    struct UnlistedDefaultSettings
    {
        PartlyReflected mode = PartlyReflected::Unlisted;
    };

    ARC_REFLECT_TYPE(UnlistedDefaultSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "tests.unlistedDefault", ::Arcane::SettingScope::Project,
                                 ::Arcane::ApplyMode::Live, ::Arcane::Audience::Game)
        ARC_REFLECT_FIELD(UnlistedDefaultSettings, mode)
            ARC_REFLECT_ATTR(Tooltip, "An enum whose default is not reflected.")
    ARC_END_REFLECT_TYPE()

    // S2-H item 5: Range bounds past what the integer field can hold.
    struct WideRangeSettings
    {
        std::int32_t  wide = 1;
        std::int64_t  deep = -1;
        std::int64_t  high = 1;
        std::uint32_t over = 1;
    };

    ARC_REFLECT_TYPE(WideRangeSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "tests.wideRange", ::Arcane::SettingScope::Project,
                                 ::Arcane::ApplyMode::Live, ::Arcane::Audience::Game)
        ARC_REFLECT_FIELD(WideRangeSettings, wide)
            ARC_REFLECT_ATTR(Tooltip, "An int32 whose upper bound overflows it.")
            ARC_REFLECT_ATTR(Range, 0.0, 1e10)
        ARC_REFLECT_FIELD(WideRangeSettings, deep)
            ARC_REFLECT_ATTR(Tooltip, "An int64 whose lower bound overflows it.")
            ARC_REFLECT_ATTR(Range, -1e30, 0.0)
        ARC_REFLECT_FIELD(WideRangeSettings, high)
            ARC_REFLECT_ATTR(Tooltip, "An int64 whose upper bound overflows it.")
            ARC_REFLECT_ATTR(Range, 0.0, 1e30)
        ARC_REFLECT_FIELD(WideRangeSettings, over)
            ARC_REFLECT_ATTR(Tooltip, "A uint32 whose upper bound overflows it.")
            ARC_REFLECT_ATTR(Range, 0.0, 1e12)
    ARC_END_REFLECT_TYPE()

    // Warnings only. Locked: a worker of an earlier test may still log.
    struct WarningCapture
    {
        mutable std::mutex lock;
        std::vector<std::string> messages;
        std::shared_ptr<spdlog::sinks::callback_sink_mt> sink;
        WarningCapture()
        {
            sink = std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg& m) {
                if (m.level != spdlog::level::warn) return;
                const std::lock_guard guard(lock);
                messages.emplace_back(m.payload.data(), m.payload.size());
            });
            Log::Engine()->sinks().push_back(sink);
        }
        ~WarningCapture()
        {
            auto& sinks = Log::Engine()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
        }
        WarningCapture(const WarningCapture&) = delete;
        WarningCapture& operator=(const WarningCapture&) = delete;
        std::size_t Count() const
        {
            const std::lock_guard guard(lock);
            return messages.size();
        }
        std::size_t Mentioning(std::string_view a, std::string_view b) const
        {
            const std::lock_guard guard(lock);
            return static_cast<std::size_t>(std::count_if(messages.begin(), messages.end(), [&](const std::string& m) {
                return m.find(a) != std::string::npos && m.find(b) != std::string::npos;
            }));
        }
    };

    const SettingsFieldDesc& FieldNamed(const SettingsTypeDesc& d, std::string_view name)
    {
        for (const SettingsFieldDesc& f : d.fields)
            if (f.name == name)
                return f;
        FAIL("no field " << name);
        return d.fields.front();
    }
}

TEST_CASE("An enum value outside the reflected table reads as index 0 and warns once per type and value", "[settings]")
{
    const Astra::TypeMeta meta = BuildMeta<UnlistedDefaultSettings>();
    const auto* codec = CodecOf(meta, "mode");
    REQUIRE(codec != nullptr);
    REQUIRE(codec->enumNames() == std::vector<std::string>{ "Listed", "AlsoListed" });
    const WarningCapture log;
    const UnlistedDefaultSettings s;                                // mode = Unlisted (2): not in the table
    CHECK(codec->read(&s).AsEnum() == 0);                           // the fallback is unchanged
    CHECK(codec->read(&s).AsEnum() == 0);
    CHECK(log.Mentioning("PartlyReflected", "2") == 1);             // named once, with the raw value
    UnlistedDefaultSettings listed;
    listed.mode = PartlyReflected::AlsoListed;
    CHECK(codec->read(&listed).AsEnum() == 1);                      // a listed value never warns
    CHECK(log.Count() == 1);
}

TEST_CASE("Integer Range bounds clamp to the field type's limits instead of overflowing", "[settings]")
{
    const SettingsTypeDesc d = DescribeSettings<WideRangeSettings>("settings-field-test");
    REQUIRE(d.error.empty());
    const SettingsFieldDesc& wide = FieldNamed(d, "tests.wideRange.wide");
    REQUIRE(wide.max.has_value());
    CHECK(wide.min->AsInt32() == 0);
    CHECK(wide.max->AsInt32() == std::numeric_limits<std::int32_t>::max());
    const SettingsFieldDesc& deep = FieldNamed(d, "tests.wideRange.deep");
    REQUIRE(deep.min.has_value());
    CHECK(deep.min->AsInt64() == std::numeric_limits<std::int64_t>::min());
    CHECK(deep.max->AsInt64() == 0);
    const SettingsFieldDesc& high = FieldNamed(d, "tests.wideRange.high");
    REQUIRE(high.max.has_value());
    CHECK(high.max->AsInt64() == std::numeric_limits<std::int64_t>::max());
    const SettingsFieldDesc& over = FieldNamed(d, "tests.wideRange.over");
    REQUIRE(over.max.has_value());
    CHECK(over.max->AsUInt32() == std::numeric_limits<std::uint32_t>::max());

    const Astra::TypeMeta meta = BuildMeta<WideRangeSettings>();
    CHECK(CodecOf(meta, "wide")->bound(-1e10)->AsInt32() == std::numeric_limits<std::int32_t>::min());
    CHECK_FALSE(CodecOf(meta, "wide")->bound(std::nan("")).has_value());   // NaN is no bound at all
    CHECK_FALSE(CodecOf(meta, "high")->bound(std::nan("")).has_value());
    CHECK_FALSE(CodecOf(meta, "over")->bound(std::nan("")).has_value());
}
