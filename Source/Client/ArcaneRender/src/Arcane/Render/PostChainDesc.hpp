#pragma once

// The scene post chain as compiled bytes (post arc, slice 2) -- what
// PostChainCache publishes and the graph's PostChainNode records. Its own
// header-only file (F5 plan 1, Task 4) so the render packet
// (RenderFramePacket.hpp) can carry a PostChainDesc without the cache class.

#include <cstdint>
#include <memory>
#include <vector>

namespace Arcane
{
    class MaterialInstance;
    class MaterialTemplate;

    // =====================================================================
    // THE COMPILED CHAIN AS BYTES -- the twin of Material2DDesc::vsBytes/
    // psBytes (Batcher2D.hpp), and it exists for the identical reason: a
    // device can build a pipeline from bytecode but cannot adopt a compiled
    // shader OBJECT it did not create. The graph's PostChainNode builds its
    // own NRI pipelines from exactly these bytes, so a recompile and a render
    // always agree about which shader ran.
    //
    // ONE target, not both: the producer already picks dxil-or-spirv by the
    // process's GraphicsBackend and the render vehicle runs on that same
    // backend (RuntimeApp builds both from one HostConfig).
    //
    // THE ONLY COMPILED FORM PostChainCache publishes.
    // =====================================================================
    struct PostChainPassDesc
    {
        std::shared_ptr<const std::vector<std::uint8_t>> vsBytes;
        std::shared_ptr<const std::vector<std::uint8_t>> psBytes;
        // Chain indices feeding this pass's InputTexture(N) slots, in slot
        // order: an EARLIER pass index, or kSceneInput (MaterialSource.hpp)
        // for the external scene colour. Already validated by
        // BuildMaterialChainSource, so a real index is always < this pass's.
        std::vector<std::uint32_t> inputs;
    };

    struct PostChainDesc
    {
        // The ONE merged layout + values every pass shares
        // (BuildMaterialChainSource's contract), so one packed CB and one
        // texture table serve the whole chain.
        std::shared_ptr<const MaterialTemplate> templ;
        std::shared_ptr<const MaterialInstance> instance;
        // The UNIFORM InputTexture decl count every pass's source carries
        // (MaterialChainBuildResult::chainInputSlots) -- the binding layout's
        // shape, not any one pass's wiring.
        std::uint32_t chainInputSlots = 1;
        std::vector<PostChainPassDesc> passes;   // chain order; passes[0] is the base
    };
}
