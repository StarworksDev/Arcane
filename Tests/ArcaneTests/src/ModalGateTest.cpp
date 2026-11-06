// Dockspace modal gate ([editor][modal]): at most one dockspace-level modal draws
// per frame, errors first. CPU-only.
#include <catch2/catch_test_macros.hpp>

#include "App/ModalGate.hpp"
#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <sstream>
#include <string>

TEST_CASE("errors beat unsaved, unsaved beats create, none is none", "[editor][modal]")
{
    using Arcane::Editor::ModalAllowed;
    using Arcane::Editor::ModalKind;
    CHECK(ModalAllowed(true, true, true, true) == ModalKind::Error);
    CHECK(ModalAllowed(true, false, true, false) == ModalKind::Error);
    CHECK(ModalAllowed(false, true, true, true) == ModalKind::UnsavedScene);
    CHECK(ModalAllowed(false, false, false, true) == ModalKind::CreateAsset);
    CHECK(ModalAllowed(false, false, false, false) == ModalKind::None);
}

TEST_CASE("the document unsaved-changes modal sits behind errors and scene intents, ahead of create", "[editor][modal]")
{
    using Arcane::Editor::ModalAllowed;
    using Arcane::Editor::ModalKind;
    // (errorQueued, sceneIntentParked, documentUnsaved, createOpen)
    CHECK(ModalAllowed(true, false, true, false) == ModalKind::Error);
    CHECK(ModalAllowed(false, true, true, false) == ModalKind::UnsavedScene);
    CHECK(ModalAllowed(false, false, true, true) == ModalKind::UnsavedDocument);
    CHECK(ModalAllowed(false, false, true, false) == ModalKind::UnsavedDocument);
}

namespace
{
    std::string ReadSrc(const char* rel)
    {
        std::ifstream in(Arcane::Test::RepoRoot() / rel);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
}

TEST_CASE("DocumentHost draws its unsaved modal only when the frame gate allows it", "[editor][modal]")
{
    const std::string host = ReadSrc("Source/Programs/ArcaneEditor/src/Documents/DocumentHost.cpp");
    const std::size_t fn = host.find("void DocumentHost::DrawAll(");
    REQUIRE(fn != std::string::npos);
    const std::size_t guard = host.find("modalBlocked", fn);
    REQUIRE(guard != std::string::npos);
    const std::size_t open = host.find("ImGui::OpenPopup(\"Unsaved Changes##dochost\")", fn);
    REQUIRE(open != std::string::npos);
    CHECK(guard < open);

    const std::string frame = ReadSrc("Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    CHECK(frame.find("m_documents.DrawAll(m_viewportDockId,") != std::string::npos);
    CHECK(frame.find("ModalKind::UnsavedDocument") != std::string::npos);
}

TEST_CASE("the toolbar Stop reports a refused Stop through the shared failure modal", "[editor][modal][play]")
{
    const std::string panels = ReadSrc("Source/Programs/ArcaneEditor/src/Panels/EditorPanels.cpp");
    CHECK(panels.find("result.stopFailed") != std::string::npos);
    CHECK(panels.find("                play.Stop(runtime, host);") == std::string::npos);

    const std::string frame = ReadSrc("Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    const std::size_t use = frame.find("toolbar.stopFailed");
    REQUIRE(use != std::string::npos);
    const std::size_t push = frame.find("PushPlayStopFailedModal()", use);
    REQUIRE(push != std::string::npos);
    CHECK(push - use < 200);
}

TEST_CASE("DrawModals opens the unsaved popup only behind the gate", "[editor][modal]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();

    const std::size_t fn = src.find("void EditorApp::DrawModals(");
    REQUIRE(fn != std::string::npos);
    const std::size_t gate = src.find("ModalAllowed(", fn);
    REQUIRE(gate != std::string::npos);
    const std::size_t open = src.find("ImGui::OpenPopup(\"Unsaved Scene\")", fn);
    REQUIRE(open != std::string::npos);
    CHECK(open > gate);
    const std::size_t branch = src.find("allowed == Arcane::Editor::ModalKind::UnsavedScene", fn);
    REQUIRE(branch != std::string::npos);
    CHECK(branch < open);
    CHECK(src.find("editor.modal.probe") != std::string::npos);
}
