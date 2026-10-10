#pragma once

// The T5-B fixture (spec 2026-09-30 s7.5-s7.12; T5-B6). Separate from
// Helpers/AssetFileOpsFakes.hpp (T5-A9's world + fake host) on purpose:
//  - Tree writes RAW files (.bin companions, hand-written .meta) and registers them
//    with Scan()/GuidOf() instead of registering on Write;
//  - it owns a CommandStack and the full AssetOpFacts inputs delete planning needs
//    (refs, sceneAssets, docs, openScene/bootScene/inputActions);
//  - Host records calls in the compact format the T5-B follow-up tests assert
//    verbatim ("Invalidate <kind>", "Evict <n>", "Activity <kind> <detail>") and
//    captures `activity`.
// Tree reuses AssetOpsWorld's tree, registry and Facts() lambdas; only Host is a
// second AssetFileOpHost (an accepted, plan-mandated exception).

#include "AssetFileOpsFakes.hpp"   // same directory: GCC/Clang search only the includer's own dir, not its includers' (MSVC does)
#include "Project/AssetFileOps.hpp"

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Project/AssetRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AssetOpsTest
{
    namespace fs = std::filesystem; using namespace Arcane::Editor;
    struct Tree : Arcane::Test::AssetOpsWorld
    {
        std::vector<Arcane::Guid> sceneAssets; std::vector<AssetOpFacts::Doc> docs; Arcane::Guid openScene, bootScene, inputActions;
        const AssetReferenceIndex* refs = nullptr; Astra::Registry ecs; Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return ecs; } };
        explicit Tree(const char* n) : AssetOpsWorld(n) {}
        // Hides AssetOpsWorld::Write on purpose: writes WITHOUT registering (Scan() registers).
        fs::path Write(std::string_view rel, std::string_view text)
        { const fs::path p = content / std::string(rel); std::error_code e; fs::create_directories(p.parent_path(), e); std::ofstream(p, std::ios::binary) << text; return p; }
        void Scan() { registry.ScanContent(content, "game"); all = registry.All(); }
        Arcane::Guid GuidOf(std::string_view rel) const { for (const auto& [g, mp] : all) if (mp == "game://" + std::string(rel)) return g; return {}; }
        AssetOpFacts Facts()
        {
            auto f = AssetOpsWorld::Facts();
            f.refs = refs; f.openSceneAssets = sceneAssets; f.docs = docs;
            f.openScene = openScene; f.bootScene = bootScene; f.inputActions = inputActions;
            f.diagSiblings = [](const fs::path& p) { return Arcane::Editor::DiagSiblingFiles(p); };   // the real provider (T5-B12)
            return f;
        }
    };
    // The B-tranche recording host: always grants the gates and Recycle, and logs the
    // call vocabulary the T5-B tests assert exactly (see the header comment).
    struct Host final : AssetFileOpHost
    {
        Tree& t; std::vector<std::string> calls; std::vector<AssetActivityEntry> activity; explicit Host(Tree& tr) : t(tr) {}
        AssetOpGates Gates() const override { return { true, true }; }
        Arcane::RebindResult Rebind(const Arcane::Guid& g, const fs::path& p) override { calls.push_back("Rebind"); return t.registry.Rebind(g, p, t.content, "game"); }
        bool Unregister(const Arcane::Guid& g) override { calls.push_back("Unregister"); return t.registry.Remove(g); }
        std::function<std::optional<Arcane::Guid>(const fs::path&)> registerOverride;   // set: replaces the registry's answer (a Register id mismatch)
        std::optional<Arcane::Guid> Register(const fs::path& p) override
        { calls.push_back("Register"); return registerOverride ? registerOverride(p) : t.registry.AddFile(p, t.content, "game"); }
        OsShell::RecycleResult Recycle(std::span<const fs::path> fl) override { calls.push_back("Recycle"); for (const auto& p : fl) { std::error_code e; fs::remove(p, e); } return { true, {}, {}, {} }; }
        bool CloseDocumentFor(const Arcane::Guid&, bool) override { calls.push_back("Close"); return true; }
        void NoteMoved(const Arcane::Guid&, const fs::path& a, const fs::path& b) override { calls.push_back("NoteMoved " + a.filename().string() + "->" + b.filename().string()); }
        void AssetsChanged(std::span<const Arcane::Guid> r, std::span<const Arcane::Guid> a) override { calls.push_back("AssetsChanged -" + std::to_string(r.size()) + " +" + std::to_string(a.size())); }
        void Invalidate(const Arcane::Guid&, AssetKind k) override { calls.push_back("Invalidate " + std::to_string(static_cast<int>(k))); }
        void EvictPaths(std::span<const fs::path> p) override { calls.push_back("Evict " + std::to_string(p.size())); }
        void Activity(AssetActivityEntry e) override { calls.push_back("Activity " + std::to_string(static_cast<int>(e.kind)) + " " + e.detail); activity.push_back(std::move(e)); }
        void ReportError(std::string title, std::string) override { calls.push_back("Error " + title); }
    };
}
