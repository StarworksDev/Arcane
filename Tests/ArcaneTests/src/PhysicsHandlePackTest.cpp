// 3D-readiness Plan A, Task 5: PhysicsBodyRef2D::handle is the packed uint64.
// The binary form of the component drops the handle (SerializationTraits v2),
// and a version-1 blob captured before the change still loads.
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"

#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    std::string ReadBytes(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
}

TEST_CASE("PhysicsBodyRef2D handle is the packed uint64 and a v1 blob loads as scale only", "[physics][handle]")
{
    using Arcane::Detail::Physics2D::PackBody;
    Manifold2D::Physics::BodyHandle h{ 7u, 3u };
    const std::uint64_t packed = PackBody(h);
    CHECK(packed == ((7ull << 32) | 3ull));
    const auto back = Arcane::Detail::Physics2D::UnpackBody(packed);
    CHECK(back.index == 7u);
    CHECK(back.generation == 3u);
    CHECK(PackBody(Manifold2D::Physics::BodyHandle{ 0u, 0u }) == 0u);

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*components);
    Arcane::RegisterPhysicsComponents2D(*components);
    Astra::Registry reg(components);
    const auto e = reg.CreateEntity();
    Arcane::PhysicsBodyRef2D ref;
    ref.handle = packed;
    ref.appliedScale = glm::vec2{ 2.f, 3.f };
    reg.AddComponent<Arcane::PhysicsBodyRef2D>(e, ref);
    const auto fresh = fs::temp_directory_path() / "arcane-handle-pack.arcscene";
    REQUIRE(reg.Save(fresh).IsOk());
    auto loaded = Astra::Registry::Load(fresh, components);
    REQUIRE(loaded.IsOk());
    std::unique_ptr<Astra::Registry> freshReg = std::move(*loaded.GetValue());
    const auto* out = freshReg->GetComponent<Arcane::PhysicsBodyRef2D>(e);
    REQUIRE(out != nullptr);
    CHECK(out->handle == 0u);
    CHECK(out->appliedScale.x == 2.f);
    CHECK(out->appliedScale.y == 3.f);
    fs::remove(fresh);

    // The captured v1 file holds one PhysicsBodyRef2D whose handle was {7, 3}
    // and appliedScale {2, 3}. It must load as handle 0 with the scale intact.
    const auto v1 = Arcane::Test::RepoRoot() / "Tests/ArcaneTests/data/physics/v1-bodyref-save.bin";
    auto fromV1 = Astra::Registry::Load(v1, components);
    REQUIRE(fromV1.IsOk());
    std::unique_ptr<Astra::Registry> v1Reg = std::move(*fromV1.GetValue());
    int count = 0;
    v1Reg->CreateView<const Arcane::PhysicsBodyRef2D>().ForEach(
        [&](Astra::Entity, const Arcane::PhysicsBodyRef2D& r)
        {
            ++count;
            CHECK(r.handle == 0u);
            CHECK(r.appliedScale.x == 2.f);
            CHECK(r.appliedScale.y == 3.f);
        });
    CHECK(count == 1);
}

TEST_CASE("the component header names no Manifold type", "[physics][handle]")
{
    const std::string text = ReadBytes(Arcane::Test::RepoRoot() /
        "Source/Runtime/ArcaneScene/src/Arcane/Scene/PhysicsComponents.hpp");
    REQUIRE(!text.empty());
    CHECK(text.find("Manifold2D") == std::string::npos);
    CHECK(text.find("Physics2DHandles.hpp") == std::string::npos);
    CHECK(text.find("std::uint64_t handle") != std::string::npos);
    CHECK(text.find("SerializationTraits") != std::string::npos);
}

namespace
{
    template<typename T>
    void Append(std::vector<std::byte>& buf, T v)
    {
        const auto* p = reinterpret_cast<const std::byte*>(&v);
        buf.insert(buf.end(), p, p + sizeof(T));
    }

    std::vector<std::byte> VersionedBuffer(std::uint32_t version)
    {
        std::vector<std::byte> buf;
        Append<std::uint64_t>(buf, Astra::TypeID<Arcane::PhysicsBodyRef2D>::Hash());
        Append<std::uint32_t>(buf, version);
        buf.insert(buf.end(), 16, std::byte{ 0xFF });
        return buf;
    }
}

TEST_CASE("a bodyref version other than 1 or 2 returns the code the reader can set", "[physics][handle]")
{
    using Astra::SerializationError;

    // Direct reader: version 0 is below MinVersion, so Astra refuses it
    // before Migrate; version 3 reaches Migrate, which over-reads on purpose.
    {
        const auto buf = VersionedBuffer(0);
        Astra::BinaryReader reader(buf);
        Arcane::PhysicsBodyRef2D c;
        auto r = reader.ReadVersionedComponent(c);
        REQUIRE(r.IsErr());
        CHECK(*r.GetError() == SerializationError::UnsupportedVersion);
    }
    {
        const auto buf = VersionedBuffer(3);
        Astra::BinaryReader reader(buf);
        Arcane::PhysicsBodyRef2D c;
        auto r = reader.ReadVersionedComponent(c);
        REQUIRE(r.IsErr());
        CHECK(*r.GetError() == SerializationError::CorruptedData);
        CHECK(c.handle == 0u);
        CHECK(c.appliedScale.x == 1.f);
        CHECK(c.appliedScale.y == 1.f);
        CHECK(reader.GetPosition() == sizeof(std::uint64_t) + sizeof(std::uint32_t));
    }
    {
        // Version 2 is the current form: two floats, handle forced to 0.
        std::vector<std::byte> buf;
        Append<std::uint64_t>(buf, Astra::TypeID<Arcane::PhysicsBodyRef2D>::Hash());
        Append<std::uint32_t>(buf, 2u);
        Append<float>(buf, 5.f);
        Append<float>(buf, 6.f);
        Astra::BinaryReader reader(buf);
        Arcane::PhysicsBodyRef2D c;
        c.handle = 99;
        auto r = reader.ReadVersionedComponent(c);
        REQUIRE(r.IsOk());
        CHECK(c.handle == 0u);
        CHECK(c.appliedScale.x == 5.f);
        CHECK(c.appliedScale.y == 6.f);
    }

    // Registry::Load collapses every reader code to CorruptedData. Patch the
    // per-element version in a copy of the captured file: the hash occurs in
    // the descriptor (next uint32 is the 16-byte size) and once per element
    // (next uint32 is the version 1).
    const auto v1 = Arcane::Test::RepoRoot() / "Tests/ArcaneTests/data/physics/v1-bodyref-save.bin";
    const std::string original = ReadBytes(v1);
    REQUIRE(!original.empty());
    const std::uint64_t hash = Astra::TypeID<Arcane::PhysicsBodyRef2D>::Hash();
    std::size_t patchAt = std::string::npos;
    int versionOnes = 0;
    for (std::size_t i = 0; i + 12 <= original.size(); ++i)
    {
        std::uint64_t h = 0;
        std::memcpy(&h, original.data() + i, sizeof h);
        if (h != hash) continue;
        std::uint32_t next = 0;
        std::memcpy(&next, original.data() + i + 8, sizeof next);
        if (next == 1u) { ++versionOnes; patchAt = i + 8; }
    }
    REQUIRE(versionOnes == 1);

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*components);
    Arcane::RegisterPhysicsComponents2D(*components);
    for (std::uint32_t bad : { 0u, 3u })
    {
        std::string copy = original;
        std::memcpy(copy.data() + patchAt, &bad, sizeof bad);
        const auto path = fs::temp_directory_path() / "arcane-bodyref-patched.arcscene";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(copy.data(), static_cast<std::streamsize>(copy.size()));
        }
        auto loaded = Astra::Registry::Load(path, components);
        REQUIRE(loaded.IsErr());
        CHECK(*loaded.GetError() == SerializationError::CorruptedData);
        fs::remove(path);
    }
}
