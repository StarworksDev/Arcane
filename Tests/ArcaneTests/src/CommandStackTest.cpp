// Arcane editor undo/redo commands + stack ([edit], CPU-only). No TypeContext
// pin: ComponentEditCommand is hash/descriptor-driven (GetComponentByHash +
// descriptor->serialize), never a type-based CreateView<T>.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/ComponentEditCommand.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include "Helpers/ConstantScan.hpp"
#include "Helpers/TestTypeContext.hpp"

namespace
{
    // Fresh registry with the scene components registered.
    std::unique_ptr<Astra::Registry> MakeReg()
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(components);
        Arcane::RegisterSceneComponents(*reg);
        return reg;
    }

    // The component descriptor for `typeName` on `entity`, via the same
    // InspectEntity path the Inspector uses. NOTE: Astra::TypeMeta::typeName is
    // the __FUNCSIG__/__PRETTY_FUNCTION__-derived name, which is namespace-
    // qualified (e.g. "Arcane::Transform"), not the bare type name --
    // adapted from the brief's literal "Transform" after verifying against
    // the real TypeID<T>::Name() implementation (Astra/Core/TypeID.hpp).
    const Astra::ComponentDescriptor* DescriptorFor(Astra::Registry& reg,
                                                    Astra::Entity e, const char* typeName)
    {
        for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
            if (ci.meta && ci.meta->typeName == typeName)
                return ci.descriptor;
        return nullptr;
    }
}

TEST_CASE("ComponentEditCommand restores a component before/after via reflection", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    Arcane::Transform lt;
    lt.position = glm::vec3(1.0f, 2.0f, 0.0f);
    reg->AddComponent<Arcane::Transform>(e, lt);

    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");
    REQUIRE(desc != nullptr);

    // before = current; mutate; after = mutated.
    std::vector<std::byte> before = Arcane::ComponentEditCommand::Snapshot(*reg, e, desc);
    reg->GetComponent<Arcane::Transform>(e)->position = glm::vec3(9.0f, 9.0f, 0.0f);
    std::vector<std::byte> after = Arcane::ComponentEditCommand::Snapshot(*reg, e, desc);
    REQUIRE(before != after);

    Arcane::ComponentEditCommand cmd([&reg]() -> Astra::Registry& { return *reg; }, e, desc, before, after, "Edit Transform");

    cmd.Undo();
    CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 1.0f);
    CHECK(reg->GetComponent<Arcane::Transform>(e)->position.y == 2.0f);

    cmd.Redo();
    CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 9.0f);
}

TEST_CASE("ComponentEditCommand no-ops on a deleted entity", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");
    REQUIRE(desc != nullptr);

    std::vector<std::byte> before = Arcane::ComponentEditCommand::Snapshot(*reg, e, desc);
    Arcane::ComponentEditCommand cmd([&reg]() -> Astra::Registry& { return *reg; }, e, desc, before, before, "noop");

    reg->DestroyEntity(e);
    CHECK_NOTHROW(cmd.Undo());   // re-resolve returns null -> safe no-op
    CHECK_NOTHROW(cmd.Redo());
}

TEST_CASE("CommandStack: one-gesture transaction undo/redo + redo cleared on new edit", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec3(0.0f);
    reg->AddComponent<Arcane::Transform>(e, lt);
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    // Gesture: Begin -> Snapshot (before) -> mutate -> Commit (after). Begin
    // returns the owner token; only its holder may close the transaction.
    Arcane::TransactionId txn = stack.Begin("move");
    stack.SnapshotComponent(e, desc);
    reg->GetComponent<Arcane::Transform>(e)->position = glm::vec3(5.0f, 0.0f, 0.0f);
    stack.Commit(txn);

    REQUIRE(stack.CanUndo());
    REQUIRE_FALSE(stack.CanRedo());

    stack.Undo();
    CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 0.0f);
    REQUIRE(stack.CanRedo());

    stack.Redo();
    CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 5.0f);

    // A new edit after an undo clears the redo stack.
    stack.Undo();                         // back to x=0, redo available
    REQUIRE(stack.CanRedo());
    txn = stack.Begin("move2"); stack.SnapshotComponent(e, desc);
    reg->GetComponent<Arcane::Transform>(e)->position = glm::vec3(7.0f, 0.0f, 0.0f);
    stack.Commit(txn);
    CHECK_FALSE(stack.CanRedo());
}

TEST_CASE("CommandStack: empty / no-op transaction is not pushed", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    const Arcane::TransactionId noop = stack.Begin("noop");
    stack.SnapshotComponent(e, desc);     // no mutation
    stack.Commit(noop);                    // before == after -> nothing pushed
    CHECK_FALSE(stack.CanUndo());

    const Arcane::TransactionId cancelled = stack.Begin("cancelled");
    stack.SnapshotComponent(e, desc);
    stack.Cancel(cancelled);
    CHECK_FALSE(stack.CanUndo());
}

TEST_CASE("CommandStack: Commit is the safe close for an ABANDONED gesture", "[edit]")
{
    // The Inspector closes a gesture whose widget stopped being drawn -- its
    // component header collapsed mid-edit, the search filter hid the field --
    // with Commit and never Cancel (EditGesture.hpp, ScopeGuard -> ClosePending).
    // These three sections are the properties that make that the right call,
    // for both an edit that was already applied and one that never was.
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    SECTION("Cancel would STRAND a mid-drag edit: it discards without reverting")
    {
        const Arcane::TransactionId txn = stack.Begin("Edit Transform.position");
        stack.SnapshotComponent(e, desc);
        reg->GetComponent<Arcane::Transform>(e)->position.x = 5.0f;   // the drag applied it live
        stack.Cancel(txn);
        CHECK_FALSE(stack.InTransaction());
        CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 5.0f);   // still applied...
        CHECK_FALSE(stack.CanUndo());                                        // ...and unreachable
    }

    SECTION("Commit keeps that same mid-drag edit undoable")
    {
        const Arcane::TransactionId txn = stack.Begin("Edit Transform.position");
        stack.SnapshotComponent(e, desc);
        reg->GetComponent<Arcane::Transform>(e)->position.x = 5.0f;
        stack.Commit(txn);
        REQUIRE(stack.CanUndo());
        stack.Undo();
        CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 0.0f);
    }

    SECTION("Commit on a gesture that applied nothing leaves the redo stack intact")
    {
        // The ctrl+click text-entry orphan: a temp input writes only on submit,
        // so the abandoned transaction holds an UNCHANGED snapshot. Commit has
        // to be as inert as Cancel there -- including not clearing a redo the
        // user had built up, which every non-empty commit does clear.
        const Arcane::TransactionId first = stack.Begin("Edit Transform.position");
        stack.SnapshotComponent(e, desc);
        reg->GetComponent<Arcane::Transform>(e)->position.x = 5.0f;
        stack.Commit(first);
        stack.Undo();
        REQUIRE(stack.CanRedo());

        const Arcane::TransactionId abandoned = stack.Begin("Edit Transform.rotation");
        stack.SnapshotComponent(e, desc);          // nothing mutated before the close
        stack.Commit(abandoned);
        CHECK_FALSE(stack.InTransaction());
        CHECK_FALSE(stack.CanUndo());              // no spurious history entry
        REQUIRE(stack.CanRedo());                  // and the redo survived
        stack.Redo();
        CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 5.0f);
    }
}

TEST_CASE("Abandon commits a live target and restores a survivor when one target is gone; Cancel still does not revert",
          "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity a = reg->CreateEntity();
    const Astra::Entity b = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(a, Arcane::Transform{});
    reg->AddComponent<Arcane::Transform>(b, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, a, "Arcane::Transform");
    REQUIRE(desc != nullptr);
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    auto x = [&reg](Astra::Entity e) { return reg->GetComponent<Arcane::Transform>(e)->position.x; };

    // Cancel: the existing contract. Both stay at 5, nothing undoable.
    {
        const Arcane::TransactionId txn = stack.Begin("Gizmo");
        stack.SnapshotComponent(a, desc);
        stack.SnapshotComponent(b, desc);
        reg->GetComponent<Arcane::Transform>(a)->position.x = 5.0f;
        reg->GetComponent<Arcane::Transform>(b)->position.x = 5.0f;
        stack.Cancel(txn);
        CHECK(x(a) == 5.0f);
        CHECK(x(b) == 5.0f);
        CHECK_FALSE(stack.CanUndo());
        reg->GetComponent<Arcane::Transform>(a)->position.x = 0.0f;
        reg->GetComponent<Arcane::Transform>(b)->position.x = 0.0f;
    }

    // Abandon with every target alive: the pose stays and is undoable.
    {
        const Arcane::TransactionId txn = stack.Begin("Gizmo");
        stack.SnapshotComponent(a, desc);
        stack.SnapshotComponent(b, desc);
        reg->GetComponent<Arcane::Transform>(a)->position.x = 5.0f;
        reg->GetComponent<Arcane::Transform>(b)->position.x = 5.0f;
        stack.Abandon(txn);
        CHECK_FALSE(stack.InTransaction());
        CHECK(x(a) == 5.0f);
        CHECK(x(b) == 5.0f);
        REQUIRE(stack.CanUndo());
        stack.Undo();
        CHECK(x(a) == 0.0f);
        CHECK(x(b) == 0.0f);
    }

    // Abandon with one target destroyed: the survivor returns to its snapshot,
    // and no undo entry is pushed.
    {
        const Arcane::TransactionId txn = stack.Begin("Gizmo");
        stack.SnapshotComponent(a, desc);
        stack.SnapshotComponent(b, desc);
        reg->GetComponent<Arcane::Transform>(a)->position.x = 7.0f;
        reg->GetComponent<Arcane::Transform>(b)->position.x = 7.0f;
        reg->DestroyEntity(b);
        stack.Abandon(txn);
        CHECK_FALSE(stack.InTransaction());
        CHECK(x(a) == 0.0f);
        CHECK_FALSE(stack.CanUndo());
    }

    // A stale owner is inert, as for Commit and Cancel.
    {
        const Arcane::TransactionId txn = stack.Begin("Gizmo");
        stack.Abandon(Arcane::TransactionId::None);
        CHECK(stack.InTransaction());
        stack.Cancel(txn);
    }
}

TEST_CASE("the gizmo's abandoned-drag branch calls Abandon, not Cancel", "[edit]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();

    const std::size_t fn = src.find("void EditorApp::UpdateGizmoInteraction(");
    REQUIRE(fn != std::string::npos);
    const std::size_t end = src.find("// Phase 7: sim advance.", fn);
    REQUIRE(end != std::string::npos);
    const std::string body = src.substr(fn, end - fn);

    const std::size_t elseBranch = body.find("// Only reached with an active drag");
    REQUIRE(elseBranch != std::string::npos);
    const std::string tail = body.substr(elseBranch);
    CHECK(tail.find("Abandon(") != std::string::npos);
    CHECK(tail.find("Cancel(") == std::string::npos);
    CHECK(src.find("editor.gizmo.probe") != std::string::npos);
}

TEST_CASE("CommandStack: a transaction groups two components into one undo step", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    reg->AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});
    const Astra::ComponentDescriptor* dLt = DescriptorFor(*reg, e, "Arcane::Transform");
    const Astra::ComponentDescriptor* dSr = DescriptorFor(*reg, e, "Arcane::SpriteRenderer");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    const Arcane::TransactionId txn = stack.Begin("multi");
    stack.SnapshotComponent(e, dLt);      // captures before = 0
    stack.SnapshotComponent(e, dSr);
    // Discriminating idempotency check: mutate BETWEEN the two dLt touches. If
    // SnapshotComponent were NOT idempotent (re-captured on the second touch),
    // the recorded `before` would be this x=3 value and Undo would restore
    // x=3 instead of the true original x=0.
    reg->GetComponent<Arcane::Transform>(e)->position.x = 3.0f;
    stack.SnapshotComponent(e, dLt);      // idempotent: MUST NOT re-capture x=3
    reg->GetComponent<Arcane::Transform>(e)->position.x = 7.0f;
    reg->GetComponent<Arcane::SpriteRenderer>(e)->sortingLayer = 4;
    stack.Commit(txn);

    stack.Undo();                          // ONE undo reverts BOTH
    CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 0.0f);   // fails if 2nd touch re-snapshotted
    CHECK(reg->GetComponent<Arcane::SpriteRenderer>(e)->sortingLayer == 0);
}

TEST_CASE("CommandStack: survives a registry object swap (resolver, no dangling Registry&)", "[edit]")
{
    // The regression guard for the Critical defect: a command committed
    // BEFORE a registry swap must not hold a dangling Astra::Registry&. The
    // resolver re-reads the CURRENT `reg` pointer on every call, so it always
    // targets whichever registry object is live, mirroring how a real
    // Runtime::RestoreRegistry/ResetRegistry swap replaces the registry object
    // out from under a long-lived CommandStack/ComponentEditCommand.
    //
    // Deliberately NOT MakeReg() here: that helper builds a brand-new
    // Astra::ComponentRegistry per call, and this test's pre-swap command
    // captures a `desc` pointer into the OLD registry's descriptor array via
    // `m_descriptor`. `reg = MakeReg()` would free that ComponentRegistry
    // (the Registry is its sole owner) out from under the still-live
    // `m_descriptor`, so the CHECK_NOTHROW(stack.Undo()) below would read
    // freed memory -- UB, and harsher than production. Production's
    // Runtime::ResetRegistry/RestoreRegistry keep the SAME shared
    // Astra::ComponentRegistry across a registry-object swap (only the
    // Registry object is replaced; see Runtime.cpp), so descriptors never
    // dangle there. Mirror that here: one shared ComponentRegistry, two
    // Registry objects. RegisterComponent<T> is idempotent (guarded by a
    // per-id atomic flag living on the ComponentRegistry -- see
    // ComponentRegistry.hpp), so registering scene components once, before
    // the swap, is enough; the post-swap Registry inherits the registration
    // through the shared ComponentRegistry, exactly as ResetRegistry does not
    // re-register component types after swapping in a fresh Registry object.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    auto reg = std::make_unique<Astra::Registry>(components);
    Arcane::RegisterSceneComponents(*reg);
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    // Commit one edit on the pre-swap registry.
    const Arcane::TransactionId pre = stack.Begin("pre-swap edit");
    stack.SnapshotComponent(e, desc);
    reg->GetComponent<Arcane::Transform>(e)->position = glm::vec3(1.0f, 0.0f, 0.0f);
    stack.Commit(pre);
    REQUIRE(stack.CanUndo());

    // Swap: frees the OLD registry OBJECT the pre-swap command captured
    // through the resolver (still the regression guard for the dangling-
    // Registry& defect). The SAME shared ComponentRegistry survives the swap,
    // so `desc`/`m_descriptor` stay valid -- no freed-descriptor read. Under
    // the old cached-Registry& design, the next Undo would dereference freed
    // memory (UAF). Under the resolver design it resolves the NEW registry.
    reg = std::make_unique<Astra::Registry>(components);

    // The pre-swap entity does not exist in the new registry -> GetComponentByHash
    // returns null -> Restore is a safe no-op. No crash = the win.
    CHECK_NOTHROW(stack.Undo());

    // Positive path: the stack must operate on the SWAPPED-IN registry, not a
    // stale one.
    const Astra::Entity e2 = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e2, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc2 = DescriptorFor(*reg, e2, "Arcane::Transform");

    const Arcane::TransactionId post = stack.Begin("post-swap edit");
    stack.SnapshotComponent(e2, desc2);
    reg->GetComponent<Arcane::Transform>(e2)->position.x = 5.0f;
    stack.Commit(post);

    stack.Undo();
    CHECK(reg->GetComponent<Arcane::Transform>(e2)->position.x == 0.0f);
}

namespace
{
    // Generic ICommand fake: counts Undo/Redo calls (the material-param /
    // future-graph edit path through CommandStack::Push).
    struct CountingCommand final : Arcane::ICommand
    {
        int* undos;
        int* redos;
        std::string label;

        CountingCommand(int* u, int* r, std::string l)
            : undos(u), redos(r), label(std::move(l)) {}
        void Undo() override { ++*undos; }
        void Redo() override { ++*redos; }
        const char* Label() const override { return label.c_str(); }
    };

    // A document-style step (spec 2026-09-30 s3.3): never a scene step, and
    // expired once its weak anchor (the "document") dies.
    struct DocStep final : Arcane::ICommand
    {
        std::weak_ptr<int> anchor;
        int* undos;
        int* redos;
        std::string label;

        DocStep(std::weak_ptr<int> a, int* u, int* r, std::string l)
            : anchor(std::move(a)), undos(u), redos(r), label(std::move(l)) {}
        void Undo() override { if (!anchor.expired()) ++*undos; }
        void Redo() override { if (!anchor.expired()) ++*redos; }
        const char* Label() const override { return label.c_str(); }
        bool AffectsScene() const override { return false; }
        bool IsExpired() const override { return anchor.expired(); }
    };

    // Reports a fixed payload size: the byte-budget unit without real bytes.
    struct SizedCommand final : Arcane::ICommand
    {
        std::size_t bytes;
        explicit SizedCommand(std::size_t b) : bytes(b) {}
        void Undo() override {}
        void Redo() override {}
        const char* Label() const override { return "sized"; }
        std::size_t PayloadBytes() const override { return bytes; }
    };

    int UndoDepth(Arcane::CommandStack& s)   // destructive: undoes everything
    {
        int n = 0;
        while (s.CanUndo()) { s.Undo(); ++n; }
        return n;
    }
}

TEST_CASE("CommandStack::Push: standalone step, transaction join, redo-clear", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    int undos = 0, redos = 0;

    SECTION("outside a gesture, Push is its own labeled undo step")
    {
        stack.Push(std::make_unique<CountingCommand>(&undos, &redos, "Edit Speed"));
        REQUIRE(stack.CanUndo());
        CHECK(std::string(stack.UndoLabel()) == "Edit Speed");
        stack.Undo();
        CHECK(undos == 1);
        stack.Redo();
        CHECK(redos == 1);
    }

    SECTION("inside a gesture, Push joins the transaction (one undo step)")
    {
        const Arcane::TransactionId txn = stack.Begin("combo");
        stack.SnapshotComponent(e, desc);
        reg->GetComponent<Arcane::Transform>(e)->position.x = 4.0f;
        stack.Push(std::make_unique<CountingCommand>(&undos, &redos, "generic"));
        stack.Commit(txn);

        stack.Undo();   // ONE undo reverts the component edit AND the generic
        CHECK(undos == 1);
        CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 0.0f);
        CHECK_FALSE(stack.CanUndo());
    }

    SECTION("a cancelled gesture discards joined generics")
    {
        const Arcane::TransactionId txn = stack.Begin("cancelled");
        stack.Push(std::make_unique<CountingCommand>(&undos, &redos, "generic"));
        stack.Cancel(txn);
        CHECK_FALSE(stack.CanUndo());
    }
}

TEST_CASE("CommandStack::Clear discards generics pushed into an open gesture", "[edit]")
{
    // Review M9 regression: Begin -> Push -> Clear used to leave the pushed
    // command queued in m_pendingGeneric; the NEXT unrelated commit silently
    // spliced it in and undoing that transaction replayed a pre-Clear edit.
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    int undos = 0, redos = 0;

    (void)stack.Begin("doomed");
    stack.Push(std::make_unique<CountingCommand>(&undos, &redos, "leaky"));
    stack.Clear("project switch");   // e.g. project switch mid-gesture

    const Arcane::TransactionId fresh = stack.Begin("fresh");
    stack.SnapshotComponent(e, desc);
    reg->GetComponent<Arcane::Transform>(e)->position.x = 2.0f;
    stack.Commit(fresh);

    stack.Undo();
    CHECK(undos == 0);   // the cleared generic must NOT replay
    CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 0.0f);
}

TEST_CASE("CommandStack: a non-owner cannot close another consumer's transaction", "[edit]")
{
    // Regression for the CRITICAL in docs/superpowers/audits/
    // 2026-07-26-outliner-s4-multiselect-hub-s1-review.md. THREE independent
    // input consumers share one stack -- a gizmo drag (opens on mouse-press,
    // closes on release, spans frames), an Inspector field gesture (opens on
    // widget activation, closes on deactivation, spans frames), and the
    // Inspector's single-shot immediate edits (open+apply+close in one call).
    // ImGui fires two of them in ONE frame routinely: clicking a gizmo handle
    // clears the ActiveId of a text box that still holds uncommitted text, so
    // IsItemDeactivatedAfterEdit() fires on the SAME frame as the gizmo press.
    //
    // Under the old `void Begin` + unconditional `Commit()`, the immediate
    // path's Commit closed the GIZMO's transaction; the rest of the drag then
    // mutated Transforms against a closed stack and the mouse-up Commit
    // no-opped, so the entire drag became silently un-undoable.
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");
    REQUIRE(desc != nullptr);

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    SECTION("a joiner's Commit does not close the owner's transaction")
    {
        const Arcane::TransactionId gizmo = stack.Begin("Gizmo");
        REQUIRE(gizmo != Arcane::TransactionId::None);
        stack.SnapshotComponent(e, desc);          // before = x 0

        // Same frame: the Inspector's ApplyImmediate opens and closes.
        const Arcane::TransactionId joined = stack.Begin("Edit Transform.position");
        CHECK(joined == Arcane::TransactionId::None);   // joined, owns nothing
        stack.Commit(joined);
        CHECK(stack.InTransaction());                   // the gizmo still owns it

        // The rest of the drag, against a stack that is still open.
        reg->GetComponent<Arcane::Transform>(e)->position.x = 5.0f;
        stack.Commit(gizmo);

        REQUIRE(stack.CanUndo());
        CHECK(std::string(stack.UndoLabel()) == "Gizmo");
        stack.Undo();
        CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 0.0f);
    }

    SECTION("a joiner's Cancel does not discard the owner's transaction")
    {
        // The mirror shape: EndGroup forwards the Deactivated flag, so an
        // Inspector field's EndGesture can fire Cancel() during a live drag.
        const Arcane::TransactionId gizmo = stack.Begin("Gizmo");
        stack.SnapshotComponent(e, desc);
        stack.Cancel(Arcane::TransactionId::None);
        CHECK(stack.InTransaction());

        reg->GetComponent<Arcane::Transform>(e)->position.x = 5.0f;
        stack.Commit(gizmo);
        REQUIRE(stack.CanUndo());
        stack.Undo();
        CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 0.0f);
    }

    SECTION("a joiner's snapshots ride along in the owner's transaction")
    {
        // Joining must not LOSE the joiner's edit: its snapshot lands in the
        // open transaction and the real owner commits it. One undo step covers
        // both consumers' mutations.
        reg->AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});
        const Astra::ComponentDescriptor* dSr = DescriptorFor(*reg, e, "Arcane::SpriteRenderer");
        REQUIRE(dSr != nullptr);

        const Arcane::TransactionId gizmo = stack.Begin("Gizmo");
        stack.SnapshotComponent(e, desc);

        const Arcane::TransactionId joined = stack.Begin("Edit SpriteRenderer.sortingLayer");
        stack.SnapshotComponent(e, dSr);
        reg->GetComponent<Arcane::SpriteRenderer>(e)->sortingLayer = 4;
        stack.Commit(joined);                      // no-op: not the owner

        reg->GetComponent<Arcane::Transform>(e)->position.x = 5.0f;
        stack.Commit(gizmo);

        stack.Undo();                              // ONE step reverts BOTH
        CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 0.0f);
        CHECK(reg->GetComponent<Arcane::SpriteRenderer>(e)->sortingLayer == 0);
    }

    SECTION("a stale owner token is inert after its transaction closed")
    {
        // The token is monotonic, not a bool: an owner that already committed
        // cannot reach into whatever transaction is open NOW.
        const Arcane::TransactionId first = stack.Begin("first");
        stack.SnapshotComponent(e, desc);
        reg->GetComponent<Arcane::Transform>(e)->position.x = 1.0f;
        stack.Commit(first);
        REQUIRE(stack.CanUndo());

        const Arcane::TransactionId second = stack.Begin("second");
        // Monotonic: a fresh open is never handed a previously-issued token, which
        // is what makes the staleness check below meaningful rather than accidental.
        // The Inspector depends on this when it commits a still-parked gesture and
        // immediately opens another for the field that just took focus.
        CHECK(second != first);
        CHECK(second != Arcane::TransactionId::None);
        stack.SnapshotComponent(e, desc);
        reg->GetComponent<Arcane::Transform>(e)->position.x = 2.0f;
        stack.Commit(first);                       // stale -> must NOT close `second`
        CHECK(stack.InTransaction());
        stack.Cancel(first);                       // stale -> must NOT discard it either
        CHECK(stack.InTransaction());
        stack.Commit(second);

        stack.Undo();
        CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 1.0f);
    }
}

TEST_CASE("ScopedTransaction only closes a transaction it opened", "[edit]")
{
    // The RAII form of the same rule: nested inside a live gesture it must join
    // (and let the outer owner commit), never commit the outer transaction from
    // its own dtor. This is what makes the Inspector's single-shot immediate
    // edits safe to run mid-drag.
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    const Arcane::TransactionId outer = stack.Begin("Gizmo");
    stack.SnapshotComponent(e, desc);
    {
        Arcane::ScopedTransaction nested(stack, "Edit Transform.position");
        nested.Snapshot(e, desc);
        reg->GetComponent<Arcane::Transform>(e)->position.x = 3.0f;
    }   // dtor must NOT commit -- it never owned the transaction
    CHECK(stack.InTransaction());
    CHECK_FALSE(stack.CanUndo());

    reg->GetComponent<Arcane::Transform>(e)->position.x = 5.0f;
    stack.Commit(outer);
    stack.Undo();
    CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 0.0f);
}

TEST_CASE("CommandStack: depth cap drops the oldest", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    Arcane::UndoLimits limits;
    limits.maxSteps = 2;
    stack.SetLimits(limits);
    for (int i = 1; i <= 3; ++i)
    {
        const Arcane::TransactionId txn = stack.Begin("e");
        stack.SnapshotComponent(e, desc);
        reg->GetComponent<Arcane::Transform>(e)->position = glm::vec3((float)i, 0.0f, 0.0f);
        stack.Commit(txn);
    }
    // Cap 2: only the last two edits are undoable (3->2, 2->1); the 0->1 op was dropped.
    stack.Undo(); stack.Undo();
    CHECK(reg->GetComponent<Arcane::Transform>(e)->position.x == 1.0f);
    CHECK_FALSE(stack.CanUndo());          // the oldest (would restore x=0) was evicted
}

TEST_CASE("StateId identifies the current state, not the number of edits", "[edit]")
{
    // This is what scene dirty-tracking is built on: SceneSession records
    // StateId() at save and compares. Undo back to the save point has to go
    // CLEAN again, which a monotonic edit counter cannot express.
    //
    // Uses a real Arcane::Runtime bound to the process-wide SharedTypeContext
    // (see Helpers/TestTypeContext.hpp and EditorPlayModeTest.cpp) rather than
    // a bare Arcane::Runtime -- a test-local Runtime would steal Arcane.dll's
    // TypeContext slot and Edit:: operations would silently report zero changes.
    Arcane::Runtime runtime(Arcane::Test::Process());
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);

    Arcane::CommandStack stack([&runtime]() -> Astra::Registry& { return runtime.Registry(); });

    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    // DescriptorFor (above) is the verified path: the brief's literal
    // `GetComponentRegistry()->GetDescriptor(Astra::TypeId<T>::Hash())` does not
    // compile -- ComponentRegistry has no GetDescriptor member (the real one is
    // GetComponentDescriptorByHash) and the type is Astra::TypeID, not TypeId.
    const Astra::ComponentDescriptor* desc = DescriptorFor(reg, e, "Arcane::Transform");
    REQUIRE(desc != nullptr);

    CHECK(stack.StateId() == 0);   // empty stack

    auto edit = [&](float x)
    {
        const Arcane::TransactionId t = stack.Begin("Move");
        stack.SnapshotComponent(e, desc);
        reg.GetComponent<Arcane::Transform>(e)->position.x = x;
        stack.Commit(t);
    };

    edit(1.0f);
    const std::uint64_t afterFirst = stack.StateId();
    CHECK(afterFirst != 0);

    edit(2.0f);
    const std::uint64_t afterSecond = stack.StateId();
    CHECK(afterSecond != afterFirst);

    stack.Undo();
    CHECK(stack.StateId() == afterFirst);   // back at the first state EXACTLY

    stack.Redo();
    CHECK(stack.StateId() == afterSecond);

    stack.Undo();
    stack.Undo();
    CHECK(stack.StateId() == 0);            // back to empty

    // A NEW edit after undoing must not re-mint a retired id -- otherwise a
    // saved marker could match a state that is not the saved one.
    edit(3.0f);
    CHECK(stack.StateId() != afterFirst);
    CHECK(stack.StateId() != afterSecond);

    stack.Clear("test");
    CHECK(stack.StateId() == 0);
}

TEST_CASE("StateId: Push (one-shot command path) mints and retires ids too, not just Commit", "[edit]")
{
    // Gap 1 (review of the StateId() work, 2026-07-27): the test above only
    // walks the Commit path (Begin/SnapshotComponent/Commit). CommandStack
    // has a SECOND place that appends to the undo stack -- Push(), the
    // one-shot-command path used for material-param edits and (later) graph
    // edits -- and it stamps its own `txn.id = m_nextId++;` in
    // CommandStack::Push, independently of Commit's. If a refactor merged
    // the two near-duplicate append blocks and dropped the stamp from Push,
    // every other test in this file would stay green while StateId() kept
    // reporting a stale id after a real Push edit -- a caller (scene
    // dirty-tracking) would then read a genuinely dirty scene as clean.
    Arcane::Runtime runtime(Arcane::Test::Process());
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);

    Arcane::CommandStack stack([&runtime]() -> Astra::Registry& { return runtime.Registry(); });
    int undos = 0, redos = 0;

    CHECK(stack.StateId() == 0);   // empty stack

    stack.Push(std::make_unique<CountingCommand>(&undos, &redos, "Edit Speed"));
    const std::uint64_t afterFirstPush = stack.StateId();
    CHECK(afterFirstPush != 0);

    stack.Push(std::make_unique<CountingCommand>(&undos, &redos, "Edit Speed 2"));
    const std::uint64_t afterSecondPush = stack.StateId();
    CHECK(afterSecondPush != afterFirstPush);

    stack.Undo();
    CHECK(stack.StateId() == afterFirstPush);   // back at the first push's state EXACTLY

    // Distinct from an id minted by the Commit path -- Push and Commit share
    // the same m_nextId generator (see both sites' comments), so a real bug
    // that stamped, say, a fixed sentinel from one path could collide with an
    // id from the other and never show up if the two paths were never mixed
    // in one test.
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(reg, e, "Arcane::Transform");
    REQUIRE(desc != nullptr);

    const Arcane::TransactionId t = stack.Begin("Move");
    stack.SnapshotComponent(e, desc);
    reg.GetComponent<Arcane::Transform>(e)->position.x = 9.0f;
    stack.Commit(t);
    const std::uint64_t afterCommit = stack.StateId();
    CHECK(afterCommit != afterFirstPush);
    CHECK(afterCommit != afterSecondPush);

    stack.Undo();
    CHECK(stack.StateId() == afterFirstPush);   // back under the Push id, not the Commit's
}

TEST_CASE("StateId: an id evicted by the depth cap is never observed again", "[edit]")
{
    // Gap 2 (review of the StateId() work, 2026-07-27): CommandStack::Commit's
    // depth cap -- now `Evict()` over `UndoLimits::maxSteps`, oldest-first --
    // physically destroys the oldest Transaction, including its id, and ids
    // are never re-minted. CommandStack.hpp's StateId comment documents this
    // as the deliberately safe direction: a caller who recorded an evicted id
    // can never see it as "clean" again -- it just stays dirty forever,
    // rather than ever risking a false "clean" read against some later state.
    // That interaction was never actually exercised. A regression that
    // weakened the eviction (e.g. popping the wrong end, or the cap
    // comparison losing its bite) would let the evicted id resurface while
    // walking Undo() back toward empty; this test walks that whole path and
    // checks every step, not just the final one.
    Arcane::Runtime runtime(Arcane::Test::Process());
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);

    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(reg, e, "Arcane::Transform");
    REQUIRE(desc != nullptr);

    Arcane::CommandStack stack([&runtime]() -> Astra::Registry& { return runtime.Registry(); });
    Arcane::UndoLimits limits;
    limits.maxSteps = 2;
    stack.SetLimits(limits);

    auto edit = [&](float x)
    {
        const Arcane::TransactionId t = stack.Begin("Move");
        stack.SnapshotComponent(e, desc);
        reg.GetComponent<Arcane::Transform>(e)->position.x = x;
        stack.Commit(t);
    };

    edit(1.0f);
    const std::uint64_t evicted = stack.StateId();   // the state this edit produced -- about to fall off the cap
    REQUIRE(evicted != 0);

    edit(2.0f);
    edit(3.0f);   // cap is 2: this Commit's eviction destroys the first transaction (and `evicted`)

    CHECK(stack.StateId() != evicted);   // the live top is a later state, never the evicted one

    while (stack.CanUndo())
    {
        stack.Undo();
        CHECK(stack.StateId() != evicted);   // must never resurface on the way down to empty
    }
    CHECK(stack.StateId() == 0);   // fully unwound past both surviving edits
}

TEST_CASE("a rename is one ComponentEditCommand undo step", "[edit]")
{
    // Task 1 (Edit::RenameEntity) gave rename its new contract; this pins the
    // mechanism Tasks 4/6 build the Inspector/Outliner rename UI on TOP of --
    // Begin/SnapshotComponent(Identity desc)/Commit around the call, same as
    // any other component edit. MakeReg() registers Identity via
    // RegisterSceneComponents (SceneModule.hpp:26), so no extra setup is needed.
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    // SSO-defeating on purpose: a heap-owning string is the case a raw byte
    // snapshot would corrupt; the serialized-blob path must round-trip it.
    const std::string longName = "A name long enough to defeat SSO ................";
    reg->AddComponent<Arcane::Identity>(e, Arcane::Identity{ Arcane::Guid::Generate(), longName });

    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Identity");
    REQUIRE(desc != nullptr);

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    SECTION("undo restores the exact prior name; redo reapplies")
    {
        const Arcane::TransactionId id = stack.Begin("Rename");
        stack.SnapshotComponent(e, desc);
        REQUIRE(Arcane::Edit::RenameEntity(*reg, e, "Short"));
        stack.Commit(id);

        stack.Undo();
        CHECK(reg->GetComponent<Arcane::Identity>(e)->name == longName);
        stack.Redo();
        CHECK(reg->GetComponent<Arcane::Identity>(e)->name == "Short");
    }

    SECTION("a no-op rename pushes no history entry")
    {
        const Arcane::TransactionId id = stack.Begin("Rename");
        stack.SnapshotComponent(e, desc);
        CHECK_FALSE(Arcane::Edit::RenameEntity(*reg, e, longName));   // unchanged
        stack.Commit(id);
        // Commit re-snapshots and drops unchanged components -- the drop's own
        // `continue` is line 51 (CommandStack.cpp:47-51) -- then pushes nothing
        // (:61-62).
        CHECK_FALSE(stack.CanUndo());
    }
}

// ---- Edit::RenameWithUndo ----------------------------------------------------
// The engine-side rename bracket the Outliner's commit site drives. Its whole
// reason to exist is the Deferred arm: joining an open transaction would let
// that transaction's Cancel discard the rename's undo coverage WITHOUT
// reverting the rename (CommandStack.cpp:75-82). These live here rather than in
// EntityOpsTest.cpp because they need a CommandStack, an Identity descriptor,
// and undo/redo assertions -- exactly this file's fixtures (MakeReg +
// DescriptorFor), none of which EntityOpsTest's raw-mutator World has.

TEST_CASE("RenameWithUndo: Renamed pushes exactly one undoable Rename step", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Identity>(e, Arcane::Identity{ Arcane::Guid::Generate(), "Before" });
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    CHECK(Arcane::Edit::RenameWithUndo(stack, *reg, e, "After")
          == Arcane::Edit::RenameResult::Renamed);
    CHECK(reg->GetComponent<Arcane::Identity>(e)->name == "After");

    REQUIRE(stack.CanUndo());
    CHECK(std::string(stack.UndoLabel()) == "Rename");
    // The scope closed its own transaction: nothing is left open, so the next
    // rename is not deferred.
    CHECK_FALSE(stack.InTransaction());

    stack.Undo();
    CHECK(reg->GetComponent<Arcane::Identity>(e)->name == "Before");
    CHECK_FALSE(stack.CanUndo());   // EXACTLY one entry, not two

    stack.Redo();
    CHECK(reg->GetComponent<Arcane::Identity>(e)->name == "After");
}

TEST_CASE("RenameWithUndo: an unchanged name is NoChange with no history entry", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Identity>(e, Arcane::Identity{ Arcane::Guid::Generate(), "Same" });
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    CHECK(Arcane::Edit::RenameWithUndo(stack, *reg, e, "Same")
          == Arcane::Edit::RenameResult::NoChange);
    CHECK(reg->GetComponent<Arcane::Identity>(e)->name == "Same");
    CHECK_FALSE(stack.CanUndo());        // the empty commit dropped
    CHECK_FALSE(stack.InTransaction());  // ...and still closed the transaction
}

TEST_CASE("RenameWithUndo: defers while a transaction is open, mutating nothing", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Identity>(e, Arcane::Identity{ Arcane::Guid::Generate(), "Before" });
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Identity");
    REQUIRE(desc != nullptr);
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    // Somebody else's gesture owns the stack (an Inspector field activation, a
    // gizmo press) -- exactly the state the Outliner's commit can land in.
    const Arcane::TransactionId owner = stack.Begin("Someone else's gesture");
    REQUIRE(owner != Arcane::TransactionId::None);

    CHECK(Arcane::Edit::RenameWithUndo(stack, *reg, e, "After")
          == Arcane::Edit::RenameResult::Deferred);
    // THE POINT: refused, not half-applied. The name is untouched, so the
    // caller's retry next frame loses nothing.
    CHECK(reg->GetComponent<Arcane::Identity>(e)->name == "Before");
    CHECK(stack.InTransaction());   // the owner's transaction is undisturbed

    SECTION("the owner cancelling cannot strand an applied rename")
    {
        stack.Cancel(owner);
        CHECK(reg->GetComponent<Arcane::Identity>(e)->name == "Before");
        CHECK_FALSE(stack.CanUndo());
        // The retry now succeeds on its own transaction.
        CHECK(Arcane::Edit::RenameWithUndo(stack, *reg, e, "After")
              == Arcane::Edit::RenameResult::Renamed);
        REQUIRE(stack.CanUndo());
        CHECK(std::string(stack.UndoLabel()) == "Rename");
        stack.Undo();
        CHECK(reg->GetComponent<Arcane::Identity>(e)->name == "Before");
    }

    SECTION("the owner committing also frees the stack for the retry")
    {
        stack.Commit(owner);   // no snapshots taken -> no history entry
        CHECK_FALSE(stack.CanUndo());
        CHECK(Arcane::Edit::RenameWithUndo(stack, *reg, e, "After")
              == Arcane::Edit::RenameResult::Renamed);
        CHECK(reg->GetComponent<Arcane::Identity>(e)->name == "After");
    }

    SECTION("Deferred pended nothing: a real edit in the owner's transaction stays invisible to it")
    {
        // WHY: the two sections above cannot distinguish a correct Deferred
        // (nothing snapshotted for Identity) from a regression that calls
        // txn.Snapshot(e, desc) BEFORE checking InTransaction() and returning
        // Deferred. Neither existing check would catch that regression: Cancel
        // discards a pending snapshot WITHOUT reverting it (CommandStack.cpp:
        // 75-82), so the name is still "Before" there either way; and with
        // nothing else touching the entity, Commit's own unchanged-drop
        // (CommandStack.cpp:50-51) would silently swallow a stray pending
        // snapshot too, so CHECK_FALSE(CanUndo()) in the committing section
        // above would still pass either way. Mutating Identity for real
        // here, via the bare mutator rather than RenameWithUndo, forces a
        // regression's stale pended "Before" bytes to disagree with the live
        // post-mutation bytes when Commit re-snapshots -- Commit would then
        // treat that disagreement as a genuine change and push an undo step.
        // A correct Deferred pended nothing for Identity, so this direct
        // edit is invisible to the owner's transaction and Commit still
        // pushes nothing.
        Arcane::Edit::RenameEntity(*reg, e, "Direct");
        stack.Commit(owner);
        CHECK_FALSE(stack.CanUndo());
    }
}

TEST_CASE("RenameWithUndo: Invalid for a dead entity and for a missing Identity", "[edit]")
{
    auto reg = MakeReg();
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    SECTION("dead entity")
    {
        const Astra::Entity e = reg->CreateEntity();
        reg->AddComponent<Arcane::Identity>(e, Arcane::Identity{ Arcane::Guid::Generate(), "Doomed" });
        reg->DestroyEntity(e);
        CHECK(Arcane::Edit::RenameWithUndo(stack, *reg, e, "After")
              == Arcane::Edit::RenameResult::Invalid);
        CHECK_FALSE(stack.CanUndo());
        CHECK_FALSE(stack.InTransaction());   // no transaction was ever opened
    }

    SECTION("live entity with no Identity -- a runtime spawn with no durable identity")
    {
        const Astra::Entity e = reg->CreateEntity();
        reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
        CHECK(Arcane::Edit::RenameWithUndo(stack, *reg, e, "After")
              == Arcane::Edit::RenameResult::Invalid);
        // Never mints identity: the entity still has none afterwards.
        CHECK(reg->GetComponent<Arcane::Identity>(e) == nullptr);
        CHECK_FALSE(stack.CanUndo());
        CHECK_FALSE(stack.InTransaction());
    }
}

TEST_CASE("TouchedSinceState: the per-entity diff against a saved baseline", "[edit]")
{
    // Registry-free path on purpose: Push with `touched` tags exercises the
    // undo-side walk, the redo-side walk, and the diverged case without any
    // component reflection in play. (Commit's changed-snapshot collection is
    // implied by the ComponentEditCommand tests above -- the entities it
    // records are exactly the commands it pushes.)
    struct Nop final : Arcane::ICommand
    {
        void Undo() override {}
        void Redo() override {}
        const char* Label() const override { return "nop"; }
    };
    auto reg = MakeReg();
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    auto push = [&stack](std::initializer_list<Astra::Entity> touched)
    {
        const std::vector<Astra::Entity> t(touched);
        stack.Push(std::make_unique<Nop>(), t);
    };
    const Astra::Entity e1(static_cast<Astra::Entity::StorageType>(1));
    const Astra::Entity e2(static_cast<Astra::Entity::StorageType>(2));
    const Astra::Entity e3(static_cast<Astra::Entity::StorageType>(3));

    // Clean at the empty-stack baseline.
    auto r0 = stack.TouchedSinceState(stack.StateId());
    CHECK(r0.baselineFound);
    CHECK(r0.entities.empty());

    push({ e1 });
    const std::uint64_t saved = stack.StateId();   // "the user saved here"
    push({ e2 });
    push({ e2, e3 });

    // Above the baseline: e2 deduplicated across two steps, plus e3.
    auto r1 = stack.TouchedSinceState(saved);
    REQUIRE(r1.baselineFound);
    CHECK(r1.entities.size() == 2);

    // Undo back to the save point: clean again.
    stack.Undo();
    stack.Undo();
    auto r2 = stack.TouchedSinceState(saved);
    CHECK(r2.baselineFound);
    CHECK(r2.entities.empty());

    // Undo PAST the save point: the baseline sits on the redo side, and the
    // undone step's entities are the diff (its edit is IN the saved state,
    // absent from the current one).
    stack.Undo();
    auto r3 = stack.TouchedSinceState(saved);
    REQUIRE(r3.baselineFound);
    REQUIRE(r3.entities.size() == 1);
    CHECK(r3.entities[0] == e1);

    // Diverge: a new commit clears redo, so the baseline becomes unreachable
    // -- the honest answer is "unknown", not a guess.
    push({ e3 });
    auto r4 = stack.TouchedSinceState(saved);
    CHECK_FALSE(r4.baselineFound);
    CHECK(r4.entities.empty());

    // A never-saved scene (baseline 0): the whole stack is the diff.
    auto r5 = stack.TouchedSinceState(0);
    REQUIRE(r5.baselineFound);
    CHECK(r5.entities.size() == 1);
}

TEST_CASE("ICommand defaults: a scene step, never expired, holding no payload", "[edit][undo]")
{
    int u = 0, r = 0;
    const CountingCommand c(&u, &r, "c");
    CHECK(c.AffectsScene());
    CHECK_FALSE(c.IsExpired());
    CHECK(c.PayloadBytes() == 0);
}

TEST_CASE("SceneStateId follows scene steps only, through undo and redo", "[edit][undo]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    int u = 0, r = 0;
    const auto anchor = std::make_shared<int>(0);

    CHECK(stack.SceneStateId() == 0);
    const Arcane::TransactionId t = stack.Begin("Move");
    stack.SnapshotComponent(e, desc);
    reg->GetComponent<Arcane::Transform>(e)->position.x = 1.0f;
    stack.Commit(t);
    const std::uint64_t sceneId = stack.SceneStateId();
    CHECK(sceneId != 0);
    CHECK(sceneId == stack.StateId());            // a Begin/Commit with a snapshot is a scene step

    stack.Push(std::make_unique<DocStep>(anchor, &u, &r, "Edit Param"));
    CHECK(stack.StateId() != sceneId);            // the document step moved StateId...
    CHECK(stack.SceneStateId() == sceneId);       // ...but not the scene's

    const Arcane::TransactionId g = stack.Begin("Drag Param");
    stack.Push(std::make_unique<DocStep>(anchor, &u, &r, "joined"));
    stack.Commit(g);
    CHECK(stack.SceneStateId() == sceneId);       // a gesture of document commands only is not a scene step

    stack.Undo();
    stack.Undo();
    CHECK(stack.SceneStateId() == sceneId);
    stack.Undo();
    CHECK(stack.SceneStateId() == 0);
    stack.Redo();
    CHECK(stack.SceneStateId() == sceneId);
}

TEST_CASE("an expired step is discarded, never spent on a Ctrl+Z", "[edit][undo]")
{
    auto reg = MakeReg();
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    int u = 0, r = 0, du = 0, dr = 0;
    stack.Push(std::make_unique<CountingCommand>(&u, &r, "Scene Step"));
    auto anchor = std::make_shared<int>(0);
    stack.Push(std::make_unique<DocStep>(anchor, &du, &dr, "Edit Param"));
    CHECK(std::string(stack.UndoLabel()) == "Edit Param");

    anchor.reset();                                      // the document closed
    REQUIRE(stack.CanUndo());
    CHECK(std::string(stack.UndoLabel()) == "Scene Step");   // the label looks past it
    stack.Undo();
    CHECK(u == 1);                                       // ONE press undid the live step
    CHECK(du == 0);
    CHECK_FALSE(stack.CanUndo());
    stack.Redo();
    CHECK(r == 1);
    CHECK_FALSE(stack.CanRedo());                        // discarded, not parked in redo
}

TEST_CASE("only expired entries left: nothing to undo or redo, empty labels", "[edit][undo]")
{
    auto reg = MakeReg();
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    int du = 0, dr = 0;
    auto anchor = std::make_shared<int>(0);
    stack.Push(std::make_unique<DocStep>(anchor, &du, &dr, "a"));
    stack.Push(std::make_unique<DocStep>(anchor, &du, &dr, "b"));
    stack.Undo();                                        // "b" moves to redo
    REQUIRE(du == 1);

    anchor.reset();
    CHECK_FALSE(stack.CanUndo());
    CHECK_FALSE(stack.CanRedo());
    CHECK(std::string(stack.UndoLabel()).empty());
    CHECK(std::string(stack.RedoLabel()).empty());
    stack.Undo();
    stack.Redo();
    CHECK(du == 1);
    CHECK(dr == 0);
}

TEST_CASE("an expired step buried under a live one is discarded when Undo reaches it", "[edit][undo]")
{
    auto reg = MakeReg();
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    int u = 0, r = 0, du = 0, dr = 0;
    auto anchor = std::make_shared<int>(0);
    stack.Push(std::make_unique<CountingCommand>(&u, &r, "A"));
    stack.Push(std::make_unique<DocStep>(anchor, &du, &dr, "Edit Param"));
    stack.Push(std::make_unique<CountingCommand>(&u, &r, "B"));
    anchor.reset();                                      // the document closed under B
    stack.Undo();                                        // B
    CHECK(u == 1);
    CHECK(std::string(stack.UndoLabel()) == "A");        // the label looks past the buried step
    stack.Undo();                                        // ONE press reaches A
    CHECK(u == 2);
    CHECK_FALSE(stack.CanUndo());
    stack.Redo();
    stack.Redo();
    CHECK(r == 2);
    CHECK_FALSE(stack.CanRedo());
    CHECK(du == 0);
    CHECK(dr == 0);
}

TEST_CASE("Clear(reason) drops history and remembers why until the next Clear", "[edit][undo]")
{
    auto reg = MakeReg();
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    int u = 0, r = 0;
    stack.Push(std::make_unique<CountingCommand>(&u, &r, "a"));
    CHECK(stack.ClearedReason().empty());

    stack.Clear("Opened scene level_one");
    CHECK_FALSE(stack.CanUndo());
    CHECK(stack.ClearedReason() == "Opened scene level_one");

    stack.Push(std::make_unique<CountingCommand>(&u, &r, "b"));
    CHECK(stack.ClearedReason() == "Opened scene level_one");   // a push does not retire it
    stack.Clear("Switched project");
    CHECK(stack.ClearedReason() == "Switched project");
}

TEST_CASE("UndoLimits: maxSteps 0 clamps to 1; the byte budget evicts oldest-first, never the top", "[edit][undo]")
{
    auto reg = MakeReg();
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    CHECK(stack.Limits() == Arcane::UndoLimits{});
    Arcane::UndoLimits limits;
    limits.maxSteps = 0;
    stack.SetLimits(limits);
    CHECK(stack.Limits().maxSteps == 1);

    limits.maxSteps = 100;
    limits.byteBudget = 100;
    stack.SetLimits(limits);
    SECTION("over budget: the oldest goes")
    {
        stack.Push(std::make_unique<SizedCommand>(60));
        stack.Push(std::make_unique<SizedCommand>(60));
        CHECK(UndoDepth(stack) == 1);
    }
    SECTION("a single step over budget stays; everything older goes")
    {
        stack.Push(std::make_unique<SizedCommand>(10));
        stack.Push(std::make_unique<SizedCommand>(500));
        CHECK(UndoDepth(stack) == 1);
    }
    SECTION("under budget: nothing goes")
    {
        stack.Push(std::make_unique<SizedCommand>(40));
        stack.Push(std::make_unique<SizedCommand>(40));
        CHECK(UndoDepth(stack) == 2);
    }
}

TEST_CASE("component steps report their blobs to the byte budget", "[edit][undo]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::Transform");
    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    Arcane::UndoLimits limits;
    limits.byteBudget = 1;
    stack.SetLimits(limits);
    for (float x : { 1.0f, 2.0f })
    {
        const Arcane::TransactionId t = stack.Begin("Move");
        stack.SnapshotComponent(e, desc);
        reg->GetComponent<Arcane::Transform>(e)->position.x = x;
        stack.Commit(t);
    }
    CHECK(UndoDepth(stack) == 1);
}
