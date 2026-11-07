// See UploadNode.hpp for the contract. Include order: NRI headers first (NriCommon.hpp).
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include "UploadNode.hpp"

#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#undef ERROR

#include <memory>
#include <vector>

namespace Arcane
{
    namespace
    {
        struct UploadTextureSlot
        {
            NriTextureCache::UploadJob job;
            RgTexture                  handle;
        };

        struct UploadMeshSlot
        {
            NriMeshBufferCache::UploadJob job;
            RgBuffer                      vertex;
            RgBuffer                      index;
        };

        // One frame's snapshot plus the graph handles minted for it. Shared by the
        // two nodes' lambdas (the second one needs the buffer handles the first
        // minted) and by the exec fn.
        struct UploadFrame
        {
            std::vector<UploadTextureSlot> textures;
            std::vector<UploadMeshSlot>    meshes;
        };
    }

    bool AddUploadNodes(RenderGraph& graph, NriTextureCache* textures, NriMeshBufferCache* meshes)
    {
        auto frame = std::make_shared<UploadFrame>();
        if (textures)
            for (const NriTextureCache::UploadJob& job : textures->BeginUploadFrame())
                frame->textures.push_back({ job, {} });
        if (meshes)
            for (const NriMeshBufferCache::UploadJob& job : meshes->BeginUploadFrame())
                frame->meshes.push_back({ job, {}, {} });
        if (frame->textures.empty() && frame->meshes.empty())
            return false;

        graph.AddNode("upload", RenderGraph::NodeKind::Copy,
            [frame](RenderGraphBuilder& builder)
            {
                // The entry is the unknown state (a fresh object); the exit is the
                // state every sampler of a cached texture assumes: ShaderRead.
                const nri::AccessLayoutStage entry{ nri::AccessBits::NONE, nri::Layout::UNDEFINED,
                                                    nri::StageBits::ALL };
                const nri::AccessLayoutStage exit{ nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                                   nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER
                                                       | nri::StageBits::COMPUTE_SHADER };
                for (UploadTextureSlot& slot : frame->textures)
                {
                    slot.handle = builder.ImportTexture("upload texture", slot.job.texture, entry, exit,
                                                        /*persistent=*/false);
                    builder.Write(slot.handle, RgUsage::CopyDst);
                }
                for (UploadMeshSlot& slot : frame->meshes)
                {
                    slot.vertex = builder.ImportBuffer("upload vertex buffer", slot.job.vertexBuffer,
                                                       slot.job.vertexBytes);
                    slot.index  = builder.ImportBuffer("upload index buffer", slot.job.indexBuffer,
                                                       slot.job.indexBytes);
                    builder.Write(slot.vertex, RgUsage::CopyDst);
                    builder.Write(slot.index, RgUsage::CopyDst);
                }
            },
            [frame, textures, meshes](RenderGraphNodeContext& context)
            {
                // Classification (ring / retry / dedicated) is each cache's job, here,
                // after ring.BeginFrame -- never in the setup fn above.
                for (const UploadTextureSlot& slot : frame->textures)
                    textures->RecordUpload(slot.job, context.cmd, context.ring);
                for (const UploadMeshSlot& slot : frame->meshes)
                    meshes->RecordUpload(slot.job, context.cmd, context.ring);
            });

        if (!frame->meshes.empty())
        {
            // No work of its own: the Reads below are what make the compiler emit the
            // CopyDst -> VertexBuffer / IndexBuffer barrier for each uploaded buffer
            // (the executor is the only CmdBarrier site, and ImportBuffer has no exit
            // state to do it with).
            graph.AddNode("upload-settle", RenderGraph::NodeKind::Compute,
                [frame](RenderGraphBuilder& builder)
                {
                    for (const UploadMeshSlot& slot : frame->meshes)
                    {
                        builder.Read(slot.vertex, RgUsage::VertexBuffer);
                        builder.Read(slot.index, RgUsage::IndexBuffer);
                    }
                },
                [](RenderGraphNodeContext&) {});
        }
        return true;
    }
}
