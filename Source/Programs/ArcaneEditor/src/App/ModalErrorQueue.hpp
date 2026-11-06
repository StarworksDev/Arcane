#pragma once
// ModalErrorQueue (architecture pass sec 7): pure error-modal state. The
// three parallel error strings (project/scene/launch) and their five copied
// ~16-line popup blocks become one queue + ONE drawing block in DrawModals.
// Errors display sequentially (FIFO) instead of racing for the popup stack.
// Pure state -- drawing stays host-side, same split as ConsoleBuffer.
#include <deque>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    struct ModalError
    {
        std::string title;     // popup title -- stays honest about which action failed
        std::string message;
    };

    class ModalErrorQueue
    {
    public:
        void Push(std::string title, std::string message)
        {
            m_queue.push_back({ std::move(title), std::move(message) });
        }
        [[nodiscard]] const ModalError* Front() const
        {
            return m_queue.empty() ? nullptr : &m_queue.front();
        }
        void Pop()   { if (!m_queue.empty()) m_queue.pop_front(); }
        void Clear() { m_queue.clear(); }

    private:
        std::deque<ModalError> m_queue;
    };

    // Servitor's "modal" host state (spec s4.3; final fix round 2, I-2): an
    // error dialog is queued or a modal popup is open, AND someone can answer
    // it -- only an editor with an interactive window. A headless editor's
    // queued dialogs (a module that failed to load, a --project that did not
    // open) are unattended: they stay log and Problems entries, and the host
    // reads ready / playing / paused by its real state.
    [[nodiscard]] inline bool ReportsModal(const ModalErrorQueue& queue, bool interactiveWindow, bool popupModalOpen)
    {
        return interactiveWindow && (queue.Front() != nullptr || popupModalOpen);
    }
}
