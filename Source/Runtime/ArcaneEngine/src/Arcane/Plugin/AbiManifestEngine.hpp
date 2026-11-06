// A macro guard, not pragma once, and placed FIRST: the [abi] comment-stability
// witness compiles a byte copy of this header under another path
// (Intermediate/AbiDigest/AbiManifestEngine.commented.hpp) ahead of the real
// one, and the copy's guard must hide the real body (a path-keyed pragma once
// would let both define the same entities in one TU).
//
// Include cycle: this header includes GameModule.hpp, whose last line includes
// Plugin/AbiManifestGame.hpp, which DECLARES AbiManifestEngineMix() before it
// includes this header again (skipped here). The definition below completes it.
#ifndef ARC_PLUGIN_ABI_MANIFEST_ENGINE_HPP
#define ARC_PLUGIN_ABI_MANIFEST_ENGINE_HPP

// The ENGINE group of the compiler-derived plugin ABI digest (3D-readiness Plan
// B, Tasks 1-2). The digest is FoldGroups(Engine, Input, Client, Physics2D)
// (Core/AbiManifestHash.hpp); this header is the Engine group, everything an
// ArcaneEngine-only game can include. The Input group (InputSnapshot) lives in
// Input/AbiManifestInput.hpp, the Client group (ClientRuntime,
// RuntimePresentation) in Client/AbiManifestClient.hpp and the Physics2D group
// (PhysicsWorld2D, the event rows, the body records) in
// Physics2D/AbiManifestPhysics2D.hpp, so a game TU computes its digest
// (Plugin/AbiManifestGame.hpp) without the Input, Client or Physics2D include
// directories: a game whose descriptor does not name ArcanePhysics2D includes
// no Physics2D or Manifold2D header through GameModule.hpp (Task 8 fix round 1).
//
// AbiManifestEngineMix() hashes what the COMPILER says about the game-facing
// by-value closure: the three Astra layout defines, then sizeof / alignof /
// offsetof of every struct in the 21 game-facing headers (the kGameFacingHeaders
// list in ArcaneSpellingGuardTest.cpp) outside the Input, Client and Physics2D groups. It
// never hashes source text, so a comment-only edit of a listed header leaves the
// value alone, and a game TU compiled with different ASTRA_ENTITY_* defines gets
// a different value.
//
// Stream of each group (FNV-1a 64, Core/AbiManifestHash.hpp); the defines are
// the Engine group's alone:
//   1. "ASTRA_ENTITY_BITS=<dec>\n", "ASTRA_ENTITY_VERSION_BITS=<dec>\n",
//      "ASTRA_MAX_COMPONENTS=<dec>\n".
//   2. Per record: "<type name>\n", sizeof (4 LE bytes), alignof (4 LE bytes),
//      then per field in declaration order: "<field>\n", offsetof (4 LE),
//      sizeof(field type) (4 LE), alignof(field type) (4 LE),
//      "<declared type>\n".
// Every type name and every declared type below is a string literal. None is
// derived from Astra::TypeID<T>::Name(): that spelling is compiler-dependent
// for aliases such as std::int64_t, which would make a Linux --check disagree
// with a Windows --write.
//
// OPAQUE FROM THE FIRST CONTAINER. A record that holds a standard-library or
// Astra container (std::string, std::vector, std::function, Astra::FlatMap,
// ...) has a sizeof / alignof / offsetof that moves with the configuration
// (MSVC _ITERATOR_DEBUG_LEVEL: Debug and Release disagree) and with the
// standard library. One build/abi-digest.txt must hold for every configuration,
// so members BEFORE the first container hash their real offset, size and
// alignment; the first container and every member after it hash those numbers
// as 0 (Opaque()), and so does the record's own sizeof / alignof. Names, field
// order and declared type names are always hashed, so adding, removing,
// reordering or retyping a field still moves the digest, and an int added
// before the first container shifts a real offset. The offsetof / sizeof
// expressions stay in the source (wrapped in Opaque()) so the compiler still
// proves every field exists and is accessible. Private members are reached
// through the friend Access struct below (the Client group's ClientAccess).
//
// This function is constexpr and header-defined, and it is NOT exported: a game
// compiles its own body (through GameModule.hpp -> AbiManifestGame.hpp), so
// ASTRA_ENTITY_BITS=64 changes ITS value.

#include <Arcane/Core/AbiAccess.hpp>
#include <Arcane/Core/AbiManifestHash.hpp>

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/GameComponents.hpp>
#include <Arcane/Plugin/GameModule.hpp>
#include <Arcane/Plugin/GameSystems.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem2D.hpp>
#include <Arcane/Scene/SceneCacheRecords.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Scene/ViewTransform.hpp>
#include <Arcane/Math/Aabb.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

namespace Arcane
{
    namespace Detail::AbiManifest
    {
        // Hashes the PRIVATE members of the classes that befriend this struct
        // (friend struct ::Arcane::Detail::AbiManifest::Access;). Game code
        // compiles those classes' inline accessors, so a private member's
        // offset is ABI. Opaque() applies from the first library container on.
        // (ClientRuntime befriends the Client group's ClientAccess instead.)
        struct Access
        {
        static constexpr void R_GameModule(Fnv& f) noexcept
        {
            f.Type("Arcane::GameModule", sizeof(Arcane::GameModule), alignof(Arcane::GameModule));
            f.Field("m_ctx", offsetof(Arcane::GameModule, m_ctx), sizeof(Arcane::GameModule::m_ctx), alignof(decltype(Arcane::GameModule::m_ctx)), "EngineContext*");
            f.Field("m_components", offsetof(Arcane::GameModule, m_components), sizeof(Arcane::GameModule::m_components), alignof(decltype(Arcane::GameModule::m_components)), "Arcane::ComponentModule*");
        }

        static constexpr void R_SystemFactoryTable(Fnv& f) noexcept
        {
            f.Type("Arcane::SystemFactoryTable", Opaque(sizeof(Arcane::SystemFactoryTable)), Opaque(alignof(Arcane::SystemFactoryTable)));
            f.Field("m_entries", Opaque(offsetof(Arcane::SystemFactoryTable, m_entries)), Opaque(sizeof(Arcane::SystemFactoryTable::m_entries)), Opaque(alignof(decltype(Arcane::SystemFactoryTable::m_entries))), "std::vector<SystemFactoryEntry>");
            f.Field("m_openOwner", Opaque(offsetof(Arcane::SystemFactoryTable, m_openOwner)), sizeof(Arcane::SystemFactoryTable::m_openOwner), alignof(decltype(Arcane::SystemFactoryTable::m_openOwner)), "const void*");
        }

        static constexpr void R_Runtime(Fnv& f) noexcept
        {
            f.Type("Arcane::Runtime", sizeof(Arcane::Runtime), alignof(Arcane::Runtime));
            f.Field("m_impl", offsetof(Arcane::Runtime, m_impl), sizeof(Arcane::Runtime::m_impl), alignof(decltype(Arcane::Runtime::m_impl)), "std::unique_ptr<Impl>");
        }

        static constexpr void R_ProcessContext(Fnv& f) noexcept
        {
            f.Type("Arcane::ProcessContext", Opaque(sizeof(Arcane::ProcessContext)), Opaque(alignof(Arcane::ProcessContext)));
            f.Field("m_owned", offsetof(Arcane::ProcessContext, m_owned), sizeof(Arcane::ProcessContext::m_owned), alignof(decltype(Arcane::ProcessContext::m_owned)), "std::unique_ptr<Arcane::TypeContext>");
            f.Field("m_context", offsetof(Arcane::ProcessContext, m_context), sizeof(Arcane::ProcessContext::m_context), alignof(decltype(Arcane::ProcessContext::m_context)), "Arcane::TypeContext*");
            f.Field("m_factories", Opaque(offsetof(Arcane::ProcessContext, m_factories)), Opaque(sizeof(Arcane::ProcessContext::m_factories)), Opaque(alignof(decltype(Arcane::ProcessContext::m_factories))), "SystemFactoryTable");
            f.Field("m_dedicated", Opaque(offsetof(Arcane::ProcessContext, m_dedicated)), sizeof(Arcane::ProcessContext::m_dedicated), alignof(decltype(Arcane::ProcessContext::m_dedicated)), "bool");
            f.Field("m_slotHeld", Opaque(offsetof(Arcane::ProcessContext, m_slotHeld)), sizeof(Arcane::ProcessContext::m_slotHeld), alignof(decltype(Arcane::ProcessContext::m_slotHeld)), "bool");
            // std::mutex is library-defined state whose size differs by standard
            // library (MSVC 80 bytes, libstdc++ 40, libc++ 64 on x64), so it hashes
            // opaque like a container. std::weak_ptr is two pointers in all three
            // (as m_owned's unique_ptr is one), so its own size and alignment are
            // real. Both follow m_factories, so their offsets are opaque either way.
            f.Field("m_jobsMutex", Opaque(offsetof(Arcane::ProcessContext, m_jobsMutex)), Opaque(sizeof(Arcane::ProcessContext::m_jobsMutex)), Opaque(alignof(decltype(Arcane::ProcessContext::m_jobsMutex))), "std::mutex");
            f.Field("m_jobs", Opaque(offsetof(Arcane::ProcessContext, m_jobs)), sizeof(Arcane::ProcessContext::m_jobs), alignof(decltype(Arcane::ProcessContext::m_jobs)), "std::weak_ptr<Arcane::JobSystem>");
        }

        static constexpr void R_PhysicsSystem2D(Fnv& f) noexcept
        {
            f.Type("Arcane::PhysicsSystem2D", sizeof(Arcane::PhysicsSystem2D), alignof(Arcane::PhysicsSystem2D));
            f.Field("m_fixedDt", offsetof(Arcane::PhysicsSystem2D, m_fixedDt), sizeof(Arcane::PhysicsSystem2D::m_fixedDt), alignof(decltype(Arcane::PhysicsSystem2D::m_fixedDt)), "float");
            f.Field("m_stepWorld", offsetof(Arcane::PhysicsSystem2D, m_stepWorld), sizeof(Arcane::PhysicsSystem2D::m_stepWorld), alignof(decltype(Arcane::PhysicsSystem2D::m_stepWorld)), "bool");
        }
        };
    }

    constexpr std::uint64_t AbiManifestEngineMix() noexcept
    {
        using Detail::AbiManifest::Fnv;
        using Detail::AbiManifest::Opaque;
        Fnv f;
        f.Define("ASTRA_ENTITY_BITS", static_cast<std::uint64_t>(ASTRA_ENTITY_BITS));
        f.Define("ASTRA_ENTITY_VERSION_BITS", static_cast<std::uint64_t>(ASTRA_ENTITY_VERSION_BITS));
        f.Define("ASTRA_MAX_COMPONENTS", static_cast<std::uint64_t>(ASTRA_MAX_COMPONENTS));

        f.Type("Astra::Entity", sizeof(Astra::Entity), alignof(Astra::Entity));
        f.Type("Arcane::Guid", sizeof(Arcane::Guid), alignof(Arcane::Guid));
        f.Field("hi", offsetof(Arcane::Guid, hi), sizeof(Arcane::Guid::hi), alignof(decltype(Arcane::Guid::hi)), "std::uint64_t");
        f.Field("lo", offsetof(Arcane::Guid, lo), sizeof(Arcane::Guid::lo), alignof(decltype(Arcane::Guid::lo)), "std::uint64_t");
        f.Type("Arcane::Aabb", sizeof(Arcane::Aabb), alignof(Arcane::Aabb));
        f.Field("min", offsetof(Arcane::Aabb, min), sizeof(Arcane::Aabb::min), alignof(decltype(Arcane::Aabb::min)), "glm::vec3");
        f.Field("max", offsetof(Arcane::Aabb, max), sizeof(Arcane::Aabb::max), alignof(decltype(Arcane::Aabb::max)), "glm::vec3");
        f.Type("Arcane::ViewTransform", sizeof(Arcane::ViewTransform), alignof(Arcane::ViewTransform));
        f.Field("view", offsetof(Arcane::ViewTransform, view), sizeof(Arcane::ViewTransform::view), alignof(decltype(Arcane::ViewTransform::view)), "glm::mat4");
        f.Field("projection", offsetof(Arcane::ViewTransform, projection), sizeof(Arcane::ViewTransform::projection), alignof(decltype(Arcane::ViewTransform::projection)), "glm::mat4");
        f.Field("viewport", offsetof(Arcane::ViewTransform, viewport), sizeof(Arcane::ViewTransform::viewport), alignof(decltype(Arcane::ViewTransform::viewport)), "glm::uvec2");
        f.Type("Arcane::MeshVertex", sizeof(Arcane::MeshVertex), alignof(Arcane::MeshVertex));
        f.Field("position", offsetof(Arcane::MeshVertex, position), sizeof(Arcane::MeshVertex::position), alignof(decltype(Arcane::MeshVertex::position)), "glm::vec3");
        f.Field("normal", offsetof(Arcane::MeshVertex, normal), sizeof(Arcane::MeshVertex::normal), alignof(decltype(Arcane::MeshVertex::normal)), "glm::vec3");
        f.Field("uv", offsetof(Arcane::MeshVertex, uv), sizeof(Arcane::MeshVertex::uv), alignof(decltype(Arcane::MeshVertex::uv)), "glm::vec2");
        f.Type("Arcane::MeshSection", Opaque(sizeof(Arcane::MeshSection)), Opaque(alignof(Arcane::MeshSection)));
        f.Field("name", Opaque(offsetof(Arcane::MeshSection, name)), Opaque(sizeof(Arcane::MeshSection::name)), Opaque(alignof(decltype(Arcane::MeshSection::name))), "std::string");
        f.Field("indexOffset", Opaque(offsetof(Arcane::MeshSection, indexOffset)), sizeof(Arcane::MeshSection::indexOffset), alignof(decltype(Arcane::MeshSection::indexOffset)), "std::uint32_t");
        f.Field("indexCount", Opaque(offsetof(Arcane::MeshSection, indexCount)), sizeof(Arcane::MeshSection::indexCount), alignof(decltype(Arcane::MeshSection::indexCount)), "std::uint32_t");
        f.Field("slotIndex", Opaque(offsetof(Arcane::MeshSection, slotIndex)), sizeof(Arcane::MeshSection::slotIndex), alignof(decltype(Arcane::MeshSection::slotIndex)), "std::uint32_t");
        f.Type("Arcane::MeshData", Opaque(sizeof(Arcane::MeshData)), Opaque(alignof(Arcane::MeshData)));
        f.Field("vertices", Opaque(offsetof(Arcane::MeshData, vertices)), Opaque(sizeof(Arcane::MeshData::vertices)), Opaque(alignof(decltype(Arcane::MeshData::vertices))), "std::vector<MeshVertex>");
        f.Field("indices", Opaque(offsetof(Arcane::MeshData, indices)), Opaque(sizeof(Arcane::MeshData::indices)), Opaque(alignof(decltype(Arcane::MeshData::indices))), "std::vector<std::uint32_t>");
        f.Field("sections", Opaque(offsetof(Arcane::MeshData, sections)), Opaque(sizeof(Arcane::MeshData::sections)), Opaque(alignof(decltype(Arcane::MeshData::sections))), "std::vector<MeshSection>");
        f.Type("Arcane::MeshSlot", Opaque(sizeof(Arcane::MeshSlot)), Opaque(alignof(Arcane::MeshSlot)));
        f.Field("name", Opaque(offsetof(Arcane::MeshSlot, name)), Opaque(sizeof(Arcane::MeshSlot::name)), Opaque(alignof(decltype(Arcane::MeshSlot::name))), "std::string");
        f.Field("material", Opaque(offsetof(Arcane::MeshSlot, material)), sizeof(Arcane::MeshSlot::material), alignof(decltype(Arcane::MeshSlot::material)), "Guid");
        f.Type("Arcane::MeshSource", sizeof(Arcane::MeshSource), alignof(Arcane::MeshSource));
        Detail::AbiManifest::Access::R_GameModule(f);
        Detail::AbiManifest::Access::R_SystemFactoryTable(f);
        Detail::AbiManifest::Access::R_Runtime(f);
        f.Type("Arcane::EngineContext", sizeof(Arcane::EngineContext), alignof(Arcane::EngineContext));
        f.Field("abiVersion", offsetof(Arcane::EngineContext, abiVersion), sizeof(Arcane::EngineContext::abiVersion), alignof(decltype(Arcane::EngineContext::abiVersion)), "uint32_t");
        f.Field("typeContext", offsetof(Arcane::EngineContext, typeContext), sizeof(Arcane::EngineContext::typeContext), alignof(decltype(Arcane::EngineContext::typeContext)), "Arcane::TypeContext*");
        f.Field("workScheduler", offsetof(Arcane::EngineContext, workScheduler), sizeof(Arcane::EngineContext::workScheduler), alignof(decltype(Arcane::EngineContext::workScheduler)), "Arcane::IWorkScheduler*");
        f.Field("taskExecutor", offsetof(Arcane::EngineContext, taskExecutor), sizeof(Arcane::EngineContext::taskExecutor), alignof(decltype(Arcane::EngineContext::taskExecutor)), "Arcane::ITaskExecutor*");
        f.Field("engine", offsetof(Arcane::EngineContext, engine), sizeof(Arcane::EngineContext::engine), alignof(decltype(Arcane::EngineContext::engine)), "Arcane::Runtime*");
        f.Field("process", offsetof(Arcane::EngineContext, process), sizeof(Arcane::EngineContext::process), alignof(decltype(Arcane::EngineContext::process)), "Arcane::ProcessContext*");
        f.Field("client", offsetof(Arcane::EngineContext, client), sizeof(Arcane::EngineContext::client), alignof(decltype(Arcane::EngineContext::client)), "Arcane::ClientRuntime*");
        f.Field("netMode", offsetof(Arcane::EngineContext, netMode), sizeof(Arcane::EngineContext::netMode), alignof(decltype(Arcane::EngineContext::netMode)), "Arcane::NetMode");
        f.Field("imguiContext", offsetof(Arcane::EngineContext, imguiContext), sizeof(Arcane::EngineContext::imguiContext), alignof(decltype(Arcane::EngineContext::imguiContext)), "void*");
        f.Field("imguiAlloc", offsetof(Arcane::EngineContext, imguiAlloc), sizeof(Arcane::EngineContext::imguiAlloc), alignof(decltype(Arcane::EngineContext::imguiAlloc)), "void*");
        f.Field("imguiFree", offsetof(Arcane::EngineContext, imguiFree), sizeof(Arcane::EngineContext::imguiFree), alignof(decltype(Arcane::EngineContext::imguiFree)), "void*");
        f.Field("imguiUserData", offsetof(Arcane::EngineContext, imguiUserData), sizeof(Arcane::EngineContext::imguiUserData), alignof(decltype(Arcane::EngineContext::imguiUserData)), "void*");
        f.Type("Arcane::PluginVTable", sizeof(Arcane::PluginVTable), alignof(Arcane::PluginVTable));
        f.Field("ABIVersion", offsetof(Arcane::PluginVTable, ABIVersion), sizeof(Arcane::PluginVTable::ABIVersion), alignof(decltype(Arcane::PluginVTable::ABIVersion)), "uint32_t (*)()");
        f.Field("Init", offsetof(Arcane::PluginVTable, Init), sizeof(Arcane::PluginVTable::Init), alignof(decltype(Arcane::PluginVTable::Init)), "bool (*)(EngineContext*)");
        f.Field("Shutdown", offsetof(Arcane::PluginVTable, Shutdown), sizeof(Arcane::PluginVTable::Shutdown), alignof(decltype(Arcane::PluginVTable::Shutdown)), "void (*)()");
        f.Field("FixedUpdate", offsetof(Arcane::PluginVTable, FixedUpdate), sizeof(Arcane::PluginVTable::FixedUpdate), alignof(decltype(Arcane::PluginVTable::FixedUpdate)), "void (*)(double)");
        f.Field("Update", offsetof(Arcane::PluginVTable, Update), sizeof(Arcane::PluginVTable::Update), alignof(decltype(Arcane::PluginVTable::Update)), "void (*)(double, double)");
        f.Field("SaveState", offsetof(Arcane::PluginVTable, SaveState), sizeof(Arcane::PluginVTable::SaveState), alignof(decltype(Arcane::PluginVTable::SaveState)), "void (*)(Arcane::BinaryWriter&)");
        f.Field("LoadState", offsetof(Arcane::PluginVTable, LoadState), sizeof(Arcane::PluginVTable::LoadState), alignof(decltype(Arcane::PluginVTable::LoadState)), "bool (*)(Arcane::BinaryReader&)");
        f.Field("DrawUI", offsetof(Arcane::PluginVTable, DrawUI), sizeof(Arcane::PluginVTable::DrawUI), alignof(decltype(Arcane::PluginVTable::DrawUI)), "void (*)()");
        f.Field("ABIDigest", offsetof(Arcane::PluginVTable, ABIDigest), sizeof(Arcane::PluginVTable::ABIDigest), alignof(decltype(Arcane::PluginVTable::ABIDigest)), "uint64_t (*)()");
        f.Type("Arcane::ProcessContextDesc", sizeof(Arcane::ProcessContextDesc), alignof(Arcane::ProcessContextDesc));
        f.Field("isDedicatedServerProcess", offsetof(Arcane::ProcessContextDesc, isDedicatedServerProcess), sizeof(Arcane::ProcessContextDesc::isDedicatedServerProcess), alignof(decltype(Arcane::ProcessContextDesc::isDedicatedServerProcess)), "bool");
        f.Field("externalTypeContext", offsetof(Arcane::ProcessContextDesc, externalTypeContext), sizeof(Arcane::ProcessContextDesc::externalTypeContext), alignof(decltype(Arcane::ProcessContextDesc::externalTypeContext)), "Arcane::TypeContext*");
        Detail::AbiManifest::Access::R_ProcessContext(f);
        f.Type("Arcane::SystemFactoryEntry", Opaque(sizeof(Arcane::SystemFactoryEntry)), Opaque(alignof(Arcane::SystemFactoryEntry)));
        f.Field("name", Opaque(offsetof(Arcane::SystemFactoryEntry, name)), Opaque(sizeof(Arcane::SystemFactoryEntry::name)), Opaque(alignof(decltype(Arcane::SystemFactoryEntry::name))), "std::string");
        f.Field("mask", Opaque(offsetof(Arcane::SystemFactoryEntry, mask)), sizeof(Arcane::SystemFactoryEntry::mask), alignof(decltype(Arcane::SystemFactoryEntry::mask)), "RoleMask");
        f.Field("phase", Opaque(offsetof(Arcane::SystemFactoryEntry, phase)), sizeof(Arcane::SystemFactoryEntry::phase), alignof(decltype(Arcane::SystemFactoryEntry::phase)), "SystemPhase");
        f.Field("instantiate", Opaque(offsetof(Arcane::SystemFactoryEntry, instantiate)), Opaque(sizeof(Arcane::SystemFactoryEntry::instantiate)), Opaque(alignof(decltype(Arcane::SystemFactoryEntry::instantiate))), "std::function<void(Arcane::SystemScheduler&)>");
        f.Field("owner", Opaque(offsetof(Arcane::SystemFactoryEntry, owner)), sizeof(Arcane::SystemFactoryEntry::owner), alignof(decltype(Arcane::SystemFactoryEntry::owner)), "const void*");
        f.Type("Arcane::Game::ComponentRegistrar", sizeof(Arcane::Game::ComponentRegistrar), alignof(Arcane::Game::ComponentRegistrar));
        f.Field("registerFn", offsetof(Arcane::Game::ComponentRegistrar, registerFn), sizeof(Arcane::Game::ComponentRegistrar::registerFn), alignof(decltype(Arcane::Game::ComponentRegistrar::registerFn)), "void (*)(Arcane::ComponentModule&)");
        f.Field("typeName", offsetof(Arcane::Game::ComponentRegistrar, typeName), sizeof(Arcane::Game::ComponentRegistrar::typeName), alignof(decltype(Arcane::Game::ComponentRegistrar::typeName)), "const char*");
        f.Field("next", offsetof(Arcane::Game::ComponentRegistrar, next), sizeof(Arcane::Game::ComponentRegistrar::next), alignof(decltype(Arcane::Game::ComponentRegistrar::next)), "ComponentRegistrar*");
        f.Type("Arcane::Game::SystemRegistrar", sizeof(Arcane::Game::SystemRegistrar), alignof(Arcane::Game::SystemRegistrar));
        f.Field("registerFn", offsetof(Arcane::Game::SystemRegistrar, registerFn), sizeof(Arcane::Game::SystemRegistrar::registerFn), alignof(decltype(Arcane::Game::SystemRegistrar::registerFn)), "void (*)(SystemFactoryTable&, RoleMask, SystemPhase)");
        f.Field("typeName", offsetof(Arcane::Game::SystemRegistrar, typeName), sizeof(Arcane::Game::SystemRegistrar::typeName), alignof(decltype(Arcane::Game::SystemRegistrar::typeName)), "const char*");
        f.Field("mask", offsetof(Arcane::Game::SystemRegistrar, mask), sizeof(Arcane::Game::SystemRegistrar::mask), alignof(decltype(Arcane::Game::SystemRegistrar::mask)), "RoleMask");
        f.Field("phase", offsetof(Arcane::Game::SystemRegistrar, phase), sizeof(Arcane::Game::SystemRegistrar::phase), alignof(decltype(Arcane::Game::SystemRegistrar::phase)), "SystemPhase");
        f.Field("next", offsetof(Arcane::Game::SystemRegistrar, next), sizeof(Arcane::Game::SystemRegistrar::next), alignof(decltype(Arcane::Game::SystemRegistrar::next)), "SystemRegistrar*");
        f.Type("Arcane::GameModuleDetail::State", sizeof(Arcane::GameModuleDetail::State), alignof(Arcane::GameModuleDetail::State));
        f.Field("ctx", offsetof(Arcane::GameModuleDetail::State, ctx), sizeof(Arcane::GameModuleDetail::State::ctx), alignof(decltype(Arcane::GameModuleDetail::State::ctx)), "EngineContext*");
        f.Field("components", offsetof(Arcane::GameModuleDetail::State, components), sizeof(Arcane::GameModuleDetail::State::components), alignof(decltype(Arcane::GameModuleDetail::State::components)), "Arcane::ComponentModule*");
        f.Field("instance", offsetof(Arcane::GameModuleDetail::State, instance), sizeof(Arcane::GameModuleDetail::State::instance), alignof(decltype(Arcane::GameModuleDetail::State::instance)), "GameModule*");
        f.Type("Arcane::Assert::EnsureScope", sizeof(Arcane::Assert::EnsureScope), alignof(Arcane::Assert::EnsureScope));
        f.Type("Arcane::LogRecord", Opaque(sizeof(Arcane::LogRecord)), Opaque(alignof(Arcane::LogRecord)));
        f.Field("seq", offsetof(Arcane::LogRecord, seq), sizeof(Arcane::LogRecord::seq), alignof(decltype(Arcane::LogRecord::seq)), "std::uint64_t");
        f.Field("unixMs", offsetof(Arcane::LogRecord, unixMs), sizeof(Arcane::LogRecord::unixMs), alignof(decltype(Arcane::LogRecord::unixMs)), "std::int64_t");
        f.Field("level", offsetof(Arcane::LogRecord, level), sizeof(Arcane::LogRecord::level), alignof(decltype(Arcane::LogRecord::level)), "int");
        f.Field("logger", Opaque(offsetof(Arcane::LogRecord, logger)), Opaque(sizeof(Arcane::LogRecord::logger)), Opaque(alignof(decltype(Arcane::LogRecord::logger))), "std::string");
        f.Field("text", Opaque(offsetof(Arcane::LogRecord, text)), Opaque(sizeof(Arcane::LogRecord::text)), Opaque(alignof(decltype(Arcane::LogRecord::text))), "std::string");
        f.Type("Arcane::LogSubscription", sizeof(Arcane::LogSubscription), alignof(Arcane::LogSubscription));
        f.Field("id", offsetof(Arcane::LogSubscription, id), sizeof(Arcane::LogSubscription::id), alignof(decltype(Arcane::LogSubscription::id)), "std::uint64_t");
        f.Type("Arcane::SceneRoot", sizeof(Arcane::SceneRoot), alignof(Arcane::SceneRoot));
        f.Field("entity", offsetof(Arcane::SceneRoot, entity), sizeof(Arcane::SceneRoot::entity), alignof(decltype(Arcane::SceneRoot::entity)), "Arcane::Entity");
        f.Type("Arcane::RenderContext2D", sizeof(Arcane::RenderContext2D), alignof(Arcane::RenderContext2D));
        f.Field("batcher", offsetof(Arcane::RenderContext2D, batcher), sizeof(Arcane::RenderContext2D::batcher), alignof(decltype(Arcane::RenderContext2D::batcher)), "class Batcher2D*");
        f.Field("view", offsetof(Arcane::RenderContext2D, view), sizeof(Arcane::RenderContext2D::view), alignof(decltype(Arcane::RenderContext2D::view)), "ViewTransform");
        f.Field("alpha", offsetof(Arcane::RenderContext2D, alpha), sizeof(Arcane::RenderContext2D::alpha), alignof(decltype(Arcane::RenderContext2D::alpha)), "float");
        f.Type("Arcane::SpriteTable", sizeof(Arcane::SpriteTable), alignof(Arcane::SpriteTable));
        f.Field("sprites", offsetof(Arcane::SpriteTable, sprites), sizeof(Arcane::SpriteTable::sprites), alignof(decltype(Arcane::SpriteTable::sprites)), "const std::unordered_map<Guid, SpriteEntry>*");
        f.Field("generation", offsetof(Arcane::SpriteTable, generation), sizeof(Arcane::SpriteTable::generation), alignof(decltype(Arcane::SpriteTable::generation)), "const std::uint64_t*");
        f.Type("Arcane::SpriteMaterialTable", sizeof(Arcane::SpriteMaterialTable), alignof(Arcane::SpriteMaterialTable));
        f.Field("materials", offsetof(Arcane::SpriteMaterialTable, materials), sizeof(Arcane::SpriteMaterialTable::materials), alignof(decltype(Arcane::SpriteMaterialTable::materials)), "const std::unordered_map<Guid, uint16_t>*");
        f.Type("Arcane::MeshTable", sizeof(Arcane::MeshTable), alignof(Arcane::MeshTable));
        f.Field("meshes", offsetof(Arcane::MeshTable, meshes), sizeof(Arcane::MeshTable::meshes), alignof(decltype(Arcane::MeshTable::meshes)), "const std::unordered_map<Guid, MeshEntry>*");
        f.Field("generation", offsetof(Arcane::MeshTable, generation), sizeof(Arcane::MeshTable::generation), alignof(decltype(Arcane::MeshTable::generation)), "const std::uint64_t*");
        f.Type("Arcane::MeshMaterialTable", sizeof(Arcane::MeshMaterialTable), alignof(Arcane::MeshMaterialTable));
        f.Field("materials", offsetof(Arcane::MeshMaterialTable, materials), sizeof(Arcane::MeshMaterialTable::materials), alignof(decltype(Arcane::MeshMaterialTable::materials)), "const std::unordered_map<Guid, ResolvedMeshMaterial>*");
        f.Type("Arcane::PhysicsInterpPose2D", sizeof(Arcane::PhysicsInterpPose2D), alignof(Arcane::PhysicsInterpPose2D));
        f.Field("position", offsetof(Arcane::PhysicsInterpPose2D, position), sizeof(Arcane::PhysicsInterpPose2D::position), alignof(decltype(Arcane::PhysicsInterpPose2D::position)), "glm::vec2");
        f.Field("angle", offsetof(Arcane::PhysicsInterpPose2D, angle), sizeof(Arcane::PhysicsInterpPose2D::angle), alignof(decltype(Arcane::PhysicsInterpPose2D::angle)), "float");
        f.Field("generation", offsetof(Arcane::PhysicsInterpPose2D, generation), sizeof(Arcane::PhysicsInterpPose2D::generation), alignof(decltype(Arcane::PhysicsInterpPose2D::generation)), "std::uint32_t");
        f.Type("Arcane::PhysicsInterpSlot2D", sizeof(Arcane::PhysicsInterpSlot2D), alignof(Arcane::PhysicsInterpSlot2D));
        f.Field("index", offsetof(Arcane::PhysicsInterpSlot2D, index), sizeof(Arcane::PhysicsInterpSlot2D::index), alignof(decltype(Arcane::PhysicsInterpSlot2D::index)), "std::uint32_t");
        f.Field("generation", offsetof(Arcane::PhysicsInterpSlot2D, generation), sizeof(Arcane::PhysicsInterpSlot2D::generation), alignof(decltype(Arcane::PhysicsInterpSlot2D::generation)), "std::uint32_t");
        f.Type("Arcane::PhysicsInterpBuffer2D", Opaque(sizeof(Arcane::PhysicsInterpBuffer2D)), Opaque(alignof(Arcane::PhysicsInterpBuffer2D)));
        f.Field("prev", Opaque(offsetof(Arcane::PhysicsInterpBuffer2D, prev)), Opaque(sizeof(Arcane::PhysicsInterpBuffer2D::prev)), Opaque(alignof(decltype(Arcane::PhysicsInterpBuffer2D::prev))), "std::vector<PhysicsInterpPose2D>");
        f.Field("slotOf", Opaque(offsetof(Arcane::PhysicsInterpBuffer2D, slotOf)), Opaque(sizeof(Arcane::PhysicsInterpBuffer2D::slotOf)), Opaque(alignof(decltype(Arcane::PhysicsInterpBuffer2D::slotOf))), "Astra::FlatMap<Astra::Entity, PhysicsInterpSlot2D>");
        f.Field("captured", Opaque(offsetof(Arcane::PhysicsInterpBuffer2D, captured)), sizeof(Arcane::PhysicsInterpBuffer2D::captured), alignof(decltype(Arcane::PhysicsInterpBuffer2D::captured)), "bool");
        f.Type("Arcane::SpriteEntry", sizeof(Arcane::SpriteEntry), alignof(Arcane::SpriteEntry));
        f.Field("uvMin", offsetof(Arcane::SpriteEntry, uvMin), sizeof(Arcane::SpriteEntry::uvMin), alignof(decltype(Arcane::SpriteEntry::uvMin)), "glm::vec2");
        f.Field("uvMax", offsetof(Arcane::SpriteEntry, uvMax), sizeof(Arcane::SpriteEntry::uvMax), alignof(decltype(Arcane::SpriteEntry::uvMax)), "glm::vec2");
        f.Field("sizeMeters", offsetof(Arcane::SpriteEntry, sizeMeters), sizeof(Arcane::SpriteEntry::sizeMeters), alignof(decltype(Arcane::SpriteEntry::sizeMeters)), "glm::vec2");
        f.Field("pivot", offsetof(Arcane::SpriteEntry, pivot), sizeof(Arcane::SpriteEntry::pivot), alignof(decltype(Arcane::SpriteEntry::pivot)), "glm::vec2");
        f.Field("textureId", offsetof(Arcane::SpriteEntry, textureId), sizeof(Arcane::SpriteEntry::textureId), alignof(decltype(Arcane::SpriteEntry::textureId)), "Guid");
        f.Type("Arcane::MeshEntry", Opaque(sizeof(Arcane::MeshEntry)), Opaque(alignof(Arcane::MeshEntry)));
        f.Field("data", Opaque(offsetof(Arcane::MeshEntry, data)), Opaque(sizeof(Arcane::MeshEntry::data)), Opaque(alignof(decltype(Arcane::MeshEntry::data))), "MeshData");
        f.Field("bounds", Opaque(offsetof(Arcane::MeshEntry, bounds)), sizeof(Arcane::MeshEntry::bounds), alignof(decltype(Arcane::MeshEntry::bounds)), "MeshBounds");
        f.Field("slots", Opaque(offsetof(Arcane::MeshEntry, slots)), Opaque(sizeof(Arcane::MeshEntry::slots)), Opaque(alignof(decltype(Arcane::MeshEntry::slots))), "std::vector<MeshSlot>");
        f.Field("source", Opaque(offsetof(Arcane::MeshEntry, source)), sizeof(Arcane::MeshEntry::source), alignof(decltype(Arcane::MeshEntry::source)), "MeshSource");
        f.Field("importedSource", Opaque(offsetof(Arcane::MeshEntry, importedSource)), sizeof(Arcane::MeshEntry::importedSource), alignof(decltype(Arcane::MeshEntry::importedSource)), "Guid");
        f.Field("rings", Opaque(offsetof(Arcane::MeshEntry, rings)), sizeof(Arcane::MeshEntry::rings), alignof(decltype(Arcane::MeshEntry::rings)), "std::uint32_t");
        f.Field("segments", Opaque(offsetof(Arcane::MeshEntry, segments)), sizeof(Arcane::MeshEntry::segments), alignof(decltype(Arcane::MeshEntry::segments)), "std::uint32_t");
        f.Field("subdivisions", Opaque(offsetof(Arcane::MeshEntry, subdivisions)), sizeof(Arcane::MeshEntry::subdivisions), alignof(decltype(Arcane::MeshEntry::subdivisions)), "std::uint32_t");
        f.Field("capsuleLengthRatio", Opaque(offsetof(Arcane::MeshEntry, capsuleLengthRatio)), sizeof(Arcane::MeshEntry::capsuleLengthRatio), alignof(decltype(Arcane::MeshEntry::capsuleLengthRatio)), "float");
        f.Type("Arcane::ResolvedMeshMaterial", sizeof(Arcane::ResolvedMeshMaterial), alignof(Arcane::ResolvedMeshMaterial));
        f.Field("baseColor", offsetof(Arcane::ResolvedMeshMaterial, baseColor), sizeof(Arcane::ResolvedMeshMaterial::baseColor), alignof(decltype(Arcane::ResolvedMeshMaterial::baseColor)), "glm::vec4");
        f.Field("blend", offsetof(Arcane::ResolvedMeshMaterial, blend), sizeof(Arcane::ResolvedMeshMaterial::blend), alignof(decltype(Arcane::ResolvedMeshMaterial::blend)), "MaterialBlendMode");
        f.Field("alphaCutoff", offsetof(Arcane::ResolvedMeshMaterial, alphaCutoff), sizeof(Arcane::ResolvedMeshMaterial::alphaCutoff), alignof(decltype(Arcane::ResolvedMeshMaterial::alphaCutoff)), "float");
        f.Field("twoSided", offsetof(Arcane::ResolvedMeshMaterial, twoSided), sizeof(Arcane::ResolvedMeshMaterial::twoSided), alignof(decltype(Arcane::ResolvedMeshMaterial::twoSided)), "bool");
        f.Field("albedo", offsetof(Arcane::ResolvedMeshMaterial, albedo), sizeof(Arcane::ResolvedMeshMaterial::albedo), alignof(decltype(Arcane::ResolvedMeshMaterial::albedo)), "Guid");
        f.Field("materialSlot", offsetof(Arcane::ResolvedMeshMaterial, materialSlot), sizeof(Arcane::ResolvedMeshMaterial::materialSlot), alignof(decltype(Arcane::ResolvedMeshMaterial::materialSlot)), "std::uint32_t");
        f.Type("Arcane::Transform", sizeof(Arcane::Transform), alignof(Arcane::Transform));
        f.Field("position", offsetof(Arcane::Transform, position), sizeof(Arcane::Transform::position), alignof(decltype(Arcane::Transform::position)), "glm::vec3");
        f.Field("rotation", offsetof(Arcane::Transform, rotation), sizeof(Arcane::Transform::rotation), alignof(decltype(Arcane::Transform::rotation)), "glm::quat");
        f.Field("scale", offsetof(Arcane::Transform, scale), sizeof(Arcane::Transform::scale), alignof(decltype(Arcane::Transform::scale)), "glm::vec3");
        f.Type("Arcane::WorldTransform", sizeof(Arcane::WorldTransform), alignof(Arcane::WorldTransform));
        f.Field("matrix", offsetof(Arcane::WorldTransform, matrix), sizeof(Arcane::WorldTransform::matrix), alignof(decltype(Arcane::WorldTransform::matrix)), "glm::mat4");
        f.Type("Arcane::WorldBounds", sizeof(Arcane::WorldBounds), alignof(Arcane::WorldBounds));
        f.Field("box", offsetof(Arcane::WorldBounds, box), sizeof(Arcane::WorldBounds::box), alignof(decltype(Arcane::WorldBounds::box)), "Aabb");
        f.Type("Arcane::SpriteRenderer", sizeof(Arcane::SpriteRenderer), alignof(Arcane::SpriteRenderer));
        f.Field("sprite", offsetof(Arcane::SpriteRenderer, sprite), sizeof(Arcane::SpriteRenderer::sprite), alignof(decltype(Arcane::SpriteRenderer::sprite)), "Guid");
        f.Field("tint", offsetof(Arcane::SpriteRenderer, tint), sizeof(Arcane::SpriteRenderer::tint), alignof(decltype(Arcane::SpriteRenderer::tint)), "glm::vec4");
        f.Field("sortingLayer", offsetof(Arcane::SpriteRenderer, sortingLayer), sizeof(Arcane::SpriteRenderer::sortingLayer), alignof(decltype(Arcane::SpriteRenderer::sortingLayer)), "int32_t");
        f.Field("orderInLayer", offsetof(Arcane::SpriteRenderer, orderInLayer), sizeof(Arcane::SpriteRenderer::orderInLayer), alignof(decltype(Arcane::SpriteRenderer::orderInLayer)), "int32_t");
        f.Field("shape", offsetof(Arcane::SpriteRenderer, shape), sizeof(Arcane::SpriteRenderer::shape), alignof(decltype(Arcane::SpriteRenderer::shape)), "SpriteShape");
        f.Field("material", offsetof(Arcane::SpriteRenderer, material), sizeof(Arcane::SpriteRenderer::material), alignof(decltype(Arcane::SpriteRenderer::material)), "Guid");
        f.Type("Arcane::MeshRenderer", sizeof(Arcane::MeshRenderer), alignof(Arcane::MeshRenderer));
        f.Field("mesh", offsetof(Arcane::MeshRenderer, mesh), sizeof(Arcane::MeshRenderer::mesh), alignof(decltype(Arcane::MeshRenderer::mesh)), "Guid");
        f.Field("materialOverride", offsetof(Arcane::MeshRenderer, materialOverride), sizeof(Arcane::MeshRenderer::materialOverride), alignof(decltype(Arcane::MeshRenderer::materialOverride)), "Guid");
        f.Field("translucencyRenderOrder", offsetof(Arcane::MeshRenderer, translucencyRenderOrder), sizeof(Arcane::MeshRenderer::translucencyRenderOrder), alignof(decltype(Arcane::MeshRenderer::translucencyRenderOrder)), "std::int32_t");
        f.Field("translucencyDepthSortBias", offsetof(Arcane::MeshRenderer, translucencyDepthSortBias), sizeof(Arcane::MeshRenderer::translucencyDepthSortBias), alignof(decltype(Arcane::MeshRenderer::translucencyDepthSortBias)), "float");
        f.Type("Arcane::PhysicsSettings2D", sizeof(Arcane::PhysicsSettings2D), alignof(Arcane::PhysicsSettings2D));
        f.Field("gravity", offsetof(Arcane::PhysicsSettings2D, gravity), sizeof(Arcane::PhysicsSettings2D::gravity), alignof(decltype(Arcane::PhysicsSettings2D::gravity)), "glm::vec2");
        f.Type("Arcane::PostProcess", sizeof(Arcane::PostProcess), alignof(Arcane::PostProcess));
        f.Field("material", offsetof(Arcane::PostProcess, material), sizeof(Arcane::PostProcess::material), alignof(decltype(Arcane::PostProcess::material)), "Guid");
        f.Type("Arcane::Camera", sizeof(Arcane::Camera), alignof(Arcane::Camera));
        f.Field("orthographicSize", offsetof(Arcane::Camera, orthographicSize), sizeof(Arcane::Camera::orthographicSize), alignof(decltype(Arcane::Camera::orthographicSize)), "float");
        f.Field("active", offsetof(Arcane::Camera, active), sizeof(Arcane::Camera::active), alignof(decltype(Arcane::Camera::active)), "bool");
        f.Field("projection", offsetof(Arcane::Camera, projection), sizeof(Arcane::Camera::projection), alignof(decltype(Arcane::Camera::projection)), "CameraProjection");
        f.Field("fovYDegrees", offsetof(Arcane::Camera, fovYDegrees), sizeof(Arcane::Camera::fovYDegrees), alignof(decltype(Arcane::Camera::fovYDegrees)), "float");
        f.Field("nearZ", offsetof(Arcane::Camera, nearZ), sizeof(Arcane::Camera::nearZ), alignof(decltype(Arcane::Camera::nearZ)), "float");
        f.Field("farZ", offsetof(Arcane::Camera, farZ), sizeof(Arcane::Camera::farZ), alignof(decltype(Arcane::Camera::farZ)), "float");
        f.Type("Arcane::Identity", Opaque(sizeof(Arcane::Identity)), Opaque(alignof(Arcane::Identity)));
        f.Field("id", offsetof(Arcane::Identity, id), sizeof(Arcane::Identity::id), alignof(decltype(Arcane::Identity::id)), "Guid");
        f.Field("name", Opaque(offsetof(Arcane::Identity, name)), Opaque(sizeof(Arcane::Identity::name)), Opaque(alignof(decltype(Arcane::Identity::name))), "std::string");
        f.Type("Arcane::Hidden", sizeof(Arcane::Hidden), alignof(Arcane::Hidden));
        f.Type("Arcane::TransformOrder", Opaque(sizeof(Arcane::TransformOrder)), Opaque(alignof(Arcane::TransformOrder)));
        f.Field("order", Opaque(offsetof(Arcane::TransformOrder, order)), Opaque(sizeof(Arcane::TransformOrder::order)), Opaque(alignof(decltype(Arcane::TransformOrder::order))), "std::vector<Arcane::Entity>");
        f.Field("parentIndex", Opaque(offsetof(Arcane::TransformOrder, parentIndex)), Opaque(sizeof(Arcane::TransformOrder::parentIndex)), Opaque(alignof(decltype(Arcane::TransformOrder::parentIndex))), "std::vector<std::uint32_t>");
        f.Field("rowOf", Opaque(offsetof(Arcane::TransformOrder, rowOf)), Opaque(sizeof(Arcane::TransformOrder::rowOf)), Opaque(alignof(decltype(Arcane::TransformOrder::rowOf))), "Astra::FlatMap<Arcane::Entity, std::uint32_t>");
        f.Field("moved", Opaque(offsetof(Arcane::TransformOrder, moved)), Opaque(sizeof(Arcane::TransformOrder::moved)), Opaque(alignof(decltype(Arcane::TransformOrder::moved))), "std::vector<std::uint8_t>");
        f.Field("dirty", Opaque(offsetof(Arcane::TransformOrder, dirty)), Opaque(sizeof(Arcane::TransformOrder::dirty)), Opaque(alignof(decltype(Arcane::TransformOrder::dirty))), "std::vector<std::uint8_t>");
        f.Field("world", Opaque(offsetof(Arcane::TransformOrder, world)), Opaque(sizeof(Arcane::TransformOrder::world)), Opaque(alignof(decltype(Arcane::TransformOrder::world))), "std::vector<glm::mat4>");
        f.Field("needsWorld", Opaque(offsetof(Arcane::TransformOrder, needsWorld)), Opaque(sizeof(Arcane::TransformOrder::needsWorld)), Opaque(alignof(decltype(Arcane::TransformOrder::needsWorld))), "std::vector<Arcane::Entity>");
        f.Field("visited", Opaque(offsetof(Arcane::TransformOrder, visited)), Opaque(sizeof(Arcane::TransformOrder::visited)), Opaque(alignof(decltype(Arcane::TransformOrder::visited))), "Astra::FlatSet<Arcane::Entity>");
        f.Field("root", Opaque(offsetof(Arcane::TransformOrder, root)), sizeof(Arcane::TransformOrder::root), alignof(decltype(Arcane::TransformOrder::root)), "Arcane::Entity");
        f.Field("structureVersion", Opaque(offsetof(Arcane::TransformOrder, structureVersion)), sizeof(Arcane::TransformOrder::structureVersion), alignof(decltype(Arcane::TransformOrder::structureVersion)), "std::uint32_t");
        f.Field("lastRun", Opaque(offsetof(Arcane::TransformOrder, lastRun)), sizeof(Arcane::TransformOrder::lastRun), alignof(decltype(Arcane::TransformOrder::lastRun)), "Arcane::Tick");
        f.Field("rebuilds", Opaque(offsetof(Arcane::TransformOrder, rebuilds)), sizeof(Arcane::TransformOrder::rebuilds), alignof(decltype(Arcane::TransformOrder::rebuilds)), "std::uint32_t");
        f.Field("runs", Opaque(offsetof(Arcane::TransformOrder, runs)), sizeof(Arcane::TransformOrder::runs), alignof(decltype(Arcane::TransformOrder::runs)), "std::uint32_t");
        f.Field("composed", Opaque(offsetof(Arcane::TransformOrder, composed)), sizeof(Arcane::TransformOrder::composed), alignof(decltype(Arcane::TransformOrder::composed)), "std::uint32_t");
        f.Type("Arcane::TransformPropagationSystem", sizeof(Arcane::TransformPropagationSystem), alignof(Arcane::TransformPropagationSystem));
        f.Type("Arcane::RigidBody2D", sizeof(Arcane::RigidBody2D), alignof(Arcane::RigidBody2D));
        f.Field("type", offsetof(Arcane::RigidBody2D, type), sizeof(Arcane::RigidBody2D::type), alignof(decltype(Arcane::RigidBody2D::type)), "BodyType2D");
        f.Field("velocity", offsetof(Arcane::RigidBody2D, velocity), sizeof(Arcane::RigidBody2D::velocity), alignof(decltype(Arcane::RigidBody2D::velocity)), "glm::vec2");
        f.Field("mass", offsetof(Arcane::RigidBody2D, mass), sizeof(Arcane::RigidBody2D::mass), alignof(decltype(Arcane::RigidBody2D::mass)), "float");
        f.Field("linearDamping", offsetof(Arcane::RigidBody2D, linearDamping), sizeof(Arcane::RigidBody2D::linearDamping), alignof(decltype(Arcane::RigidBody2D::linearDamping)), "float");
        f.Field("fixedRotation", offsetof(Arcane::RigidBody2D, fixedRotation), sizeof(Arcane::RigidBody2D::fixedRotation), alignof(decltype(Arcane::RigidBody2D::fixedRotation)), "bool");
        f.Field("bullet", offsetof(Arcane::RigidBody2D, bullet), sizeof(Arcane::RigidBody2D::bullet), alignof(decltype(Arcane::RigidBody2D::bullet)), "bool");
        f.Type("Arcane::Fixture2D", sizeof(Arcane::Fixture2D), alignof(Arcane::Fixture2D));
        f.Field("kind", offsetof(Arcane::Fixture2D, kind), sizeof(Arcane::Fixture2D::kind), alignof(decltype(Arcane::Fixture2D::kind)), "ShapeKind2D");
        f.Field("radius", offsetof(Arcane::Fixture2D, radius), sizeof(Arcane::Fixture2D::radius), alignof(decltype(Arcane::Fixture2D::radius)), "float");
        f.Field("halfLen", offsetof(Arcane::Fixture2D, halfLen), sizeof(Arcane::Fixture2D::halfLen), alignof(decltype(Arcane::Fixture2D::halfLen)), "float");
        f.Field("halfW", offsetof(Arcane::Fixture2D, halfW), sizeof(Arcane::Fixture2D::halfW), alignof(decltype(Arcane::Fixture2D::halfW)), "float");
        f.Field("halfH", offsetof(Arcane::Fixture2D, halfH), sizeof(Arcane::Fixture2D::halfH), alignof(decltype(Arcane::Fixture2D::halfH)), "float");
        f.Field("localPos", offsetof(Arcane::Fixture2D, localPos), sizeof(Arcane::Fixture2D::localPos), alignof(decltype(Arcane::Fixture2D::localPos)), "glm::vec2");
        f.Field("localAngle", offsetof(Arcane::Fixture2D, localAngle), sizeof(Arcane::Fixture2D::localAngle), alignof(decltype(Arcane::Fixture2D::localAngle)), "float");
        f.Field("density", offsetof(Arcane::Fixture2D, density), sizeof(Arcane::Fixture2D::density), alignof(decltype(Arcane::Fixture2D::density)), "float");
        f.Field("friction", offsetof(Arcane::Fixture2D, friction), sizeof(Arcane::Fixture2D::friction), alignof(decltype(Arcane::Fixture2D::friction)), "float");
        f.Field("restitution", offsetof(Arcane::Fixture2D, restitution), sizeof(Arcane::Fixture2D::restitution), alignof(decltype(Arcane::Fixture2D::restitution)), "float");
        f.Field("categoryBits", offsetof(Arcane::Fixture2D, categoryBits), sizeof(Arcane::Fixture2D::categoryBits), alignof(decltype(Arcane::Fixture2D::categoryBits)), "uint32_t");
        f.Field("maskBits", offsetof(Arcane::Fixture2D, maskBits), sizeof(Arcane::Fixture2D::maskBits), alignof(decltype(Arcane::Fixture2D::maskBits)), "uint32_t");
        f.Field("isSensor", offsetof(Arcane::Fixture2D, isSensor), sizeof(Arcane::Fixture2D::isSensor), alignof(decltype(Arcane::Fixture2D::isSensor)), "bool");
        f.Field("contactEvents", offsetof(Arcane::Fixture2D, contactEvents), sizeof(Arcane::Fixture2D::contactEvents), alignof(decltype(Arcane::Fixture2D::contactEvents)), "bool");
        f.Field("sensorEvents", offsetof(Arcane::Fixture2D, sensorEvents), sizeof(Arcane::Fixture2D::sensorEvents), alignof(decltype(Arcane::Fixture2D::sensorEvents)), "bool");
        f.Field("hitEvents", offsetof(Arcane::Fixture2D, hitEvents), sizeof(Arcane::Fixture2D::hitEvents), alignof(decltype(Arcane::Fixture2D::hitEvents)), "bool");
        f.Type("Arcane::Collider2D", Opaque(sizeof(Arcane::Collider2D)), Opaque(alignof(Arcane::Collider2D)));
        f.Field("fixtures", Opaque(offsetof(Arcane::Collider2D, fixtures)), Opaque(sizeof(Arcane::Collider2D::fixtures)), Opaque(alignof(decltype(Arcane::Collider2D::fixtures))), "std::vector<Fixture2D>");
        f.Type("Arcane::PhysicsBodyRef2D", sizeof(Arcane::PhysicsBodyRef2D), alignof(Arcane::PhysicsBodyRef2D));
        f.Field("handle", offsetof(Arcane::PhysicsBodyRef2D, handle), sizeof(Arcane::PhysicsBodyRef2D::handle), alignof(decltype(Arcane::PhysicsBodyRef2D::handle)), "std::uint64_t");
        f.Field("appliedScale", offsetof(Arcane::PhysicsBodyRef2D, appliedScale), sizeof(Arcane::PhysicsBodyRef2D::appliedScale), alignof(decltype(Arcane::PhysicsBodyRef2D::appliedScale)), "glm::vec2");
        Detail::AbiManifest::Access::R_PhysicsSystem2D(f);

        return f.h;
    }
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif   // ARC_PLUGIN_ABI_MANIFEST_ENGINE_HPP
