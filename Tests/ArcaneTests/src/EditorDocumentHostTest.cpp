// DocumentHost (shader-editor Slice 5): the PURE document-list + unsaved-close
// confirm state machine + extension routing, driven with fake documents (no
// ImGui -- DrawAll is never called). Also pins the param-decl -> widget mapping.

#include <catch2/catch_test_macros.hpp>

#include "Documents/DocumentHost.hpp"
#include "Documents/SpriteDocument.hpp"
#include "Widgets/MaterialParamWidgets.hpp"

#include <Arcane/Sprite/SpriteAsset.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using Arcane::Editor::DocumentHost;
using Arcane::Editor::EditorDocument;

namespace
{
    struct FakeDoc final : EditorDocument
    {
        std::string title;
        Arcane::Guid guid;
        bool dirty = false;
        bool previewUploads = false;
        bool saveSucceeds = true;
        int  saveCalls = 0;
        int  reopened = 0;
        std::filesystem::path movedTo;

        FakeDoc(std::string t, Arcane::Guid g, bool d) : title(std::move(t)), guid(g), dirty(d) {}

        const std::string& Title() const override { return title; }
        Arcane::Guid AssetGuid() const override { return guid; }
        bool Dirty() const override { return dirty; }
        bool PreviewUploadsPending() const noexcept override { return previewUploads; }
        bool Save() override
        {
            ++saveCalls;
            if (saveSucceeds) dirty = false;
            return saveSucceeds;
        }
        void Draw(bool&) override {}
        void NoteReopened() override { ++reopened; }
        void NoteMoved(const std::filesystem::path& p) override { movedTo = p; }
    };
}

TEST_CASE("RequestSaveFromInspector reports the save gesture's outcome, so the app's route can warn on a refusal", "[editor][inspector]")
{
    // Integration residual 2b: the Inspector's Ctrl+S route discarded the
    // result, so a refused save (read-only file, failed write) was silent.
    FakeDoc doc("p.arcinput", Arcane::Guid::Generate(), true);
    doc.saveSucceeds = false;
    const Arcane::Editor::InspectorSaveOutcome refused = Arcane::Editor::RequestSaveFromInspector(&doc);
    CHECK(refused.doc == &doc);
    CHECK(refused.result == Arcane::Editor::SaveGestureResult::Refused);
    CHECK(doc.saveCalls == 1);
    doc.saveSucceeds = true;
    const Arcane::Editor::InspectorSaveOutcome saved = Arcane::Editor::RequestSaveFromInspector(&doc);
    CHECK(saved.doc == &doc);
    CHECK(saved.result == Arcane::Editor::SaveGestureResult::Saved);
    CHECK_FALSE(doc.Dirty());
    // Not a document (the scene and asset sources): nothing saved, no doc.
    CHECK(Arcane::Editor::RequestSaveFromInspector(nullptr).doc == nullptr);
}

TEST_CASE("DocumentHost closes clean docs immediately, confirms dirty ones", "[editor]")
{
    DocumentHost host;
    auto* clean = static_cast<FakeDoc*>(host.Add(
        std::make_unique<FakeDoc>("clean", Arcane::Guid::Generate(), false)));
    auto* dirty = static_cast<FakeDoc*>(host.Add(
        std::make_unique<FakeDoc>("dirty", Arcane::Guid::Generate(), true)));
    REQUIRE(host.Count() == 2);
    CHECK(host.AnyDirty());

    host.RequestClose(clean);
    CHECK(host.Count() == 1);
    CHECK_FALSE(host.HasPendingConfirm());

    SECTION("save-and-close saves then closes")
    {
        host.RequestClose(dirty);
        REQUIRE(host.HasPendingConfirm());
        CHECK(host.PendingConfirmDoc() == dirty);
        host.ConfirmSaveAndClose();
        CHECK(host.Count() == 0);
        CHECK_FALSE(host.HasPendingConfirm());
    }

    SECTION("a FAILED save keeps the document open and pending")
    {
        dirty->saveSucceeds = false;
        host.RequestClose(dirty);
        host.ConfirmSaveAndClose();
        CHECK(host.Count() == 1);
        CHECK(host.HasPendingConfirm());
        CHECK(dirty->saveCalls == 1);
        host.CancelClose();
        CHECK_FALSE(host.HasPendingConfirm());
    }

    SECTION("discard closes without saving")
    {
        host.RequestClose(dirty);
        host.ConfirmDiscard();
        CHECK(host.Count() == 0);
    }

    SECTION("cancel keeps the document")
    {
        host.RequestClose(dirty);
        host.CancelClose();
        CHECK(host.Count() == 1);
        CHECK_FALSE(host.HasPendingConfirm());
        CHECK(dirty->saveCalls == 0);
    }

    SECTION("a second close request while one is pending is ignored")
    {
        auto* dirty2 = static_cast<FakeDoc*>(host.Add(
            std::make_unique<FakeDoc>("dirty2", Arcane::Guid::Generate(), true)));
        host.RequestClose(dirty);
        host.RequestClose(dirty2);   // pending slot taken -> ignored
        CHECK(host.PendingConfirmDoc() == dirty);
        host.ConfirmDiscard();
        CHECK(host.Count() == 1);    // dirty2 still open
    }
}

TEST_CASE("DocumentHost routes extensions and focuses instead of reopening", "[editor]")
{
    DocumentHost host;
    const Arcane::Guid stable = Arcane::Guid::Generate();
    int factoryCalls = 0;
    host.RegisterFactory(".arcmat",
        [&](const std::filesystem::path& p) -> std::unique_ptr<EditorDocument>
        {
            ++factoryCalls;
            return std::make_unique<FakeDoc>(p.stem().string(), stable, false);
        });

    EditorDocument* first = host.OpenPath("materials/glow.arcmat");
    REQUIRE(first != nullptr);
    CHECK(host.Count() == 1);

    // Same asset guid -> the open document is returned, no second window.
    EditorDocument* again = host.OpenPath("materials/GLOW.arcmat");   // case-insensitive ext
    CHECK(again == first);
    CHECK(host.Count() == 1);
    CHECK(factoryCalls == 2);   // factory ran (it produces the guid) but its doc was dropped

    // Unregistered extension -> null, nothing added.
    CHECK(host.OpenPath("something.png") == nullptr);
    CHECK(host.Count() == 1);

    CHECK(host.FindByGuid(stable) == first);
    CHECK(host.FindByGuid(Arcane::Guid::Generate()) == nullptr);
}

TEST_CASE("DocumentHost peek resolves focus-not-reopen without constructing", "[editor]")
{
    // Review m4: constructing a duplicate document just to discard it is not
    // free (its ctor submits compiles on the live doc's coalesce keys). A
    // registered peek must dedupe BEFORE the factory runs.
    DocumentHost host;
    const Arcane::Guid stable = Arcane::Guid::Generate();
    int factoryCalls = 0;
    host.RegisterFactory(".arcmat",
        [&](const std::filesystem::path& p) -> std::unique_ptr<EditorDocument>
        {
            ++factoryCalls;
            return std::make_unique<FakeDoc>(p.stem().string(), stable, false);
        },
        [&](const std::filesystem::path&) { return stable; });

    EditorDocument* first = host.OpenPath("materials/glow.arcmat");
    REQUIRE(first != nullptr);
    CHECK(factoryCalls == 1);

    EditorDocument* again = host.OpenPath("materials/glow.arcmat");
    CHECK(again == first);
    CHECK(host.Count() == 1);
    CHECK(factoryCalls == 1);   // the peek short-circuited: NO throwaway construct
}

TEST_CASE("DocumentHost::SaveAllDirty saves every dirty document, reporting failures", "[editor]")
{
    DocumentHost host;
    auto* ok = static_cast<FakeDoc*>(host.Add(
        std::make_unique<FakeDoc>("ok", Arcane::Guid::Generate(), true)));
    auto* bad = static_cast<FakeDoc*>(host.Add(
        std::make_unique<FakeDoc>("bad", Arcane::Guid::Generate(), true)));
    bad->saveSucceeds = false;

    CHECK(host.SaveAllDirty() == 1);
    CHECK_FALSE(ok->Dirty());
    CHECK(bad->Dirty());
    CHECK(ok->saveCalls == 1);
    CHECK(bad->saveCalls == 1);
}

TEST_CASE("DocumentHost::CloseAll drops every document and any pending confirm", "[editor]")
{
    DocumentHost host;
    host.Add(std::make_unique<FakeDoc>("clean", Arcane::Guid::Generate(), false));
    auto* dirty = static_cast<FakeDoc*>(host.Add(
        std::make_unique<FakeDoc>("dirty", Arcane::Guid::Generate(), true)));
    host.RequestClose(dirty);
    REQUIRE(host.HasPendingConfirm());

    host.CloseAll();
    CHECK(host.Count() == 0);
    CHECK_FALSE(host.HasPendingConfirm());
    CHECK_FALSE(host.AnyDirty());
}

TEST_CASE("DocumentHost tells its observer about every open and every close, before destruction", "[editor]")
{
    DocumentHost host;
    std::vector<std::string> events;
    host.SetObserver({
        [&](EditorDocument& d) { events.push_back("open:" + d.Title()); },
        [&](EditorDocument& d) { events.push_back("close:" + d.Title()); } });
    auto* a = static_cast<FakeDoc*>(host.Add(std::make_unique<FakeDoc>("a", Arcane::Guid::Generate(), false)));
    host.Add(std::make_unique<FakeDoc>("b", Arcane::Guid::Generate(), false));
    host.RequestClose(a);                            // clean: closes now
    host.CloseAll();                                 // b
    REQUIRE(events.size() == 4);
    CHECK(events[0] == "open:a");
    CHECK(events[1] == "open:b");
    CHECK(events[2] == "close:a");
    CHECK(events[3] == "close:b");
}

TEST_CASE("Param decls map to their editor widgets", "[editor][material]")
{
    using Arcane::Editor::ParamWidget;
    using Arcane::Editor::WidgetFor;
    using Arcane::MatParamType;

    STATIC_CHECK(WidgetFor(MatParamType::Float) == ParamWidget::SliderFloat);
    STATIC_CHECK(WidgetFor(MatParamType::Float2) == ParamWidget::DragFloat2);
    STATIC_CHECK(WidgetFor(MatParamType::Float4) == ParamWidget::DragFloat4);
    STATIC_CHECK(WidgetFor(MatParamType::Color) == ParamWidget::ColorEdit);
    STATIC_CHECK(WidgetFor(MatParamType::Texture) == ParamWidget::TexturePicker);
}

TEST_CASE("DocumentHost: an OpenPath that resolves to an open document re-selects it (both dedup branches)", "[editor][inspector]")
{
    // Final fix R: "open this asset" re-selects the page like a fresh open.
    const Arcane::Guid stable = Arcane::Guid::Generate();
    auto factory = [&](const std::filesystem::path& p) -> std::unique_ptr<EditorDocument>
    { return std::make_unique<FakeDoc>(p.stem().string(), stable, false); };

    SECTION("the peek branch")
    {
        DocumentHost host;
        host.RegisterFactory(".arcmat", factory, [&](const std::filesystem::path&) { return stable; });
        auto* first = static_cast<FakeDoc*>(host.OpenPath("materials/glow.arcmat"));
        REQUIRE(first != nullptr);
        CHECK(first->reopened == 0);             // a fresh open is not a RE-open
        CHECK(host.OpenPath("materials/glow.arcmat") == first);
        CHECK(first->reopened == 1);
    }
    SECTION("the peek-less fallback dedup branch")
    {
        DocumentHost host;
        host.RegisterFactory(".arcmat", factory);
        auto* first = static_cast<FakeDoc*>(host.OpenPath("materials/glow.arcmat"));
        REQUIRE(first != nullptr);
        CHECK(host.OpenPath("materials/glow.arcmat") == first);
        CHECK(first->reopened == 1);
    }
}

TEST_CASE("NoteAssetMoved retargets only that guid; CloseForAssetRemoval closes a dirty doc unsaved", "[editor][assetops]")
{
    DocumentHost host; const Arcane::Guid a = Arcane::Guid::Generate();
    auto* da = static_cast<FakeDoc*>(host.Add(std::make_unique<FakeDoc>("a", a, false)));
    auto* db = static_cast<FakeDoc*>(host.Add(std::make_unique<FakeDoc>("b", Arcane::Guid::Generate(), true)));
    host.NoteAssetMoved(a, "C:/p/Content/x/a2.arcmat");
    CHECK((da->movedTo == std::filesystem::path("C:/p/Content/x/a2.arcmat") && db->movedTo.empty()));
    host.RequestClose(db); REQUIRE(host.PendingConfirmDoc() == db);
    host.CloseForAssetRemoval(db);
    CHECK((host.Count() == 1 && !host.HasPendingConfirm()));
}
TEST_CASE("A SpriteDocument saved after NoteMoved writes the new path; the old path stays absent", "[editor][assetops]")
{
    namespace fs = std::filesystem; const fs::path d = fs::temp_directory_path() / "arcane_notemoved_sprite_test";
    std::error_code ec; fs::remove_all(d, ec); fs::create_directories(d);
    Arcane::SpriteAssetData data; data.id = Arcane::Guid::Generate(); REQUIRE(Arcane::SaveSpriteAsset(d / "old.arcsprite", data));
    Arcane::Editor::SpriteDocument doc(Arcane::Editor::SpriteDocument::Services{}, d / "old.arcsprite", data);
    fs::rename(d / "old.arcsprite", d / "new.arcsprite"); doc.NoteMoved(d / "new.arcsprite");
    CHECK(doc.Title() == "new");                       // empty name: the stem fallback follows
    REQUIRE(doc.Save());
    CHECK((fs::exists(d / "new.arcsprite") && !fs::exists(d / "old.arcsprite")));
    fs::remove_all(d, ec);
}

TEST_CASE("DocumentHost::HasFactory matches the registered extension case-insensitively", "[editor]")
{
    Arcane::Editor::DocumentHost host;
    host.RegisterFactory(".arcmat", [](const std::filesystem::path&) -> std::unique_ptr<Arcane::Editor::EditorDocument> { return nullptr; });
    CHECK(host.HasFactory("D:/p/a.arcmat"));
    CHECK(host.HasFactory("D:/p/A.ARCMAT"));
    CHECK_FALSE(host.HasFactory("D:/p/a.png"));
    CHECK_FALSE(host.HasFactory("D:/p/noext"));
}

namespace
{
    struct FocusDoc final : EditorDocument
    {
        std::string title; Arcane::Guid guid = Arcane::Guid::Generate(); bool dirty = false; bool focused = false;
        explicit FocusDoc(std::string t, bool d = false) : title(std::move(t)), dirty(d) {}
        const std::string& Title() const override { return title; }
        Arcane::Guid AssetGuid() const override { return guid; }
        bool Dirty() const override { return dirty; }
        bool Save() override { dirty = false; return true; }
        void Draw(bool&) override {}
        bool WindowFocused() const override { return focused; }
        void NoteMoved(const std::filesystem::path&) override {}
    };
}

TEST_CASE("DocumentHost::CloseTarget is the focused document, else the last active one, never a closed one", "[editor][shortcuts]")
{
    DocumentHost host;
    auto* a = static_cast<FocusDoc*>(host.Add(std::make_unique<FocusDoc>("a")));
    auto* b = static_cast<FocusDoc*>(host.Add(std::make_unique<FocusDoc>("b")));
    CHECK(host.CloseTarget() == nullptr);

    b->focused = true;
    host.NoteFocus();
    CHECK(host.CloseTarget() == b);
    b->focused = false;
    host.NoteFocus();
    CHECK(host.CloseTarget() == b);

    a->focused = true;
    host.NoteFocus();
    CHECK(host.CloseTarget() == a);
    host.RequestClose(a);
    a = nullptr;
    CHECK(host.CloseTarget() == nullptr);
}

TEST_CASE("document.close on a dirty document parks the normal save prompt", "[editor][shortcuts]")
{
    DocumentHost host;
    auto* d = static_cast<FocusDoc*>(host.Add(std::make_unique<FocusDoc>("d", true)));
    d->focused = true;
    host.NoteFocus();
    host.RequestClose(host.CloseTarget());
    CHECK(host.HasPendingConfirm());
    CHECK(host.PendingConfirmDoc() == d);
    CHECK(host.Count() == 1);
}

TEST_CASE("DocumentHost::AnyPreviewUploadsPending is true while any open document's preview has an upload in flight",
          "[editor][upload]")
{
    // The settle conjunct (Plan C Task 5): a document preview context renders into the chrome, and the frame
    // that queues its mesh looks identical to the one that uploads it, so settled captures must wait.
    DocumentHost host;
    CHECK_FALSE(host.AnyPreviewUploadsPending());   // no documents
    auto* a = static_cast<FakeDoc*>(host.Add(std::make_unique<FakeDoc>("a", Arcane::Guid::Generate(), false)));
    auto* b = static_cast<FakeDoc*>(host.Add(std::make_unique<FakeDoc>("b", Arcane::Guid::Generate(), false)));
    CHECK_FALSE(host.AnyPreviewUploadsPending());
    b->previewUploads = true;                        // one document's preview is mid-upload
    CHECK(host.AnyPreviewUploadsPending());
    a->previewUploads = true;
    b->previewUploads = false;
    CHECK(host.AnyPreviewUploadsPending());          // whichever it is
    a->previewUploads = false;
    CHECK_FALSE(host.AnyPreviewUploadsPending());    // settled
}
