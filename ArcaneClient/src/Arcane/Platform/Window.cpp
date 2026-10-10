#include <Arcane/Platform/Window.hpp>

#include <Arcane/Base/Diagnostics.hpp>   // RequestCleanExit -- the session-end route (crash window plan 1, task 9)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Platform.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#if ARC_PLATFORM_MACOS
#include <SDL3/SDL_metal.h>
#endif

#include <stb_image.h>   // decode the icon file; implementation lives in Assets/StbImpl.cpp (same DLL)

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#if defined(_WIN32)
// The session-end hook below reads msg->message, and SDL3/SDL_system.h only
// FORWARD-declares MSG (`typedef struct tagMSG MSG;`) -- so the real header
// has to come first.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <SDL3/SDL_system.h>   // SDL_SetWindowsMessageHook (SDL 3.2.0+)
#endif

namespace Arcane
{
    namespace
    {
        struct FolderCbCtx { Arcane::Window::FolderPickedCallback cb; void* user; };

        // SDL hands us the full result list; the editor wants only the first folder
        // (allow_many=false), or nullptr on cancel/error. Invoked exactly once -> we
        // own and free the heap ctx here.
        void SDLCALL FolderDialogTrampoline(void* userdata, const char* const* filelist, int /*filter*/)
        {
            std::unique_ptr<FolderCbCtx> ctx(static_cast<FolderCbCtx*>(userdata));
            const char* picked = (filelist && filelist[0]) ? filelist[0] : nullptr;
            if (ctx->cb) ctx->cb(picked, ctx->user);
        }

#if defined(_WIN32)
        // THE END-SESSION ROUTE (crash window plan 1, task 9; spec S5.7).
        // Logging off or shutting down Windows does NOT send WM_CLOSE, so the
        // ordinary quit path this host already has is never entered: the
        // process is simply killed once the session-end budget runs out, which
        // is how an editor with unsaved work loses it silently.
        //
        // SDL_SetWindowsMessageHook runs before TranslateMessage for EVERY
        // message on this thread, and its return value means "let the message
        // continue to be processed" -- it is NOT the window procedure's return
        // value, so returning true here still lets DefWindowProc answer
        // WM_QUERYENDSESSION with TRUE (we consent to the shutdown; refusing
        // it is a different feature and not one we want).
        //
        // WM_ENDSESSION with wParam == TRUE is the commitment: the session IS
        // ending. RequestCleanExit arms the exit deadline and runs the host's
        // hook, which begins the ordinary exit (and, from plan 3, autosaves
        // first). wParam == FALSE means an earlier query was vetoed and
        // nothing is ending -- ignoring that case is the whole reason the
        // wParam check exists.
        bool SDLCALL SessionEndHook(void* /*userdata*/, MSG* msg)
        {
            if (msg)
            {
                if (msg->message == WM_ENDSESSION && msg->wParam == TRUE)
                    Arcane::Diagnostics::RequestCleanExit();
            }
            return true;   // never swallow a message
        }

        // ONCE PER PROCESS, not per window: SDL's hook slot is process-wide
        // (last writer wins), and a host that opens a second window -- or
        // recreates its window on a project switch -- must not end up
        // re-registering the same callback and paying for the SDL_GetError
        // round trip again. Both windowed hosts share this Window type, which
        // is why the registration lives here rather than in either host.
        std::atomic<bool> g_sessionEndHookInstalled{ false };
#endif
    }

    bool Window::Create(const WindowDesc& desc)
    {
        if (m_window)
        {
            ARC_WARN("Window::Create called on an already-created window");
            return false;
        }

        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            ARC_ERROR("SDL_InitSubSystem(VIDEO) failed: {}", SDL_GetError());
            return false;
        }

        SDL_WindowFlags flags = 0;
        if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
        if (desc.hidden)    flags |= SDL_WINDOW_HIDDEN;
#if ARC_PLATFORM_MACOS
        // macOS: the Vulkan backend (MoltenVK) presents to a CAMetalLayer that
        // NRI wraps with VK_EXT_metal_surface (NativeHandle below), so the
        // window is a METAL window. SDL_WINDOW_VULKAN would make SDL load its
        // own Vulkan library at creation -- a second loader path nothing uses.
        // Only Cocoa has Metal views: SDL refuses SDL_WINDOW_METAL on the
        // "offscreen" driver (the test lanes), and with NO graphics flag SDL
        // 3.2 on macOS defaults the window to OpenGL, which offscreen cannot
        // load. So every other macOS window is created with an external
        // graphics context: SDL attaches no API, and NativeHandle has no layer
        // to hand out there anyway.
        const char* driver = SDL_GetCurrentVideoDriver();
        const bool cocoa = driver && SDL_strcmp(driver, "cocoa") == 0;
        if (desc.vulkan && cocoa) flags |= SDL_WINDOW_METAL;
        const SDL_PropertiesID createProps = SDL_CreateProperties();
        SDL_SetStringProperty(createProps, SDL_PROP_WINDOW_CREATE_TITLE_STRING, desc.title.c_str());
        SDL_SetNumberProperty(createProps, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, (Sint64)desc.width);
        SDL_SetNumberProperty(createProps, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, (Sint64)desc.height);
        SDL_SetNumberProperty(createProps, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (Sint64)flags);
        if (!(flags & SDL_WINDOW_METAL))
            SDL_SetBooleanProperty(createProps, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
        m_window = SDL_CreateWindowWithProperties(createProps);
        SDL_DestroyProperties(createProps);
#else
        if (desc.vulkan)    flags |= SDL_WINDOW_VULKAN;

        m_window = SDL_CreateWindow(desc.title.c_str(),
                                    (int)desc.width, (int)desc.height, flags);
#endif
        if (!m_window)
        {
            ARC_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }

#if defined(_WIN32)
        // Right after the window exists, and exactly once per process -- see
        // SessionEndHook above for what it is for and why the registration is
        // shared rather than per host.
        if (!g_sessionEndHookInstalled.exchange(true, std::memory_order_acq_rel))
            SDL_SetWindowsMessageHook(&SessionEndHook, nullptr);
#endif

        ARC_INFO("Window created: '{}' {}x{}", desc.title, desc.width, desc.height);
        return true;
    }

    void Window::Destroy()
    {
        if (m_window)
        {
#if ARC_PLATFORM_MACOS
            if (m_metalView)
            {
                SDL_Metal_DestroyView(m_metalView);
                m_metalView = nullptr;
            }
#endif
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    Window::~Window()
    {
        Destroy();
    }

    WindowEvents Window::PumpEvents()
    {
        WindowEvents events;
        if (!m_window) return events;

        const SDL_WindowID myId = SDL_GetWindowID(m_window);
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            // Engine-internal modules (ImGui) see every event first, before
            // Window's own quit/resize handling.
            if (m_tap)
                m_tap(&e, m_tapUser);

            switch (e.type)
            {
            case SDL_EVENT_QUIT:
                // Name the source. SDL_EVENT_QUIT is GLOBAL and unfiltered --
                // anything in the process that reaches WM_QUIT on this thread
                // (a plugin, a native modal, SDL tearing down its last window)
                // lands here and reads as "the user closed the editor". When a
                // quit is spurious, distinguishing it from a real window close
                // is the whole diagnosis, so log which one it was.
                ARC_INFO("Window: SDL_EVENT_QUIT (process-wide, not this window's close button)");
                events.quitRequested = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (e.window.windowID == myId)
                {
                    ARC_INFO("Window: close requested for this window (id {})", myId);
                    events.quitRequested = true;
                }
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (e.window.windowID == myId)
                {
                    events.resized = true;
                    events.width   = (uint32_t)e.window.data1;
                    events.height  = (uint32_t)e.window.data2;
                }
                break;
            default:
                break;
            }
        }
        return events;
    }

    void Window::SetTitle(const std::string& title)
    {
        if (!m_window) return;
        SDL_SetWindowTitle(m_window, title.c_str());
    }

    float Window::DisplayScale() const
    {
        if (!m_window)
            return 1.0f;
        const float s = SDL_GetWindowDisplayScale(m_window);
        return s > 0.0f ? s : 1.0f;
    }

    void Window::SetSize(uint32_t width, uint32_t height)
    {
        if (!m_window)
            return;
        SDL_SetWindowSize(m_window, (int)width, (int)height);
    }

    void Window::Show(bool activate)
    {
        if (!m_window) return;
        if (!activate)
        {
            // The automation reveal (T3-D6 fix round 1): shown, never
            // activated, never raised. SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN=0
            // makes SDL_ShowWindow map it without activation (SW_SHOWNA on
            // Windows); the hint is put back at once so no later window (a
            // multi-viewport ImGui platform window, the next interactive
            // Show) inherits it. A window that is merely mapped still
            // presents, so the windowed self-capture sees the real frame.
            SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
            SDL_ShowWindow(m_window);
            SDL_ResetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN);
            return;
        }
        SDL_ShowWindow(m_window);
        // Raise, not just show (2026-07-30 desk-check, Task 8d defect B): a
        // bare SDL_ShowWindow left the editor opening BEHIND the existing
        // window stack. Both callers reveal this window one statement before
        // destroying the pre-device splash, and that splash is
        // WS_EX_APPWINDOW | WS_EX_TOPMOST and holds the foreground
        // (BootSplashWindow.cpp's ShowWindow(SW_SHOW) activates it) -- so
        // without this the editor is merely un-hidden UNDER a topmost window,
        // and when the splash is then destroyed Windows hands the foreground
        // to whatever is next in the Z-order, which is some other app.
        //
        // ORDERING, and why it is this way round rather than raising after
        // the splash closes: this runs while our own process still owns the
        // foreground window. Windows' foreground lock permits a process that
        // owns the current foreground window to set it, so the raise is
        // allowed here and reliably succeeds. Do it AFTER the splash is gone
        // instead and the foreground has already been handed to another
        // process -- the very defect -- at which point SetForegroundWindow is
        // exactly what the lock exists to refuse, and Windows flashes the
        // taskbar button instead of surfacing us. Raising first also means
        // the foreground is never briefly parked on a destroyed window.
        //
        // SDL_RaiseWindow, not ::SetForegroundWindow: the portable call is
        // documented as "raise above other windows and gain the input focus",
        // it is what SDL's own multi-viewport backend uses
        // (imgui_impl_sdl3.cpp:1226), and on Windows it lands on
        // SetForegroundWindow/SetFocus anyway. Project.cpp's
        // FocusWindowOfProcess keeps the raw Win32 route because it targets
        // ANOTHER process's HWND, which SDL cannot express -- a different
        // problem, not a precedent for this one.
        SDL_RaiseWindow(m_window);
    }

    bool Window::SetIcon(const std::filesystem::path& path)
    {
        if (!m_window)
            return false;

        // Resolve relative paths against the exe dir (not the CWD) so the icon loads
        // regardless of where the editor is launched from -- matching Assets/ShaderPaths
        // and the editor's exe-relative font/data layout. SDL_GetBasePath() is SDL-owned.
        std::filesystem::path resolved = path;
        if (resolved.is_relative())
            if (const char* base = SDL_GetBasePath())
                resolved = std::filesystem::path(base) / path;

        int w = 0, h = 0, comp = 0;
        unsigned char* pixels = stbi_load(resolved.string().c_str(), &w, &h, &comp, 4);
        if (!pixels || w <= 0 || h <= 0)
        {
            ARC_WARN("Window::SetIcon: failed to load '{}'", resolved.string());
            if (pixels) stbi_image_free(pixels);
            return false;
        }

        // SDL_PIXELFORMAT_RGBA32 is byte-order R,G,B,A -- exactly stb's 4-channel layout.
        // SDL_SetWindowIcon copies the surface into the window, so the surface and its
        // backing pixels are ours to free immediately after.
        SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32,
                                                     pixels, w * 4);
        bool ok = false;
        if (surface)
        {
            ok = SDL_SetWindowIcon(m_window, surface);
            SDL_DestroySurface(surface);
        }
        else
        {
            ARC_WARN("Window::SetIcon: SDL_CreateSurfaceFrom failed: {}", SDL_GetError());
        }
        stbi_image_free(pixels);
        return ok;
    }

    void Window::SetNativeEventTap(NativeEventTap tap, void* user)
    {
        m_tap     = tap;
        m_tapUser = user;
    }

    void Window::ShowOpenFolderDialog(FolderPickedCallback cb, void* user) const
    {
        auto* ctx = new FolderCbCtx{ cb, user };   // freed by the trampoline
        SDL_ShowOpenFolderDialog(&FolderDialogTrampoline, ctx, m_window, nullptr, false);
    }

    namespace
    {
        // File-dialog context: owns the filter STRINGS too -- SDL requires the
        // filter array to stay valid until the callback fires, so everything
        // lives on the heap with the ctx and is freed by the trampoline.
        struct FileCbCtx
        {
            Window::FilePickedCallback cb;
            void* user;
            std::string filterName, filterPattern;
            std::string defaultPath;   // owned here too: outlive the async dialog
            SDL_DialogFileFilter filter{};
        };

        void SDLCALL FileDialogTrampoline(void* userdata, const char* const* filelist, int /*filter*/)
        {
            std::unique_ptr<FileCbCtx> ctx(static_cast<FileCbCtx*>(userdata));
            const char* picked = (filelist && filelist[0]) ? filelist[0] : nullptr;
            if (ctx->cb) ctx->cb(picked, ctx->user);
        }

        FileCbCtx* MakeFileCtx(Window::FilePickedCallback cb, void* user,
                               const char* filterName, const char* filterPattern,
                               const char* defaultPath)
        {
            auto* ctx = new FileCbCtx{ cb, user,
                                       filterName ? filterName : "",
                                       filterPattern ? filterPattern : "",
                                       defaultPath ? defaultPath : "" };
            ctx->filter.name = ctx->filterName.c_str();
            ctx->filter.pattern = ctx->filterPattern.c_str();
            return ctx;
        }
    }

    void Window::ShowSaveFileDialog(FilePickedCallback cb, void* user,
                                    const char* filterName, const char* filterPattern,
                                    const char* defaultPath) const
    {
        FileCbCtx* ctx = MakeFileCtx(cb, user, filterName, filterPattern, defaultPath);   // freed by the trampoline
        const bool hasFilter = filterName && filterPattern;
        SDL_ShowSaveFileDialog(&FileDialogTrampoline, ctx, m_window,
                               hasFilter ? &ctx->filter : nullptr, hasFilter ? 1 : 0,
                               ctx->defaultPath.empty() ? nullptr : ctx->defaultPath.c_str());
    }

    void Window::ShowOpenFileDialog(FilePickedCallback cb, void* user,
                                    const char* filterName, const char* filterPattern,
                                    const char* defaultPath) const
    {
        FileCbCtx* ctx = MakeFileCtx(cb, user, filterName, filterPattern, defaultPath);   // freed by the trampoline
        const bool hasFilter = filterName && filterPattern;
        SDL_ShowOpenFileDialog(&FileDialogTrampoline, ctx, m_window,
                               hasFilter ? &ctx->filter : nullptr, hasFilter ? 1 : 0,
                               ctx->defaultPath.empty() ? nullptr : ctx->defaultPath.c_str(),
                               /*allow_many=*/false);
    }

    void Window::GetPixelSize(uint32_t& width, uint32_t& height) const
    {
        if (!m_window) { width = 0; height = 0; return; }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(m_window, &w, &h);
        width  = (uint32_t)w;
        height = (uint32_t)h;
    }

    bool Window::IsMinimized() const
    {
        if (!m_window) return false;
        return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0;
    }

    bool Window::IsFocused() const
    {
        if (!m_window) return false;
        return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    }

    void* Window::NativeHandle() const
    {
        if (!m_window) return nullptr;
#if ARC_PLATFORM_WINDOWS
        return SDL_GetPointerProperty(SDL_GetWindowProperties(m_window),
                                      SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif ARC_PLATFORM_MACOS
        // macOS port: the CAMetalLayer of a Metal view SDL attaches to the
        // NSWindow, created on first ask and owned by this Window. The
        // "offscreen" driver (no Cocoa window) has none: nullptr, as on Linux.
        if (!m_metalView)
        {
            if (!SDL_GetPointerProperty(SDL_GetWindowProperties(m_window),
                                        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr))
                return nullptr;
            m_metalView = SDL_Metal_CreateView(m_window);
            if (!m_metalView)
            {
                ARC_ERROR("SDL_Metal_CreateView failed: {}", SDL_GetError());
                return nullptr;
            }
        }
        return SDL_Metal_GetLayer(m_metalView);
#else
        // Linux port: whichever window system SDL's video driver is on. An X11
        // Window is an integer XID, carried pointer-sized; the "offscreen"
        // driver has neither, which reads as "no native handle" (nullptr).
        const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
        if (void* surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr))
            return surface;
        const Sint64 xid = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(xid));
#endif
    }

    void* Window::NativeDisplay() const
    {
#if ARC_PLATFORM_WINDOWS || ARC_PLATFORM_MACOS
        return nullptr;
#else
        if (!m_window) return nullptr;
        const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
        if (void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr))
            return display;
        return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
#endif
    }

    bool Window::IsWaylandWindow() const
    {
#if ARC_PLATFORM_WINDOWS || ARC_PLATFORM_MACOS
        return false;
#else
        return m_window && SDL_GetPointerProperty(SDL_GetWindowProperties(m_window),
                                                  SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr) != nullptr;
#endif
    }
}
