// The disk-resolution half of the Hub: turning a RECORDED project path into
// the (root, manifest) pair the commands act on, plus the manifest's ABI and
// the folder a project lives in. IO over project.rs's pure rules -- split out
// of lib.rs alongside spawn.rs so the command file stays an IPC surface.
// project.rs itself stays fs-free and unit-tested; the functions here are the
// ones that touch the live disk, which is why they live apart.

use std::path::{Path, PathBuf};

use crate::{project, store};

/// The manifest a recorded path names, LENIENTLY: the path itself when it is
/// a .arcproj, else the folder's single .arcproj. None for an ambiguous or
/// unreadable folder. The one resolution manifest_abi and manifest_guid
/// share (they each carried a copy). Lenient means "for READING": a
/// manifest-shaped path is believed without asking whether its folder holds
/// other projects -- fine for an ABI or a guid, never for a destructive
/// command (that is resolve_owned_project's job).
fn lenient_manifest(project_path: &Path) -> Option<PathBuf> {
    if project_path.extension().is_some_and(|e| e.eq_ignore_ascii_case(project::MANIFEST_EXT)) {
        return Some(project_path.to_path_buf());
    }
    let names = file_names(project_path).ok()?;
    Some(project_path.join(project::pick_manifest(&names)?))
}

/// The plain FILE names directly inside `dir`.
fn file_names(dir: &Path) -> std::io::Result<Vec<String>> {
    Ok(std::fs::read_dir(dir)?
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
        .map(|e| e.file_name().to_string_lossy().to_string())
        .collect())
}

/// The ABI a project was BUILT AGAINST, read from its manifest.
///
/// This is the number `Runtime::OpenProject` (Runtime.cpp:387) compares against
/// the engine's own constant and refuses on mismatch, so it is the only honest
/// input to the Hub's compatibility display. None = unknown (no manifest,
/// ambiguous folder, unreadable JSON), which the UI treats as "cannot prove a
/// conflict" rather than as a fault.
pub fn manifest_abi(project_path: &Path) -> Option<u32> {
    let file = lenient_manifest(project_path)?;
    project::parse_manifest_abi(&std::fs::read_to_string(file).ok()?)
}

/// The project's durable identity, read from its manifest and canonicalised.
///
/// None = no usable identity: no manifest, ambiguous folder, or a manifest
/// that predates the guid field (the engine self-heals those at open time,
/// so absence is a temporary state, not a fault). This is the key the Hub
/// uses to recognise a MOVED project -- see state::heal_by_guid for the rule
/// that keeps a hand-copied folder from impersonating the original.
pub fn manifest_guid(project_path: &Path) -> Option<String> {
    let file = lenient_manifest(project_path)?;
    project::parse_manifest_guid(&std::fs::read_to_string(file).ok()?)
}

/// The FOLDER a project lives in: the parent of its .arcproj, or the recorded
/// path itself when the entry is folder-shaped (how opens were recorded before
/// the dialog asked for a manifest). Paired with `projectDir` in
/// src/lib/format.ts, which does the same job for display.
pub fn project_dir(p: &Path) -> PathBuf {
    if p.extension().is_some_and(|e| e.eq_ignore_ascii_case(project::MANIFEST_EXT)) {
        return p.parent().map(Path::to_path_buf).unwrap_or_else(|| p.to_path_buf());
    }
    p.to_path_buf()
}

/// A recorded path resolved to (project root, manifest file) -- LENIENTLY.
///
/// Applies the engine's own rule for a FOLDER: exactly one .arcproj, or it is
/// ambiguous and refused (Project.cpp:142-160). A manifest-shaped path is
/// taken as given, WITHOUT checking whether its folder holds other
/// manifests: right for the cover, a launch, Locate and add_project, which
/// only read that one file. NOT safe for anything that acts on the whole
/// FOLDER -- delete, rename, duplicate use resolve_owned_project.
pub fn resolve_project(recorded: &Path) -> Result<(PathBuf, PathBuf), String> {
    if recorded.is_file() {
        if !recorded.extension().is_some_and(|e| e.eq_ignore_ascii_case(project::MANIFEST_EXT)) {
            return Err(format!("{} is not a .arcproj file", recorded.display()));
        }
        let root = recorded
            .parent()
            .ok_or_else(|| format!("{} has no parent folder", recorded.display()))?;
        return Ok((root.to_path_buf(), recorded.to_path_buf()));
    }
    if !recorded.is_dir() {
        return Err(format!("{} is no longer on disk", recorded.display()));
    }
    let names = file_names(recorded)
        .map_err(|e| format!("could not read {}: {e}", recorded.display()))?;
    let file = project::pick_manifest(&names).ok_or_else(|| {
        format!(
            "{} does not contain exactly one .arcproj, so there is no single \
             project to act on",
            recorded.display()
        )
    })?;
    Ok((recorded.to_path_buf(), recorded.join(file)))
}

/// A recorded path resolved to (project root, manifest file) for a command
/// that acts on the WHOLE FOLDER: delete (to the recycle bin), rename,
/// duplicate. `verb` names the act for the refusal message.
///
/// THE INCIDENT THIS CLOSES: resolve_project takes a manifest-shaped path at
/// its word, so "D:/Games/A.arcproj" resolved to root "D:/Games" -- and
/// Delete then sent the whole of D:/Games, every other project in it, to
/// the recycle bin; Rename renamed it; Duplicate copied it. The folder is
/// only "this project's folder" when it holds exactly one .arcproj and that
/// one IS this manifest (the engine's own exactly-one rule, applied to the
/// parent of a file as well as to a folder).
pub fn resolve_owned_project(recorded: &Path, verb: &str) -> Result<(PathBuf, PathBuf), String> {
    let (root, manifest) = resolve_project(recorded)?;
    let names = file_names(&root).map_err(|e| format!("could not read {}: {e}", root.display()))?;
    let mine = manifest.file_name().unwrap_or_default().to_string_lossy().to_string();
    let owns = project::pick_manifest(&names).is_some_and(|only| only.eq_ignore_ascii_case(&mine));
    if !owns {
        return Err(format!(
            "{} holds other projects too, so the Hub will not {verb} the folder; do it in Explorer",
            root.display()
        ));
    }
    Ok((root, manifest))
}

/// Covers larger than this are ignored rather than shipped to the webview: a
/// base64 data URL is ~4/3 the file, held in memory per card. The editor's
/// auto-screenshot is a few hundred KB; this cap only ever meets a giant
/// hand-placed PNG, whose card falls back to the monogram.
const COVER_MAX_BYTES: u64 = 2 * 1024 * 1024;
const PNG_MAGIC: [u8; 8] = [0x89, b'P', b'N', b'G', b'\r', b'\n', 0x1a, b'\n'];

/// The cover image for a project card, as a `data:image/png;base64,` URL.
///
/// Unreal's model outright: an explicit `<ManifestStem>.png` beside the
/// manifest wins (the hand-picked thumbnail), else the editor's auto-written
/// `Saved/AutoScreenshot.png`. Read fresh on every call -- the auto shot
/// changes whenever the editor saves, and at card counts the reread is
/// nothing. None for missing/oversized/non-PNG files: the card has a
/// monogram to fall back to, and a broken image icon would be worse.
pub fn project_cover(recorded: &Path) -> Option<String> {
    use base64::Engine as _;
    let (root, manifest) = resolve_project(recorded).ok()?;
    let stem = manifest.file_stem()?.to_string_lossy().to_string();
    let candidates =
        [root.join(format!("{stem}.png")), root.join("Saved").join("AutoScreenshot.png")];
    for c in candidates {
        // Size from the metadata FIRST: reading a giant hand-placed PNG whole
        // just to throw it away cost its full size in memory and IO on every
        // card refresh.
        let Ok(meta) = std::fs::metadata(&c) else { continue };
        if !meta.is_file() || meta.len() > COVER_MAX_BYTES {
            continue;
        }
        let Ok(bytes) = std::fs::read(&c) else { continue };
        if bytes.len() as u64 <= COVER_MAX_BYTES && bytes.starts_with(&PNG_MAGIC) {
            return Some(format!(
                "data:image/png;base64,{}",
                base64::engine::general_purpose::STANDARD.encode(bytes)
            ));
        }
    }
    None
}

/// What a recursive scan found. `ambiguous` counts folders holding MORE than
/// one .arcproj (the engine refuses those, so the Hub must not guess);
/// `truncated` is the no-silent-caps flag -- true when the visit budget ran
/// out, so "scan finished" can never quietly mean "scan gave up".
pub struct ScanOutcome {
    pub manifests: Vec<PathBuf>,
    pub ambiguous: u32,
    pub truncated: bool,
}

/// How many directories one scan may visit. A mistaken "scan C:\" should end
/// in a truthful truncation report, not a minutes-long hang.
const SCAN_VISIT_BUDGET: usize = 100_000;

/// Walk `dir` recursively collecting every folder that holds exactly one
/// .arcproj (Godot's Scan, with the engine's own one-manifest rule). Skips
/// dot-directories and the DUPLICATE_SKIP names -- build output can contain
/// staged copies of a project that would import as duplicates. Keeps
/// descending past a found project: nothing forbids a project tree holding
/// sample projects deeper in.
pub fn scan_tree(dir: &Path) -> Result<ScanOutcome, String> {
    if !dir.is_dir() {
        return Err(format!("{} is not a folder", dir.display()));
    }
    let mut out = ScanOutcome { manifests: Vec::new(), ambiguous: 0, truncated: false };
    let mut queue = vec![dir.to_path_buf()];
    let mut visited = 0usize;

    while let Some(d) = queue.pop() {
        visited += 1;
        if visited > SCAN_VISIT_BUDGET {
            out.truncated = true;
            break;
        }
        // Unreadable directories are skipped, not fatal: a scan of a big tree
        // WILL meet something access-denied, and one locked folder must not
        // void the whole import.
        let Ok(entries) = std::fs::read_dir(&d) else { continue };

        let mut files = Vec::new();
        let mut manifest_count = 0u32;
        for entry in entries.filter_map(|e| e.ok()) {
            let name = entry.file_name().to_string_lossy().to_string();
            let Ok(ty) = entry.file_type() else { continue };
            if ty.is_dir() {
                if name.starts_with('.') || project::skip_in_duplicate(&name) {
                    continue;
                }
                queue.push(entry.path());
            } else if ty.is_file() {
                if name.to_ascii_lowercase().ends_with(".arcproj") {
                    manifest_count += 1;
                }
                files.push(name);
            }
        }
        if manifest_count > 1 {
            out.ambiguous += 1;
        } else if let Some(m) = project::pick_manifest(&files) {
            out.manifests.push(d.join(m));
        }
    }
    // Stable order for the list and the tests: the queue is depth-first in
    // whatever order read_dir served, which is not a contract.
    out.manifests.sort();
    Ok(out)
}

/// Recursively copy `from` into `to`, skipping `project::DUPLICATE_SKIP`
/// directory names at every depth. Fails on the first IO error and leaves
/// whatever was copied -- the CALLER removes the partial tree, because the
/// rollback promise is part of duplicate_project's user-facing contract, not
/// of a copy primitive.
///
/// Symlinks are neither followed nor copied: a link out of the project tree
/// must not let "duplicate this project" copy an unbounded amount of
/// somewhere else, and a broken link in the copy would be worse than none.
pub fn copy_tree(from: &Path, to: &Path) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|e| format!("could not create {}: {e}", to.display()))?;
    let entries = std::fs::read_dir(from)
        .map_err(|e| format!("could not read {}: {e}", from.display()))?;
    for entry in entries {
        let entry = entry.map_err(|e| format!("could not read {}: {e}", from.display()))?;
        let name = entry.file_name();
        let src = entry.path();
        let ty = entry
            .file_type()
            .map_err(|e| format!("could not inspect {}: {e}", src.display()))?;
        if ty.is_dir() {
            if project::skip_in_duplicate(&name.to_string_lossy()) {
                continue;
            }
            copy_tree(&src, &to.join(&name))?;
        } else if ty.is_file() {
            if project::skip_file_in_duplicate(&name.to_string_lossy()) {
                continue;
            }
            std::fs::copy(&src, to.join(&name))
                .map_err(|e| format!("could not copy {}: {e}", src.display()))?;
        }
    }
    Ok(())
}

/// A duplicate freshly made on disk, ready for the caller to list.
#[derive(Debug)]
pub struct Duplicated {
    pub name: String,
    pub manifest: PathBuf,
    pub abi: u32,
    /// The copy's OWN identity, freshly minted -- never the original's.
    pub guid: String,
}

/// The disk half of Duplicate: copy the project beside itself into the first
/// free "X Copy" / "X Copy N" slot (skipping build output and .git at any
/// depth -- project::DUPLICATE_SKIP has the reasoning per name), rename the
/// manifest and the name inside it, and REGENERATE its guid: a copy is a new
/// project, and two manifests sharing one identity is the Unity productGUID
/// trap. Asset GUIDs, by contrast, copy as-is: they live in asset files/.meta
/// sidecars and are scoped to the project's own registry, which rescans on
/// open, so two projects holding the same asset GUIDs never meet.
/// Any failure past the first write removes new_root -- it did not exist
/// before this call, so the remove can only take the partial copy just made.
pub fn duplicate_project_files(recorded: &Path) -> Result<Duplicated, String> {
    // Owned, not lenient: copying the parent of a manifest that shares its
    // folder with other projects would copy all of them.
    let (root, manifest) = resolve_owned_project(recorded, "duplicate")?;
    let parent = root
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .ok_or_else(|| format!("{} has no parent folder to copy within", root.display()))?;

    let base = project::display_name(&recorded.to_string_lossy());
    let mut pick = None;
    for n in 1..=99 {
        let name = project::copy_name(&base, n);
        let dir = parent.join(&name);
        if !dir.exists() {
            pick = Some((name, dir));
            break;
        }
    }
    let (new_name, new_root) =
        pick.ok_or_else(|| format!("99 copies of {base} already exist here"))?;
    // The suffix can push a legal name over the 64-char cap; gate it like a
    // typed name rather than minting a folder rename_project would refuse.
    if let Some(why) = project::name_error(&new_name) {
        return Err(why);
    }

    // Read and edit the manifest in memory BEFORE any disk work (the
    // rename_project rule: discover a malformed .arcproj while nothing has
    // been created yet).
    let text = std::fs::read_to_string(&manifest)
        .map_err(|e| format!("could not read {}: {e}", manifest.display()))?;
    let fresh_guid = project::new_guid();
    let edited =
        project::set_guid_in_manifest(&project::rename_in_manifest(&text, &new_name)?, &fresh_guid)?;

    let fail = |e: String| {
        let _ = std::fs::remove_dir_all(&new_root);
        Err(e)
    };
    if let Err(e) = copy_tree(&root, &new_root) {
        return fail(e);
    }
    let copied = new_root.join(manifest.file_name().unwrap_or_default());
    let new_manifest = new_root.join(format!("{new_name}.{}", project::MANIFEST_EXT));
    if copied != new_manifest {
        if let Err(e) = std::fs::rename(&copied, &new_manifest) {
            return fail(format!("could not rename {}: {e}", copied.display()));
        }
    }
    // Atomic (temp + rename): a manifest truncated mid-write is a project
    // the engine refuses to open.
    if let Err(e) = store::write_bytes_atomic(&new_manifest, edited.as_bytes()) {
        return fail(format!("could not write {}: {e}", new_manifest.display()));
    }

    Ok(Duplicated {
        name: new_name,
        manifest: new_manifest,
        abi: project::parse_manifest_abi(&edited).unwrap_or(0),
        guid: fresh_guid,
    })
}

/// Rename `from` to `to`, including a CASE-ONLY rename ("mygame" ->
/// "MyGame"), which goes through a temporary sibling in two steps. The old
/// rename_project skipped a case-only folder rename outright (the two names
/// normalise to one key), so the folder kept its old spelling forever while
/// the manifest inside took the new one. Two steps work on every filesystem
/// the Hub meets, including ones where a direct case-only rename is a
/// silent no-op. A failed second step puts the first back.
fn rename_case_safe(from: &Path, to: &Path) -> std::io::Result<()> {
    let same_ignoring_case = from.to_string_lossy().to_lowercase() == to.to_string_lossy().to_lowercase();
    if !same_ignoring_case {
        return std::fs::rename(from, to);
    }
    if from.as_os_str() == to.as_os_str() {
        return Ok(());
    }
    let mut tmp_name = to.file_name().unwrap_or_default().to_os_string();
    tmp_name.push(format!(".hub-rename-{}", std::process::id()));
    let tmp = to.with_file_name(tmp_name);
    std::fs::rename(from, &tmp)?;
    std::fs::rename(&tmp, to).inspect_err(|_| {
        let _ = std::fs::rename(&tmp, from);
    })
}

/// The name an existing entry of `dir` actually has on disk, matched
/// case-insensitively -- a recorded path may spell it differently, and a
/// case-only rename is decided against the DISK spelling.
fn disk_name(dir: &Path) -> Option<String> {
    let want = dir.file_name()?.to_string_lossy().to_lowercase();
    std::fs::read_dir(dir.parent()?)
        .ok()?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().to_string())
        .find(|n| n.to_lowercase() == want)
}

/// The disk half of Rename: the project's folder, its .arcproj file and the
/// `name` inside that manifest, in that order, so the step most likely to
/// fail comes first and every later failure rolls back. `new_name` must
/// already have passed project::name_error (the caller's gate). Returns the
/// new manifest path. The folder must be OWNED (resolve_owned_project):
/// renaming the parent of a manifest that shares it would rename the other
/// projects with it.
pub fn rename_project_files(recorded: &Path, new_name: &str) -> Result<PathBuf, String> {
    let (root, manifest) = resolve_owned_project(recorded, "rename")?;
    let parent = root
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .ok_or_else(|| format!("{} has no parent folder to rename within", root.display()))?;
    let new_root = parent.join(new_name);

    // Read and edit the manifest in memory first: a malformed .arcproj is
    // discovered while nothing has moved.
    let text = std::fs::read_to_string(&manifest)
        .map_err(|e| format!("could not read {}: {e}", manifest.display()))?;
    let edited = project::rename_in_manifest(&text, new_name)?;

    // Same folder ignoring case = a case-only rename (or none): the
    // collision check must not fire on the project's own folder.
    let same_folder =
        new_root.to_string_lossy().to_lowercase() == root.to_string_lossy().to_lowercase();
    if !same_folder && new_root.exists() {
        return Err(format!("{} already exists", new_root.display()));
    }
    let moving_folder = !same_folder || disk_name(&root).is_some_and(|n| n != new_name);

    if moving_folder {
        rename_case_safe(&root, &new_root).map_err(|e| {
            format!(
                "could not rename {} to {}: {e}. Is the project open in the editor?",
                root.display(),
                new_root.display()
            )
        })?;
    }
    let undo_folder = || {
        if moving_folder {
            let _ = rename_case_safe(&new_root, &root);
        }
    };

    // The manifest, now inside the renamed folder.
    let old_manifest = new_root.join(manifest.file_name().unwrap_or_default());
    let new_manifest = new_root.join(format!("{new_name}.{}", project::MANIFEST_EXT));
    let moving_manifest = old_manifest.as_os_str() != new_manifest.as_os_str();
    if moving_manifest {
        if let Err(e) = rename_case_safe(&old_manifest, &new_manifest) {
            undo_folder();
            return Err(format!("could not rename {}: {e}", old_manifest.display()));
        }
    }

    // Atomic (temp + rename): a manifest truncated mid-write is a project
    // the engine refuses to open.
    if let Err(e) = store::write_bytes_atomic(&new_manifest, edited.as_bytes()) {
        if moving_manifest {
            let _ = rename_case_safe(&new_manifest, &old_manifest);
        }
        undo_folder();
        return Err(format!("could not write {}: {e}", new_manifest.display()));
    }
    Ok(new_manifest)
}

// These touch the real filesystem (that is this module's whole job), so each
// test gets its own scratch dir -- same pattern as store.rs's tests.
#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("arcane-hub-resolve-test-{tag}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    // A fixed identity for fixtures; tests that care about regeneration
    // assert against this exact value.
    const SRC_GUID: &str = "a5e0c1de-1111-4222-8333-444455556666";

    fn write_manifest(dir: &Path, name: &str, abi: u32) -> PathBuf {
        let m = dir.join(format!("{name}.arcproj"));
        std::fs::write(&m, project::manifest_json(name, abi, SRC_GUID).unwrap()).unwrap();
        m
    }

    fn mk(dir: &Path, rel: &str) -> PathBuf {
        let p = dir.join(rel);
        std::fs::create_dir_all(&p).unwrap();
        p
    }

    // Smallest possible valid-magic "PNG": the 8 magic bytes. project_cover
    // only checks the magic (decoding is the webview's job), so this is
    // enough to stand in for a real file.
    const TINY_PNG: [u8; 8] = [0x89, b'P', b'N', b'G', b'\r', b'\n', 0x1a, b'\n'];

    #[test]
    fn project_cover_prefers_the_named_png_then_the_auto_screenshot() {
        let dir = scratch("cover");
        let m = write_manifest(&dir, "G", 7);
        assert_eq!(project_cover(&m), None, "no files, no cover -- the monogram's case");

        std::fs::create_dir_all(dir.join("Saved")).unwrap();
        std::fs::write(dir.join("Saved/AutoScreenshot.png"), TINY_PNG).unwrap();
        assert!(project_cover(&m).is_some(), "the editor's auto shot serves");

        std::fs::write(dir.join("G.png"), TINY_PNG).unwrap();
        let named = project_cover(&m).unwrap();
        assert!(named.starts_with("data:image/png;base64,"));
        // Both candidates exist; the hand-placed one must win. Distinguish by
        // content: give the named file an extra byte.
        std::fs::write(dir.join("G.png"), [&TINY_PNG[..], &[0u8]].concat()).unwrap();
        assert_ne!(project_cover(&m).unwrap(), named, "the named PNG wins over the auto shot");
    }

    #[test]
    fn project_cover_refuses_a_file_that_is_not_a_png() {
        let dir = scratch("cover-notpng");
        let m = write_manifest(&dir, "G", 7);
        std::fs::write(dir.join("G.png"), b"JFIF pretending").unwrap();
        assert_eq!(project_cover(&m), None, "magic check, not extension trust");
    }

    #[test]
    fn scan_tree_finds_nested_projects_and_reports_what_it_refused() {
        let dir = scratch("scan");
        // Two real projects at different depths...
        write_manifest(&mk(&dir, "A"), "A", 7);
        write_manifest(&mk(&dir, "deep/nested/B"), "B", 7);
        // ...an ambiguous folder (two manifests -- the engine would refuse it)...
        let amb = mk(&dir, "Amb");
        std::fs::write(amb.join("X.arcproj"), b"{}").unwrap();
        std::fs::write(amb.join("Y.arcproj"), b"{}").unwrap();
        // ...and projects a scan must NOT surface: inside a dot-dir and inside
        // build output (a staged copy would import as a duplicate).
        write_manifest(&mk(&dir, ".hidden/C"), "C", 7);
        write_manifest(&mk(&dir, "Binaries/Staged"), "S", 7);

        let out = scan_tree(&dir).unwrap();
        let names: Vec<String> = out
            .manifests
            .iter()
            .map(|m| m.file_name().unwrap().to_string_lossy().to_string())
            .collect();
        assert_eq!(names, ["A.arcproj", "B.arcproj"], "sorted, and only the real ones");
        assert_eq!(out.ambiguous, 1);
        assert!(!out.truncated);
    }

    #[test]
    fn scan_tree_refuses_a_path_that_is_not_a_folder() {
        let dir = scratch("scan-notdir");
        let f = dir.join("file.txt");
        std::fs::write(&f, b"x").unwrap();
        assert!(scan_tree(&f).is_err());
        assert!(scan_tree(&dir.join("missing")).is_err());
    }

    #[test]
    fn copy_tree_copies_content_and_skips_build_output_at_every_depth() {
        let dir = scratch("copytree");
        let src = dir.join("Src");
        // A UE-shaped project: Content + Source at the root, build output at
        // the root AND under a plugin -- the any-depth case.
        std::fs::create_dir_all(src.join("Content/Maps")).unwrap();
        std::fs::write(src.join("Content/Maps/level.ascene"), b"scene").unwrap();
        std::fs::write(src.join("G.arcproj"), b"{}").unwrap();
        std::fs::create_dir_all(src.join("Binaries")).unwrap();
        std::fs::write(src.join("Binaries/Game.dll"), b"x").unwrap();
        std::fs::create_dir_all(src.join("Plugins/P/Intermediate")).unwrap();
        std::fs::write(src.join("Plugins/P/Intermediate/junk.obj"), b"x").unwrap();
        std::fs::write(src.join("Plugins/P/P.dll"), b"p").unwrap();
        std::fs::create_dir_all(src.join(".git")).unwrap();
        std::fs::write(src.join(".git/HEAD"), b"ref").unwrap();
        // Saved/ travels (the cover lives there) -- but the editor lock is the
        // ORIGINAL's positional identity and must not: a copy made while the
        // original's editor runs would pass lock liveness validation, wear a
        // running badge, and "focus" the wrong editor on double-click.
        std::fs::create_dir_all(src.join("Saved")).unwrap();
        std::fs::write(src.join("Saved/editor.lock"), b"{\"pid\":1,\"start\":2}").unwrap();
        std::fs::write(src.join("Saved/AutoScreenshot.png"), b"png").unwrap();

        let dst = dir.join("Dst");
        copy_tree(&src, &dst).unwrap();

        assert!(dst.join("Content/Maps/level.ascene").is_file());
        assert!(dst.join("G.arcproj").is_file());
        assert!(dst.join("Plugins/P/P.dll").is_file());
        assert!(!dst.join("Binaries").exists(), "root build output is skipped");
        assert!(!dst.join("Plugins/P/Intermediate").exists(), "nested build output too");
        assert!(!dst.join(".git").exists(), "a duplicate is not a second checkout");
        assert!(dst.join("Saved/AutoScreenshot.png").is_file(), "the cover travels");
        assert!(!dst.join("Saved/editor.lock").exists(), "the lock must not");
    }

    #[test]
    fn duplicate_project_files_copies_renames_and_numbers_slots() {
        let dir = scratch("dup");
        let src = mk(&dir, "G");
        std::fs::write(src.join("G.arcproj"), project::manifest_json("G", 8, SRC_GUID).unwrap()).unwrap();
        std::fs::create_dir_all(src.join("Content")).unwrap();
        std::fs::write(src.join("Content/level.ascene"), b"scene").unwrap();
        std::fs::create_dir_all(src.join("Binaries")).unwrap();
        std::fs::write(src.join("Binaries/Game.dll"), b"x").unwrap();

        let dup = duplicate_project_files(&src).unwrap();
        assert_eq!(dup.name, "G Copy");
        assert_eq!(dup.abi, 8, "the abi rides the rewritten manifest");
        assert_eq!(dup.manifest, dir.join("G Copy").join("G Copy.arcproj"),
                   "manifest renamed to match the copy's name");
        let text = std::fs::read_to_string(&dup.manifest).unwrap();
        assert!(text.contains("\"G Copy\""), "the name inside is rewritten: {text}");
        assert!(dir.join("G Copy/Content/level.ascene").is_file(), "content travels");
        assert!(!dir.join("G Copy/Binaries").exists(), "build output does not");

        // The copy is a NEW project: its manifest carries a fresh, valid guid,
        // never the original's (the Unity productGUID trap).
        let copy_guid = project::parse_manifest_guid(&text).expect("the copy has a valid guid");
        assert_ne!(copy_guid, SRC_GUID, "identity must be regenerated, not copied");
        assert_eq!(dup.guid, copy_guid, "the returned guid is the one on disk");

        // The next duplicate takes the next slot.
        let dup2 = duplicate_project_files(&src).unwrap();
        assert_eq!(dup2.name, "G Copy 2");
    }

    #[test]
    fn copy_tree_of_a_missing_source_fails_without_inventing_a_destination_tree() {
        let dir = scratch("copytree-missing");
        let dst = dir.join("Dst");
        assert!(copy_tree(&dir.join("NotThere"), &dst).is_err());
    }

    #[test]
    fn a_manifest_file_resolves_to_its_parent_and_itself() {
        let dir = scratch("file");
        let m = write_manifest(&dir, "G", 7);
        let (root, manifest) = resolve_project(&m).unwrap();
        assert_eq!(root, dir);
        assert_eq!(manifest, m);
    }

    #[test]
    fn a_folder_with_exactly_one_manifest_resolves() {
        let dir = scratch("folder");
        let m = write_manifest(&dir, "G", 7);
        let (root, manifest) = resolve_project(&dir).unwrap();
        assert_eq!(root, dir);
        assert_eq!(manifest, m);
    }

    #[test]
    fn a_folder_with_two_manifests_is_refused_as_ambiguous() {
        // The engine's own rule (Project.cpp): acting on the wrong one of two
        // projects is worse than making the user pick.
        let dir = scratch("ambiguous");
        write_manifest(&dir, "A", 7);
        write_manifest(&dir, "B", 7);
        let err = resolve_project(&dir).unwrap_err();
        assert!(err.contains("exactly one"), "unexpected message: {err}");
    }

    #[test]
    fn a_non_arcproj_file_is_refused() {
        let dir = scratch("wrongfile");
        let f = dir.join("readme.txt");
        std::fs::write(&f, "hello").unwrap();
        assert!(resolve_project(&f).unwrap_err().contains(".arcproj"));
    }

    #[test]
    fn a_path_no_longer_on_disk_is_refused() {
        let dir = scratch("goneness");
        let err = resolve_project(&dir.join("nope")).unwrap_err();
        assert!(err.contains("no longer on disk"), "unexpected message: {err}");
    }

    #[test]
    fn manifest_abi_round_trips_what_create_project_writes() {
        // manifest_json is the writer create_project uses; reading back the
        // abi it stamped is the whole compatibility story in one round trip.
        let dir = scratch("abi");
        let m = write_manifest(&dir, "G", 9);
        assert_eq!(manifest_abi(&m), Some(9), "by manifest file");
        assert_eq!(manifest_abi(&dir), Some(9), "by project folder");
    }

    #[test]
    fn manifest_abi_is_none_for_garbage_or_nothing() {
        let dir = scratch("badabi");
        assert_eq!(manifest_abi(&dir), None, "empty folder proves nothing");
        std::fs::write(dir.join("X.arcproj"), "{ not json").unwrap();
        assert_eq!(manifest_abi(&dir.join("X.arcproj")), None);
    }

    #[test]
    fn project_dir_strips_only_a_manifest_leaf() {
        assert_eq!(project_dir(Path::new("C:/g/G.arcproj")), PathBuf::from("C:/g"));
        assert_eq!(project_dir(Path::new("C:/g")), PathBuf::from("C:/g"));
    }

    // ---- owned resolution (delete / rename / duplicate) -------------------

    #[test]
    fn owned_resolution_refuses_a_manifest_whose_folder_holds_other_projects() {
        // THE CRITICAL BUG: a file-shaped path resolved to its PARENT, and
        // Delete sent the whole parent -- every project in it -- to the bin.
        let dir = scratch("owned-two");
        let a = write_manifest(&dir, "A", 7);
        let b = write_manifest(&dir, "B", 7);
        for (m, verb) in [(&a, "delete"), (&b, "rename"), (&a, "duplicate")] {
            let err = resolve_owned_project(m, verb).unwrap_err();
            assert!(err.contains("holds other projects too"), "unexpected: {err}");
            assert!(err.contains(&format!("will not {verb} the folder")), "unexpected: {err}");
        }
        // Lenient resolution (cover, launch) is unchanged for the same path.
        assert_eq!(resolve_project(&a).unwrap().0, dir);
    }

    #[test]
    fn owned_resolution_accepts_a_folder_whose_only_manifest_is_this_one() {
        let dir = scratch("owned-one");
        let m = write_manifest(&dir, "G", 7);
        assert_eq!(resolve_owned_project(&m, "delete").unwrap(), (dir.clone(), m.clone()));
        assert_eq!(resolve_owned_project(&dir, "delete").unwrap(), (dir.clone(), m.clone()));
        // Case-insensitive about the file's own spelling.
        let shouty = dir.join("g.ARCPROJ");
        assert!(resolve_owned_project(&shouty, "delete").is_ok());
    }

    #[test]
    fn duplicate_refuses_a_folder_shared_with_other_projects() {
        let root = scratch("dup-shared");
        let shared = mk(&root, "Games");
        let a = write_manifest(&shared, "A", 7);
        write_manifest(&shared, "B", 7);
        assert!(duplicate_project_files(&a).unwrap_err().contains("holds other projects too"));
        assert!(!root.join("A Copy").exists(), "nothing was copied");
    }

    // ---- rename_project_files ---------------------------------------------

    fn names_in(dir: &Path) -> Vec<String> {
        let mut v: Vec<String> = std::fs::read_dir(dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().to_string())
            .collect();
        v.sort();
        v
    }

    #[test]
    fn rename_moves_the_folder_the_manifest_and_the_name_inside() {
        let root = scratch("rename");
        let g = mk(&root, "Old");
        let m = write_manifest(&g, "Old", 7);
        std::fs::create_dir_all(g.join("Content")).unwrap();

        let new = rename_project_files(&m, "New").unwrap();
        assert_eq!(new, root.join("New").join("New.arcproj"));
        assert_eq!(names_in(&root), ["New"], "the old folder is gone");
        assert!(root.join("New/Content").is_dir(), "content travels with the folder");
        let text = std::fs::read_to_string(&new).unwrap();
        assert_eq!(project::parse_manifest_guid(&text).as_deref(), Some(SRC_GUID), "only the name changes");
        assert!(text.contains("\"New\""), "{text}");
    }

    #[test]
    fn a_case_only_rename_renames_the_folder_too() {
        // The old command compared normalised keys and skipped the folder
        // rename, leaving "mygame/MyGame.arcproj" forever.
        let root = scratch("rename-case");
        let g = mk(&root, "mygame");
        let m = write_manifest(&g, "mygame", 7);

        let new = rename_project_files(&m, "MyGame").unwrap();
        assert_eq!(names_in(&root), ["MyGame"], "the folder takes the new case on disk");
        assert_eq!(names_in(&root.join("MyGame")), ["MyGame.arcproj"]);
        assert!(std::fs::read_to_string(new).unwrap().contains("\"MyGame\""));
    }

    #[test]
    fn rename_refuses_a_name_that_is_already_taken() {
        let root = scratch("rename-collide");
        let m = write_manifest(&mk(&root, "A"), "A", 7);
        mk(&root, "B");
        let err = rename_project_files(&m, "B").unwrap_err();
        assert!(err.contains("already exists"), "{err}");
        assert!(m.is_file(), "nothing moved");
    }

    #[test]
    fn rename_refuses_a_folder_shared_with_other_projects() {
        let root = scratch("rename-shared");
        let shared = mk(&root, "Games");
        let a = write_manifest(&shared, "A", 7);
        write_manifest(&shared, "B", 7);
        assert!(rename_project_files(&a, "Z").unwrap_err().contains("holds other projects too"));
        assert_eq!(names_in(&root), ["Games"], "the shared folder is untouched");
    }

    #[test]
    fn project_cover_skips_an_oversized_file_without_reading_it_whole() {
        let dir = scratch("cover-big");
        let m = write_manifest(&dir, "G", 7);
        let mut big = TINY_PNG.to_vec();
        big.resize(COVER_MAX_BYTES as usize + 1, 0);
        std::fs::write(dir.join("G.png"), &big).unwrap();
        assert_eq!(project_cover(&m), None, "over the cap: the monogram serves");
    }
}
