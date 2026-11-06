#include <Arcane/Edit/CommandStack.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/ComponentEditCommand.hpp>
#include <Astra/Component/Component.hpp>
#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <utility>

namespace
{
    // Opens `file` for writing at its COMMITTED end. Bytes past it are a
    // failed write's leftovers and get overwritten.
    bool OpenAtEnd(const Arcane::Detail::UndoSpillFile& file, std::fstream& out)
    {
        std::error_code ec;
        std::filesystem::create_directories(file.path.parent_path(), ec);
        out.open(file.path, std::ios::in | std::ios::out | std::ios::binary);
        if (!out) { out.clear(); out.open(file.path, std::ios::out | std::ios::binary); }
        if (!out) return false;
        out.seekp(static_cast<std::streamoff>(file.size));
        return static_cast<bool>(out);
    }
}

namespace Arcane
{
    CommandStack::CommandStack(std::function<Astra::Registry&()> resolve)
        : m_resolve(std::move(resolve))
    {
    }

    void CommandStack::SetLimits(UndoLimits limits)
    {
        m_limits = limits;
        if (m_limits.maxSteps == 0)
            m_limits.maxSteps = 1;   // the old ctor's clamp
    }

    // Oldest-first while over EITHER bound. The top is never evicted.
    // PayloadBytes is summed live: a RegistryStateCommand gains its redo blob
    // on first Undo, after it was pushed.
    void CommandStack::Evict()
    {
        const auto bytesOf = [](const Transaction& t)
        {
            std::uint64_t n = 0;
            for (const auto& c : t.commands) n += c->PayloadBytes();
            return n;
        };
        std::uint64_t total = 0;
        for (const Transaction& t : m_undo) total += bytesOf(t);
        while (m_undo.size() > 1 &&
               (m_undo.size() > m_limits.maxSteps || total > m_limits.byteBudget))
        {
            if (m_undo.front().affectsScene)
                m_evictedSceneId = m_undo.front().id;   // oldest-first: the last one popped is the newest
            total -= bytesOf(m_undo.front());
            m_undo.pop_front();
        }
    }

    std::shared_ptr<Detail::UndoSpillFile>& CommandStack::AssemblingSpill()
    {
        if (!m_assembling)
            m_assembling = std::make_shared<Detail::UndoSpillFile>(
                m_spillDir / (std::to_string(m_spillSeq++) + ".bin"));
        return m_assembling;
    }

    UndoPayload CommandStack::MakePayload(std::vector<std::byte>&& bytes)
    {
        UndoPayload p;
        p.m_size  = bytes.size();
        p.m_bytes = std::move(bytes);
        if (m_spillDir.empty() || p.m_size <= m_limits.spillThreshold)
            return p;
        auto& file = AssemblingSpill();
        std::fstream out;
        if (!OpenAtEnd(*file, out) ||
            !out.write(reinterpret_cast<const char*>(p.m_bytes.data()), static_cast<std::streamsize>(p.m_size)) ||
            !out.flush())
        {
            ARC_WARN("Undo: spilling a {}-byte payload to '{}' failed -- it stays in memory",
                     p.m_size, file->path.generic_string());
            return p;
        }
        p.m_file   = file;
        p.m_offset = file->size;
        file->size += p.m_size;
        file->live += p.m_size;   // released by the payload's dtor / move-over
        std::vector<std::byte>().swap(p.m_bytes);   // leaves memory
        return p;
    }

    std::optional<UndoPayload> CommandStack::MakePayloadFromFile(const std::filesystem::path& source)
    {
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(source, ec);
        if (ec) return std::nullopt;
        std::ifstream in(source, std::ios::binary);
        if (!in) return std::nullopt;
        UndoPayload p;
        p.m_size = static_cast<std::size_t>(size);
        if (!m_spillDir.empty() && size > m_limits.spillThreshold)
        {
            auto& file = AssemblingSpill();
            std::fstream out;
            bool ok = OpenAtEnd(*file, out);
            std::vector<char> chunk(std::size_t{1} << 20);   // 1 MB: never the whole file in RAM
            for (std::uintmax_t left = size; ok && left > 0;)
            {
                const auto n = static_cast<std::streamsize>(std::min<std::uintmax_t>(left, chunk.size()));
                ok = static_cast<bool>(in.read(chunk.data(), n)) && static_cast<bool>(out.write(chunk.data(), n));
                left -= static_cast<std::uintmax_t>(n);
            }
            if (ok && out.flush())
            {
                p.m_file   = file;
                p.m_offset = file->size;
                file->size += size;
                file->live += size;   // see MakePayload
                return p;
            }
            ARC_WARN("Undo: spilling '{}' to '{}' failed -- it stays in memory",
                     source.generic_string(), file->path.generic_string());
            in.clear();
            in.seekg(0);
        }
        p.m_bytes.resize(p.m_size);
        if (!in.read(reinterpret_cast<char*>(p.m_bytes.data()), static_cast<std::streamsize>(p.m_size)))
            return std::nullopt;
        return p;
    }

    TransactionId CommandStack::Begin(std::string label)
    {
        if (m_openId != TransactionId::None)
            return TransactionId::None;   // already open; keep the first, caller JOINS it
        m_openId = static_cast<TransactionId>(m_nextId++);
        m_openLabel = std::move(label);
        m_pending.clear();
        m_pendingTouched.clear();
        return m_openId;
    }

    void CommandStack::SnapshotComponent(Astra::Entity entity,
                                         const Astra::ComponentDescriptor* descriptor)
    {
        if (m_openId == TransactionId::None || !descriptor)
            return;
        // Idempotent: first touch of (entity, descriptor) snapshots; later ones no-op.
        for (const Pending& p : m_pending)
            if (p.entity == entity && p.descriptor == descriptor)
                return;
        m_pending.push_back(Pending{
            entity, descriptor,
            ComponentEditCommand::Snapshot(m_resolve(), entity, descriptor) });
    }

    void CommandStack::Commit(TransactionId owner)
    {
        // Not the owner (a joiner passes None; a committed consumer passes a
        // stale id) -> leave the open transaction to whoever owns it.
        if (owner == TransactionId::None || owner != m_openId)
            return;
        Transaction txn;
        txn.label = m_openLabel;
        for (Pending& p : m_pending)
        {
            std::vector<std::byte> after =
                ComponentEditCommand::Snapshot(m_resolve(), p.entity, p.descriptor);
            if (after == p.before)
                continue;   // unchanged -> drop
            // CHANGED snapshots only: an entity that was snapshotted but not
            // actually edited must not pick up an unsaved marker.
            txn.touched.push_back(p.entity);
            txn.commands.push_back(std::make_unique<ComponentEditCommand>(
                m_resolve, p.entity, p.descriptor,
                std::move(p.before), std::move(after), m_openLabel));
        }
        for (auto& c : m_pendingGeneric)
            txn.commands.push_back(std::move(c));
        txn.touched.insert(txn.touched.end(),
                           m_pendingTouched.begin(), m_pendingTouched.end());
        m_openId = TransactionId::None;
        m_pending.clear();
        m_pendingGeneric.clear();
        m_pendingTouched.clear();
        if (txn.commands.empty())
        {
            m_assembling.reset();   // nothing changed -> no history entry (and no file)
            return;
        }
        txn.affectsScene = std::any_of(txn.commands.begin(), txn.commands.end(),
                                       [](const std::unique_ptr<ICommand>& c) { return c->AffectsScene(); });

        // Stamp the state this transaction produced. m_nextId is the same
        // monotonic source TransactionId::Begin draws from, so ids are unique
        // across BOTH uses and a committed state id can never collide with a
        // live transaction token.
        txn.id = m_nextId++;
        txn.spill = std::move(m_assembling);   // the step adopts its file
        m_undo.push_back(std::move(txn));
        m_redo.clear();
        Evict();
    }

    void CommandStack::Cancel(TransactionId owner)
    {
        if (owner == TransactionId::None || owner != m_openId)
            return;   // see Commit: only the owner may discard.
        m_openId = TransactionId::None;
        m_assembling.reset();
        m_pending.clear();
        m_pendingGeneric.clear();
        m_pendingTouched.clear();
    }

    void CommandStack::Abandon(TransactionId owner)
    {
        if (owner == TransactionId::None || owner != m_openId)
            return;   // see Commit: only the owner may close.
        if (m_pending.empty() && m_pendingGeneric.empty())
        {
            Cancel(owner);
            return;
        }
        Astra::Registry& reg = m_resolve();
        const bool allLive = std::all_of(m_pending.begin(), m_pending.end(), [&](const Pending& p) {
            return p.descriptor != nullptr && reg.GetComponentByHash(p.entity, p.descriptor->hash) != nullptr;
        });
        if (allLive && m_pendingGeneric.empty())
        {
            Commit(owner);
            return;
        }
        // Restore is private on the command: build the same command Commit would
        // and Undo() the temporary, which writes `before` back (a no-op for a
        // component that is gone).
        for (Pending& p : m_pending)
        {
            if (p.descriptor == nullptr || reg.GetComponentByHash(p.entity, p.descriptor->hash) == nullptr)
                continue;
            ComponentEditCommand restore(m_resolve, p.entity, p.descriptor, std::move(p.before), {}, m_openLabel);
            restore.Undo();
        }
        Cancel(owner);
    }

    void CommandStack::Push(std::unique_ptr<ICommand> command,
                            std::span<const Astra::Entity> touched)
    {
        if (!command)
            return;
        if (m_openId != TransactionId::None)
        {
            m_pendingGeneric.push_back(std::move(command));
            m_pendingTouched.insert(m_pendingTouched.end(), touched.begin(), touched.end());
            return;
        }
        Transaction txn;
        txn.label = command->Label();
        txn.touched.assign(touched.begin(), touched.end());
        txn.affectsScene = command->AffectsScene();
        txn.commands.push_back(std::move(command));
        // See Commit: same stamp-before-push rule, same shared m_nextId source.
        txn.id = m_nextId++;
        txn.spill = std::move(m_assembling);   // the step adopts its file
        m_undo.push_back(std::move(txn));
        m_redo.clear();
        Evict();
    }

    bool CommandStack::Expired(const Transaction& t) noexcept
    {
        return !t.commands.empty() &&
               std::all_of(t.commands.begin(), t.commands.end(),
                           [](const std::unique_ptr<ICommand>& c) { return c->IsExpired(); });
    }

    void CommandStack::DiscardExpired(std::deque<Transaction>& d) noexcept
    {
        while (!d.empty() && Expired(d.back()))
            d.pop_back();
    }

    const CommandStack::Transaction* CommandStack::TopLive(const std::deque<Transaction>& d) noexcept
    {
        for (auto it = d.rbegin(); it != d.rend(); ++it)
            if (!Expired(*it))
                return &*it;
        return nullptr;
    }

    bool CommandStack::CanUndo() const noexcept { return TopLive(m_undo) != nullptr; }
    bool CommandStack::CanRedo() const noexcept { return TopLive(m_redo) != nullptr; }

    void CommandStack::Undo()
    {
        DiscardExpired(m_undo);
        if (m_undo.empty())
            return;
        Transaction txn = std::move(m_undo.back());
        m_undo.pop_back();
        // Payloads made while this step replays (RegistryStateCommand's
        // after-capture) land in ITS file, not the next step's.
        std::swap(m_assembling, txn.spill);
        for (auto it = txn.commands.rbegin(); it != txn.commands.rend(); ++it)
            (*it)->Undo();   // reverse order
        std::swap(m_assembling, txn.spill);
        m_redo.push_back(std::move(txn));
    }

    void CommandStack::Redo()
    {
        DiscardExpired(m_redo);
        if (m_redo.empty())
            return;
        Transaction txn = std::move(m_redo.back());
        m_redo.pop_back();
        std::swap(m_assembling, txn.spill);   // see Undo
        for (auto& c : txn.commands)
            c->Redo();       // forward order
        std::swap(m_assembling, txn.spill);
        m_undo.push_back(std::move(txn));
    }

    std::uint64_t CommandStack::SceneStateId() const noexcept
    {
        for (auto it = m_undo.rbegin(); it != m_undo.rend(); ++it)
            if (it->affectsScene)
                return it->id;
        // Eviction took every scene step: the scene is still in the state the
        // newest evicted one produced, so a save under N document edits keeps
        // reading clean and undo can never reach below it.
        return m_evictedSceneId;
    }

    const char* CommandStack::UndoLabel() const noexcept
    {
        const Transaction* t = TopLive(m_undo);
        return t ? t->label.c_str() : "";
    }
    const char* CommandStack::RedoLabel() const noexcept
    {
        const Transaction* t = TopLive(m_redo);
        return t ? t->label.c_str() : "";
    }

    void CommandStack::Clear(std::string reason)
    {
        m_clearedReason = std::move(reason);
        m_undo.clear();
        m_redo.clear();
        m_evictedSceneId = 0;
        m_openId = TransactionId::None;
        m_pending.clear();
        m_pendingGeneric.clear();
        m_pendingTouched.clear();
        m_assembling.reset();
    }

    CommandStack::TouchedSince CommandStack::TouchedSinceState(std::uint64_t savedStateId) const
    {
        TouchedSince out;
        if (StateId() == savedStateId)
        {
            out.baselineFound = true;   // at the save point: nothing differs
            return out;
        }

        // Dedup while accumulating: several steps commonly touch one entity.
        auto add = [&out](const std::vector<Astra::Entity>& touched)
        {
            for (Astra::Entity e : touched)
                if (std::find(out.entities.begin(), out.entities.end(), e) == out.entities.end())
                    out.entities.push_back(e);
        };

        // Baseline BELOW the current state (the normal case): every undo entry
        // ABOVE it is the diff. savedStateId 0 is the empty-stack bottom, so
        // the whole stack is the diff -- with the StateId eviction caveat
        // mapped here: entries Evict() dropped took their touched lists
        // with them, so a 0-baseline diff can understate after 100+ steps.
        for (auto it = m_undo.rbegin(); it != m_undo.rend(); ++it)
        {
            if (it->id == savedStateId)
            {
                out.baselineFound = true;   // this entry PRODUCED the saved state
                return out;
            }
            add(it->touched);
        }
        // m_evictedSceneId is a reachable bottom too: only document steps
        // (touched empty) can have been evicted after it, so the diff above
        // it is complete.
        if (savedStateId == 0 || savedStateId == m_evictedSceneId)
        {
            out.baselineFound = true;
            return out;
        }

        // Baseline AHEAD of the current state (the user undid past the save
        // point): the redo entries up to AND INCLUDING the baseline's are the
        // diff -- the baseline entry's edit is IN the saved state and absent
        // from the current one. m_redo's back is the next-to-redo (Undo
        // push_back / Redo pop_back), so back-to-front walks forward in time.
        out.entities.clear();
        for (auto it = m_redo.rbegin(); it != m_redo.rend(); ++it)
        {
            add(it->touched);
            if (it->id == savedStateId)
            {
                out.baselineFound = true;
                return out;
            }
        }

        // Unreachable (evicted, or diverged past the save point): unknowable.
        out.entities.clear();
        out.baselineFound = false;
        return out;
    }

    // ---- ScopedTransaction --------------------------------------------------
    ScopedTransaction::ScopedTransaction(CommandStack& stack, std::string label)
        : m_stack(stack), m_id(stack.Begin(std::move(label)))
    {
    }
    ScopedTransaction::~ScopedTransaction()
    {
        // Both are no-ops when m_id is None (this scope joined an already-open
        // transaction) -- the owner closes it, so a nested scope never commits
        // or discards someone else's gesture.
        if (m_cancelled) m_stack.Cancel(m_id);
        else             m_stack.Commit(m_id);
    }
    void ScopedTransaction::Snapshot(Astra::Entity entity,
                                     const Astra::ComponentDescriptor* descriptor)
    {
        m_stack.SnapshotComponent(entity, descriptor);
    }
}
