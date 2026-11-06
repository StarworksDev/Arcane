#pragma once

// The diagnostic state (ArcaneCore; the editor's Problems panel, the runtime and
// the server share it): key -> the CURRENT set published under it.
// See Arcane/Base/Diagnostics.hpp for the publication-group contract; this is
// the consumer side of it. Pure data with no ImGui dependency so the [diagnostics]
// units drive it headlessly -- ProblemsPanel.cpp owns all presentation.

#include <Arcane/Core/Api.hpp>
#include <Arcane/Base/Diagnostics.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    // Independent severity toggles (node-page phase s8.2): the floor model made
    // Warnings a no-op while Info was on and Errors impossible to hide.
    enum class SeverityMask : std::uint8_t { None = 0, Error = 1, Warning = 2, Info = 4, All = 7 };
    [[nodiscard]] constexpr SeverityMask operator|(SeverityMask a, SeverityMask b) noexcept
    {
        return static_cast<SeverityMask>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
    }
    [[nodiscard]] constexpr SeverityMask MaskOf(Arcane::DiagSeverity s) noexcept
    {
        switch (s)
        {
            case Arcane::DiagSeverity::Error:   return SeverityMask::Error;
            case Arcane::DiagSeverity::Warning: return SeverityMask::Warning;
            case Arcane::DiagSeverity::Info:    return SeverityMask::Info;
        }
        return SeverityMask::None;
    }
    [[nodiscard]] constexpr SeverityMask MaskFrom(bool error, bool warning, bool info) noexcept
    {
        return (error ? SeverityMask::Error : SeverityMask::None) | (warning ? SeverityMask::Warning : SeverityMask::None)
             | (info ? SeverityMask::Info : SeverityMask::None);
    }

    // True when `d`'s severity is in `mask` AND the (case-insensitive) search
    // text occurs in its message, code or detail. Empty search matches
    // everything. Free function so the panel, the store and the Console share
    // one definition of "matches".
    [[nodiscard]] ARC_CORE_API bool MatchesDiagnosticFilter(const Arcane::Diagnostic& d, SeverityMask mask,
                                               std::string_view search) noexcept;

    class ARC_CORE_API DiagnosticStore
    {
    public:
        DiagnosticStore() = default;
        ~DiagnosticStore();

        DiagnosticStore(const DiagnosticStore&)            = delete;
        DiagnosticStore& operator=(const DiagnosticStore&) = delete;

        // Replace `key`'s set. An empty span erases the key entirely.
        void Publish(std::string_view key, std::span<const Arcane::Diagnostic> diags);
        void Clear(std::string_view key);
        void ClearAll();

        [[nodiscard]] std::size_t Count(Arcane::DiagSeverity severity) const;
        // Flattened across keys, errors first, then warnings, then info. Stable
        // within a severity (key order, then publication order).
        [[nodiscard]] std::vector<Arcane::Diagnostic> Snapshot() const;
        [[nodiscard]] std::vector<Arcane::Diagnostic> Filtered(SeverityMask mask, std::string_view search) const;

        // Route Arcane::Diagnostics::Publish into THIS store. Uninstall is called
        // by the destructor too -- the sink slot outlives nothing, and a torn-down
        // store must never be dispatched into (mirrors EditorApp's grouped
        // ConsoleDiagnostics::sink erase-at-Shutdown rule). Uninstall clears the
        // seam's slot via ClearSinkIfCurrent (identity-checked), not SetSink(nullptr, nullptr):
        // the slot is process-wide last-writer-wins, so an unconditional clear
        // from a stale owner's teardown would silently disconnect whichever
        // OTHER store installed after this one.
        void InstallAsEngineSink();
        void UninstallEngineSink();

    private:
        static void SinkTrampoline(std::string_view key,
                                   std::span<const Arcane::Diagnostic> diags, void* user);

        mutable std::mutex                                        m_mutex;
        std::map<std::string, std::vector<Arcane::Diagnostic>>    m_byKey;
        bool                                                      m_installed = false;
    };
}
