#pragma once

// CookQueue -- F2b Task 12: the editor's background texture cook, watcher-
// triggered, hash-decided, never blocks. Pure logic, no ImGui and no
// EditorApp dependency (same "[editor]-unit-testable headlessly" pattern as
// ModuleBuild.hpp's composition half): a watcher (EditorAppProject.cpp's
// PollAssetWatch, ~1 Hz mtime sweep) calls NoteChanged() whenever a texture
// source or its .meta sidecar changed on disk; this class owns the
// hash-gate-and-coalesce decision of WHEN to actually run
// Arcane::AssetPipeline::CookSession::CookProject, submits that run through
// an injected Submit function (JobSystem::Submit in production, so a cook
// never runs on -- or blocks -- the caller), and hands finished results back
// to Pump(), called once per frame on the SAME thread NoteChanged() is
// called from.
//
// THREADING CONTRACT: NoteChanged()/CookPending()/Pump() are all meant to be
// called from ONE thread (the editor's main thread in production). The
// CookSession this class owns is touched ONLY from inside a Submit()'d job
// (a JobSystem worker thread) -- m_running is the mutual-exclusion flag that
// guarantees at most one such job is ever mid-CookProject at a time, so two
// job invocations can never touch the SAME CookSession concurrently. The
// completion callback installed via SetOnCookComplete runs ONLY from inside
// Pump(), i.e. on the calling thread, never on the worker -- so it is safe
// for that callback to touch GPU-adjacent state (NriTextureCache, the
// Assets facade, Diagnostics::Publish) the way a worker thread must not.
//
// COALESCING, not per-change queuing: a burst of N NoteChanged() calls while
// a cook is already running collapses into at most ONE follow-up
// CookProject pass (not N), because CookProject re-scans the whole project
// and is itself hash-gated -- nothing is lost by folding several change
// notifications into one pass that runs after all of them landed.

#include "Arcane/AssetPipeline/CookSession.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    class CookQueue
    {
    public:
        // `submit` is JobSystem::Submit in production (`[this](std::function<void()> job)
        // { m_runtime->Jobs().Submit(std::move(job)); }`); a test installs a
        // manually-pumped fake that just stashes `job` instead of running it,
        // so the queuing/coalescing logic is exercised with full control over
        // WHEN each pass actually executes.
        using SubmitFn = std::function<void(std::function<void()>)>;
        using CompletionFn = std::function<void(const Arcane::AssetPipeline::CookResult&)>;

        // `textureDefaults` seeds SetTextureDefaults' value (S6-6) without scheduling a
        // pass, so the project's first cook already resolves against its own values.
        CookQueue(std::filesystem::path projectDir, SubmitFn submit,
                  const Arcane::AssetPipeline::TextureMetaSettings& textureDefaults = {});

        // BLOCKS until any in-flight background pass finishes. RunOnePass
        // captures `this` and may still be executing on a JobSystem worker
        // (JobSystem::Submit is fire-and-forget with no per-task join) --
        // destroying this object while that is true would be a dangling-
        // `this` use-after-free the instant the worker's next loop iteration
        // (or its final result push) touches m_mutex/m_session/m_results.
        // Production trigger is a project switch (EditorApp::
        // ResetPerProjectState), a rare, user-paced event, never per-frame --
        // a bounded wait here is invisible next to CookProject's own cost.
        ~CookQueue();

        CookQueue(const CookQueue&) = delete;
        CookQueue& operator=(const CookQueue&) = delete;

        // Test-only seam, forwarded verbatim to the owned CookSession --
        // mirrors CookSession::SetTextureImporterForTesting (F2c Task 8
        // renamed CookSession's own seam; this wrapper's public name is
        // unchanged). Never called by production code (EditorApp always
        // gets the real importer via CookSession's own default).
        void SetImporterForTesting(Arcane::AssetPipeline::CookSession::ImporterFn fn);

#if !defined(ARC_BUILD_DIST)
        // Test-only seam: runs on the pass's own thread at the one instant RunOnePass has
        // decided to go idle (m_running already false, mutex released), just before the job
        // returns. The worker copies the hook under the lock before m_running clears and calls the copy,
        // since the queue may be destroyed the instant m_running is false. A test calls NoteChanged() from it to prove an edit landing in that window
        // is cooked, not dropped. Compiled out of Dist; never set by production code.
        void SetPassExitHookForTesting(std::function<void()> fn) { m_passExitHook = std::move(fn); }
#endif

        // Fires from INSIDE Pump() (see the class comment's threading
        // contract), once per CookResult a finished background pass
        // produced since the last Pump(). Install once, before the first
        // NoteChanged() -- not meant to change mid-session.
        void SetOnCookComplete(CompletionFn fn);

        // The watcher's hook: a texture source or its .meta sidecar changed
        // on disk (or, harmlessly, might not actually have -- CookSession's
        // own hash-gate is what decides that; this just asks for a pass).
        // HASH-GATE + COALESCE: if a cook is already running, this only
        // marks "run again once it finishes" instead of submitting a second,
        // concurrent CookProject over the same CookSession -- see the class
        // comment. Safe to call from a single main-thread ~1 Hz poll, which
        // is the only production caller.
        void NoteChanged();

        // Whether a cook is currently in flight (submitted, not yet
        // finished) -- the EditorAppFrame verify/capture/bless gate's own
        // "cooks are pending" question (a DIFFERENT, synchronous gate; see
        // that call site's own comment for why it does not go through this
        // class at all).
        [[nodiscard]] bool CookPending() const;

        // Settings arc S6-6: the project's assets.import.texture.* values, the defaults
        // every texture .meta field it leaves out resolves to. Main thread, every frame
        // (EditorApp passes Settings<TextureMetaSettings>()): a CHANGE is stored and
        // re-cooks the project (NoteChanged), so exactly the textures whose resolved
        // settings moved get new artifacts; an unchanged value is a no-op. The pass
        // copies it under m_mutex before CookProject, so a worker never reads it torn.
        void SetTextureDefaults(const Arcane::AssetPipeline::TextureMetaSettings& defaults);

        // Delivers every CookResult a finished background pass produced
        // since the last Pump() call, on the CALLING thread, via the
        // installed completion callback (SetOnCookComplete) -- the only
        // place that callback ever runs. Production: called once per frame,
        // from the same safe window EditorAppProject.cpp's other per-frame
        // polls already use (PumpEditorDocuments, strictly after this
        // frame's own render phase and strictly before the next one's --
        // see SceneRenderResolver::InvalidateMesh's own comment for why that
        // window is safe for GPU-adjacent invalidation).
        void Pump();

    private:
        // The Submit()'d job's body. May loop internally (see NoteChanged's
        // own comment) rather than resubmitting through m_submit for every
        // coalesced follow-up pass.
        void RunOnePass();

        std::filesystem::path m_projectDir;
        SubmitFn m_submit;
        CompletionFn m_onComplete;
        Arcane::AssetPipeline::CookSession m_session;   // touched ONLY from inside RunOnePass

        mutable std::mutex m_mutex;
        bool m_running = false;   // guarded by m_mutex -- exactly one RunOnePass in flight at a time
        bool m_dirty = false;     // guarded by m_mutex -- "run one more pass before going idle"
        Arcane::AssetPipeline::TextureMetaSettings m_textureDefaults{};   // guarded by m_mutex (S6-6)
        std::vector<Arcane::AssetPipeline::CookResult> m_results;   // guarded by m_mutex
#if !defined(ARC_BUILD_DIST)
        std::function<void()> m_passExitHook;   // test seam, see SetPassExitHookForTesting
#endif
    };
}
