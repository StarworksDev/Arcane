#pragma once

#include <imgui.h>

// Injecting a key the way a USER on this platform presses it. With
// io.ConfigMacOSXBehaviors (ImGui's default on __APPLE__) AddKeyEvent swaps
// Ctrl and Super, because a Mac keyboard's Cmd arrives as Super and is meant
// to act as Ctrl: an injected ImGuiMod_Ctrl would become Super and miss every
// Ctrl+X shortcut. A test that means "the shortcut modifier" injects Super
// there instead, so ImGui sees the same Ctrl a Cmd press produces.
namespace Arcane::TestKeys
{
    inline void AddKeyEvent(ImGuiIO& io, ImGuiKey key, bool down)
    {
        if (io.ConfigMacOSXBehaviors)
        {
            if (key == ImGuiMod_Ctrl)          key = ImGuiMod_Super;
            else if (key == ImGuiKey_LeftCtrl)  key = ImGuiKey_LeftSuper;
            else if (key == ImGuiKey_RightCtrl) key = ImGuiKey_RightSuper;
        }
        io.AddKeyEvent(key, down);
    }
}
