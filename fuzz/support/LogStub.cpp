// Fuzz-only stand-in for Source/Runtime/ArcaneCore/src/Arcane/Base/Log.cpp: the engine log
// is a silent spdlog logger, so a harness links the parser it targets without
// the backlog/file-sink/crash machinery Log.cpp drags in. Only the entry
// points the targeted sources call are provided.

#include <Arcane/Base/Log.hpp>
#include <Arcane/Util/LogServerSettingsData.hpp>

#include <spdlog/sinks/null_sink.h>

#include <memory>

namespace Arcane::Log
{
    spdlog::logger* Engine()
    {
        static const std::shared_ptr<spdlog::logger> logger =
            std::make_shared<spdlog::logger>("fuzz", std::make_shared<spdlog::sinks::null_sink_mt>());
        return logger.get();
    }

    // CVarRegistry::UnregisterModuleRange calls this; the stub logger has no Mosaic level targets.
    void UnregisterMosaicLevelTargetsInRange(const void*, std::size_t) noexcept {}
}

namespace Arcane
{
    // Logger.hpp (pulled in by SecureRandom.cpp) reads these; defaults are fine, and
    // the real definitions (Util/LogServerSettings.cpp) would drag in the CVar registry.
    LogServerSettings PublishedLogServerSettings() { return {}; }
    LogServerFileSettings PublishedLogServerFileSettings() { return {}; }
}
