#include "Settings/EditorThumbnailSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

#include <bit>
#include <format>

namespace Arcane::Editor
{
    float ThumbnailCheckerCell(const EditorThumbnailSettings& s)
    {
        return static_cast<float>(s.size) / 4.0f;
    }

    std::string ThumbnailCacheSuffix(const EditorThumbnailSettings& s)
    {
        const EditorThumbnailSettings d{};
        const auto same = [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); };
        if (s.size == d.size && same(s.time, d.time) && same(s.meshFovDegrees, d.meshFovDegrees) &&
            same(s.framingMargin, d.framingMargin))
            return {};
        // Bit patterns, not decimal text: two floats that print alike but
        // render differently must not share a file.
        return std::format(".s{}t{:08x}f{:08x}m{:08x}", s.size, std::bit_cast<std::uint32_t>(s.time),
                           std::bit_cast<std::uint32_t>(s.meshFovDegrees), std::bit_cast<std::uint32_t>(s.framingMargin));
    }

    ARC_REFLECT_TYPE(EditorThumbnailSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.thumbnail", SettingScope::Project, ApplyMode::Restart, Audience::Editor)
        ARC_REFLECT_FIELD(EditorThumbnailSettings, size)
            ARC_REFLECT_ATTR(DisplayName, "Thumbnail size") ARC_REFLECT_ATTR(Range, 16.0, 512.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "asset browser preview resolution png")
            ARC_REFLECT_ATTR(Tooltip, "Side, in pixels, of the Asset Browser's material and mesh thumbnails: the rendered "
                                      "image, the PNG cached in Saved/Thumbnails and the loader's cap. Read when the editor "
                                      "starts; a change re-harvests the thumbnails.")
        ARC_REFLECT_FIELD(EditorThumbnailSettings, time)
            ARC_REFLECT_ATTR(DisplayName, "Thumbnail time") ARC_REFLECT_ATTR(Range, 0.0, 3600.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "asset browser preview clock animation")
            ARC_REFLECT_ATTR(Tooltip, "The fixed shader clock, in seconds, every material thumbnail renders at, so an "
                                      "animated material always yields the same picture. Read when the editor starts.")
        ARC_REFLECT_FIELD(EditorThumbnailSettings, maxRetries)
            ARC_REFLECT_ATTR(DisplayName, "Renderer failures allowed") ARC_REFLECT_ATTR(Range, 1.0, 100.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "asset browser preview retries give up")
            ARC_REFLECT_ATTR(Tooltip, "How many times the thumbnail renderer may fail and be rebuilt before thumbnails are "
                                      "switched off for the rest of the session. Also how many frames without upload progress a "
                                      "thumbnail or a cover waits before giving up on that picture.")
        ARC_REFLECT_FIELD(EditorThumbnailSettings, meshFovDegrees)
            ARC_REFLECT_ATTR(DisplayName, "Mesh field of view") ARC_REFLECT_ATTR(Range, 10.0, 120.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "asset browser preview camera fov")
            ARC_REFLECT_ATTR(Tooltip, "Vertical field of view, in degrees, of the camera that frames a mesh thumbnail. Read "
                                      "when the editor starts.")
        ARC_REFLECT_FIELD(EditorThumbnailSettings, framingMargin)
            ARC_REFLECT_ATTR(DisplayName, "Mesh framing margin") ARC_REFLECT_ATTR(Range, 0.0, 2.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "asset browser preview camera padding fit")
            ARC_REFLECT_ATTR(Tooltip, "Room left around a mesh past a tight fit of its bounds, as a fraction of the "
                                      "distance (0.15 = 15%). Read when the editor starts.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorThumbnailSettings);
}
