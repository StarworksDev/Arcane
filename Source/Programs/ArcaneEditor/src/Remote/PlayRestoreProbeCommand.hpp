#pragma once
// editor.play.failnextrestore (Task 7, review fix round 1): a Dist-stripped test
// command that arms PlaySession::FailNextRestoreForTesting through
// EditorApp::ArmFailNextRestore, so Servitor can force the failed-Stop branch
// (play.stop answers play.refused, Play stays) and the second Stop that restores.
// The name has no hyphen: command names are dotted [a-z0-9]+ segments (spec s5.1).
#if !defined(ARC_BUILD_DIST)

namespace Arcane::Editor
{
class EditorApp;

// Main thread. The editor installs itself at its remote arming site.
void SetPlayRestoreProbeApp(EditorApp* app);

// editor.play.failnextrestore (no args); module "" (host), safe to call twice.
// One shot: the next Stop's restore fails, the Stop after it restores.
void RegisterPlayRestoreProbeCommand();

} // namespace Arcane::Editor

#endif
