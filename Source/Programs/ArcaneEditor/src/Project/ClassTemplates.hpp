#pragma once

// ClassTemplates: the PURE text half of Assets -> Create -> C++ Class (the
// third step of the editor<->IDE surface; Unreal's GameProjectUtils::
// AddCodeToProject in miniature). Given a template kind, a class name and the
// project's name, produce the header (and, for the kinds that have one, the
// source) as strings. Nothing here touches a filesystem, ImGui, or the
// engine -- EditorApp::MintCppClass writes the files, registers them under
// source://, regenerates the solution and opens the .cpp in Visual Studio.
//
// The three kinds and what they emit:
//   Component  -- a reflected struct (ASTRA_REFLECT_TYPE/FIELD, the shape
//                 Components.hpp and HotReloadShared.hpp use) in the header,
//                 and a .cpp carrying the ONE ARC_COMPONENT(ns::T) line
//                 (Arcane/Plugin/GameComponents.hpp), so the type is live
//                 after one Rebuild Game Module with no edit to Init.
//   System     -- an Astra::SystemTraits functor plus a .cpp containing its
//                 ARC_SYSTEM declaration. The selected phase and role are
//                 explicit; semantic order stays in Before/After traits.
//   PlainClass -- a class in the project namespace with its own .cpp.
//
// Templates are embedded here rather than shipped as data files (UE's
// Engine/Content/Editor/Templates/*.template): three short texts, testable
// as strings, and nothing yet asks to edit them outside the engine.

#include <Arcane/Plugin/SystemFactory.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <Arcane/Core/Constant.hpp>

namespace Arcane::Editor::ClassTemplates
{
    enum class Kind : int { Component = 0, System, PlainClass, Count };

    // Fixed Update / Update. The Render phase is retired (F5 plan 1 Task 5:
    // no host runs it), so the wizard does not offer it.
    ARC_CONSTANT("enum and array arity: one per system phase choice")
    inline constexpr int kSystemPhaseChoiceCount = 2;
    ARC_CONSTANT("enum and array arity: one per system role choice")
    inline constexpr int kSystemRoleChoiceCount  = 3;

    struct SystemOptions
    {
        Arcane::SystemPhase phase = Arcane::SystemPhase::FixedUpdate;
        Arcane::RoleMask    role  = Arcane::RoleMask::Both;
    };

    [[nodiscard]] const char* KindLabel(Kind kind);   // "Component" / "System" / "Plain class"
    [[nodiscard]] const char* SystemPhaseChoiceLabel(int index) noexcept;
    [[nodiscard]] const char* SystemRoleChoiceLabel(int index) noexcept;
    [[nodiscard]] SystemOptions SystemOptionsForChoiceIndices(
        int phaseIndex, int roleIndex) noexcept;

    // nullopt when `name` is a usable C++ identifier for a type; otherwise the
    // reason, in the words the dialog shows. Refuses: empty, a leading digit,
    // any character outside [A-Za-z0-9_], and C++ keywords. Qualification
    // ("A::B") is refused too -- the project namespace is applied by Render.
    [[nodiscard]] std::optional<std::string> ValidateClassName(std::string_view name);

    // The manifest name is free text ("My Game"); the namespace it becomes must
    // be an identifier: every other character -> '_', a leading digit gets a
    // '_' prefix, and an empty result falls back to "Game".
    [[nodiscard]] std::string NamespaceForProject(std::string_view projectName);

    struct Rendered
    {
        std::string headerName;   // "<Class>.hpp"
        std::string header;
        std::string sourceName;   // "<Class>.cpp", or empty for a header-only kind
        std::string source;       // empty for a header-only kind
    };

    // Render `kind` for `className` (already validated) in `projectName`'s
    // namespace. Every text ends in a newline and carries no template token.
    [[nodiscard]] Rendered Render(Kind kind,
                                  std::string_view className,
                                  std::string_view projectName,
                                  SystemOptions options = {});

    // ---- the game descriptor (engine modularization s4) -----------------------
    // A C++ Class makes a project a code project, so the wizard also writes the
    // game module's `<sourceDir>/<Module>.arcmodule` -- the engine modules the
    // game links -- when the project has none yet. It is the DEFAULT set (what
    // a descriptor-less game links anyway, Arcane/Project/GameDescriptor.hpp),
    // so writing it changes no build; an existing descriptor is never touched.
    struct DescriptorFile
    {
        std::string fileName;   // "<Module>.arcmodule"
        std::string text;       // JSON, type Game, ends in a newline
    };
    [[nodiscard]] DescriptorFile RenderGameDescriptor(std::string_view moduleName);

    // The game module's name: the manifest's gameModule stem
    // ("ReferenceGame.dll" -> "ReferenceGame"), else the project name -- the
    // name build/arcane.lua's arcane_game_module(<name>) looks the descriptor up by.
    [[nodiscard]] std::string GameModuleNameFor(std::string_view gameModule, std::string_view projectName);
}
