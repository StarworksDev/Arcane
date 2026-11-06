#pragma once

// CreateAssetDialog (asset-manager redesign, Plan 1 Task 12): the ONE create
// flow. Spec s7's invariant, restated here because it is the whole point of
// this unit: **no creation path may bypass `CreateAssetRequest`**. Every
// producer -- the Assets menu, the panel's `+ Create` popup, the rail's `+`,
// a row's `Create` submenu, the Asset page's `New Instance...`, and (Plan 3)
// a graph pin-drag -- is a thin raiser of that request; `EditorApp::
// BeginCreateAsset` is the ONE entry that opens this dialog and
// `EditorApp::ConsumeCreateResult` the ONE dispatcher that mints from it.
//
// TWO HALVES, DELIBERATELY SPLIT ACROSS THE HEADER/TU LINE:
//   * this HEADER is ImGui-FREE and carries the PURE half (the request/result
//     value types, the per-kind vocabulary, and `ValidateCreateName`), so the
//     [editor] test can drive the validation rules headlessly -- the test exe
//     compiles NO ImGui TU (premake5.lua's ArcaneTests file list, which
//     source-compiles only the pure editor units), so the validation MUST be
//     reachable without one. Same header-inline pattern AssetPanelModel.hpp's
//     own pure helpers already use.
//   * the .cpp is the ImGui modal, and is compiled into ArcaneEditor ONLY.
//
// Task 12 shipped Material + MaterialInstance end-to-end; Task 13 completes
// the other three -- Mesh (Name + Location alone is enough), Sprite (a
// texture picker + its mint-or-reuse notice), Scene (a "set as boot"
// checkbox) -- and their dispatch (see DrawCreateAssetDialog's own comment).

#include "Panels/AssetPanelModel.hpp"   // AssetKind (the producer-side reconciliation below)

#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialSource.hpp>   // MaterialSurface (the Material kind combo)
#include <Arcane/Mesh/MeshAsset.hpp>            // MeshSource (the Create > Mesh > preset, F4 plan 1 T11)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <Arcane/Core/Constant.hpp>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetPanelModel;

    // WHAT can be created. NOT AssetKind (Panels/AssetPanelModel.hpp): that enum
    // classifies what a registry entry IS (ten values, including kinds nothing
    // can mint -- Texture, Audio, Font, Data, Diagnostic, Other), while this
    // one enumerates the five things the Create menu OFFERS, and splits
    // Material into the two shapes that need different dialog fields
    // (a fresh material vs. an instance OF one). The two numberings are
    // therefore unrelated -- see CreateKindForAssetKind below, which is the
    // ONLY sanctioned bridge between them.
    // CppClass (the editor<->IDE surface, step 3): Assets -> Create -> C++
    // Class. The one kind whose files land under Source/, not Content/ --
    // CreateKindRoot below is what keys that -- and whose "asset" is a pair
    // of files (Project/ClassTemplates.hpp renders them); the header is the
    // primary (uniqueness validates against ".hpp"), the .cpp is derived.
    enum class CreateAssetKind : std::uint8_t
    { Material, MaterialInstance, Mesh, Sprite, Scene, CppClass, InputActions };

    ARC_CONSTANT("enum and array arity: one per CreateAssetKind")
    inline constexpr int kCreateAssetKindCount = 7;

    // A request to create something. Raised by producers, consumed by
    // EditorApp::BeginCreateAsset. `prefillParent` pre-fills the kind's one
    // asset-valued field (MaterialInstance -> the parent material; Sprite ->
    // the source texture, Task 13); `prefillSurface` pre-picks the Material
    // surface combo (a MaterialSurface value; -1 = none, let the dialog
    // default); `prefillMeshSource` (F4 plan 1 Task 11, spec s8) pre-picks
    // the Mesh kind's generator (a MeshSource value; -1 = none, MeshAssetData's
    // own Cube default) -- `Create > Mesh > <primitive>` raises THIS request
    // with the preset, so the submenu is five thin raisers of the one request
    // rather than a second creation path.
    struct CreateAssetRequest
    {
        CreateAssetKind kind = CreateAssetKind::Material;
        Arcane::Guid    prefillParent;   // instance parent / sprite texture
        int             prefillSurface = -1; // pre-picked MaterialSurface, -1 none
        int             prefillMeshSource = -1; // pre-picked MeshSource (Mesh kind), -1 none
        std::string     cppDefaultFolder;   // CppClass only: CppClassDefaultFolder(manifest.sourceDir), seeded by the app -- see CppClassDefaultFolder below
    };

    // ---- per-kind vocabulary (pure; shared by the dialog and its dispatcher)

    [[nodiscard]] inline const char* CreateKindTitle(CreateAssetKind k)
    {
        switch (k)
        {
            case CreateAssetKind::Material:         return "Create Material";
            case CreateAssetKind::MaterialInstance: return "Create Material Instance";
            case CreateAssetKind::Mesh:             return "Create Mesh";
            case CreateAssetKind::Sprite:           return "Create Sprite";
            case CreateAssetKind::Scene:            return "Create Scene";
            case CreateAssetKind::CppClass:         return "Create C++ Class";
            case CreateAssetKind::InputActions:     return "Create Input Actions";
        }
        return "Create Asset";
    }

    // The directory under the project root a kind's files land in: every
    // asset kind is Content/ (the game:// mount), C++ classes are Source/
    // (the source:// mount -- the directory build/arcane.lua's game-module
    // glob compiles). The dialog's Location combo and ConsumeCreateResult's
    // target path both key off THIS, so the two can never disagree about
    // where a create goes.
    [[nodiscard]] inline const char* CreateKindRoot(CreateAssetKind k)
    {
        return k == CreateAssetKind::CppClass ? "Source" : "Content";
    }

    // The file extension the kind mints. Materials and material INSTANCES are
    // both ".arcmat" -- an instance is a material file with a `parent`
    // (MaterialAssetData), not a distinct format.
    [[nodiscard]] inline const char* CreateKindExtension(CreateAssetKind k)
    {
        switch (k)
        {
            case CreateAssetKind::Material:
            case CreateAssetKind::MaterialInstance: return ".arcmat";
            case CreateAssetKind::Mesh:             return ".arcmesh";
            case CreateAssetKind::Sprite:           return ".arcsprite";
            case CreateAssetKind::Scene:            return ".arcscene";
            case CreateAssetKind::CppClass:         return ".hpp";   // the primary of the pair
            case CreateAssetKind::InputActions:     return ".arcinput";
        }
        return "";
    }

    // The Location combo's per-kind default (spec s7: "Location (combo of
    // existing content directories, per-kind default)"), relative to the
    // project's Content/. Empty string = Content/ itself.
    [[nodiscard]] inline const char* CreateKindDefaultFolder(CreateAssetKind k)
    {
        switch (k)
        {
            case CreateAssetKind::Material:
            case CreateAssetKind::MaterialInstance: return "materials/";
            case CreateAssetKind::Mesh:             return "meshes/";
            case CreateAssetKind::Sprite:           return "sprites/";
            case CreateAssetKind::Scene:            return "scenes/";
            case CreateAssetKind::CppClass:         return "";   // Source/ itself
            case CreateAssetKind::InputActions:     return "input/";
        }
        return "";
    }

    // The Location combo's default for a C++ CLASS specifically, derived from
    // the manifest's sourceDir (ProjectManifest.hpp): the game module's own
    // directory, relative to the source:// mount root (Source/), in the
    // model's folder-string shape -- "" for the root itself, "Game/" for
    // "Source/Game". CreateKindDefaultFolder(CppClass) stays "" as the
    // manifest-less fallback; CreateKindRoot(CppClass) stays "Source" (the
    // MOUNT does not move -- plan ruling S2).
    [[nodiscard]] inline std::string CppClassDefaultFolder(std::string_view sourceDir)
    {
        std::string dir(sourceDir);
        while (!dir.empty() && dir.back() == '/')
            dir.pop_back();
        if (dir == "Source" || dir.rfind("Source/", 0) != 0)
            return "";
        return dir.substr(7) + "/";
    }

    // The noun the uniqueness message names ("a <noun> named X already exists
    // here"). Derived from the EXTENSION rather than the kind because
    // ValidateCreateName is deliberately kind-agnostic: it validates a name
    // against a directory and an extension, which is all a filesystem
    // uniqueness question needs, and that keeps the function reusable for a
    // kind this enum does not carry yet.
    [[nodiscard]] inline const char* CreateNounForExtension(std::string_view extension)
    {
        if (extension.empty())         return "folder";   // T5 s7.8: New Folder... validates a bare name
        if (extension == ".arcmat")    return "material";
        if (extension == ".arcmesh")   return "mesh";
        if (extension == ".arcsprite") return "sprite";
        if (extension == ".arcscene")  return "scene";
        if (extension == ".hpp")       return "class";
        if (extension == ".arcinput")  return "input actions asset";
        return "file";
    }

    // ---- the five mesh primitives (F4 plan 1 Task 11, spec s8) -------------
    // ONE vocabulary read by three consumers: the `Create > Mesh >` submenu
    // (its labels + the MeshSource each raises), the scene's `Add > 3D Object >`
    // submenu (its labels, the Identity name of the spawned entity, and the
    // `Content/meshes/<Name>.arcmesh` file EditorApp::MintOrReusePrimitiveMesh
    // looks up by path and mints on first use), and the dispatcher. Spelled
    // here, in the ImGui-free half, so the [editor][create] test pins it.
    //
    // Menu order is spec s8's (Cube / Plane / Sphere / Cylinder / Capsule),
    // NOT MeshSource's persisted numbering (Plane = 0). Imported is not a
    // primitive: nothing can generate it, so it has no name and no file here.
    inline constexpr Arcane::MeshSource kPrimitiveMeshSources[] = {
        Arcane::MeshSource::Cube,     Arcane::MeshSource::Plane,   Arcane::MeshSource::UvSphere,
        Arcane::MeshSource::Cylinder, Arcane::MeshSource::Capsule,
    };
    ARC_CONSTANT("enum and array arity: one per primitive mesh source")
    inline constexpr int kPrimitiveMeshSourceCount = 5;

    // The primitive's name: the submenu label, the .arcmesh stem, and the
    // spawned entity's Identity name, all at once. "Sphere" for UvSphere (the
    // file name the brief fixes; MeshDocument's combo says "UV Sphere", which is
    // a display label for the generator, not a file stem). nullptr for a
    // non-primitive source.
    [[nodiscard]] inline const char* PrimitiveMeshName(Arcane::MeshSource s)
    {
        switch (s)
        {
            case Arcane::MeshSource::Cube:     return "Cube";
            case Arcane::MeshSource::Plane:    return "Plane";
            case Arcane::MeshSource::UvSphere: return "Sphere";
            case Arcane::MeshSource::Cylinder: return "Cylinder";
            case Arcane::MeshSource::Capsule:  return "Capsule";
            case Arcane::MeshSource::Imported: return nullptr;
        }
        return nullptr;
    }

    // The primitive's file, relative to the project's Content/: the Mesh
    // kind's own default folder + name + extension -- "meshes/Cube.arcmesh".
    // FOLDER SPELLING: spec s8 writes `Content/Meshes/`, but the folder is
    // spelled LOWERCASE `meshes/` everywhere the editor already touches it
    // (CreateKindDefaultFolder(Mesh), ReferenceProject/Content/meshes/ with its
    // golden_prop + reference_cube). Windows resolves either spelling to the one
    // directory, but the registry keys on the path and a golden/witness pin
    // must never see two spellings -- so this reuses CreateKindDefaultFolder
    // rather than introducing a second one. Empty for a non-primitive source.
    [[nodiscard]] inline std::string PrimitiveMeshRelativePath(Arcane::MeshSource s)
    {
        const char* name = PrimitiveMeshName(s);
        if (!name)
            return {};
        return std::string(CreateKindDefaultFolder(CreateAssetKind::Mesh)) + name
             + CreateKindExtension(CreateAssetKind::Mesh);
    }

    // ---- the Material "Kind" combo (spec s7: sprite / mesh / post) --------
    // The COMBO's own order, which is NOT MaterialSurface's numbering
    // (Fullscreen=0, Sprite=1, Mesh=2 -- MaterialSource.hpp:71): the mock's
    // menu hint reads "sprite / mesh / post", and `Fullscreen` DISPLAYS as
    // "post" everywhere in this panel (spec's global constraint). Two
    // orderings, so two explicit mappings rather than a cast -- the same
    // discipline CreateKindForAssetKind above applies to the other enum pair
    // this task had to reconcile.
    inline constexpr const char* kMaterialSurfaceLabels[] = { "sprite", "mesh", "post" };
    ARC_CONSTANT("enum and array arity: one per MaterialSurface")
    inline constexpr int kMaterialSurfaceCount = 3;

    [[nodiscard]] inline int MaterialSurfaceComboIndex(Arcane::MaterialSurface s)
    {
        switch (s)
        {
            case Arcane::MaterialSurface::Sprite:     return 0;
            case Arcane::MaterialSurface::Mesh:       return 1;
            case Arcane::MaterialSurface::Fullscreen: return 2;
        }
        return 2;
    }

    [[nodiscard]] inline Arcane::MaterialSurface MaterialSurfaceForComboIndex(int index)
    {
        switch (index)
        {
            case 0:  return Arcane::MaterialSurface::Sprite;
            case 1:  return Arcane::MaterialSurface::Mesh;
            default: return Arcane::MaterialSurface::Fullscreen;
        }
    }

    // The dialog's default when nothing pre-picked a surface: "post"
    // (Fullscreen). Chosen to PRESERVE what the retired `Assets -> Create ->
    // Material...` menu item minted (CreateMaterialAt's own Fullscreen
    // default) -- collapsing two menu items into one dialog must not silently
    // change what the surviving one produces. Now
    // editor.assets.newMaterialDefaultSurface (settings S6-38; per-project,
    // default 2), read each time the dialog opens and clamped to the combo.
    [[nodiscard]] int MaterialSurfaceDefaultIndex();

    // The subkind pill text for a material's surface -- the SAME three strings
    // the combo offers, so the picker's pills and the combo can never drift.
    [[nodiscard]] inline const char* MaterialSurfacePillText(Arcane::MaterialSurface s)
    { return kMaterialSurfaceLabels[MaterialSurfaceComboIndex(s)]; }

    // ---- the producer-side AssetKind -> CreateAssetKind bridge -------------
    // The rail's per-kind `+` and a row's `Create` submenu both start from an
    // AssetKind (what the rail row / the clicked row IS) and must raise a
    // CreateAssetKind (what the dialog creates). THE TWO ENUMS DO NOT SHARE A
    // NUMBERING -- AssetKind::Sprite is 6 and CreateAssetKind::Sprite is 3 --
    // so the bridge is spelled ONCE, here, and applied at the PRODUCER: the
    // `requestCreateKind` field's contract stays "a CreateAssetKind value",
    // with no tagged-source branch at the consumer to keep in lockstep.
    // nullopt for the six AssetKinds nothing can mint (Texture/Audio/Font/
    // Data/Diagnostic/Other) -- matching the Asset Browser rail's own
    // RailKindCreatable gate, which is what keeps those rails from offering a
    // `+` in the first place.
    [[nodiscard]] inline std::optional<CreateAssetKind> CreateKindForAssetKind(AssetKind k)
    {
        switch (k)
        {
            case AssetKind::Material: return CreateAssetKind::Material;
            case AssetKind::Mesh:     return CreateAssetKind::Mesh;
            case AssetKind::Sprite:   return CreateAssetKind::Sprite;
            case AssetKind::Scene:    return CreateAssetKind::Scene;
            case AssetKind::Source:   return CreateAssetKind::CppClass;
            case AssetKind::InputActions: return CreateAssetKind::InputActions;
            default:                  return std::nullopt;
        }
    }

    // ---- name validation (PURE; the [editor] test's whole surface) ---------
    struct CreateNameCheck { bool ok = false; std::string message; };

    // Validate a proposed asset name against the directory it would land in.
    // Rules run in UE's deliberate cheap->expensive order (its own
    // AssetViewUtils.cpp:1420-1509 precedent): syntax first (a pure string
    // scan), then length (arithmetic), then uniqueness LAST -- the only rule
    // that touches the filesystem, so an obviously-bad name never pays for a
    // stat. Each rule's message is DISTINCT: the character message and the
    // "already exists" message must never be confusable, or the fix a user
    // has to make stops being obvious from the text.
    //
    // `extension` is the kind's extension WITH its dot (".arcmat"); it counts
    // toward the length budget and completes the file name the uniqueness
    // check looks for. `targetDir` may be nonexistent (a folder the create
    // will make) -- `exists()` on a path under it simply answers false, which
    // is the correct answer for uniqueness.
    // The length budget, in characters of the composed absolute path
    // (directory + separator + stem + extension). 240 rather than Windows'
    // own 260 MAX_PATH: the created asset's siblings -- an Intermediate/
    // Artifacts/ path keyed off it, a ".meta" sidecar -- are longer than the
    // asset itself, so the asset needs headroom under the real ceiling, not
    // to sit exactly on it.
    ARC_CONSTANT("hardware/OS/API limit: Windows MAX_PATH, less a margin for the folder")
    inline constexpr std::size_t kCreateNameMaxPathChars = 240;

    // Rules 0-2 only (syntax + length) -- no filesystem. The asset file-op planner
    // (Project/AssetFileOps) answers uniqueness from its own facts.
    [[nodiscard]] inline CreateNameCheck ValidateCreateNameSyntax(std::string_view name,
                                                                  const std::filesystem::path& targetDir,
                                                                  std::string_view extension)
    {
        // ---- Rule 0/1: syntax. A pure string scan, first because it is the
        // cheapest and because a name that cannot be a file name at all makes
        // every later question meaningless.
        if (name.empty())
            return { false, "enter a name" };

        // The deny-set, verbatim: \ / : * ? " < > | -- the characters Windows
        // refuses in a file name, which is also the set POSIX's `/` is a
        // subset of, so one list serves both. Path separators are IN the set
        // on purpose: a name is a LEAF, never a sub-path (the Location combo
        // is how a different directory is chosen).
        constexpr std::string_view kDenied = "\\/:*?\"<>|";
        if (name.find_first_of(kDenied) != std::string_view::npos)
            return { false, "a name cannot contain any of  \\ / : * ? \" < > |" };

        // Leading/trailing dots and spaces: Windows SILENTLY STRIPS both when
        // it creates the file, so accepting them would mint an asset under a
        // name the user did not type -- and the uniqueness check below would
        // have asked about the un-stripped spelling, so it could not even
        // catch the resulting collision.
        const auto isDotOrSpace = [](char c) { return c == '.' || c == ' '; };
        if (isDotOrSpace(name.front()) || isDotOrSpace(name.back()))
            return { false, "a name cannot start or end with a dot or a space" };

        // ---- Rule 2: length. Arithmetic only -- no filesystem call yet.
        // Measured in NATIVE path characters (wchar_t on Windows) rather than
        // through path::string(), which converts through the active codepage
        // and can THROW on a name this function is specifically supposed to
        // return a verdict about.
        const std::size_t total = targetDir.native().size() + 1u   // + separator
                                + name.size() + extension.size();
        if (total >= kCreateNameMaxPathChars)
            return { false, "that name makes the full path too long ("
                            + std::to_string(total) + " of "
                            + std::to_string(kCreateNameMaxPathChars) + " characters)" };

        return { true, {} };
    }

    // Rename-only (spec s7.6: "the extension is fixed (it is the kind)"): a stem
    // that ends in the asset's own extension, in any letter case, is refused, so
    // typing "wall.png" for a .png never plans a silent wall.png.png. Only the
    // file's OWN extension is refused -- "ship.v2", or "wall.jpg" for a .png
    // (wall.jpg.png), stays legal. Create and Duplicate never call this.
    [[nodiscard]] inline CreateNameCheck RefuseTypedExtension(std::string_view stem, std::string_view ext)
    {
        const auto fold = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        if (!ext.empty() && stem.size() >= ext.size()
            && std::equal(ext.begin(), ext.end(), stem.end() - static_cast<std::ptrdiff_t>(ext.size()),
                          [&](char a, char b) { return fold(a) == fold(b); }))
            return { false, "the extension is fixed; type the name without " + std::string(ext) };
        return { true, {} };
    }

    // A rename stem's pure rules: rules 0-2 first (their messages and order kept),
    // then the fixed-extension rule. No filesystem.
    [[nodiscard]] inline CreateNameCheck ValidateRenameStemSyntax(std::string_view stem,
                                                                  const std::filesystem::path& dir,
                                                                  std::string_view ext)
    {
        if (CreateNameCheck syntax = ValidateCreateNameSyntax(stem, dir, ext); !syntax.ok)
            return syntax;
        return RefuseTypedExtension(stem, ext);
    }

    [[nodiscard]] inline CreateNameCheck ValidateCreateName(std::string_view name,
                                                            const std::filesystem::path& targetDir,
                                                            std::string_view extension)
    {
        if (CreateNameCheck syntax = ValidateCreateNameSyntax(name, targetDir, extension); !syntax.ok)
            return syntax;

        // ---- Rule 3: uniqueness. LAST -- the only rule that touches the
        // filesystem. Its message is deliberately UNLIKE the two above: it
        // names the colliding asset and what kind it is, so the fix ("pick
        // another name" / "open the one that is already there") reads
        // straight off the text.
        std::error_code ec;
        const std::filesystem::path target =
            targetDir / (std::string(name) + std::string(extension));
        if (std::filesystem::exists(target, ec))
            return { false, std::string("a ") + CreateNounForExtension(extension)
                            + " named " + std::string(name) + " already exists here" };

        return { true, {} };
    }

    // T5 s7.6: the Rename validator -- rules 0-3 plus the fixed-extension rule
    // (ValidateRenameStemSyntax), except a target equivalent to `currentFile`
    // (case-only on NTFS) is free; a byte-identical stem is a no-op.
    [[nodiscard]] inline CreateNameCheck ValidateRenameName(std::string_view stem, const std::filesystem::path& currentFile)
    {
        if (stem == currentFile.stem().string()) return { true, {} };
        const std::filesystem::path dir = currentFile.parent_path(); const std::string ext = currentFile.extension().string();
        if (CreateNameCheck c = ValidateRenameStemSyntax(stem, dir, ext); !c.ok) return c;
        std::error_code ec; const std::filesystem::path target = dir / (std::string(stem) + ext);
        if (std::filesystem::exists(target, ec) && std::filesystem::equivalent(target, currentFile, ec)) return { true, {} };
        return ValidateCreateName(stem, dir, ext);
    }

    // ---- the modal --------------------------------------------------------

    // Modal state, owned by EditorApp between frames (a modal spans frames;
    // its in-progress fields cannot live on the draw's stack). `open` is what
    // EditorApp::BeginCreateAsset sets and the dialog clears.
    struct CreateDialogState
    {
        bool open = false;
        CreateAssetRequest request{};
        char name[128] = {};
        int  folderIndex = 0;       // into the folder combo
        int  surface = 0;           // Material kind combo
        Arcane::Guid parent, texture;
        bool setAsBoot = false;
        bool pickerOpen = false;
        int  classTemplate = 0;     // CppClass: a ClassTemplates::Kind value (Template combo)
        // CppClass/System only. Keep UI indices here, not engine enums: stale
        // persisted or test-injected values are clamped at the dialog boundary,
        // then converted once by ClassTemplates when the files are minted.
        int  systemPhaseIndex = 0;  // Fixed Update / Update (Render is retired)
        int  systemRoleIndex = 0;   // Both / Server / Client
        // Set once DrawCreateAssetDialog has seeded folderIndex for THIS
        // request; BeginCreateAsset resets it to false along with everything
        // else. Deliberately separate from ImGui's own IsPopupOpen(title):
        // the popup can be closed out from under an open request by a
        // competing dockspace-level modal (the error queue, a parked scene
        // intent) re-arming at the same stack level, and on resume that would
        // read as "just opened" and silently re-seed folderIndex over
        // whatever the user had already picked in the Location combo.
        bool seeded = false;
        // The Location the dialog seeds on open, relative to the kind's root
        // ("materials", "" = the root itself); nullopt = the kind's default
        // folder. Set by MakeCreateDialogState for an instance created FROM a
        // project material: it lands beside its parent (T3-D5, Unreal parity).
        std::optional<std::string> defaultFolder;
    };

    // What a completed dialog hands back. `folder` is relative to the kind's
    // ROOT (CreateKindRoot: Content/ for every asset kind, Source/ for
    // CppClass) -- "materials", or "" for the root itself -- and the
    // dispatcher joins it onto that root, which is the only place a created
    // file can register and resolve by GUID (Project.cpp: "game" -> root /
    // "Content", "source" -> root / "Source").
    struct CreateAssetResult
    {
        CreateAssetKind kind = CreateAssetKind::Material;
        std::string name; std::string folder;    // relative to CreateKindRoot(kind)
        int surface = 0;                          // Material: MaterialSurface value
        int classTemplate = 0;                    // CppClass: ClassTemplates::Kind value
        int systemPhaseIndex = 0;                 // CppClass/System: choice index
        int systemRoleIndex = 0;                  // CppClass/System: choice index
        int meshSource = -1;                      // Mesh: the request's MeshSource preset (-1 = MeshAssetData's Cube default)
        Arcane::Guid parent, texture; bool setAsBoot = false;
        // Task 13: Sprite's mint-or-reuse notice carries an "Open existing"
        // button (spec s7: "it says so and offers to open it") -- when this
        // is valid, EVERY other field above is meaningless: ConsumeCreateResult
        // selects + opens THIS already-registered sprite instead of dispatching
        // a mint, closing the dialog without creating anything. A second result
        // shape rather than overloading `texture` (which already names the
        // SOURCE texture the user picked, not the derived sprite to open).
        Arcane::Guid openExisting;
    };

    // ---- Location-combo folder choices (T5 s7.8: hoisted out of
    // CreateAssetDialog.cpp's anonymous namespace verbatim, so the Move to...
    // modal and the tests share them) ----

    // One entry in the Location combo. `display` is what the combo shows
    // ("Content/materials"); `relative` is what CreateAssetResult carries
    // ("materials", or "" for Content/ itself), which the dispatcher joins
    // onto the project's content root.
    struct FolderChoice
    {
        std::string display;
        std::string relative;
    };

    // The model's folder strings are mount-path directories with a
    // trailing slash ("materials/"), and root-level assets fold into the
    // synthetic bucket "Content/" (MakeBaseEntry's own comment). Both
    // shapes normalise to the same pair here. `root` is the kind's
    // CreateKindRoot ("Content" / "Source"): the display is "<root>/rel".
    inline FolderChoice MakeFolderChoice(const std::string& relDir, const char* root)
    {
        std::string rel = relDir;
        while (!rel.empty() && rel.back() == '/')
            rel.pop_back();
        if (rel.empty())
            return { root, "" };   // the root itself names no subdirectory
        return { std::string(root) + "/" + rel, rel };
    }

    // A model folder KEY -> the directory relative to its mount root, or
    // nullopt when the key belongs to another mount. Two key shapes
    // (AssetPanelEntry::folder's own doc): the game mount is UNQUALIFIED
    // ("Content/" is its root, "materials/" nested); every other mount is
    // QUALIFIED ("source://" is its root, "source://combat/" nested).
    inline std::optional<std::string> RelativeDirOfFolderKey(const std::string& key, const char* root)
    {
        const bool wantSource = std::string_view(root) == "Source";
        if (const std::size_t sep = key.find("://"); sep != std::string::npos)
        {
            if (!wantSource || key.substr(0, sep) != "source")
                return std::nullopt;
            return key.substr(sep + 3);   // "" for the root, "combat/" nested
        }
        if (wantSource)
            return std::nullopt;
        return key == "Content/" ? std::string() : key;
    }

    // Distinct create-able folders: every directory the project's OWN
    // files already use under the kind's root, plus the kind's default,
    // plus the root itself.
    //
    // ONE mount only -- "game://" for every asset kind, "source://" for
    // CppClass. The model groups folders across every mount (an engine://
    // and a game:// "materials/" share one Browse group), but a created
    // file can only land -- and only register + resolve by GUID -- under
    // the project's own root for that kind (Project.cpp mounts "game" at
    // root/Content and "source" at root/Source). Offering an engine or
    // plugin folder here would offer a target RegisterCreatedAsset refuses.
    inline std::vector<FolderChoice> BuildFolderChoices(const AssetPanelModel& model,
                                                        CreateAssetKind kind,
                                                        const std::string& cppDefaultFolder)
    {
        const char* root = CreateKindRoot(kind);
        std::set<std::string> folders;                     // relative dirs, "" = root
        folders.insert("");                                // always offer the root
        // CppClass's default folder comes from the manifest's sourceDir
        // (CppClassDefaultFolder, seeded onto the request by BeginCreateAsset);
        // every other kind keeps its fixed CreateKindDefaultFolder.
        folders.insert(kind == CreateAssetKind::CppClass
                           ? cppDefaultFolder
                           : std::string(CreateKindDefaultFolder(kind)));
        for (const auto& [guid, e] : model.Entries())
        {
            (void)guid;
            if (const auto rel = RelativeDirOfFolderKey(e.folder, root))
                folders.insert(*rel);
        }
        // T5 s7.8: an empty Content/ folder has no entry, but is still a
        // place to create into (its key is unqualified "game", so the
        // "Source" root never matches it).
        for (const std::string& key : model.EmptyFolders())
            if (const auto rel = RelativeDirOfFolderKey(key, root))
                folders.insert(*rel);

        std::vector<FolderChoice> out;
        out.reserve(folders.size());
        // Root first (it is the parent of everything else), then the rest
        // in the set's own alphabetical order -- a stable, predictable
        // list rather than unordered_map iteration order.
        out.push_back(MakeFolderChoice("", root));
        for (const std::string& f : folders)
            if (!f.empty())
                out.push_back(MakeFolderChoice(f, root));
        return out;
    }

    // The Content/ root, every folder an entry lives in, and every empty
    // folder -- the Location list of the Move to... modal (s7.8), which is
    // not tied to a CreateAssetKind's default folder.
    [[nodiscard]] inline std::vector<FolderChoice> BuildContentFolderChoices(const AssetPanelModel& m)
    {
        std::set<std::string> folders;
        for (const auto& [g, e] : m.Entries()) if (const auto rel = RelativeDirOfFolderKey(e.folder, "Content")) folders.insert(*rel);
        for (const auto& k : m.EmptyFolders()) if (const auto rel = RelativeDirOfFolderKey(k, "Content")) folders.insert(*rel);
        std::vector<FolderChoice> out{ MakeFolderChoice("", "Content") }; for (const auto& f : folders) if (!f.empty()) out.push_back(MakeFolderChoice(f, "Content"));
        return out;
    }
    // The dialog's starting state for one request -- what
    // EditorApp::BeginCreateAsset opens (it adds only the C++ Class default
    // folder, which needs the project). `open` is set; every other field
    // starts fresh, so a cancelled dialog leaves nothing behind for the next.
    //   * `prefillSurface` (a MaterialSurface VALUE, -1 = none) becomes the
    //     surface combo's index through MaterialSurfaceComboIndex.
    //   * `prefillParent` lands in the one asset-valued field the kind has:
    //     an instance's parent, a sprite's texture. Its picker starts
    //     expanded only when there is nothing prefilled to show.
    //   * A MaterialInstance whose parent the model knows is named
    //     "<parent>_Inst" (Unreal's own default for a new instance, T3-D4),
    //     suffixed "_Inst2", "_Inst3", ... while that file already exists, so
    //     the dialog never opens on "already exists" (T3-D5). Its Location
    //     defaults to the parent's own folder when the parent lives under the
    //     project's Content/; an engine or plugin parent keeps materials/.
    // `projectRoot` is the open project's root (the dialog's own
    // `project.Root()`): the name's uniqueness is asked of the files there.
    [[nodiscard]] CreateDialogState MakeCreateDialogState(const CreateAssetRequest& request,
                                                          const AssetPanelModel& model,
                                                          const std::filesystem::path& projectRoot);

    // T5 s7.8: the Location combo (a "Location" label over a full-width combo
    // of `folders[i].display`), shared by the create dialog and the Move to...
    // modal. Clamps `index` into range first; returns true the frame a row
    // is picked. `folders` must not be empty (both builders lead with the root).
    bool DrawLocationCombo(const std::vector<FolderChoice>& folders, int& index);

    // Draw the modal for `st.request.kind`. Returns a completed result the
    // frame Create (or Sprite's "Open existing") was clicked, nullopt
    // otherwise (including every frame the dialog is merely up). Cancel /
    // Escape / the title bar's x close it and return nullopt.
    //
    // Material and MaterialInstance carry their full field set (Task 12);
    // Task 13 completes the other three: Mesh needs nothing beyond the
    // shared Name + Location anatomy (MeshAssetData's own defaults are
    // already a complete, valid asset); Sprite gets a texture picker plus
    // the mint-or-reuse notice (model fold data); Scene gets a "set as
    // boot" checkbox.
    std::optional<CreateAssetResult> DrawCreateAssetDialog(CreateDialogState& st,
                                                           const AssetPanelModel& model,
                                                           const Arcane::Project& project);
}
