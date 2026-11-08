#pragma once

// editor.thumbnail.* -- the Asset Browser's material and mesh thumbnails
// (settings sweep S6-39, inventory R1 "thumbnails" and Part 3's mesh framing
// margin). Project scope: the thumbnails are a project's Saved/Thumbnails
// cache, and the thumbnail golden set is bound to these defaults.
//
// LATCHED: MaterialPreviewHarvester copies the block once, when it is created,
// so a change applies on the next editor start (Restart) and never mixes two
// thumbnail sizes in one session.
//
// Plain struct, no settings machinery: ARC_SETTINGS and the reflection live in
// EditorThumbnailSettings.cpp. Every default is the pre-sweep literal it
// replaced.

#include <cstdint>
#include <string>

namespace Arcane::Editor
{
    struct EditorThumbnailSettings
    {
        std::uint32_t size           = 64;      // px; the rendered target, the PNG and the loader's cap
        float         time           = 0.35f;   // the fixed material clock every thumbnail renders at
        std::int32_t  maxRetries     = 3;       // vehicle failures before the harvester gives up for the session; ALSO the frames without upload progress a thumbnail or cover waits before giving up on that picture
        float         meshFovDegrees = 35.0f;   // vertical FOV of the mesh thumbnail camera
        float         framingMargin  = 0.15f;   // breathing room past a tight fit of a mesh's bounds (0.15 = 15%)
    };

    // The thumbnail checkerboard cell: a quarter of the side (16 at 64), the
    // shader editor's 32-at-512 read at a sixteenth of the area.
    [[nodiscard]] float ThumbnailCheckerCell(const EditorThumbnailSettings& s);

    // The persisted PNG's file-name suffix, keyed by every setting that changes
    // the picture (size, time, mesh FOV, framing margin). EMPTY at the defaults,
    // so the default cache is the pre-sweep "<guid>.png"; any other value names
    // a different file, so a PNG harvested under other settings is never served.
    [[nodiscard]] std::string ThumbnailCacheSuffix(const EditorThumbnailSettings& s);
}
