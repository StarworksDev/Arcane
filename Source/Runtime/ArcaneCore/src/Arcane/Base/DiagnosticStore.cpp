#include <Arcane/Base/DiagnosticStore.hpp>

#include <algorithm>
#include <cctype>
#include <tuple>

namespace Arcane
{
    namespace
    {
        [[nodiscard]] bool ContainsNoCase(std::string_view haystack, std::string_view needle) noexcept
        {
            if (needle.empty())
                return true;
            if (needle.size() > haystack.size())
                return false;
            const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
            for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
            {
                std::size_t j = 0;
                while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j]))
                    ++j;
                if (j == needle.size())
                    return true;
            }
            return false;
        }

        // Error sorts first, so a HIGHER severity must compare LOWER.
        [[nodiscard]] int SeverityRank(Arcane::DiagSeverity s) noexcept
        {
            switch (s)
            {
                case Arcane::DiagSeverity::Error:   return 0;
                case Arcane::DiagSeverity::Warning: return 1;
                case Arcane::DiagSeverity::Info:    return 2;
            }
            return 3;
        }
    }

    bool MatchesDiagnosticFilter(const Arcane::Diagnostic& d, SeverityMask mask, std::string_view search) noexcept
    {
        if ((static_cast<std::uint8_t>(mask) & static_cast<std::uint8_t>(MaskOf(d.severity))) == 0)
            return false;
        return ContainsNoCase(d.message, search) || ContainsNoCase(d.code, search) || ContainsNoCase(d.detail, search);
    }

    DiagnosticStore::~DiagnosticStore()
    {
        UninstallEngineSink();
    }

    void DiagnosticStore::Publish(std::string_view key, std::span<const Arcane::Diagnostic> diags)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (diags.empty())
        {
            m_byKey.erase(std::string(key));
            return;
        }
        m_byKey[std::string(key)].assign(diags.begin(), diags.end());
    }

    void DiagnosticStore::Clear(std::string_view key)
    {
        Publish(key, {});
    }

    void DiagnosticStore::ClearAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_byKey.clear();
    }

    std::size_t DiagnosticStore::Count(Arcane::DiagSeverity severity) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::size_t n = 0;
        for (const auto& [key, list] : m_byKey)
            for (const Arcane::Diagnostic& d : list)
                if (d.severity == severity)
                    ++n;
        return n;
    }

    std::vector<Arcane::Diagnostic> DiagnosticStore::Snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Arcane::Diagnostic> out;
        for (const auto& [key, list] : m_byKey)
            out.insert(out.end(), list.begin(), list.end());
        // stable_sort: within one severity, key order then publication order is
        // the order the user watched them appear in.
        std::stable_sort(out.begin(), out.end(),
                         [](const Arcane::Diagnostic& a, const Arcane::Diagnostic& b)
                         { return SeverityRank(a.severity) < SeverityRank(b.severity); });
        return out;
    }

    std::vector<Arcane::Diagnostic> DiagnosticStore::Filtered(SeverityMask mask, std::string_view search) const
    {
        std::vector<Arcane::Diagnostic> out = Snapshot();
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const Arcane::Diagnostic& d)
                                 { return !MatchesDiagnosticFilter(d, mask, search); }),
                  out.end());
        return out;
    }

    void DiagnosticStore::SinkTrampoline(std::string_view key,
                                         std::span<const Arcane::Diagnostic> diags, void* user)
    {
        static_cast<DiagnosticStore*>(user)->Publish(key, diags);
    }

    void DiagnosticStore::InstallAsEngineSink()
    {
        Arcane::Diagnostics::SetSink(&DiagnosticStore::SinkTrampoline, this);
        m_installed = true;
    }

    void DiagnosticStore::UninstallEngineSink()
    {
        if (!m_installed)
            return;
        // ClearSinkIfCurrent, not SetSink(nullptr, nullptr): the seam's sink
        // slot is process-wide and last-writer-wins, so an unconditional clear
        // here would silently unslot a DIFFERENT store that installed after
        // this one (same stale-registration hazard as a dangling plugin
        // descriptor). If we are not the current sink, someone else already
        // is -- leave it alone. Reset m_installed either way: this call is
        // done being "installed" regardless of whose slot it finds.
        std::ignore = Arcane::Diagnostics::ClearSinkIfCurrent(&DiagnosticStore::SinkTrampoline, this);
        m_installed = false;
    }
}
