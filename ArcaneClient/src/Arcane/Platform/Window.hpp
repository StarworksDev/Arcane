#pragma once

// Platform module: SDL3 window + event pump. Quit/resize surface only;
// input lives in Arcane/Input (snapshot-driven action system).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Platform/RenderWindowSettings.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

struct SDL_Window;

namespace Arcane
{
    struct WindowDesc
    {
        std::string title = "Arcane";
        // render.window.*'s defaults. The host window's live extent comes from
        // the published settings (GpuContext); RenderWindowSettings.hpp is the
        // plain struct, free of the settings headers NRI cannot follow.
        uint32_t width    = RenderWindowSettings{}.width;
        uint32_t height   = RenderWindowSettings{}.height;
        bool resizable    = RenderWindowSettings{}.resizable;
        bool hidden       = false;  // tests create hidden windows
        bool vulkan       = false;  // set true for Vulkan windows (SDL_WINDOW_VULKAN)
    };

    struct WindowEvents
    {
        bool quitRequested = false;  // SDL_EVENT_QUIT or window close (ESC is the host's business via InputActions)
        bool resized       = false;  // pixel size changed since last pump
        uint32_t width     = 0;      // valid when resized
        uint32_t height    = 0;
    };

    class ARC_API Window
    {
    public:
        Window() = default;
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        bool Create(const WindowDesc& desc);
        void Destroy();

        WindowEvents PumpEvents();

        void SetTitle(const std::string& title);
        [[nodiscard]] float DisplayScale() const;   // the monitor's content scale (1.0 without a window)
        void SetSize(uint32_t width, uint32_t height);

        // Un-hide a window created with WindowDesc::hidden. Hosts create hidden
        // and show at the first presented frame, so a window never exists in an
        // undrawn state. Idempotent.
        //
        // Also RAISES the window to the foreground (Task 8d defect B) -- this
        // is the launch reveal, and a newly launched app belongs on top. Folded
        // in here rather than offered as a separate Raise() precisely so a host
        // cannot reveal its window and forget: both callers (EditorApp::
        // StageSplashReady, RuntimeApp::StageFinalize) reveal one statement
        // before destroying the foreground-holding splash, and the raise has to
        // happen in that order. See Window.cpp for the ordering argument.
        //
        // activate = false (T3-D6 fix round 1): map the window WITHOUT taking
        // the foreground and without the raise -- an automation run's reveal
        // (Editor::HostPresentationFor). A person may be working at the desk
        // while a scripted windowed capture runs; it must never steal focus.
        void Show(bool activate = true);

        // Set the OS window icon (title bar + taskbar) from an image file. Opt-in per
        // host: only a host that calls this gets a custom icon -- others keep the SDL
        // default. Decodes to RGBA via stb (compiled into this DLL); SDL_SetWindowIcon
        // takes its own copy, so the surface + pixels are freed immediately. Returns
        // false if there is no window yet or the image fails to decode (logged).
        bool SetIcon(const std::filesystem::path& path);

        // Native event tap for engine-internal modules (ImGui): invoked
        // once per SDL event during PumpEvents, BEFORE Window's own
        // handling. `event` is a const SDL_Event* passed opaquely so SDL
        // types stay out of public headers. One tap; null uninstalls.
        using NativeEventTap = void (*)(const void* event, void* user);
        void SetNativeEventTap(NativeEventTap tap, void* user);

        // Observability for the one-flavor invariant: the tap is the only route
        // by which mouse BUTTON events reach ImGui, so "is it installed" is a
        // property worth asserting rather than trusting.
        [[nodiscard]] bool HasNativeEventTap() const noexcept { return m_tap != nullptr; }

        void GetPixelSize(uint32_t& width, uint32_t& height) const;
        bool IsMinimized() const;
        bool IsFocused() const;                           // holds keyboard input focus (false for a hidden window)

        void* NativeHandle() const;                       // HWND on Windows; wl_surface* or the X11 Window id (as a pointer-sized value) on Linux; CAMetalLayer* on macOS
        // Linux port: the native DISPLAY a swapchain needs alongside the handle --
        // X11 Display* or wl_display*; nullptr on Windows and on display-less
        // video drivers (SDL's "offscreen"). IsWaylandWindow says which pair it is.
        void* NativeDisplay() const;
        bool  IsWaylandWindow() const;
        SDL_Window* SdlWindow() const { return m_window; }

        // Native folder-picker (editor "Open Project"). Async: SDL surfaces the result
        // during a later PumpEvents by invoking `cb` on the main thread with the chosen
        // folder path, or nullptr on cancel/error. `cb`/SDL types stay out of this
        // header (same opaque pattern as NativeEventTap). Uses THIS window's SDL
        // instance -- the only one that owns the video subsystem.
        using FolderPickedCallback = void (*)(const char* path, void* user);
        void ShowOpenFolderDialog(FolderPickedCallback cb, void* user) const;

        // Native save/open FILE dialogs (same async trampoline contract as the
        // folder picker: `cb` fires on the SDL dialog thread with the chosen
        // path, or nullptr on cancel/error). One optional filter --
        // `filterName` ("Arcane Material") + `filterPattern` ("arcmat",
        // semicolon-separated for multiple); pass null/null for all files.
        // `defaultPath` seeds the dialog's starting location/filename (null =
        // OS default). The save dialog does NOT force the extension -- callers
        // append it when missing.
        using FilePickedCallback = void (*)(const char* path, void* user);
        void ShowSaveFileDialog(FilePickedCallback cb, void* user,
                                const char* filterName, const char* filterPattern,
                                const char* defaultPath = nullptr) const;
        void ShowOpenFileDialog(FilePickedCallback cb, void* user,
                                const char* filterName, const char* filterPattern,
                                const char* defaultPath = nullptr) const;

    private:
        SDL_Window*    m_window  = nullptr;
        // macOS: the SDL_MetalView whose CAMetalLayer NativeHandle() returns
        // (created on first ask; void* is SDL_MetalView's own spelling).
        mutable void*  m_metalView = nullptr;
        NativeEventTap m_tap     = nullptr;
        void*          m_tapUser = nullptr;
    };
}
