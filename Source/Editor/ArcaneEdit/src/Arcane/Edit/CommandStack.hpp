#pragma once
#include <Arcane/Modules/ArcaneEditApi.hpp>

// Arcane/Edit: undo/redo history. The undo unit is a Transaction of 1..N
// ComponentEditCommands (Unreal FTransaction model). Begin/SnapshotComponent
// (idempotent snapshot-on-first-touch)/Commit/Cancel groups a gesture into one
// step. ARC_EDIT_API; Arcane Editor owns one and brackets its Inspector edits.

#include <Arcane/Modules/ArcaneEditApi.hpp>
#include <Arcane/Core/Constant.hpp>   // ARC_CONSTANT
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/UndoPayload.hpp>

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane
{
    // Ownership token for ONE open transaction, minted by CommandStack::Begin.
    // Monotonic per stack; `None` owns nothing. Only the call that actually
    // opened a transaction receives its live id, and Commit/Cancel ignore any
    // other id -- see Begin for why that has to be enforced rather than trusted.
    enum class TransactionId : std::uint64_t { None = 0 };

    // Undo bounds (spec 2026-09-30 s3.3(e)). The stack never reads a cvar:
    // the editor pushes editor.undo.* in through SetLimits (s2.4).
    struct UndoLimits
    {
        ARC_CONSTANT("engine fallback for an unconfigured CommandStack; the editor always pushes editor.undo.*")
        std::size_t   maxSteps       = 100;              // 0 clamps to 1
        ARC_CONSTANT("engine fallback for an unconfigured CommandStack; the editor always pushes editor.undo.*")
        std::uint64_t byteBudget     = 512ull << 20;     // RAM + spilled bytes
        ARC_CONSTANT("engine fallback for an unconfigured CommandStack; the editor always pushes editor.undo.*")
        std::uint64_t spillThreshold = 256ull << 10;     // payloads ABOVE it spill
        friend bool operator==(const UndoLimits&, const UndoLimits&) = default;
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std::function/deque/vector/string members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARC_EDIT_API CommandStack
    {
    public:
        // `resolve` returns the CURRENT live registry each call (see
        // ComponentEditCommand's ctor comment) -- the stack never caches a
        // Registry& itself, so it survives Runtime::RestoreRegistry/ResetRegistry
        // swapping the registry object out from under it.
        explicit CommandStack(std::function<Astra::Registry&()> resolve);

        // Takes effect at the next push or commit (eviction runs there).
        void SetLimits(UndoLimits limits);
        [[nodiscard]] const UndoLimits& Limits() const noexcept { return m_limits; }

        // Spill target (<project>/Saved/UndoCache from the editor); empty =
        // memory-only. The editor owns wiping it at project open/close.
        void SetSpillDirectory(std::filesystem::path dir) { m_spillDir = std::move(dir); }
        [[nodiscard]] const std::filesystem::path& SpillDirectory() const noexcept { return m_spillDir; }
        // Payload factories. Above Limits().spillThreshold (and with a spill
        // directory) the bytes go to the CURRENT step's file: the step about
        // to be pushed/committed, or the one being undone/redone. A failed
        // write keeps the payload in memory with one WARN.
        [[nodiscard]] UndoPayload MakePayload(std::vector<std::byte>&& bytes);
        // Streams in 1 MB chunks straight to the spill file above the
        // threshold, else reads into memory. nullopt = source unreadable.
        [[nodiscard]] std::optional<UndoPayload> MakePayloadFromFile(const std::filesystem::path& source);

        // Non-copyable: m_undo/m_redo hold move-only ICommand transactions, and
        // this class is dllexport'd -- MSVC eagerly instantiates implicit
        // special members for exported classes, so an implicit copy ctor would
        // hard-error trying to copy std::unique_ptr<ICommand>. Delete explicitly.
        CommandStack(const CommandStack&) = delete;
        CommandStack& operator=(const CommandStack&) = delete;

        // Transaction grouping.
        //
        // Opens a transaction and returns its OWNER TOKEN. When one is already
        // open the FIRST is kept and this returns TransactionId::None -- the
        // caller has JOINED it: its snapshots ride along and are committed (or
        // discarded) by the real owner, so joining never loses an edit.
        //
        // Ownership is a checked token rather than a convention because several
        // INDEPENDENT input consumers share one stack -- a gizmo drag, an
        // Inspector field gesture (both spanning frames), and the Inspector's
        // single-shot immediate edits -- and ImGui fires two of them in ONE frame
        // routinely: pressing a gizmo handle clears the ActiveId of a text box
        // holding uncommitted text, so that box's deactivate-after-edit lands in
        // the same frame as the press. Under the previous `void Begin` +
        // unconditional Commit, the single-shot path's Commit closed the GIZMO's
        // transaction; the remainder of the drag then mutated Transforms against
        // a closed stack and the mouse-up Commit no-opped, making the whole drag
        // silently un-undoable. A monotonic token also makes a STALE owner inert:
        // a consumer that already committed cannot reach into whatever
        // transaction happens to be open now, which a bool "I opened it" flag
        // would happily do.
        [[nodiscard]] TransactionId Begin(std::string label);
        // Idempotent before-snapshot of (entity, descriptor) into the open
        // transaction. Call BEFORE the live edit mutates the component.
        void SnapshotComponent(Astra::Entity entity, const Astra::ComponentDescriptor* descriptor);
        // Both no-op unless `owner` is the currently-open transaction's token, so
        // a joiner (None) and a stale owner are inert instead of clobbering the
        // consumer that owns the stack now.
        void Commit(TransactionId owner);   // capture afters; push if any changed; clear redo; close
        void Cancel(TransactionId owner);   // discard the open transaction (no push, no revert)
        // Close a gesture whose input vanished mid-drag (gizmo target deleted or
        // deselected). Every target still has its component: Commit, so the pose
        // the author dragged to stays and is undoable. Any target gone: write the
        // survivors back to their before-snapshot and drop the transaction (no undo
        // entry). A generic command pushed into the open transaction is dropped in
        // the mixed case by Cancel; gizmo drags do not push generic commands.
        // No pending snapshots at all behaves as Cancel.
        void Abandon(TransactionId owner);

        // Push an ALREADY-APPLIED generic command (the ICommand contract: the
        // live edit happened, the command only reverses/replays). Joins the open
        // transaction when one is open (committed/cancelled with it), otherwise
        // becomes its own one-command undo step labeled by cmd->Label(). This is
        // the non-component edit path -- material param edits, and later graph
        // edits, share the ONE undo history through it.
        //
        // `touched`: the scene entities this command's edit affected, for
        // TouchedSinceState below. Generic commands cannot be introspected
        // (RegistryStateCommand is opaque registry bytes), so the CALLER names
        // them -- ApplyRegistryMutation threads them from the structural call
        // sites, which know their semantic targets. Empty = touches no
        // entities (correct for asset edits like material params).
        void Push(std::unique_ptr<ICommand> command,
                  std::span<const Astra::Entity> touched = {});

        void Undo();
        void Redo();
        // Both look past EXPIRED entries (spec s3.3(c), UE skips expired transactions).
        [[nodiscard]] bool CanUndo() const noexcept;
        [[nodiscard]] bool CanRedo() const noexcept;
        // Structural mementos refuse to run inside an open gesture (Cancel
        // would discard their undo coverage without reverting the edit --
        // see ApplyRegistryMutation).
        [[nodiscard]] bool InTransaction() const noexcept { return m_openId != TransactionId::None; }
        [[nodiscard]] const char* UndoLabel() const noexcept;
        [[nodiscard]] const char* RedoLabel() const noexcept;
        // Drops all history (scene open, project switch, module reload). The
        // reason feeds Edit > "Can't undo after: <reason>" (spec s3.3(d), UE
        // ET:1490-1496) and lasts until the next Clear.
        void Clear(std::string reason);
        [[nodiscard]] const std::string& ClearedReason() const noexcept { return m_clearedReason; }

        // Identifies the CURRENT state: the id of the transaction on top of the
        // undo stack, 0 when the stack is empty.
        //
        // Exists so a caller can record "the state I saved" and later ask
        // whether anything has changed since. Undoing back to that state
        // restores its id, so undo-to-the-save-point reads as clean -- which a
        // simple change counter gets wrong. If the recorded transaction is
        // evicted (SetLimits bounds) its id becomes unreachable and the caller
        // stays dirty; that is the safe direction, and the same caveat Qt
        // documents for QUndoStack's clean state.
        [[nodiscard]] std::uint64_t StateId() const noexcept
        {
            return m_undo.empty() ? 0u : m_undo.back().id;
        }

        // The id of the topmost undo entry that AFFECTS THE SCENE (spec
        // s3.3(a)). SceneSession's dirty flag compares this, so a
        // material/sprite/mesh/input-actions step never marks the scene
        // unsaved, and undo back to the save point still reads clean. With no
        // scene step left it is the newest EVICTED scene step's id (the state
        // the scene is still in), or 0 when none was evicted since Clear.
        [[nodiscard]] std::uint64_t SceneStateId() const noexcept;

        // The entities whose state differs from `savedStateId` (the value
        // StateId() returned when the caller saved) -- the per-entity form of
        // the StateId dirty test, and the source for the Outliner's unsaved
        // asterisks. Field edits contribute automatically (Commit records the
        // entities of every CHANGED snapshot); structural edits contribute
        // what their call sites tagged through Push's `touched`.
        struct TouchedSince
        {
            // False when the baseline is UNREACHABLE from the current state:
            // its transaction was evicted (SetLimits bounds), or the user
            // undid past the save point and then committed new work (which
            // clears redo). The per-entity answer is then unknowable, and
            // callers should treat EVERY entity as possibly modified -- the
            // same safe direction StateId's eviction caveat takes.
            bool baselineFound = false;
            std::vector<Astra::Entity> entities;   // deduplicated, unordered
        };
        [[nodiscard]] TouchedSince TouchedSinceState(std::uint64_t savedStateId) const;

    private:
        struct Transaction
        {
            std::string label;
            std::vector<std::unique_ptr<ICommand>> commands;
            // Scene entities this step changed (TouchedSinceState's unit):
            // the changed-snapshot entities plus anything Push tagged.
            std::vector<Astra::Entity> touched;
            // Identifies the STATE this transaction produced. Stamped from
            // m_nextId (the same monotonic generator TransactionId uses), so an
            // id is never re-minted and a retired state can never be mistaken
            // for a live one.
            std::uint64_t id = 0;
            // Any part affects the scene (ICommand::AffectsScene). Fixed at
            // push/commit; a component snapshot always makes it true.
            bool affectsScene = true;
            std::shared_ptr<Detail::UndoSpillFile> spill;   // this step's file (lazily made)
        };
        struct Pending
        {
            Astra::Entity                     entity;
            const Astra::ComponentDescriptor* descriptor;
            std::vector<std::byte>            before;
        };

        // A transaction with nothing left to act on: every command expired
        // (a component snapshot never is, so a snapshot keeps it live).
        static bool Expired(const Transaction& t) noexcept;
        // Pop expired entries off the TOP, releasing their payloads. Discarding
        // (not skipping in place) keeps undo/redo order sound.
        static void DiscardExpired(std::deque<Transaction>& d) noexcept;
        static const Transaction* TopLive(const std::deque<Transaction>& d) noexcept;
        // Oldest-first eviction while over either UndoLimits bound; never the top.
        void Evict();
        // The current step's spill file, made on first use (<dir>/<seq>.bin).
        std::shared_ptr<Detail::UndoSpillFile>& AssemblingSpill();

        std::function<Astra::Registry&()> m_resolve;
        UndoLimits                        m_limits;
        std::uint64_t                     m_evictedSceneId = 0;   // the newest EVICTED scene step: SceneStateId's floor

        std::deque<Transaction> m_undo;
        std::deque<Transaction> m_redo;

        // m_openId doubles as the "is one open" flag (None = closed); m_nextId
        // only ever increases, so a committed token can never be re-minted.
        TransactionId        m_openId = TransactionId::None;
        std::uint64_t        m_nextId = 1;
        std::string          m_openLabel;
        std::vector<Pending> m_pending;
        std::vector<std::unique_ptr<ICommand>> m_pendingGeneric;   // Push while open
        std::vector<Astra::Entity>             m_pendingTouched;   // Push's tags while open
        std::string                            m_clearedReason;    // why the last Clear ran

        std::filesystem::path                  m_spillDir;         // empty = memory-only
        std::shared_ptr<Detail::UndoSpillFile> m_assembling;       // the current step's file
        std::uint64_t                          m_spillSeq = 1;     // next <seq>.bin
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    // RAII form for single-scope edits (gizmo drag-commit, programmatic
    // multi-edit, the Inspector's single-shot immediate edits). Commits in the
    // dtor unless Cancel() was called -- but ONLY if it opened the transaction:
    // constructed inside a live gesture it JOINS, and then neither commits nor
    // cancels, leaving that to the owner. NOT for a gesture that spans frames
    // (an Inspector field drag): the token has to outlive the scope, so those
    // use explicit Begin/Commit with the token parked in persistent state.
    class ARC_EDIT_API ScopedTransaction
    {
    public:
        ScopedTransaction(CommandStack& stack, std::string label);
        ~ScopedTransaction();
        void Snapshot(Astra::Entity entity, const Astra::ComponentDescriptor* descriptor);
        void Cancel() noexcept { m_cancelled = true; }
        // False when this scope joined an already-open transaction rather than
        // opening its own (so the dtor will leave the stack alone).
        [[nodiscard]] bool OwnsTransaction() const noexcept { return m_id != TransactionId::None; }

        ScopedTransaction(const ScopedTransaction&) = delete;
        ScopedTransaction& operator=(const ScopedTransaction&) = delete;

    private:
        CommandStack& m_stack;
        TransactionId m_id = TransactionId::None;
        bool          m_cancelled = false;
    };
}
