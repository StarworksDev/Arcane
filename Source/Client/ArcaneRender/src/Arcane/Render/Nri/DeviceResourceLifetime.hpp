#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// DeviceResourceLifetime -- the device-wide last-use record behind bindless
// slot recycling (3D-readiness arc spec 3.5, approach A).
//
// A bindless slot is sampled by every NriGraphContext on a device that binds
// the device table, and each context has its OWN submission timeline: a fence
// value from one context says nothing about another's. So the record keeps,
// per slot key, one entry per sampling context -- the context identity plus
// the fence value of that context's submit that sampled the slot -- and a slot
// may be recycled only when EVERY entry's fence has completed on ITS OWN
// timeline. A context with no entry is not a waiter.
//
// The record never owns a fence or a clock. The caller supplies the
// completion query (a function of (context, fence)), so the policy is
// testable with two mock timelines and no device.
//
// Not thread-safe: one record per device, driven by the thread that records
// and submits.

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    struct LastUse
    {
        const void*   context = nullptr;   // the NriGraphContext identity, not a fence from another timeline
        std::uint64_t fence   = 0;
    };

    class ARC_RENDER_API DeviceResourceLifetime
    {
    public:
        using CompletedFn = bool (*)(const void* context, std::uint64_t fence, void* user);

        // Records that `context` sampled `slotKey` in the submit that signals
        // `fence` on that context's timeline. A second note from the same
        // context raises its fence (fences are nondecreasing per timeline).
        void NoteSample(std::uint64_t slotKey, const void* context, std::uint64_t fence);

        // True when every NoteSample for slotKey has a completed fence.
        // A context with no note is not a waiter, and a key with no notes is
        // retired.
        [[nodiscard]] bool AllRetired(std::uint64_t slotKey, CompletedFn completed, void* user) const;

        // Forgets every note for slotKey (the slot was recycled).
        void Clear(std::uint64_t slotKey);

        // Forgets every note made by `context` (the context is going away; its
        // timeline will never be queried again).
        void ForgetContext(const void* context);

        // The notes for slotKey, for diagnostics and tests.
        [[nodiscard]] const std::vector<LastUse>* Uses(std::uint64_t slotKey) const;

    private:
        std::unordered_map<std::uint64_t, std::vector<LastUse>> m_uses;
    };
}
