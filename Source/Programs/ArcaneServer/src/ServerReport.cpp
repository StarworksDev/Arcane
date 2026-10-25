#include "ServerReport.hpp"

#include <Json.hpp>

#include <fstream>

namespace Arcane::Server
{
    std::string ServerReport::ToJson() const
    {
        nlohmann::json j;
        j["schemaVersion"]             = kSchemaVersion;
        j["host"]                      = "ArcaneServer";
        j["netMode"]                   = netMode;
        j["isDedicatedServerProcess"]  = isDedicatedServerProcess;

        j["project"] = { { "opened",    projectOpened },
                          { "name",     projectName },
                          { "engineAbi", projectAbi } };

        j["module"] = { { "path",       modulePath },
                         { "loaded",     moduleLoaded },
                         { "generation", moduleGeneration } };

        j["framesTicked"] = framesTicked;
        j["fixedDt"]      = fixedDt;
        j["extractFrameCalls"] = extractFrameCalls;

        j["systems"] = { { "fixedUpdate",          fixedUpdate },
                          { "update",               update },
                          { "render",               render },
                          { "hasPhysics",            hasPhysics },
                          { "hasPhysicsWorld",       hasPhysicsWorld },
                          { "hasPropagation",        hasPropagation },
                          { "hasRenderSubmission",   hasRenderSubmission } };

        j["presentation"] = { { "clientAttached",             clientAttached },
                               { "clientDllLoadedAtBoot",      clientDllLoadedAtBoot },
                               { "clientDllLoadedAfterModule", clientDllLoadedAfterModule } };

        j["exitReason"] = exitReason;

        // error_handler_t::replace, not the default throw -- same reasoning as
        // VerifyReport::ToJson: this report is the ONE thing an agent (or a
        // Servitor-shaped consumer, parsing it without linking the engine) has
        // to learn what this run observed, so a malformed byte in a string
        // field must degrade to U+FFFD in the output, never throw out of a
        // caller that is often already exiting.
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    bool ServerReport::WriteTo(const std::string& path) const
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        const std::string json = ToJson();
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        return static_cast<bool>(out);
    }
}
