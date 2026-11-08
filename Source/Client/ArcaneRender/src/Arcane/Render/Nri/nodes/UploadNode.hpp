#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// UploadNode -- the graph's `upload` copy node (3D-readiness Plan C, Task 5).
//
// WHAT IT REPLACES. Mesh and texture residency used to upload through NRI's
// HelperInterface::UploadData, which submits and WAITS INSIDE a Resolve call
// (a blocking device wait in the middle of declaring a frame). A Resolve of a
// payload with no bytes on the GPU now creates the device object, queues the
// upload and answers NOT-READY; this node, declared FIRST in the frame, records
// the copies into the frame's own command buffer instead.
//
// THE CONTRACT (user ruling 2026-11-06, option A: deferral):
//   * The node declares a SNAPSHOT of the queue taken BEFORE this frame's
//     declaration began. An entry queued during this frame's declaration waits
//     for the next frame.
//   * An entry becomes READY only after its upload was recorded and the frame
//     submitted, so its first consumer is a LATER frame's declaration. The
//     expected timeline is: frame N queues, N+1 uploads, N+2 draws.
//   * No consumer ever binds, samples or draws a not-ready entry, so the upload
//     frame declares NO consumer read of the uploaded resources -- there is
//     nothing to suppress on a retry.
//
// BARRIERS. The executor stays the ONLY CmdBarrier site. A texture is imported
// at the unknown state and leaves the frame in its ShaderRead exit triple. A
// buffer import takes no exit state, so a second node, `upload-settle`, reads
// the vertex and index buffers as VertexBuffer / IndexBuffer: the compiler
// derives the CopyDst -> VERTEX_BUFFER / INDEX_BUFFER barrier and later frames,
// which do not re-import the buffers, find them already in the state their
// draws bind them in.
//
// CLASSIFICATION happens in the exec fn, after ring.BeginFrame: ring / retry /
// dedicated staging (NriUploadStaging.hpp). The setup fn only declares.

#include <Arcane/Render/Nri/NriUploadMode.hpp>   // UploadBudget

namespace Arcane
{
    class NriMeshBufferCache;
    class NriTextureCache;
    class RenderGraph;

    // Takes the caches' snapshot and declares `upload` (and `upload-settle`
    // when a mesh is in the snapshot). Declares nothing, and costs nothing, when
    // neither cache has a queued upload. Either cache may be null. Returns
    // whether a node was declared.
    //
    // `budget` is this frame's ring budget (render.upload.frameBudgetPercent of a slot): the snapshot
    // takes queued entries FIFO while their staging bytes fit it, and a payload over its cap uploads
    // through dedicated staging. The rest stay queued for the next frame.
    ARC_RENDER_API bool AddUploadNodes(RenderGraph& graph, NriTextureCache* textures,
                                       NriMeshBufferCache* meshes, UploadBudget budget);
}
