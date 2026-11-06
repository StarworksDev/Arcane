#include <Arcane/Render/RenderErrorLatch.hpp>

namespace Arcane
{
    RenderErrorLatch& RenderErrorLatch::Instance()
    {
        static RenderErrorLatch s_instance;
        return s_instance;
    }

    uint64_t RenderErrorCount()
    {
        return RenderErrorLatch::Instance().ErrorCount();
    }

    uint64_t RenderErrorTotal()
    {
        return RenderErrorLatch::Instance().ErrorTotal();
    }

    void ResetRenderErrorCount()
    {
        RenderErrorLatch::Instance().ResetForTest();
    }

    void NoteRenderErrorForTest(const char* tag, const char* text) noexcept
    {
        RenderErrorLatch::Instance().NoteError(tag, text);
    }

    void SetRenderDeviceRemovedHookForTest(void (*hook)()) noexcept
    {
        RenderErrorLatch::Instance().SetDeviceRemovedHook(hook);
    }

    void (*RenderDeviceRemovedHookForTest() noexcept)()
    {
        return RenderErrorLatch::Instance().CurrentDeviceRemovedHook();
    }
}
