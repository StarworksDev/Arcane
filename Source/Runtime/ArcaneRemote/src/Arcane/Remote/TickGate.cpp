#include <Arcane/Remote/TickGate.hpp>

#if !defined(ARC_BUILD_DIST)

#include <algorithm>

namespace Arcane::Remote
{

void TickGate::SetRunning(bool running)
{
    std::lock_guard lock(m_mutex);
    m_running = running;
}

void TickGate::SetPaused(bool paused)
{
    std::lock_guard lock(m_mutex);
    m_paused = paused;
}

void TickGate::RequestSteps(std::uint32_t n)
{
    std::lock_guard lock(m_mutex);
    m_steps = std::max(m_steps, n);   // one budget: overlapping requests share the ticks (G-1)
}

void TickGate::TrimSteps(std::uint32_t keep)
{
    std::lock_guard lock(m_mutex);
    m_steps = std::min(m_steps, keep);
}

void TickGate::ClearSteps()
{
    std::lock_guard lock(m_mutex);
    m_steps = 0;
}

TickDecision TickGate::Consume()
{
    std::lock_guard lock(m_mutex);
    if (m_running && !m_paused) {
        m_steps = 0; // a free run makes any pending single steps moot
        return TickDecision::Run;
    }
    if (m_steps > 0) {
        --m_steps;
        return TickDecision::Step;
    }
    return TickDecision::Idle;
}

bool TickGate::Running() const
{
    std::lock_guard lock(m_mutex);
    return m_running;
}

bool TickGate::Paused() const
{
    std::lock_guard lock(m_mutex);
    return m_paused;
}

bool TickGate::Held() const
{
    std::lock_guard lock(m_mutex);
    return !m_running || m_paused;
}

std::uint32_t TickGate::StepsRemaining() const
{
    std::lock_guard lock(m_mutex);
    return m_steps;
}

std::string HeldWindowTitle(std::string_view base, bool held)
{
    std::string title(base);
    if (held)
        title += " [held]";
    return title;
}

} // namespace Arcane::Remote

#endif
