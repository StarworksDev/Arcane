#pragma once
// editor.gizmo.probe (Task 6, review fix round 1): a Dist-stripped test command
// that arms a two-target gizmo drag and deletes one target, so the same frame's
// UpdateGizmoInteraction takes its abandoned-drag branch. A headless editor has no
// viewport press, so the probe fills the GizmoDrag fields the press path fills
// (EditorApp::ArmGizmoAbandonProbe). Servitor then reads the survivor.
#if !defined(ARC_BUILD_DIST)

namespace Arcane::Editor
{
class EditorApp;

// Main thread. The editor installs itself at its remote arming site.
void SetGizmoProbeApp(EditorApp* app);

// editor.gizmo.probe (no args); module "" (host), safe to call twice. Edit mode only.
//   data {survivor:"GizmoProbeA", beforeX:0, sentinelX:5}: the survivor's x after the
//   abandon must equal beforeX and not sentinelX. host.unavailable when there is no
//   live scene root or a drag is already open.
void RegisterGizmoProbeCommand();

} // namespace Arcane::Editor

#endif
