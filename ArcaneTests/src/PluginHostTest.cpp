// PluginHost: versioned copy-and-load, ABI check, last-good rollback, hot swap.
// No window, no device. Uses the HotReloadPlugin V1/V2/Bad/InitFail DLLs copied
// next to ArcaneTests.exe (relative paths; run the exe FROM its output dir).

#include <catch2/catch_test_macros.hpp>
#include "Helpers/ModuleNames.hpp"   // fixture module file names per platform

#include <Arcane/Base/ProcessContext.hpp>       // SystemFactories() -- the C1 size instrument
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>      // SystemFactoryTable::Size()
#include <Arcane/Render/RenderErrorLatch.hpp>   // RenderErrorCount()
#include <Arcane/Scene/Components.hpp>   // Arcane::Transform (the engine-roster survivor check)

#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"   // the SAME Pulse type the plugin uses
#include <Arcane/Plugin/PluginHost.hpp>

#include <Arcane/Base/Log.hpp>   // Log::Engine() sinks (the OnShutdown probe)

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <memory>
#include <string>

#include <cstddef>
#include <filesystem>

using Arcane::HotReloadTest::Pulse;
// The plugin's other component type: a HOST-side base owner for every type the
// module registers, so the module's unload pops its shadow onto a live entry
// rather than onto nothing (see the RegisterComponent pairs below).
using Arcane::HotReloadTest::RoleCounters;

namespace
{
    int ReadPulse(Arcane::Runtime& rt)
    {
        int v = 0;
        rt.Registry().CreateView<Pulse>().ForEach([&](Astra::Entity, Pulse& p) { v = p.ticks; });
        return v;
    }
    // Drive k fixed steps through the HOST (primary + every secondary), the way every
    // product host advances the sim -- FixedUpdateAll, never a raw Vtable()->FixedUpdate.
    // The host entry point is also where the module's cvar scope opens around the call
    // (settings spec s4.4; PluginHost.cpp ScopedCall): the fixture AddCallbacks from
    // OnFixedUpdate, and a tick driven through the raw vtable would leave that callback
    // untagged, so it would outlive the unload as a dangling pointer and fault the next
    // [cvar][hotreload] case (CVarModuleLifetimeTest). The single-vtable StepK helper
    // this file used to carry was exactly that bypass; it is gone.
    void StepAllK(Arcane::Runtime& rt, Arcane::PluginHost& host, int k)
    {
        for (int i = 0; i < k; ++i)
            rt.Loop().Advance(1.0 / 60.0, [&](double dt){ host.FixedUpdateAll(dt); }, [&](double,double){});
    }
}

TEST_CASE("PluginHost loads a plugin and runs it across the ABI", "[hotreload]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();   // engine sees the type so views resolve
    rt.Components()->RegisterComponent<RoleCounters>();

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    REQUIRE(host.IsLoaded());

    StepAllK(rt, host, 5);
    CHECK(ReadPulse(rt) == 5);                     // V1 increments by 1
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();
}

// The GameModule hook-order probe (spec 2026-09-13 s7): ARC_GAME_MODULE's
// Shutdown calls OnShutdown BEFORE it closes the module's ComponentModule
// handle (the instance goes first so a module can still touch its own
// components). HotReloadPlugin's OnShutdown logs whether its Components()
// handle is still open at that moment; Unload() then resets the registry
// (fresh-boot semantics), so the evidence is read off the engine logger --
// Log::Engine() is ArcaneClient.dll's ONE logger, and the plugin's ARC_INFO
// lands in it (the SerializationNegativeTest capture shape).
TEST_CASE("GameModule: OnShutdown runs while the module's component handle is still open", "[hotreload]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();
    rt.Components()->RegisterComponent<RoleCounters>();

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    StepAllK(rt, host, 2);
    REQUIRE(ReadPulse(rt) == 2);

    std::string captured;
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [&](const spdlog::details::log_msg& m) { captured.append(m.payload.data(), m.payload.size()).push_back('\n'); });
    Arcane::Log::Engine()->sinks().push_back(sink);
    host.Unload();                                 // Shutdown -> OnShutdown (logs) -> handle closes -> unmap
    {
        auto& sinks = Arcane::Log::Engine()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
    }
    INFO(captured);
    CHECK(captured.find("HotReloadPlugin: OnShutdown with handle open") != std::string::npos);
    CHECK(captured.find("with handle closed") == std::string::npos);
}

TEST_CASE("Hot swap V1->V2 preserves state AND runs the new code", "[hotreload]")
{
    // Guard against ordering contamination: ensure we start with genuine V1 content.
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();
    rt.Components()->RegisterComponent<RoleCounters>();

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    StepAllK(rt, host, 5);
    REQUIRE(ReadPulse(rt) == 5);

    // Swap the binary under the watched path, then force a state-preserving reload.
    std::filesystem::copy_file(Arcane::Test::ModuleFile("HotReloadPluginV2"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);
    REQUIRE(host.ForceReload());                   // SaveState -> unload -> load V2 -> Init -> LoadState

    CHECK(ReadPulse(rt) == 5);                     // STATE SURVIVED the swap
    StepAllK(rt, host, 1);
    CHECK(ReadPulse(rt) == 15);                    // NEW CODE LIVE: V2 step is +10
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();

    // restore the fixture for re-runs
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);
}

TEST_CASE("ABI mismatch rolls back to last-good; session survives", "[hotreload]")
{
    // Guard against ordering contamination: ensure we start with genuine V1 content.
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();
    rt.Components()->RegisterComponent<RoleCounters>();

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    StepAllK(rt, host, 3);
    REQUIRE(ReadPulse(rt) == 3);

    std::filesystem::copy_file(Arcane::Test::ModuleFile("HotReloadPluginBad"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);
    CHECK_FALSE(host.ForceReload());               // ABI mismatch -> reload fails
    CHECK(host.IsLoaded());                        // still on last-good
    StepAllK(rt, host, 1);
    CHECK(ReadPulse(rt) == 4);                      // last-good V1 still running, state intact
    host.Unload();

    // restore the fixture for re-runs
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);
}

TEST_CASE("Host drives a secondary plugin alongside the primary", "[hotreload]")
{
    // A multi-module host: the primary game module PLUS one secondary plugin module,
    // both sharing the Runtime. AddPlugin registers the secondary BEFORE Load(); Load
    // brings up the primary, then the secondary. Both V1 (primary, +1) and V2 (secondary,
    // +10) target the SAME singleton Pulse entity -- V2's Init finds the one V1 created
    // and caches the same handle -- so ONE FixedUpdateAll step lands +1 AND +10 on it.
    // The value 11 is unique to "both ran": primary-only would be 1, secondary-only 10.
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();
    rt.Components()->RegisterComponent<RoleCounters>();

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    host.AttachRuntime(rt);
    host.AddPlugin(std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV2")));   // secondary, +10 per step
    REQUIRE(host.Load());
    REQUIRE(host.IsLoaded());

    StepAllK(rt, host, 1);
    CHECK(ReadPulse(rt) == 11);                    // primary(+1) AND secondary(+10) both drove

    // A primary hot-reload quiesces the secondary, reloads the primary (state preserved),
    // then re-establishes the secondary on the post-reload registry. If the secondary
    // were dropped, the next step would land only the primary's +1 (12); re-established,
    // both drive again for +11 (22). This exercises ShutdownPluginsLive/InitPluginsLive.
    REQUIRE(host.ForceReload());
    CHECK(ReadPulse(rt) == 11);                    // primary state survived the reload
    StepAllK(rt, host, 1);
    CHECK(ReadPulse(rt) == 22);                    // secondary re-established AND driven

    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();
}

TEST_CASE("Plugins-only host (no primary module) loads and drives its secondaries", "[hotreload]")
{
    // The editor opening a project that ships plugin modules but no gameModule: the host is
    // built with an EMPTY primary path and brings up only its AddPlugin() secondaries. No
    // primary means IsLoaded()==false / Vtable()==null, but the secondaries still run through
    // the *All drivers.
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();
    rt.Components()->RegisterComponent<RoleCounters>();

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path{});           // no primary game module
    host.AttachRuntime(rt);
    host.AddPlugin(std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1"))); // one secondary (+1/step)
    REQUIRE(host.Load());
    CHECK_FALSE(host.IsLoaded());            // no PRIMARY is loaded...
    CHECK(host.Vtable() == nullptr);

    StepAllK(rt, host, 3);
    CHECK(ReadPulse(rt) == 3);               // ...yet the secondary runs via FixedUpdateAll

    // Reload is a clean no-op success on a plugins-only host (secondaries never hot-reload),
    // leaving the running plugin and its state untouched.
    CHECK(host.ForceReload());
    StepAllK(rt, host, 1);
    CHECK(ReadPulse(rt) == 4);
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();
}

TEST_CASE("A secondary whose Init FAILS leaves no system factory pointing into its unmapped image", "[hotreload]")
{
    // Final-review fix wave, C1. LoadInitPlugins' failure branch used to let the
    // local std::optional<Plugin> die at its `return false` -- FreeLibrary -- while
    // everything that module's OnInit had registered was still in the ProcessContext's
    // PROCESS-LIFETIME SystemFactoryTable: InitImage had already opened and closed the
    // owner bracket, and the image was never pushed into `plugins`, so
    // DisownPluginImages never saw it and nothing ever called ClearOwner(image.base).
    // The next Runtime construction then instantiated a std::function compiled into
    // freed code.
    //
    // HotReloadPluginInitFail is the fixture that reaches that branch: a clean ABI, a
    // clean load, two RegisterSystem calls, and THEN `return false` from OnInit. The
    // TABLE SIZE is the instrument -- it is process-wide and shared by every case in
    // this suite, so the claim is "back to where this case found it", measured, not a
    // fixed number.
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();
    rt.Components()->RegisterComponent<RoleCounters>();

    const std::size_t before = Arcane::Test::Process().SystemFactories().Size();

    {
        Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
        REQUIRE(host.AttachRuntime(rt));
        host.AddPlugin(std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginInitFail")));
        // The primary comes up, the secondary refuses, and Load unwinds the WHOLE
        // session (no half-loaded host) -- so every factory either module registered
        // is gone by the time this returns.
        CHECK_FALSE(host.Load());
        CHECK_FALSE(host.IsLoaded());
        CHECK(Arcane::Test::Process().SystemFactories().Size() == before);
    }
    CHECK(Arcane::Test::Process().SystemFactories().Size() == before);

    // AND THE PROCESS IS STILL USABLE, which is the consequence the size number
    // stands in for: a NEW world built on the same ProcessContext instantiates the
    // table's entries. With a stale entry surviving, this call dispatches a
    // std::function whose code has been unmapped -- an access violation, not a
    // wrong answer. It must instantiate nothing and return cleanly.
    Arcane::Runtime fresh(Arcane::Test::Process());
    CHECK(fresh.InstantiateModuleSystems() == 0);

    // The world that WAS attached is intact too (Unload reset its registry; it
    // still takes entities and resolves the module's shared component types).
    rt.Registry().CreateEntityWith(Pulse{7});
    CHECK(ReadPulse(rt) == 7);
    CHECK(Arcane::RenderErrorCount() == 0);
}

TEST_CASE("Unloading a plugin restores the descriptors it overrode", "[hotreload]")
{
    // REGRESSION lineage: diagnosed 2026-07-31 as a permanent dangling-descriptor
    // AV; first fixed by UnregisterModuleRange (purge to a hole); now the
    // ComponentModule owner stack RESTORES the previous owner instead. This
    // test binary registers Pulse anonymously (below), the plugin's handle
    // overrides it, and unload must pop back to the test binary's entry --
    // non-null AND callable (a dangling restore faults right here).
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();
    rt.Components()->RegisterComponent<RoleCounters>();

    const Astra::ComponentID pulseId = Astra::TypeID<Pulse>::Value();
    const Astra::ComponentDescriptor* base = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(base != nullptr);

    {
        Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
        host.AttachRuntime(rt);
        REQUIRE(host.Load());
        REQUIRE(rt.Components()->GetComponentDescriptor(pulseId) != nullptr);
        host.Unload();
    }

    // Restored to the test binary's own registration: same slot address
    // (m_components[id] is pointer-stable), callable code in THIS image.
    const Astra::ComponentDescriptor* restored = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(restored == base);
    REQUIRE(restored->defaultConstruct != nullptr);
    alignas(Pulse) std::byte pbuf[sizeof(Pulse)];
    restored->DefaultConstruct(pbuf);
    restored->Destruct(pbuf);

    // The engine roster half is unchanged in spirit: Transform survives, callable.
    const Astra::ComponentID transformId = Astra::TypeID<Arcane::Transform>::Value();
    const Astra::ComponentDescriptor* tf = rt.Components()->GetComponentDescriptor(transformId);
    REQUIRE(tf != nullptr);
    alignas(Arcane::Transform) std::byte buf[sizeof(Arcane::Transform)];
    tf->DefaultConstruct(buf);
    tf->Destruct(buf);

    // Meta acceptance (spec 2026-08-09 section 4): Pulse is REFLECTED, and its meta was
    // rebound to the plugin image while loaded. After unload+restore, consumers
    // that look meta up fresh must get a live entry -- and the restored
    // descriptor's cached meta pointer must AGREE with the registry (the Astra
    // side rebinds in place at a stable address on pop).
    const Astra::TypeMeta* liveMeta = Astra::MetaRegistry::Instance().Get(Astra::TypeID<Pulse>::Hash());
    REQUIRE(liveMeta != nullptr);
    CHECK(restored->meta == liveMeta);

    CHECK(Arcane::RenderErrorCount() == 0);
}

TEST_CASE("Unloading secondaries leaves no descriptor aimed at their images", "[hotreload]")
{
    // The audited live bug: plugins.clear() used to FreeLibrary secondaries with
    // no purge, leaving Pulse's descriptor aimed at the unmapped V2 image
    // permanently. Now V2's own ComponentModule resets in its Shutdown (and the
    // host's DisownPluginImages nets a forgetter). After Unload, the descriptor
    // must be the test binary's restored base -- calling through it faults if
    // any dangling entry survived.
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();
    rt.Components()->RegisterComponent<RoleCounters>();
    const Astra::ComponentID pulseId = Astra::TypeID<Pulse>::Value();
    const Astra::ComponentDescriptor* base = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(base != nullptr);

    {
        Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
        host.AttachRuntime(rt);
        host.AddPlugin(std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV2")));   // secondary
        REQUIRE(host.Load());
        StepAllK(rt, host, 1);
        // Primary AND secondary both pushed handles over the base: depth-2 stack.
        host.Unload();
    }

    const Astra::ComponentDescriptor* restored = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(restored == base);
    alignas(Pulse) std::byte buf[sizeof(Pulse)];
    restored->DefaultConstruct(buf);      // faults if a dead image's entry was restored
    restored->Destruct(buf);
    CHECK(Arcane::RenderErrorCount() == 0);
}

TEST_CASE("Reload failure with no last-good yields an honest dead state", "[hotreload]")
{
    // The double-failure fix's reachable branch: a reload whose new image fails AND
    // for which there is no last-good to roll back to (rolledBack == false). The host
    // must surface an honest dead state -- IsLoaded()==false / Vtable()==null truly
    // meaning "no plugin" -- rather than installing a half-assigned "loaded" image
    // that would silently freeze the sim behind the main loop's `if (vt)` guards.
    //
    // NOTE: the OTHER double-failure sub-case (a real last-good whose plugin the
    // teardown already emptied, then its temp copy fails to reload) is NOT cleanly
    // reachable from the public fixture API: that temp copy is OS-locked while the
    // module is loaded and is only unlocked mid-ForceReload (inside TeardownImage),
    // where a test cannot intervene. Both sub-cases run the SAME fix code (the
    // rolledBack gate -> reset current + honest ARC_ERROR), so this covers it.

    // A dedicated bad-image source so we never clobber the shared V1 fixture.
    std::filesystem::copy_file(Arcane::Test::ModuleFile("HotReloadPluginBad"), "HotReloadBadSrc.dll",
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();
    rt.Components()->RegisterComponent<RoleCounters>();

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("HotReloadBadSrc.dll"));
    host.AttachRuntime(rt);
    CHECK_FALSE(host.ForceReload());   // new image fails, no last-good -> double failure
    CHECK_FALSE(host.IsLoaded());      // honest: no plugin
    CHECK(host.Vtable() == nullptr);

    // The session recovers cleanly from the dead state: a valid image still loads.
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), "HotReloadBadSrc.dll",
                               std::filesystem::copy_options::overwrite_existing);
    REQUIRE(host.Load());
    REQUIRE(host.IsLoaded());
    REQUIRE(host.Vtable() != nullptr);
    StepAllK(rt, host, 3);
    CHECK(ReadPulse(rt) == 3);
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();

    std::error_code ec;
    std::filesystem::remove("HotReloadBadSrc.dll", ec);
}

TEST_CASE("Resolving a type the UNLOADED plugin registered first is safe", "[hotreload][typecontext]")
{
    // The scenario behind vendored Astra 056063c (stamp b8291b9), pinned HERE because
    // the fix itself is pinned upstream: Astra's TypeContext records a TypeIdentity the
    // FIRST time any module resolves a type, and that identity used to carry a
    // `const std::type_info*` owned by the resolving image. When the first registrar was
    // a plugin DLL, every LATER resolve of that type ran IsTypeIdentityCollision
    // (TypeContext.hpp:162 pre-fix) over a type_info in an image that no longer existed
    // -- the order-dependent SIGSEGV the Core-DLL split closeout recorded at
    // EntityOps.cpp:101. Since 056063c the field is `uint64_t rttiName`, a hash of the
    // mangled name, so the same compare reads two owned values and cannot dangle.
    //
    // Determinism comes from the TYPE, not from --order: ProbeResource is touched by the
    // plugin's OnInit and by nothing else in this exe (HotReloadShared.hpp spells out
    // that rule), so the plugin is its first registrar however Catch2 shuffles.
    // Deliberately NOT asserted while the module is mapped -- a GetResource<ProbeResource>()
    // before Unload() would have the EXE resolve the type with the image still loaded,
    // which still trips the old crash but stops this case pinning "resolve AFTER unload".
    //
    // The user's original lex-order repro of the same fault, kept as documentation
    // rather than a second case (it depends on suite ordering; this one does not):
    //   ArcaneTests.exe "PlaySession routes Play/Stop through the hosted module*,CreateEntityInScene refuses*" --order lex
    std::filesystem::copy_file(Arcane::Test::BuiltModule("HotReloadPluginV1"), Arcane::Test::ModuleFile("HotReloadPluginV1"),
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>();
    rt.Components()->RegisterComponent<RoleCounters>();

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("HotReloadPluginV1")));
    REQUIRE(host.AttachRuntime(rt));
    REQUIRE(host.Load());        // OnInit's SetResource<ProbeResource> is the type's first resolve
    REQUIRE(host.IsLoaded());

    host.Unload();               // the registrar's image unmaps HERE

    // THE PIN: this exe's first-ever resolve of ProbeResource, against an entry whose
    // identity was recorded by code that is gone. Pre-fix this faulted inside
    // IsTypeIdentityCollision; now it is a uint64 compare that finds the existing id.
    const Astra::ComponentID id = Astra::TypeID<Arcane::HotReloadTest::ProbeResource>::Value();
    CHECK(id != Astra::INVALID_COMPONENT);   // resolved, not refused as a collision

    // Unload() reset the registry (fresh-boot semantics), so the resource itself is
    // gone -- reading it through the just-resolved id is the second safe touch.
    CHECK(rt.Registry().GetResource<Arcane::HotReloadTest::ProbeResource>() == nullptr);
    CHECK(Arcane::RenderErrorCount() == 0);
}
