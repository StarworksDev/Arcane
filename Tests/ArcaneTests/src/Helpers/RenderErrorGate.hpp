#pragma once

// The suite's D3D12/Vulkan validation gate, split out of the Catch2 listener
// so a test can feed it counts directly.
//
// The counts are RenderErrorTotal: monotonic since process start.
// ResetRenderErrorCount clears RenderErrorCount and does not touch the total,
// so a case cannot hide a rise by resetting before the listener samples.
//
// On this desk ID3D12InfoQueue1 does not register, so a stored D3D12 message
// reaches the total only inside ~NriDevice. The listener records the total at
// testCaseStarting and compares it at testCaseEnded, which runs after that
// case's locals -- and so its device -- are destroyed. A rise in a case that
// is not tagged [expects-render-errors] is remembered. Catch2 listeners cannot
// fail a test, so main prints Summary() after session.run and returns
// ExitStatus() when it is non-zero.
//
// A rise seen at the next BeginCase or at EndRun belongs to the case that
// last ended. When that case was tagged, the rise is recorded as its late
// report and is not an offender. When it was not, the rise is an offender
// under the gap's name (or kAfterLastCase), so it is not charged to a later
// test.
//
// testRunEnded samples from ~RunContext, while Session::run is still on the
// stack. main samples once more after run() returns, after the helpers have
// released any device they still hold.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Arcane::Test
{
    class RenderErrorGate
    {
    public:
        struct Offender
        {
            std::string   name;
            std::uint64_t before = 0;
            std::uint64_t after  = 0;
        };

        // A rise observed after a tagged case had already ended, attributed
        // to that case and therefore not an offender.
        struct LateAttribution
        {
            std::string   caseName;
            std::uint64_t before = 0;
            std::uint64_t after  = 0;
        };

        // EndRun uses this name. It is not a test case; it is the sample
        // after the last case, for a device released with the run.
        static constexpr const char* kAfterLastCase = "(after the last test case)";

        void BeginRun(std::uint64_t total) { m_accounted = total; }

        void BeginCase(std::string name, bool expectsErrors, std::uint64_t total)
        {
            if (total > m_accounted)
            {
                AccountLate(total, std::string("(between test cases, before '") + name + "')");
            }
            m_inCase    = true;
            m_expects   = expectsErrors;
            m_caseName  = std::move(name);
            m_caseStart = total;
            m_accounted = total;
        }

        void EndCase(std::uint64_t total)
        {
            if (m_inCase)
            {
                if (!m_expects && total > m_caseStart)
                    Note(m_caseName, m_caseStart, total);
                m_closed        = true;
                m_closedExpects = m_expects;
                m_closedName    = m_caseName;
                m_inCase        = false;
            }
            m_accounted = total;
        }

        // Also the sample main takes after Session::run returns. A second
        // call accounts only a further rise.
        void EndRun(std::uint64_t total)
        {
            if (total > m_accounted)
                AccountLate(total, kAfterLastCase);
        }

        [[nodiscard]] int ExitStatus() const { return m_offenders.empty() ? 0 : 1; }

        [[nodiscard]] const std::vector<Offender>& Offenders() const { return m_offenders; }

        [[nodiscard]] const std::vector<LateAttribution>& LateAttributions() const
        {
            return m_late;
        }

        [[nodiscard]] std::string Summary() const
        {
            if (m_offenders.empty())
                return {};

            std::string out;
            out += "ArcaneTests: RenderErrorTotal rose where no [expects-render-errors] tag allows it.\n";
            out += "The total is monotonic. ResetRenderErrorCount does not clear it.\n";
            out += "Sampled at testCaseEnded, at testRunEnded, and once more after Session::run returns.\n";
            for (const Offender& offender : m_offenders)
            {
                out += "  ";
                out += offender.name;
                out += ": RenderErrorTotal ";
                out += std::to_string(offender.before);
                out += " -> ";
                out += std::to_string(offender.after);
                out += " (+";
                out += std::to_string(offender.after - offender.before);
                out += ")\n";
            }
            return out;
        }

    private:
        void Note(std::string name, std::uint64_t before, std::uint64_t after)
        {
            m_offenders.push_back(Offender{std::move(name), before, after});
        }

        void AccountLate(std::uint64_t total, std::string untaggedName)
        {
            if (m_closed && m_closedExpects)
            {
                m_late.push_back(LateAttribution{m_closedName, m_accounted, total});
            }
            else
            {
                Note(std::move(untaggedName), m_accounted, total);
            }
            m_accounted = total;
        }

        std::uint64_t                 m_accounted = 0;
        bool                          m_inCase    = false;
        bool                          m_expects   = false;
        std::string                   m_caseName;
        std::uint64_t                 m_caseStart = 0;
        bool                          m_closed    = false;
        bool                          m_closedExpects = false;
        std::string                   m_closedName;
        std::vector<Offender>         m_offenders;
        std::vector<LateAttribution>  m_late;
    };
}
