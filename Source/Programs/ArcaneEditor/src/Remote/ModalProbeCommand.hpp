#pragma once
// editor.modal.probe (Task 5, review fix round 1): a Dist-stripped test command
// that arms the two competing dockspace modals (a queued error and an open create
// dialog) and reads which one DrawModals let draw, so Servitor can witness
// ModalGate. The command holds the EditorApp installed at the arming site and
// calls only EditorApp::ArmModalProbe / DismissModalProbe / ModalProbeKind.
#if !defined(ARC_BUILD_DIST)

namespace Arcane::Editor
{
class EditorApp;

// Main thread. The editor installs itself at its remote arming site; a command only runs
// while that session is up (the app outlives it), so no teardown clear is needed.
void SetModalProbeApp(EditorApp* app);

// editor.modal.probe op=arm|dismiss|drawn; module "" (host), safe to call twice.
//   arm     queue "Probe Error" and open the Material create dialog; data {createOpen}.
//           Errors (create refused: no project) when the dialog did not open.
//   dismiss pop the front error, as the modal's OK button does.
//   drawn   data {kind}: the winner the previous DrawModals stored
//           (Error, UnsavedScene, CreateAsset, None).
void RegisterModalProbeCommand();

} // namespace Arcane::Editor

#endif
