#include "Documents/ShaderEditorDocument.hpp"
#include "Input/EditorActions.hpp"

#include "Documents/CustomBodyPreview.hpp"       // the Custom node's capped body preview (editor.shader.bodyPreview*)
#include "Documents/MaterialSpherePreview.hpp"   // the mesh-surface preview sphere, shared with the thumbnails (T3-D6)
#include "Documents/ShaderGraphCategoryColors.hpp"   // GraphCategoryHeaderColor: the node title band fill (s5.1.4)
#include "Documents/ShaderGraphPinLegend.hpp"   // the canvas's pin colour legend (T3-D1)
#include "Documents/ShaderGraphPinTypes.hpp"    // pin palette + paint rule + type/tooltip words (T3-D1)
#include "Panels/AssetPanelModel.hpp"
#include "Panels/AssetReferenceField.hpp"   // AssetRefRow: the texture param row (s5.3)
#include "Widgets/CanvasEditScope.hpp"   // CanvasCreateScope/CanvasDeleteScope: the unconditional-End rule
#include "Widgets/CanvasPopupScope.hpp"
#include "Widgets/ColorPickerPopup.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"   // StableTextEdit: the stable-buffer text-commit helper
#include "Widgets/GraphCanvasBackdrop.hpp"   // DrawGraphCanvasBackdrop -- the pre-ed::Begin grid blit
#include "Widgets/GraphCanvasStyle.hpp"   // node chrome metrics + grid palette + accents -- shared with the Graph lens
#include "Widgets/GraphFit.hpp"         // GraphFitToContent -- capped fit-on-open (s4.5)
#include "Widgets/GraphPinDot.hpp"       // DrawGraphPinDot -- the filled/ring port dot, paint only
#include "Widgets/GraphWire.hpp"         // bezier/lerp/brighten/view-scale + the links channel -- ditto
#include "Widgets/GraphZoomLevels.hpp"   // ApplyZoomLevels (editor.graph.zoomLevels) -- shared with the Graph lens
#include "Widgets/UiMetrics.hpp"         // Ui::Px: the header gap follows editor.ui.scale
#include "Settings/GraphCanvasSettings.hpp"   // editor.graph.*: header gap, cull band, pin dot, selection modifier
#include "Settings/DocumentSettings.hpp"      // editor.shader.* / editor.preview.*: caps, drag speeds, the checker
#include "Settings/InspectorSettings.hpp"     // editor.inspector.materialPreviewFraction / nodePageMinTextRun
#include "Widgets/IconsLucide.h"   // ICON_LC_EYE: the pass-canvas preview-cut marker
#include "Widgets/MaterialParamWidgets.hpp"
#include "Widgets/PropertyGrid.hpp"   // the material page's sections (s5.3)

// The preview vehicle. Include-order note
// for anything moved above it: this header reaches <NRI.h> and
// Extensions/NRIDeviceCreation.h, whose nri::Message enumerator is literally
// named ERROR -- the same collision every file under Render/Nri documents. It
// sits here (rather than first) because it is what EditorApp.hpp already does
// and the same TUs already compile that way.
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/RenderOverlayNode.hpp>   // InvalidateUserTextureNow
#include <Arcane/Host/HostConfig.hpp>                 // CreateOffscreen's knobs

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Assert.hpp>   // ARC_ENSURE: SurfaceOf's unknown-index guard
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Assets/AssetId.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Material/MaterialGraph.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/ShaderConventions.hpp>

#include <Astra/Reflection/Attribute.hpp>   // Astra::Range: the Alpha cutoff row

#include <imgui.h>
// AddSettingsHandler / FindSettingsHandler (:3502-3504), ImGuiSettingsHandler
// (:2212-2225) and MarkIniSettingsDirty (:3499) are internal-only -- ImGui's
// ini extension point has never been in the public header.
#include <imgui_internal.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <Arcane/Core/Constant.hpp>

namespace Arcane::Editor
{
    namespace
    {
        // editor.graph.dragSpeed: a constant / parameter default's per-pixel
        // drag step on a graph node (S6-35).
        [[nodiscard]] float NodeDragSpeed() { return Arcane::Settings<GraphCanvasSettings>().dragSpeed; }

        // The mesh-surface preview box's caption (T3-D6): the line that used
        // to BE the whole preview, kept as the honest note under the sphere.
        constexpr const char* kMeshPreviewCaption =
            "Mesh material: not compiled here -- preview it on a mesh in the viewport";

        // The caption's height wrapped at `width`, plus the item spacing above it.
        float MeshPreviewCaptionHeight(float width)
        {
            return ImGui::CalcTextSize(kMeshPreviewCaption, nullptr, false, (std::max)(1.0f, width)).y
                 + ImGui::GetStyle().ItemSpacing.y;
        }

        // s5.3 (9.28 #25): the preview square's height cap, as a share of the
        // page (editor.inspector.materialPreviewFraction, S6-37).
        float MaterialPreviewFraction()
        {
            return Arcane::Settings<InspectorSettings>().materialPreviewFraction;
        }

        const AssetRefServices& NoAssetRefServices()
        {
            static const AssetRefServices none{};   // headless documents: the cell's null-services behaviour (s4.2)
            return none;
        }
        // The label cell of the row just drawn is hovered: the value widget is
        // LastItemData, so its rect bounds the row and its left edge ends the label.
        bool LabelCellHovered()
        {
            const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax(), m = ImGui::GetMousePos();
            return ImGui::IsWindowHovered() && m.y >= lo.y && m.y < hi.y && m.x < lo.x && m.x >= ImGui::GetWindowPos().x;
        }

        // m_surface is an INDEX -- ImGui::Combo hands back an int -- and these
        // three functions are the ONLY conversion between it and the real
        // MaterialSurface / .arcmat `kind`. They are TOTAL in every direction
        // on purpose.
        //
        // WHY THAT MATTERS (F2a): the pre-F2a SurfaceOf mapped "anything that
        // is not 1" onto Fullscreen, and the two initialization sites collapsed
        // MaterialSurfaceForKind(kind) to `== Sprite ? 1 : 0`. The instant
        // MaterialSurface gained a third enumerator (Task 3), a "mesh"-kind
        // .arcmat opened here read as index 0 -- it DISPLAYED "Fullscreen",
        // compiled under the fullscreen template, and one touch of the picker
        // rewrote its `kind` to match, silently re-kinding the asset. An int is
        // exactly the laundering MaterialSource.cpp's two ARC_ENSURE guards
        // cannot catch: the enum is already gone before they see it.
        ARC_CONSTANT("ID space: the Surface combo's item indices (MaterialSurface <-> row)")
        constexpr int kSurfaceFullscreen = 0;
        ARC_CONSTANT("ID space: the Surface combo's item indices (MaterialSurface <-> row)")
        constexpr int kSurfaceSprite     = 1;
        ARC_CONSTANT("ID space: the Surface combo's item indices (MaterialSurface <-> row)")
        constexpr int kSurfaceMesh       = 2;

        Arcane::MaterialSurface SurfaceOf(int surface)
        {
            if (surface == kSurfaceSprite) return Arcane::MaterialSurface::Sprite;
            if (surface == kSurfaceMesh)   return Arcane::MaterialSurface::Mesh;
            // Fullscreen is still the fallback -- but an index that is none of
            // the three is a bug in this file, not a material, so it says so
            // once rather than answering as if it knew.
            ARC_ENSURE(surface == kSurfaceFullscreen,
                       "ShaderEditorDocument: unknown preview-surface index -- "
                       "falling back to Fullscreen");
            return Arcane::MaterialSurface::Fullscreen;
        }

        int SurfaceIndexOf(Arcane::MaterialSurface surface)
        {
            if (surface == Arcane::MaterialSurface::Sprite) return kSurfaceSprite;
            if (surface == Arcane::MaterialSurface::Mesh)   return kSurfaceMesh;
            return kSurfaceFullscreen;
        }

        // The .arcmat `kind` string a surface index re-kinds a BASE material
        // to. Paired with SurfaceIndexOf so the round trip is closed: the
        // picker can only ever write a kind that maps straight back to the
        // index it was showing.
        const char* KindForSurfaceIndex(int surface)
        {
            if (surface == kSurfaceSprite) return "sprite";
            if (surface == kSurfaceMesh)   return "mesh";
            return "fullscreen";
        }

        // The mesh kind's FIXED param decl list -- F2a's two declared params,
        // `baseColor`/`albedo` (F2a spec sec 2's table; MeshMaterialCache.hpp's
        // own header comment is the consumer-side citation). A mesh material
        // carries no snippet, so there is no //@param text to
        // parse a MaterialTemplate out of the usual way; this is the mesh
        // decl path the params panel needs instead, built once per Rebuild()
        // and fed through the SAME MaterialTemplate/MaterialInstance/
        // DrawParamsPanel machinery every other surface uses -- baseColor
        // draws through the ColorEdit arm, albedo through DrawTextureParam,
        // both by ordinary MatParamType dispatch (WidgetFor). No sourceHash
        // identity matters here (nothing compiles against it), so it is 0.
        Arcane::MaterialTemplate MeshParamTemplate(const std::string& name)
        {
            std::vector<Arcane::ParamDecl> decls;
            Arcane::ParamDecl baseColor;
            baseColor.name = "baseColor";
            baseColor.type = Arcane::MatParamType::Color;
            baseColor.def  = Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f);
            decls.push_back(baseColor);
            Arcane::ParamDecl albedo;
            albedo.name = "albedo";
            albedo.type = Arcane::MatParamType::Texture;
            albedo.def  = Arcane::MatParamValue::MakeTexture(Arcane::Guid{});
            decls.push_back(albedo);
            return Arcane::MaterialTemplate::Build(name, /*sourceHash=*/0, std::move(decls));
        }

        // One param edit as an undo step. The live edit already happened (the
        // ICommand contract); Undo restores the BEFORE override state (value or
        // no-override), Redo re-applies the AFTER. Doc-identity (review M3): the
        // command holds the DOCUMENT weakly and forwards by name hash to its
        // CURRENT instance -- a recompile swaps the instance but the step stays
        // live; closing the document expires the anchor (the step goes inert).
        class ParamEditCommand final : public Arcane::ICommand
        {
        public:
            ParamEditCommand(std::weak_ptr<ShaderEditorDocument*> anchor,
                             std::uint32_t nameHash, std::string label,
                             bool hadBefore, Arcane::MatParamValue before,
                             bool hasAfter, Arcane::MatParamValue after)
                : m_anchor(std::move(anchor)), m_nameHash(nameHash),
                  m_label(std::move(label)), m_hadBefore(hadBefore),
                  m_before(before), m_hasAfter(hasAfter), m_after(after)
            {
            }

            void Undo() override { Apply(m_hadBefore, m_before); }
            void Redo() override { Apply(m_hasAfter, m_after); }
            const char* Label() const override { return m_label.c_str(); }
            // Spec s3.3: a document step never dirties the scene, and it is
            // EXPIRED once its document closed (the anchor died).
            bool AffectsScene() const override { return false; }
            bool IsExpired() const override
            {
                const auto doc = m_anchor.lock();
                return !doc || !*doc;
            }

        private:
            void Apply(bool hasValue, const Arcane::MatParamValue& value)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyParamEdit(m_nameHash, hasValue, value);
            }

            std::weak_ptr<ShaderEditorDocument*> m_anchor;
            std::uint32_t m_nameHash;
            std::string   m_label;
            bool          m_hadBefore;
            Arcane::MatParamValue m_before;
            bool          m_hasAfter;
            Arcane::MatParamValue m_after;
        };

        // One mesh-metadata edit (blend / alpha cutoff / two-sided) as an undo
        // step: ParamEditCommand's shape over MaterialAssetData's three fields
        // rather than an instance param. The live edit already happened; Undo
        // restores the BEFORE state, Redo re-applies the AFTER state as it
        // LANDED (post-clamp). Doc-identity through the same weak anchor: the
        // metadata lives on m_data, which no recompile swaps, but a closed
        // document still has to leave the step inert.
        class MeshMaterialMetadataCommand final : public Arcane::ICommand
        {
        public:
            using State = ShaderEditorDocument::MeshMaterialMetadataState;

            MeshMaterialMetadataCommand(std::weak_ptr<ShaderEditorDocument*> anchor, std::string label,
                                        State before, State after)
                : m_anchor(std::move(anchor)), m_label(std::move(label)),
                  m_before(std::move(before)), m_after(std::move(after))
            {
            }

            void Undo() override { Apply(m_before); }
            void Redo() override { Apply(m_after); }
            const char* Label() const override { return m_label.c_str(); }
            // Spec s3.3: a document step never dirties the scene, and it is
            // EXPIRED once its document closed (the anchor died).
            bool AffectsScene() const override { return false; }
            bool IsExpired() const override
            {
                const auto doc = m_anchor.lock();
                return !doc || !*doc;
            }

        private:
            void Apply(const State& state)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyMeshMaterialMetadata(state);
            }

            std::weak_ptr<ShaderEditorDocument*> m_anchor;
            std::string m_label;
            State       m_before;
            State       m_after;
        };

        std::uint64_t StageKey(const Arcane::Guid& id, bool vertex,
                               std::size_t pass = 0)
        {
            // Coalesce per (document, stage, pass): a newer submit of the SAME
            // stage supersedes; stages never cancel each other. The pass bits
            // sit at << 8, clear of the stage bits and the sprite cache's
            // guid^0x4/0x8 keys.
            return (id.hi ^ (id.lo * 1099511628211ull)) ^ (vertex ? 0x1u : 0x2u) ^
                   (static_cast<std::uint64_t>(pass) << 8);
        }

        // ---- Graph canvas plumbing (Slice 9) ----
        namespace ed = ax::NodeEditor;

        // Pass-canvas fixed ids (chain index c = node id c+1; these sit far
        // above any realistic pass count).
        ARC_CONSTANT("ID space: the pass canvas node and link id bases")
        constexpr std::uint32_t kPassOutputNodeId = 900000;
        ARC_CONSTANT("ID space: the pass canvas node and link id bases")
        constexpr std::uint32_t kPassSceneNodeId  = 900001;   // the Scene source
        ARC_CONSTANT("ID space: the pass canvas node and link id bases")
        constexpr std::uint32_t kPassOutputLinkId = 800000;

        // Pin id encoding: node id * 1000 + slot band. Inputs at +1.., outputs
        // at +501.. (a node type never has anywhere near 500 pins).
        ARC_CONSTANT("ID space: input and output pin id bases inside a node's 1000-id block")
        constexpr std::uint64_t kPinInBase = 1, kPinOutBase = 501;
        ed::PinId InPin(std::uint32_t node, std::uint32_t pin)
        { return ed::PinId(node * 1000ull + kPinInBase + pin); }
        ed::PinId OutPin(std::uint32_t node, std::uint32_t pin)
        { return ed::PinId(node * 1000ull + kPinOutBase + pin); }
        struct DecodedPin
        {
            std::uint32_t node = 0, pin = 0;
            bool isInput = false, valid = false;
        };
        DecodedPin DecodePin(ed::PinId id)
        {
            DecodedPin d;
            if (!id)
                return d;
            const std::uint64_t v = static_cast<std::uint64_t>(id.Get());
            d.node = static_cast<std::uint32_t>(v / 1000ull);
            const std::uint64_t rem = v % 1000ull;
            d.isInput = rem < kPinOutBase;
            d.pin = static_cast<std::uint32_t>(d.isInput ? rem - kPinInBase
                                                         : rem - kPinOutBase);
            d.valid = d.node != 0;
            return d;
        }

        // -------------------------------------------------------------------
        // GRAPH CANVAS PALETTE -- the single place the canvas and node colors
        // live, on the same rule EditorWidgets.cpp:239-253 states for the
        // inspector's constants: no magic colors inside draw calls.
        //
        // All values are DISPLAY-REFERRED: ImGui draws post-tonemap into the
        // backbuffer and samples the backdrop RT straight through
        // (imgui.hlsl:1-5), so these are what the user sees.
        //
        // Tones follow the Unity Shader Graph reference -- a near-flat dark
        // canvas, a node body one step above it, a title band one step below
        // the body, a border one step above the body again.
        // -------------------------------------------------------------------
        // The canvas surface IS the editor's panel tone, not a second opinion
        // about it: a graph document's body is the same flat dark surface every
        // other panel body is. Referencing the theme constant keeps them from
        // drifting apart; the value is unchanged (#1e1e1e), so the approved
        // canvas look is untouched.
        constexpr const ImVec4& kCanvasColor      = Theme::kPanel;                        // #1e1e1e
        // The node, group, pin and grid colours are theme cvars,
        // editor.theme.graph.* (settings S6-27, Settings/GraphThemeSettings.hpp;
        // GraphThemeDefaults holds today's values and their history): the
        // grid pair is shared with the Graph lens, and the selection/hover
        // accents live in Widgets/GraphCanvasStyle.hpp (selection IS
        // Theme::kAmber; hover is editor.theme.graph.hoverBorder). Read per
        // frame; what ApplyGraphCanvasStyle writes into the node-editor style
        // (node body/border, group fill/border, hover) is re-applied on a
        // change by RefreshGraphCanvasStyle, so every one is Live.
        ImVec4 NodeBodyColor()   { return GraphThemeColor(&GraphThemeSettings::nodeBody); }      // #2d2d30
        ImVec4 NodeTitleColor()  { return GraphThemeColor(&GraphThemeSettings::nodeTitle); }     // #232326
        ImVec4 NodeTitleText()   { return GraphThemeColor(&GraphThemeSettings::nodeTitleText); } // #cecfd4
        ImVec4 NodeBadgeText()   { return GraphThemeColor(&GraphThemeSettings::nodeBadgeText); }
        ImVec4 PinDynamicColor() { return GraphThemeColor(&GraphThemeSettings::pinDynamic); }

        // Pin/wire colors by PIN WIDTH (PinColorForWidth over
        // editor.theme.graph.pin*, and the paint rule for a resolved dynamic
        // pin) live in
        // Documents/ShaderGraphPinTypes.hpp (T3-D1), which the canvas, the
        // node page and the canvas legend all read.
        //
        // Unity's texture-red-orange row has a counterpart, just not on the
        // graph canvas: a material graph samples textures through params, but
        // every pin on the PASS canvas is a full-frame RGBA render target. So
        // PinTextureColor() below is that reserved row, spent where a texture
        // pin actually exists.
        // Every pass-canvas pin carries the same thing -- an RGBA render target
        // -- so the pass canvas uses ONE colour throughout rather than a type
        // scale it has no types to fill. Distinct from the 4-lane magenta on
        // purpose: a pass wire moves a whole image between stages, which is a
        // different kind of edge from a float4 moving between expressions, and
        // the two canvases sit one breadcrumb click apart.
        ImVec4 PinTextureColor() { return GraphThemeColor(&GraphThemeSettings::pinTexture); }   // red-orange

        // Node geometry (canvas units at zoom 1). The four chrome metrics --
        // rounding and the three border widths -- moved to
        // Widgets/GraphCanvasStyle.hpp (GraphNodeRounding(),
        // GraphNodeBorderWidth(), GraphNodeHovBorderWidth(),
        // GraphNodeSelBorderWidth(), 2026-09-09): they are the canvas's own
        // language, not this canvas's taste, and were the same four literals in
        // the Graph lens. The padding pair below is NOT shared -- it is exactly
        // what the two canvases disagree about (the Graph lens lays its rows out
        // by hand with zero NodePadding): editor.graph.nodePadding (S6-44).
        // Restart: its BOOT value (GraphNodePaddingAtBoot), never the published
        // snapshot, so a pending edit leaves an open canvas alone until restart.
        float NodePadX() { return GraphNodePaddingAtBoot().x; }
        float NodePadY() { return GraphNodePaddingAtBoot().y; }
        // Breathing room between the BOTTOM EDGE OF THE TITLE BAND and the first
        // body row. Not the same thing as NodePadY(): that one is the band's own
        // internal padding (how far the band extends past the title text), this
        // one is body space below the band. Without it the first pin row does
        // not merely sit flush -- it renders INSIDE the band, because ImGui
        // places the next item one ItemSpacing.y (4 px) under the title text
        // while the band reaches NodePadY() (6 px) under it.
        //
        // Canvas units, like every other constant here: everything inside
        // ed::Begin/End is authored in canvas space, so this scales with zoom on
        // its own and must not be pre-multiplied by the zoom. It is the setting
        // editor.graph.nodeHeaderGap (S6-34) at the UI scale (Ui::Px, s16.11 --
        // exact at scale 1).
        float NodeHeaderGap() { return Ui::Px(Settings<GraphCanvasSettings>().nodeHeaderGap); }

        // Off-screen culling guard band, as a fraction of the visible canvas
        // extent added to EVERY side. UE's value verbatim:
        // NodePanelDefs::GuardBandArea = 0.25f (SNodePanel.cpp:224), used as
        // drawSize * -0.25 .. drawSize * 1.25 (:1588-1589). Generous on
        // purpose -- the band is what stops a node at the edge oscillating
        // between full content and stand-in as the view drifts, and it means a
        // node is already fully built by the time it scrolls in. The setting
        // editor.graph.cullGuardBand (S6-34).
        float CullGuardBand() { return Settings<GraphCanvasSettings>().cullGuardBand; }
        // The pin dot's RADIUS is this canvas's own (the Graph lens draws 4.5f
        // for spec §11.2's 9px): editor.graph.pinDotRadius (S6-34). Its segment
        // count and ring width are shared (GraphPinSegments / kGraphPinRingWidth,
        // Widgets/GraphCanvasStyle.hpp).
        float PinDotRadius() { return Settings<GraphCanvasSettings>().pinDotRadius; }

        // ---- Gradient wires -------------------------------------------------
        // The two-layer technique -- transparent ed::Link for interaction, a
        // hand-drawn curve in the links channel for the paint -- and the whole
        // derivation of that channel index now live once in
        // Widgets/GraphWire.hpp (kGraphLinkChannel, 2026-09-09). The Graph lens
        // used to defer to the copy that stood here; both now read the header.
        // The bezier evaluation, the sRGB lerp and the brighten moved there too
        // (GraphCubicBezierAt / GraphLerpColor / GraphBrightenColor) -- each was
        // byte-identical to the Graph lens's copy.

        // ZOOM STOPS + ApplyZoomLevels moved to Widgets/GraphZoomLevels.hpp
        // (2026-09-09) so the Assets panel's Graph lens can install the same
        // table instead of hand-copying it -- see that header for the full
        // rationale (ApplyZoomLevels; the table itself is the setting
        // editor.graph.zoomLevels since S6-34) and docs/specs/
        // 2026-09-06-asset-manager-redesign-design.md §19 for the bug this
        // fixed. `ApplyZoomLevels` below still names the header's definition
        // via using-directive-free lookup (it is in namespace Arcane::Editor,
        // which this anonymous namespace nests inside).

        // The view-scale helper moved to Widgets/GraphWire.hpp as
        // GraphViewScale (2026-09-09) -- the reciprocal flip and its "THE TRAP"
        // note were byte-identical in the Graph lens. RENDERING LOD BOUNDARIES (the
        // tier boundaries -- editor.graph.lod.* since S6-34 -- and NodeLODForScale,
        // the third column of UE's zoom table) moved to Widgets/GraphNodeLod.hpp
        // alongside the NodeLOD enum, so the Graph lens reads the table instead
        // of copying 0.250 out of it.

        // FillRgba (ImVec4 -> the plain float[4] GraphGridColors holds) had one
        // caller, the backdrop composition, and moved with it into
        // Widgets/GraphCanvasBackdrop.hpp -- where it is the same four
        // assignments the Graph lens had written as an inline lambda.

        // One port dot: FILLED when a wire is attached, a hollow ring when not
        // (the Shader Graph reading -- "this port carries something" is visible
        // without tracing the wire), plus the grey "adapts" ring on a resolved
        // dynamic pin (GraphPinPaint). Advances the cursor by exactly the dot,
        // so the caller follows with SameLine + the label. Returns the dot's
        // CENTRE in canvas space -- the pin rows anchor their wire pivot off it.
        ImVec2 DrawPinDot(const GraphPinPaint& paint, bool connected)
        {
            const float lineH = ImGui::GetTextLineHeight();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(PinDotRadius() * 2.0f, lineH));
            const ImVec2 c(p.x + PinDotRadius(), p.y + lineH * 0.5f);
            // The three draw calls are Widgets/GraphPinDot.hpp's (2026-09-09);
            // what stays here is the LAYOUT -- the cursor advance and the centre
            // this function exists to hand back. The Graph lens shares the
            // paint and none of that.
            const ImVec4 ring = PinDynamicColor();
            DrawGraphPinDot(ImGui::GetWindowDrawList(), c, paint.color,
                            NodeBodyColor(), PinDotRadius(), connected,
                            paint.adapts ? &ring : nullptr);
            return c;
        }
        // The pass canvas's dots: one colour, no type to resolve.
        ImVec2 DrawPinDot(const ImVec4& color, bool connected)
        {
            return DrawPinDot(GraphPinPaint{ color, false }, connected);
        }

        // Horizontal spacer that right-aligns a row of `rowWidth` inside a
        // content column of `contentWidth`. Both are canvas units; a
        // non-positive slack draws nothing, which is what the first frame of a
        // brand-new node (no measured width yet) gets.
        void RightAlignRow(float contentWidth, float rowWidth)
        {
            const float slack = contentWidth - rowWidth;
            if (slack <= 1.0f)
                return;
            ImGui::Dummy(ImVec2(slack, 0.0f));
            ImGui::SameLine(0.0f, 0.0f);
        }

        // The node's title band, drawn AFTER ed::EndNode because it spans the
        // node's final measured width and that only exists once the node has
        // been laid out. GetNodeBackgroundDrawList paints into the node's own
        // user-background channel -- above its body fill, below its content and
        // pin chrome (imgui_node_editor.cpp:135-140) -- which is exactly where a
        // header band belongs. Coordinates are canvas space, the space both
        // GetNodePosition and plain ImGui use inside ed::Begin/End.
        //
        // `headerMaxY` must be the TITLE row's rect bottom, captured before any
        // body content is submitted: pinning the band's lower edge to that
        // rather than to the node's content extent is what keeps a header gap
        // readable as body space instead of being swallowed by a taller band.
        //
        // Returns the node's measured size (zero before its first layout), so a
        // caller that caches a width reads it off this same query instead of
        // asking the library twice.
        ImVec2 DrawNodeTitleBand(std::uint32_t nodeId, float headerMaxY, ImVec4 color = NodeTitleColor())
        {
            const ImVec2 nodePos  = ed::GetNodePosition(ed::NodeId(nodeId));
            const ImVec2 nodeSize = ed::GetNodeSize(ed::NodeId(nodeId));
            if (nodeSize.x <= 0.0f)
                return nodeSize;
            if (ImDrawList* bg = ed::GetNodeBackgroundDrawList(ed::NodeId(nodeId)))
                bg->AddRectFilled(
                    ImVec2(nodePos.x + GraphNodeBorderWidth(), nodePos.y + GraphNodeBorderWidth()),
                    ImVec2(nodePos.x + nodeSize.x - GraphNodeBorderWidth(),
                           headerMaxY + NodePadY()),
                    ImGui::GetColorU32(color),
                    GraphNodeRounding(), ImDrawFlags_RoundCornersTop);
            return nodeSize;
        }

        // THIS canvas's answers to the shared style desc
        // (Widgets/GraphCanvasStyle.hpp). All 15 ed::Style writes -- 10 colours
        // and 5 scalars -- and their reasoning are there; what is here is only
        // what this canvas differs on. Every other field takes the shared
        // default -- the editor-wide canvas language -- which is the whole point
        // of the desc. This canvas's old block wrote the same 15; the Graph
        // lens's wrote 13, omitting the group pair (see the header).
        GraphCanvasStyleDesc ShaderCanvasStyleDesc()
        {
            GraphCanvasStyleDesc d;
            // The published snapshot: applied at canvas creation and
            // re-applied per frame on a change (RefreshGraphCanvasStyle).
            d.nodeBody    = NodeBodyColor();    // #2d2d30, the Unity SG reference tone
            d.nodeBorder  = GraphThemeColor(&GraphThemeSettings::nodeBorder);
            d.groupBg     = GraphThemeColor(&GraphThemeSettings::groupBg);       // this canvas HAS group (comment) nodes
            d.groupBorder = GraphThemeColor(&GraphThemeSettings::groupBorder);
            // Content-driven nodes: ImGui measures them, so they need padding
            // (the boot value: editor.graph.nodePadding is Restart).
            d.nodePadding = ImVec4(NodePadX(), NodePadY(), NodePadX(), NodePadY());
            return d;
        }

        // Re-key a saved-params entry old -> new. Merge rule (assisted rename):
        // when BOTH names exist the new-name value wins and the orphan drops.
        void RekeySavedParam(
            std::vector<std::pair<std::string, Arcane::MatParamValue>>& params,
            const std::string& oldName, const std::string& newName)
        {
            const auto oldIt = std::find_if(params.begin(), params.end(),
                [&](const auto& p) { return p.first == oldName; });
            if (oldIt == params.end())
                return;
            const bool hasNew = std::any_of(params.begin(), params.end(),
                [&](const auto& p) { return p.first == newName; });
            if (hasNew)
            {
                ARC_WARN("param rename: a '{}' value already exists -- dropping "
                         "the orphaned '{}' entry", newName, oldName);
                params.erase(oldIt);
            }
            else
                oldIt->first = newName;
        }

        // Case-insensitive substring match for the create-menu search field.
        bool ContainsInsensitive(const char* hay, const char* needle)
        {
            const std::size_t n = std::strlen(needle);
            if (n == 0)
                return true;
            for (const char* p = hay; *p; ++p)
            {
                std::size_t i = 0;
                while (i < n && p[i] &&
                       std::tolower(static_cast<unsigned char>(p[i])) ==
                           std::tolower(static_cast<unsigned char>(needle[i])))
                    ++i;
                if (i == n)
                    return true;
            }
            return false;
        }

        // Would adding data-flow edge from->to close a cycle? Yes iff `from` is
        // already downstream of `to`. (Codegen re-detects as the backstop; this
        // check is what makes the canvas REFUSE the wire silently, SG-style.)
        bool WouldCycle(const Arcane::MaterialGraph& g, std::uint32_t from, std::uint32_t to)
        {
            if (from == to)
                return true;
            std::vector<std::uint32_t> stack{ to };
            std::unordered_set<std::uint32_t> seen;
            while (!stack.empty())
            {
                const std::uint32_t cur = stack.back();
                stack.pop_back();
                if (cur == from)
                    return true;
                if (!seen.insert(cur).second)
                    continue;
                for (const Arcane::GraphLink& l : g.links)
                    if (l.fromNode == cur)
                        stack.push_back(l.toNode);
            }
            return false;
        }

        // ---- Inline pin literals (a value ON an unwired input pin) ----

        // Whether a pin takes a literal at all (the SEAM SCOPE exclusion list)
        // and how many lanes it stores are ENGINE predicates:
        // Arcane::GraphPinAcceptsLiteral / Arcane::GraphPinLiteralLanes, declared beside
        // GraphNodeInputPin and defined beside the emission switch they mirror
        // (MaterialGraph.hpp:385-403, MaterialGraph.cpp:1234-1285). They used
        // to be duplicated here, which made a future argOr-bypassing node type
        // a silent dead widget with nothing to fail; the engine copy has a
        // truth-table test over every node type instead.

        // The literal widget's starting value on a pin with no literal yet:
        // codegen's Constant neutral (Arcane::GraphPinNeutralDefault, the one
        // truth -- node page s5.1.8), splatted to the widget's lanes the way
        // Adapt splats a width-1 default (Tiling & Offset's tiling shows
        // (1, 1)), so an untouched field never lies and a first drag starts
        // from the value the shader uses. Expression/Passthrough leave 0 (the
        // caller prints `hlsl` instead). Both surfaces -- canvas and node page
        // -- seed through here.
        void SeedPinNeutral(const Arcane::GraphPinNeutral& nd, int lanes, float out[4]) noexcept
        {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            if (nd.kind != Arcane::GraphPinNeutralKind::Constant)
                return;
            for (int k = 0; k < lanes && k < 4; ++k)
                out[k] = nd.lanes == 1 ? nd.v[0] : nd.v[k];
        }

        // Writes `pin`'s literal: ONE entry per pin, updated IN PLACE (born on
        // first touch), its first `lanes` lanes from `v`, the rest zeroed. A
        // duplicate would make serialization non-deterministic: the writer
        // sorts by pin with std::sort, which is unstable
        // (MaterialGraph.cpp:1382-1384), and the reader keeps the FIRST entry
        // for a pin (:1561-1562). Both surfaces -- canvas and node page --
        // write through here.
        void SetPinLiteral(Arcane::GraphNode& node, std::uint32_t pin, int lanes, const float v[4])
        {
            Arcane::GraphPinLiteral* slot = nullptr;
            for (Arcane::GraphPinLiteral& pl : node.pinLiterals)
                if (pl.pin == pin)
                {
                    slot = &pl;
                    break;
                }
            if (!slot)
            {
                Arcane::GraphPinLiteral fresh;
                fresh.pin = pin;
                node.pinLiterals.push_back(fresh);
                slot = &node.pinLiterals.back();
            }
            for (int i = 0; i < 4; ++i)
                slot->v[i] = i < lanes ? v[i] : 0.0f;
        }

        // Drops `pin`'s literal (the pin reads its neutral again). Not for the
        // custom-pin removal, which renumbers the surviving entries afterwards.
        void ErasePinLiteral(Arcane::GraphNode& node, std::uint32_t pin)
        {
            std::erase_if(node.pinLiterals, [pin](const Arcane::GraphPinLiteral& pl) { return pl.pin == pin; });
        }

        // The node page's target id scope (s5.1.4 step 3), RAII and pushed before
        // any Rows: TextRow / numeric drafts key per node, and a Rows table always
        // ends before its id pops (TargetIdScope's rule, InputActionsInspectorPage.cpp:51-61).
        struct NodePageIdScope
        {
            NodePageIdScope(std::size_t pass, std::uint32_t id)
            {
                ImGui::PushID("node");
                ImGui::PushID(static_cast<int>(pass));
                ImGui::PushID(static_cast<int>(id));
            }
            ~NodePageIdScope() { ImGui::PopID(); ImGui::PopID(); ImGui::PopID(); }
            NodePageIdScope(const NodePageIdScope&) = delete;
            NodePageIdScope& operator=(const NodePageIdScope&) = delete;
        };

        // T3-D2 (user decision 2026-10-02): how much of a pin row's wiring /
        // default text must stay readable AFTER the type word. Below dot + the
        // widest type word + this many characters, the node page shows the dot
        // alone and the word leads the row's hover tooltip. The WIDEST word any
        // pin can show (PinTypeText over every declared/resolved pair), not the
        // row's own: every row of a page makes the same call, so a page never
        // mixes worded and dot-only rows and its dots stay in one column.
        // editor.inspector.nodePageMinTextRun (S6-37).
        int NodePageMinTextRun()
        {
            return Arcane::Settings<InspectorSettings>().nodePageMinTextRun;
        }

        // The chip's dot slot: it fits a dot WITH its outer ring, ringed or
        // not, so the type words of a section line up.
        float PinChipSlot() { return 2.0f * (PinDotRadius() + GraphPinOuterRingGap() + GraphPinOuterRingWidth()); }

        // The value-cell width the chip needs to show its word (T3-D2), from
        // the CURRENT font: the dot slot, the widest word ANY pin shows (so
        // every row of a page makes the same call and the dots line up), the
        // gaps, and the cvar's run of 'x'-wide characters after it.
        float NodePageTypeWordCellWidth()
        {
            float widest = 0.0f;
            for (int declared = 0; declared <= 4; ++declared)
                for (int resolved = 0; resolved <= 4; ++resolved)
                    widest = std::max(widest, ImGui::CalcTextSize(PinTypeText(declared, resolved).c_str()).x);
            const ImGuiStyle& style = ImGui::GetStyle();
            return PinChipSlot() + style.ItemInnerSpacing.x + widest + style.ItemSpacing.x +
                   ImGui::CalcTextSize("x").x * static_cast<float>(NodePageMinTextRun());
        }

        // A node page pin row's type chip (T3-D1): the pin's dot -- painted by
        // the canvas's own rule and painter, ring included -- then its type
        // word, dim. Submitted as a RowDecor::lead, so it leads the value cell
        // and the word is never the part a narrow Inspector cuts (the wiring
        // text after it is, with its whole text one hover away, s4.1(e)).
        // Narrower than NodePageTypeWordCellWidth (T3-D2) it draws the dot
        // alone and RETURNS the word: the row's hover tooltip leads with it
        // (and the dot tooltips it), so the cell keeps its room for the text.
        // Centred on the row's FRAME line (a framed value widget's text sits
        // FramePadding.y down; a read-only row's text takes the same baseline),
        // with a text-height dummy so a read-only row keeps its height.
        std::string PinTypeChip(const GraphPinPaint& paint, bool wired, const std::string& type)
        {
            const bool showWord = ImGui::GetContentRegionAvail().x >= NodePageTypeWordCellWidth();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(PinChipSlot(), ImGui::GetTextLineHeight()));
            const ImVec2 c(p.x + PinChipSlot() * 0.5f, p.y + ImGui::GetFrameHeight() * 0.5f);
            const ImVec4 ring = PinDynamicColor();
            DrawGraphPinDot(ImGui::GetWindowDrawList(), c, paint.color,
                            ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), PinDotRadius(), wired,
                            paint.adapts ? &ring : nullptr);
            if (!showWord)
            {
                ImGui::SetItemTooltip("%s", type.c_str());
                return type;
            }
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);   // the dot belongs to its word
            ImGui::TextDisabled("%s", type.c_str());
            return {};
        }

        // The resolved widths of node `id`, or { 0, 0 } (unresolved) when the
        // map has no entry.
        Arcane::GraphNodeWidths WidthsOf(const std::unordered_map<std::uint32_t, Arcane::GraphNodeWidths>& widths,
                                         std::uint32_t id)
        {
            const auto it = widths.find(id);
            return it != widths.end() ? it->second : Arcane::GraphNodeWidths{};
        }

        // "<display>[ '<param>'].<pin>" for a wired input's source (s5.1.4 pin table).
        std::string WireSourceText(const Arcane::MaterialGraph& g, const Arcane::GraphLink& l)
        {
            const Arcane::GraphNode* src = g.FindNode(l.fromNode);
            if (!src || l.fromPin >= Arcane::GraphNodeOutputCount(*src))
                return "?";
            std::string display = Arcane::GraphNodeInfo(src->type).display;
            if (src->type == Arcane::GraphNodeType::Param || src->type == Arcane::GraphNodeType::TextureSample)
                display += " '" + src->paramName + "'";
            return display + "." + Arcane::GraphNodeOutputPin(*src, l.fromPin).name;
        }

        // Constant: %g for one lane, "(a, b[, c, d])" for more; Expression: its hlsl;
        // Passthrough: the plain-language rule.
        std::string FormatPinNeutral(const Arcane::GraphPinNeutral& p)
        {
            if (p.kind == Arcane::GraphPinNeutralKind::Expression)
                return p.hlsl ? p.hlsl : "";
            if (p.kind == Arcane::GraphPinNeutralKind::Passthrough)
                return "unchanged (only a wire contributes)";
            char buf[32];
            if (p.lanes <= 1)
            {
                std::snprintf(buf, sizeof(buf), "%g", p.v[0]);
                return buf;
            }
            std::string out = "(";
            for (int i = 0; i < p.lanes && i < 4; ++i)
            {
                std::snprintf(buf, sizeof(buf), "%g", p.v[i]);
                out += (i ? ", " : "") + std::string(buf);
            }
            return out + ")";
        }

        // Value equality for a pass's optional graph, for the gesture builders'
        // no-op guard ONLY. MaterialGraph is a plain aggregate with no
        // operator== (MaterialGraph.hpp:334-372; neither GraphNode nor
        // GraphLink has one either), so this rides the existing public
        // serialization: GraphToJson's text is byte-stable for equal graphs,
        // which MaterialGraphTest.cpp:755 and :1386 already assert. Doc-local
        // on purpose -- an engine-header operator== is a wider commitment than
        // this guard needs.
        //
        // Cost is one serialization per GESTURE CLOSE (not per frame, not per
        // drag tick), against graphs of tens of nodes -- the same order as the
        // whole-graph copy the step itself carries.
        //
        // Conservative in the one direction that matters: any float pair that
        // dumps differently (0.0 vs -0.0) reads as CHANGED and still pushes.
        bool GraphOptEqual(const std::optional<Arcane::MaterialGraph>& a,
                           const std::optional<Arcane::MaterialGraph>& b)
        {
            if (a.has_value() != b.has_value())
                return false;   // nullopt vs engaged: a real difference
            if (!a.has_value())
                return true;
            return Arcane::GraphToJson(*a).dump() == Arcane::GraphToJson(*b).dump();
        }

        // One graph gesture as an undo step (same doc-identity anchor pattern
        // as ParamEditCommand). Whole-graph before/after: our graphs are tens
        // of nodes -- the SG full-snapshot-undo pathology was per-edit JSON
        // reserialization + full preview regeneration, neither of which this
        // does (ApplyGraphState reuses the debounced compile loop).
        class GraphEditCommand final : public Arcane::ICommand
        {
        public:
            GraphEditCommand(std::weak_ptr<ShaderEditorDocument*> anchor, std::string label,
                             std::size_t pass,
                             std::optional<Arcane::MaterialGraph> before,
                             std::optional<Arcane::MaterialGraph> after)
                : m_anchor(std::move(anchor)), m_label(std::move(label)), m_pass(pass),
                  m_before(std::move(before)), m_after(std::move(after))
            {
            }

            void Undo() override { Apply(m_before); }
            void Redo() override { Apply(m_after); }
            const char* Label() const override { return m_label.c_str(); }
            // Spec s3.3: a document step never dirties the scene, and it is
            // EXPIRED once its document closed (the anchor died).
            bool AffectsScene() const override { return false; }
            bool IsExpired() const override
            {
                const auto doc = m_anchor.lock();
                return !doc || !*doc;
            }

        private:
            void Apply(const std::optional<Arcane::MaterialGraph>& state)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyGraphState(m_pass, state);
            }

            std::weak_ptr<ShaderEditorDocument*> m_anchor;
            std::string m_label;
            std::size_t m_pass;   // which pass's graph the step edits (0 = base)
            std::optional<Arcane::MaterialGraph> m_before, m_after;
        };

        // One pass-canvas STRUCTURAL gesture as an undo step (add/remove/
        // rewire/reorder/rename): whole pass-list before/after through the
        // same doc-identity anchor.
        class PassListCommand final : public Arcane::ICommand
        {
        public:
            PassListCommand(std::weak_ptr<ShaderEditorDocument*> anchor, std::string label,
                            ShaderEditorDocument::PassListState before,
                            ShaderEditorDocument::PassListState after)
                : m_anchor(std::move(anchor)), m_label(std::move(label)),
                  m_before(std::move(before)), m_after(std::move(after))
            {
            }

            void Undo() override { Apply(m_before); }
            void Redo() override { Apply(m_after); }
            const char* Label() const override { return m_label.c_str(); }
            // Spec s3.3: a document step never dirties the scene, and it is
            // EXPIRED once its document closed (the anchor died).
            bool AffectsScene() const override { return false; }
            bool IsExpired() const override
            {
                const auto doc = m_anchor.lock();
                return !doc || !*doc;
            }

        private:
            void Apply(const ShaderEditorDocument::PassListState& state)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplyPassListState(state);
            }

            std::weak_ptr<ShaderEditorDocument*> m_anchor;
            std::string m_label;
            ShaderEditorDocument::PassListState m_before, m_after;
        };
    }

    // InputTextMultiline over std::string (the imgui_stdlib resize pattern) +
    // pending cursor jump: the CallbackAlways pass moves the cursor to the
    // requested line once the widget is active -- stb_textedit then scrolls the
    // cursor into view, which is exactly click-to-jump. UserData is the
    // DOCUMENT (per-doc state, review m2 -- a shared static could deliver one
    // document's jump to another on a same-frame focus race); this forwarder is
    // the declared friend, so it may touch the members directly.
    struct SnippetCallbackForwarder
    {
        static int Callback(ImGuiInputTextCallbackData* data)
        {
            auto* doc = static_cast<ShaderEditorDocument*>(data->UserData);
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                // ActiveSnippet: pass chains route the editor at the selected
                // pass's buffer (pass 0 = m_snippet, unchanged single-pass).
                std::string& buf = doc->ActiveSnippet();
                buf.resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = buf.data();
            }
            else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways &&
                     doc->m_callbackJumpLine > 0)
            {
                int line = 1;
                int pos = 0;
                while (line < doc->m_callbackJumpLine && pos < data->BufTextLen)
                {
                    if (data->Buf[pos] == '\n')
                        ++line;
                    ++pos;
                }
                data->CursorPos = pos;
                data->SelectionStart = data->SelectionEnd = pos;
                doc->m_callbackJumpLine = 0;
            }
            return 0;
        }
    };

    ShaderEditorDocument::ShaderEditorDocument(DocServices services,
                                               std::filesystem::path path,
                                               Arcane::MaterialAssetData data)
        : m_services(services), m_path(std::move(path)), m_data(std::move(data))
    {
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + (m_data.IsInstance() ? " (Instance)###matdoc_"
                                                       : " (Material)###matdoc_") +
                        m_data.id.ToString();
        m_snippet = m_data.snippet;
        m_anchor = std::make_shared<ShaderEditorDocument*>(this);
        // ALWAYS open on the chain overview -- single-pass materials included.
        // The pipeline is what a fullscreen material IS, so it is what the
        // document opens on; branching on pass count would make which screen
        // you land on depend on the asset, and "where am I" would stop being
        // answerable before the window finished drawing. A single-pass overview
        // is just base -> Output, which is thin but true -- and it is where Add
        // Pass lives.
        //
        // Corrected downstream for the two surfaces with no chain: Draw forces
        // this false on a sprite surface, and instances never reach the branch.
        m_inChainView = true;
        // Seed the history with where the document opened, so the FIRST jump
        // has somewhere to go back to. Without this the opening view would
        // never be a destination.
        m_navHistory.push_back(ViewEntry{ m_inChainView, m_activePass });
        m_navIndex = 0;

        if (!IsInstance() || ResolveParentChain())
        {
            // Preview surface follows the asset's kind (instances inherit the
            // chain BASE's kind -- the base owns the snippet and the surface).
            const std::string& kind =
                IsInstance() && !m_parentChain.empty() ? m_parentChain.back().kind
                                                       : m_data.kind;
            m_surface = SurfaceIndexOf(Arcane::MaterialSurfaceForKind(kind));
            // Regenerate every graph-owned pass (deterministic codegen == the
            // saved snippets, so this leaves the doc clean) then compile; for
            // text-only docs the regen loop no-ops into a plain Rebuild.
            RegenerateFromGraph();
        }
    }

    ShaderEditorDocument::~ShaderEditorDocument()
    {
        // Retract this document's diagnostic rows -- a closed material must not
        // leave stale errors in the Problems panel. Exactly Publish(key, {}).
        Arcane::Diagnostics::Clear(DiagnosticKey());

        // Documents are DESTROYED synchronously on close and there is no
        // on-close hook (DocumentHost::Close erases the unique_ptr; CloseAll
        // does it for every document on a project switch). The X-button path is
        // already safe -- requestClose is raised INSIDE Draw and acted on after
        // the draw loop, so the ScopeGuard above has run -- but any close that
        // destroys the document between a gesture parking and its next Draw (a
        // close hotkey, the project-switch CloseAll) would strand the
        // transaction open, and a stranded transaction leaves InTransaction()
        // true editor-wide: structural edits refused AND Ctrl+Z/Ctrl+Y dead.
        // Closing here is what makes that unreachable.
        //
        // It is not free: a close that LANDS a step also clears the REDO stack
        // (CommandStack.cpp:70 -- reached only for a non-empty transaction,
        // since :61-62 returns first when nothing changed). So closing a
        // document mid-gesture discards redo history. That is the accepted
        // cost of ClosePending's commit-not-cancel rule, which exists because
        // Cancel would discard the transaction WITHOUT reverting the edits the
        // user already watched happen.
        if (UndoStack())
            EditGesture::ClosePending(*UndoStack(), m_gesture);

        if (m_graphCtx)
        {
            ed::DestroyEditor(m_graphCtx);
            m_graphCtx = nullptr;
        }
        if (m_passCanvasCtx)
        {
            ed::DestroyEditor(m_passCanvasCtx);
            m_passCanvasCtx = nullptr;
        }

        // LAST, and it is the one teardown here that spans two objects the
        // editor owns separately: the chrome context's ImGuiNri holds a view
        // over this document's preview output, so that view has to be
        // destroyed before the texture is. DestroyGraphPreview carries the
        // ordering and the reason.
        //
        // AFTER the node-editor contexts rather than before, deliberately:
        // ed::DestroyEditor touches only ImGui/CPU state, so nothing between
        // here and there can draw this texture again -- but keeping the GPU
        // teardown last is the same "borrower first, GPU last" shape
        // EditorApp::Shutdown uses, and it is what a reader will expect.
        DestroyGraphPreview();
    }

    void ShaderEditorDocument::NoteMoved(const std::filesystem::path& p)
    {
        m_path = p;
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + (m_data.IsInstance() ? " (Instance)###matdoc_" : " (Material)###matdoc_") + m_data.id.ToString();
        PublishDiagnostics();   // the File locators carry m_path
    }

    // T5 s7.5: what Save would write (Save below) -- the parent plus every texture param. A bound instance's
    // overrides are the truth Save harvests (only those the bound template declares); before the first bind
    // the loaded m_data.params are kept, so they are the answer.
    std::vector<Arcane::Guid> ShaderEditorDocument::LiveReferences() const
    {
        std::vector<Arcane::Guid> o;
        if (m_data.parent.IsValid())
            o.push_back(m_data.parent);
        const auto tex = [&](const Arcane::MatParamValue& v)
        {
            if (v.type == Arcane::MatParamType::Texture && v.tex.IsValid())
                o.push_back(v.tex);
        };
        if (m_instance && m_boundTemplate)
        {
            for (const auto& [h, v] : m_instance->Overrides())
                if (m_boundTemplate->Find(h))
                    tex(v);
        }
        else
            for (const auto& [n, v] : m_data.params)
                tex(v);
        return o;
    }

    void ShaderEditorDocument::FlushGesture()
    {
        if (Arcane::CommandStack* s = UndoStack())
            EditGesture::ClosePending(*s, m_gesture);
    }

    bool ShaderEditorDocument::ResolveParentChain()
    {
        m_parentChain.clear();
        const Arcane::Project* project =
            m_services.runtime ? m_services.runtime->CurrentProject() : nullptr;
        if (!project)
        {
            m_parseErrors = { "instance materials need an open project (parent lookup)" };
            return false;
        }

        Arcane::Guid cursor = m_data.parent;
        std::vector<Arcane::Guid> visited{ m_data.id };
        while (cursor.IsValid())
        {
            for (const Arcane::Guid& seen : visited)
            {
                if (seen == cursor)
                {
                    m_parseErrors = { "parent chain contains a cycle" };
                    return false;
                }
            }
            visited.push_back(cursor);

            const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(cursor));
            if (!path)
            {
                m_parseErrors = { "parent material " + cursor.ToString() +
                                  " is not in the asset registry" };
                return false;
            }
            auto parent = Arcane::LoadMaterialAsset(*path);
            if (!parent)
            {
                m_parseErrors = { "parent material failed to load: " +
                                  path->generic_string() };
                return false;
            }
            cursor = parent->parent;
            m_parentChain.push_back(std::move(*parent));
        }

        if (m_parentChain.empty() || m_parentChain.back().IsInstance())
        {
            m_parseErrors = { "parent chain never reaches a base material" };
            return false;
        }
        return true;
    }

    const std::string& ShaderEditorDocument::SnippetSource() const
    {
        return IsInstance() && !m_parentChain.empty() ? m_parentChain.back().snippet
                                                      : m_snippet;
    }

    void ShaderEditorDocument::Rebuild()
    {
        // Mesh materials are not authored OR compiled in this document
        // (MaterialSurface's own comment, Material/MaterialSource.hpp): they
        // stitch no shader source, and MeshMaterialCache reads baseColor/
        // albedo straight out of saved params, never through MaterialSource/
        // ShaderCompiler. This check runs BEFORE the compiler/sources guard
        // below on purpose -- baseColor/albedo need neither a ShaderCompiler
        // nor a ShaderSourceProvider to exist, so a mesh document opened with
        // a bare DocServices{} (every basic headless test's shape, and a
        // legitimate host state before those services exist) still gets its
        // params bound. Without this guard ahead of that one, a mesh-kind
        // document (openable since F2b Task 13's CreateMaterialAt surface
        // argument) would either silently skip binding entirely (services
        // absent) or -- once services exist -- call MaterialTemplateFile/
        // BuildMaterialShaderSource with MaterialSurface::Mesh below, both of
        // which silently fall back to the FULLSCREEN template/bindings (their
        // own ARC_ENSURE guards) and submit a real compile job for HLSL
        // nothing downstream ever reads. Invalidate whatever a PRIOR Rebuild
        // left in flight (the surface picker locks on mesh, but a document
        // can still open straight onto one), then bind the FIXED mesh decl
        // list synchronously (MeshParamTemplate) so DrawParamsPanel has a
        // template/instance to draw baseColor/albedo against -- there is no
        // async compile step to wait for here, so PromotePendingInstance runs
        // immediately rather than from a compile-job callback (BindIfComplete's
        // own path).
        if (SurfaceOf(m_surface) == Arcane::MaterialSurface::Mesh)
        {
            m_vsJob = m_psJob = 0;
            m_jobsInFlight  = 0;      // s3.2: every live id was just invalidated
            m_submitRefused = false;
            m_vsBytes.clear();
            m_psBytes.clear();
            m_passJobs.clear();
            m_parseErrors.clear();
            m_pendingTemplate = std::make_shared<Arcane::MaterialTemplate>(MeshParamTemplate(m_title));
            m_metas.assign(m_pendingTemplate->Params().size(), Arcane::ParamMeta{});
            PromotePendingInstance();
            return;
        }

        if (!m_services.compiler || !m_services.sources)
            return;

        const char* templateFile = Arcane::MaterialTemplateFile(SurfaceOf(m_surface));
        const auto templateText = m_services.sources->Get(templateFile);
        if (!templateText)
        {
            m_parseErrors = { std::string("template not found: ") + templateFile };
            return;
        }

        // Invalidate every in-flight job (both paths): a result for a source
        // this Rebuild replaced must not bind.
        m_vsJob = m_psJob = 0;
        m_jobsInFlight  = 0;      // s3.2: every live id was just invalidated
        m_submitRefused = false;
        m_vsBytes.clear();
        m_psBytes.clear();
        m_passJobs.clear();

        if (CompilesAsChain())
        {
            // An instance compiles its BASE's chain (CompilesAsChain); a base
            // compiles its own, with the LIVE base snippet (SnippetSource).
            const Arcane::MaterialAssetData& src = *CompiledSource();
            std::vector<Arcane::MaterialChainPassDesc> descs;
            descs.reserve(1 + src.passes.size());
            descs.push_back({ SnippetSource(), src.baseInputs });
            for (const Arcane::MaterialPass& p : src.passes)
                descs.push_back({ p.snippet, p.inputs });

            // The editor ALWAYS builds in post mode: scene reads must author
            // and preview here (the stand-in feeds them); only a non-post
            // RUNTIME consumer refuses them.
            Arcane::MaterialChainBuildResult build = Arcane::BuildMaterialChainSource(
                *templateText, descs, m_title, src.vertexSnippet,
                /*externalInput=*/true);
            m_passInputs = std::move(build.passInputs);
            m_chainInputSlots = build.chainInputSlots;
            m_vsLineOffset = 0;
            if (!src.vertexSnippet.empty() && !build.hlsl.empty())
                if (const std::size_t at = build.hlsl[0].find(src.vertexSnippet);
                    at != std::string::npos)
                    m_vsLineOffset = static_cast<int>(std::count(
                        build.hlsl[0].begin(),
                        build.hlsl[0].begin() + static_cast<std::ptrdiff_t>(at), '\n'));
            m_parseErrors = std::move(build.errors);
            for (std::size_t p = 0; p < build.passErrors.size(); ++p)
                for (const std::string& e : build.passErrors[p])
                    m_parseErrors.push_back(PassLabel(p) + ": " + e);

            m_passLineOffsets.assign(build.hlsl.size(), 0);
            for (std::size_t p = 0; p < build.hlsl.size(); ++p)
                if (const std::size_t at = build.hlsl[p].find(descs[p].snippet);
                    at != std::string::npos)
                    m_passLineOffsets[p] = static_cast<int>(std::count(
                        build.hlsl[p].begin(),
                        build.hlsl[p].begin() + static_cast<std::ptrdiff_t>(at), '\n'));
            m_snippetLineOffset = m_passLineOffsets.empty() ? 0 : m_passLineOffsets[0];
            m_pendingTemplate =
                std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
            m_metas = std::move(build.metas);

            m_passJobs.resize(build.hlsl.size());
            for (std::size_t p = 0; p < build.hlsl.size(); ++p)
            {
                Arcane::ShaderCompileRequest req;
                req.debugName = m_title + "_p" + std::to_string(p) + ".hlsl";
                req.sourceUtf8 = build.hlsl[p];
                req.entry = Arcane::kPsEntry;
                req.profile = Arcane::kPsProfile;
                req.coalesceKey = StageKey(m_data.id, /*vertex=*/false, p);
                m_passJobs[p].psJob = SubmitCompile(req);   // a copy -- req is reused
                req.entry = Arcane::kVsEntry;
                req.profile = Arcane::kVsProfile;
                req.coalesceKey = StageKey(m_data.id, /*vertex=*/true, p);
                m_passJobs[p].vsJob = SubmitCompile(std::move(req));
            }
            return;
        }

        // Instances inherit the base's vertex stage (they carry no snippets).
        const std::string& vertexSnippet = CompiledVertexSnippet();
        Arcane::MaterialBuildResult build =
            Arcane::BuildMaterialShaderSource(*templateText, SnippetSource(), m_title,
                                              SurfaceOf(m_surface), vertexSnippet);
        m_parseErrors = std::move(build.errors);
        m_vsLineOffset = 0;
        if (!vertexSnippet.empty())
            if (const std::size_t at = build.hlsl.find(vertexSnippet);
                at != std::string::npos)
                m_vsLineOffset = static_cast<int>(std::count(
                    build.hlsl.begin(),
                    build.hlsl.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
        // The snippet rides verbatim into the stitched source -- its line
        // offset maps compiler diag lines back into snippet space (error-row
        // jump; graph node badges via the codegen line map).
        m_snippetLineOffset = 0;
        if (const std::size_t at = build.hlsl.find(SnippetSource());
            at != std::string::npos)
            m_snippetLineOffset = static_cast<int>(
                std::count(build.hlsl.begin(), build.hlsl.begin() + at, '\n'));
        m_pendingTemplate = std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
        m_metas = std::move(build.metas);

        Arcane::ShaderCompileRequest req;
        req.debugName = m_title + ".hlsl";
        req.sourceUtf8 = build.hlsl;
        req.entry = Arcane::kPsEntry;
        req.profile = Arcane::kPsProfile;
        req.coalesceKey = StageKey(m_data.id, /*vertex=*/false);
        m_psJob = SubmitCompile(req);   // a copy -- req is reused
        req.entry = Arcane::kVsEntry;
        req.profile = Arcane::kVsProfile;
        req.coalesceKey = StageKey(m_data.id, /*vertex=*/true);
        m_vsJob = SubmitCompile(std::move(req));
        m_vsBytes.clear();
        m_psBytes.clear();
    }

    std::uint64_t ShaderEditorDocument::SubmitCompile(Arcane::ShaderCompileRequest req)
    {
        const std::uint64_t id = m_services.compiler->Submit(std::move(req), Now());
        if (id != 0)
            ++m_jobsInFlight;
        else
            m_submitRefused = true;   // ShaderCompiler::Submit returns 0 when unavailable
        return id;
    }

    bool ShaderEditorDocument::ConsumeResult(const Arcane::ShaderCompileResult& result)
    {
        // Chain-mode jobs first (mutually exclusive with the single-path ids --
        // Rebuild clears whichever set is not in play).
        for (std::size_t p = 0; p < m_passJobs.size(); ++p)
        {
            PassJobs& pj = m_passJobs[p];
            const bool chainVs = result.jobId == pj.vsJob && pj.vsJob != 0;
            const bool chainPs = result.jobId == pj.psJob && pj.psJob != 0;
            if (!chainVs && !chainPs)
                continue;
            if (m_jobsInFlight > 0) --m_jobsInFlight;   // a live job answered (s3.2)
            const auto& target = m_services.backend == Arcane::GraphicsBackend::Vulkan
                                     ? result.spirv : result.dxil;
            if (chainPs)
            {
                pj.diags = target.diags;
                if (p == 0)
                    m_diags = target.diags;   // single-path mirror
                // Badges belong to the ACTIVE pass's canvas (any pass may be
                // graph-owned now).
                RebuildDiagBadges();
            }
            if (chainVs && p == 0)
                m_vsDiags = target.diags;   // one vertex stage, every pass alike
            if (target.succeeded)
            {
                (chainVs ? pj.vsBytes : pj.psBytes) = target.bytecode;
                BindIfComplete();
            }
            return true;
        }

        const bool isVs = result.jobId == m_vsJob && m_vsJob != 0;
        const bool isPs = result.jobId == m_psJob && m_psJob != 0;
        if (!isVs && !isPs)
            return false;
        if (m_jobsInFlight > 0) --m_jobsInFlight;   // a live job answered (s3.2)

        const auto& target = m_services.backend == Arcane::GraphicsBackend::Vulkan
                                 ? result.spirv : result.dxil;
        if (isPs)
        {
            m_diags = target.diags;   // the ps stage carries the designer-relevant diags
            RebuildDiagBadges();
        }
        if (isVs)
            m_vsDiags = target.diags;   // the vertex stage's own errors (displace)
        if (target.succeeded)
        {
            (isVs ? m_vsBytes : m_psBytes) = target.bytecode;
            BindIfComplete();
        }
        return true;
    }

    void ShaderEditorDocument::BindIfComplete()
    {
        if (CompilesAsChain())
        {
            BindChainIfComplete();
            return;
        }
        const bool sprite = m_surface == 1;
        // THE READINESS TEST IS THE BLOBS + THE TEMPLATE. The bytes below are
        // the whole product of this function: it builds no device object.
        if (m_vsBytes.empty() || m_psBytes.empty() || !m_pendingTemplate)
            return;

        // ---- THE SEVERANCE ----
        // The stitched blobs are RETAINED rather than consumed: a device can
        // build a pipeline from bytecode but cannot adopt a compiled shader
        // OBJECT it did not create, and the preview builds its own NRI
        // pipeline from exactly these bytes.
        auto vsBlob = std::make_shared<const std::vector<std::uint8_t>>(std::move(m_vsBytes));
        auto psBlob = std::make_shared<const std::vector<std::uint8_t>>(std::move(m_psBytes));
        // The members are the IN-FLIGHT accumulators, not storage: the moves
        // above leave them valid-but-unspecified, and these two clears are the
        // same two statements that used to sit after createShader, meaning the
        // same thing (this compile is spent; the next Rebuild refills them).
        m_vsBytes.clear();
        m_psBytes.clear();

        PromotePendingInstance();

        // The device-free publication, AFTER the promote: the desc has to
        // name the instance the pipelines were just built against, or a live
        // param edit would be written into one and read from the other.
        // A non-chain fullscreen material declares NO InputTexture
        // (BuildMaterialShaderSource's chainInputs defaults to 0), so its
        // graph description is a one-pass chain with zero input slots -- which
        // is layout-identical, because both the source generator and
        // PostChainNode size the texture range as
        // templ->TextureCount() + chainInputSlots.
        if (sprite)
        {
            m_graphPost = Arcane::PostChainDesc{};
            m_graphSpriteBlobs = SpriteBlobs{ vsBlob, psBlob };
        }
        else
        {
            m_graphSpriteBlobs = SpriteBlobs{};
            m_graphPost.templ           = m_boundTemplate;
            m_graphPost.instance        = m_instance;
            m_graphPost.chainInputSlots = 0;
            m_graphPost.passes.assign(1, Arcane::PostChainPassDesc{ vsBlob, psBlob, {} });
        }
        PublishGraphPreview();
    }

    void ShaderEditorDocument::PromotePendingInstance()
    {
        // Promote pending -> bound. Instance mode first layers the parent chain
        // (base's saved values innermost) so resolution walks child override ->
        // parents -> //@param default. Then migrate this document's own
        // overrides by name hash (first bind applies the asset's saved values;
        // params a snippet edit dropped are rejected by Set and retired).
        auto fresh = std::make_shared<Arcane::MaterialInstance>(m_pendingTemplate);
        for (auto it = m_parentChain.rbegin(); it != m_parentChain.rend(); ++it)
        {
            Arcane::ApplyMaterialParams(*it, *fresh);
            fresh = std::make_shared<Arcane::MaterialInstance>(
                std::shared_ptr<const Arcane::MaterialInstance>(fresh));
        }
        if (m_instance)
        {
            // Overrides forward by hash -- through the pending-rename
            // translation, so an assisted rename carries the value onto the
            // renamed decl instead of retiring it.
            for (const auto& [hash, value] : m_instance->Overrides())
                fresh->Set(TranslateOverrideHash(hash, *m_pendingTemplate), value);
        }
        else
        {
            Arcane::ApplyMaterialParams(m_data, *fresh);
        }
        // Re-baseline the dirty verdict against the FRESH instance's serial
        // space; unsaved param edits ride across the swap as the base flag.
        const bool paramsWereDirty = ParamsDirty();
        m_instance = std::move(fresh);
        m_paramsBaseDirty = paramsWereDirty;
        m_savedParamSerial = m_instance->EffectiveSerial();
        m_boundTemplate = m_pendingTemplate;
        m_boundMetas = m_metas;
    }

    void ShaderEditorDocument::BindChainIfComplete()
    {
        // THE DESCRIPTION BELOW IS THE WHOLE PRODUCT of this function: it
        // builds no device object.
        if (!m_pendingTemplate || m_passJobs.empty())
            return;
        for (const PassJobs& pj : m_passJobs)
            if (pj.vsBytes.empty() || pj.psBytes.empty())
                return;   // a stage is still in flight (or failed -- last-good stays)

        // Retained for every pass, so the graph recorder reads the ONE compile
        // (see BindIfComplete's severance note).
        std::vector<Arcane::PostChainPassDesc> graphPasses;
        graphPasses.reserve(m_passJobs.size());
        for (std::size_t p = 0; p < m_passJobs.size(); ++p)
        {
            PassJobs& pj = m_passJobs[p];
            Arcane::PostChainPassDesc gp;
            gp.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(pj.vsBytes));
            gp.psBytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(pj.psBytes));
            if (p < m_passInputs.size())
                gp.inputs = m_passInputs[p];
            // Same statements, same meaning: this compile is spent.
            pj.vsBytes.clear();
            pj.psBytes.clear();
            graphPasses.push_back(std::move(gp));
        }

        PromotePendingInstance();

        // ...and the device-free twin, published after the promote for the
        // reason BindIfComplete states.
        m_graphSpriteBlobs = SpriteBlobs{};
        m_graphPost.templ           = m_boundTemplate;
        m_graphPost.instance        = m_instance;
        m_graphPost.chainInputSlots = m_chainInputSlots;
        m_graphPost.passes          = std::move(graphPasses);
        PublishGraphPreview();
    }

    // =====================================================================
    // THE PREVIEW
    // =====================================================================
    // Declared against the SAME machinery the runtime renders a scene with:
    //
    //   * the checkerboard backdrop and the sprite quad -> a DEVICE-LESS
    //     Batcher2D drained by Batch2DNode;
    //   * the fullscreen material / pass chain -> FrameDesc::post, i.e. the
    //     PostChainDesc this document publishes, run by PostChainNode;
    //   * kSceneInput -> the batch node's CANVAS, which is where the
    //     checkerboard lands -- so the pass samples the canvas the checker was
    //     drawn into rather than a separate stand-in texture. One fewer
    //     texture, same picture.
    //
    // WHAT IS DELIBERATELY ABSENT, so the gap stays named rather than
    // discovered: per-node THUMBNAILS on the graph canvas. That would need one
    // render target and one pass PER GRAPH NODE, and a graph frame declares
    // exactly one canvas and one output -- N thumbnails is N frames or N
    // contexts, neither of which is a preview.
    void ShaderEditorDocument::EnsureGraphPreviewContext()
    {
        // The seam is unset in every headless test (no EditorApp at all),
        // and its resolver returns null during boot: either way this is a
        // null check that never latches (s3.2).
        if (m_graphPreview || !m_services.hostConfig || !m_services.chromeGraph)
            return;
        Arcane::NriGraphContext* chrome = m_services.chromeGraph();
        if (!chrome)
            return;   // the seam is not up yet (boot): a null check, never a latch

        // NodeSet{} -- batch + post + tonemap and nothing else. A preview has
        // no host chrome, no game HUD and nothing to pick, and NodeSet's own
        // doc says a node a context will never declare is a descriptor pool
        // nobody reads.
        ++m_previewVehicleAttempts;
        m_graphPreview = Arcane::NriGraphContext::CreateOffscreen(
            *m_services.hostConfig, chrome->Device(),
            m_graphPreviewSize, m_graphPreviewSize);
        if (!m_graphPreview)
        {
            // Degraded, not fatal, and it degrades to exactly what a missing
            // device already degrades to: no preview image. The refusal is
            // already logged inside CreateOffscreen.
            m_previewVehicleFailed = true;   // Tick stops retrying; a later bind still tries (s3.2)
            ARC_WARN("ShaderEditorDocument '{}': the graph preview context could not be created "
                     "-- this document shows no preview", m_title);
            return;
        }
        m_previewVehicleFailed = false;
        m_previewHud = chrome->ImGuiHud();

        // The two injected seams, copied from EditorApp::CreateGraphVehicles'
        // viewport block and for the same reason: a material's texture params
        // are Guids, and this device has to resolve them itself. Both re-read
        // CurrentProject() per call, so they survive a project switch.
        if (m_services.runtime)
        {
            m_graphPreview->SetAssetResolver(
                [rt = m_services.runtime](const Arcane::Guid& id)
                    -> std::optional<std::filesystem::path>
                {
                    const Arcane::Project* project = rt->CurrentProject();
                    return project ? project->ResolveAsset(Arcane::AssetId::FromGuid(id))
                                   : std::nullopt;
                });
            m_graphPreview->SetPixelSupply(
                [rt = m_services.runtime](const Arcane::Guid& id) -> const Arcane::PixelData*
                {
                    return rt->AssetsFacade().PixelsFor(id);
                });
        }

        // The mesh-surface preview's ONE mesh (T3-D6): the sphere, served by
        // value-captured shared ownership so a retired vehicle never reaches
        // back into a destroyed document.
        if (!m_previewSphere)
            m_previewSphere = std::make_shared<const Arcane::MeshData>(BuildMaterialPreviewSphere());
        m_graphPreview->SetMeshSupply(
            [sphere = m_previewSphere](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult
            {
                if (id == kMaterialPreviewSphereId && sphere && !sphere->vertices.empty())
                    return { sphere.get(), Arcane::MeshResolveState::Ready };
                return { nullptr, Arcane::MeshResolveState::Failed };
            });
        m_meshPreviewPresented = false;

        // The document's OWN recorder -- owned rather than borrowed from the
        // editor: this frame is declared from Tick (phase 13), long after
        // phase 10 drained the editor's scene batcher, and two owners of one
        // batcher is how two frames' content merges into one. (Create took a
        // device + ShaderLibrary pair it was only ever passed nulls for; both
        // parameters went at ABI v15.)
        m_graphBatch = Arcane::Batcher2D::Create();
        if (!m_graphBatch)
            ARC_WARN("ShaderEditorDocument '{}': the graph preview batcher could not be created",
                     m_title);
    }

    void ShaderEditorDocument::PublishGraphPreview()
    {
        // Called from both bind sites (BindIfComplete, BindChainIfComplete).
        // Every call below is a no-op when the preview seam is null, which is
        // what keeps this one line at each bind site rather than a branch.
        EnsureGraphPreviewContext();
        RefreshGraphSpriteBinding();
    }

    void ShaderEditorDocument::RefreshGraphSpriteBinding()
    {
        // The shared instance pointer (so live param edits keep flowing
        // through PackCB), and a register-then-update shape. The material is
        // registered from BYTES -- Batch2DNode builds its own NRI pipeline
        // from them -- and the texture params resolve by Guid through this
        // context's NriTextureCache. That is the ONLY shape a registration
        // has: Material2DDesc carries no device objects at all.
        if (!m_graphBatch || m_surface != 1 || !m_graphSpriteBlobs.vs ||
            !m_graphSpriteBlobs.ps || !m_instance || !m_boundTemplate)
            return;
        // A re-compile is a NEW blob pointer; nothing else can change what the
        // registration says, so this is a pointer compare rather than a
        // re-registration every frame.
        if (m_graphSpriteStamp == m_graphSpriteBlobs.ps.get() &&
            m_graphSpriteMaterial != Arcane::Batcher2D::kInvalidMaterialId)
            return;

        Arcane::Material2DDesc desc;
        desc.templ    = m_boundTemplate;
        desc.instance = m_instance;
        desc.vsBytes  = m_graphSpriteBlobs.vs;
        desc.psBytes  = m_graphSpriteBlobs.ps;
        // No texture table to size: the t1.. WIDTH comes from
        // `templ->TextureCount()`, and the recorder resolves the declared
        // Guids off `instance` itself.

        if (m_graphSpriteMaterial != Arcane::Batcher2D::kInvalidMaterialId)
        {
            if (!m_graphBatch->UpdateMaterial(m_graphSpriteMaterial, std::move(desc)))
                ARC_WARN("ShaderEditorDocument '{}': graph sprite preview update failed", m_title);
        }
        else
        {
            m_graphSpriteMaterial = m_graphBatch->RegisterMaterial(std::move(desc));
        }
        m_graphSpriteStamp = m_graphSpriteBlobs.ps.get();
    }

    std::uint64_t ShaderEditorDocument::GraphPreviewTextureId() const noexcept
    {
        return m_graphPreview ? m_graphPreview->OffscreenTextureId() : 0;
    }

    void ShaderEditorDocument::RenderGraphPreview(double dt)
    {
        if (!m_graphPreview || !m_graphBatch)
            return;

        const bool sprite = m_surface == 1;
        const bool mesh = SurfaceOf(m_surface) == Arcane::MaterialSurface::Mesh;
        const bool haveSprite =
            sprite && m_graphSpriteMaterial != Arcane::Batcher2D::kInvalidMaterialId;
        const bool haveFullscreen = !sprite && !mesh &&
            m_graphPost.templ && m_graphPost.instance && !m_graphPost.passes.empty();
        // A mesh surface compiles nothing: it is ready the moment its params
        // are bound (Rebuild's synchronous MeshParamTemplate promote).
        const bool haveMesh = mesh && m_instance && m_previewSphere;
        // NOTHING COMPILED YET IS NOT A FRAME. Declaring one would clear the
        // output to the checkerboard and then show it as "the preview", which
        // is the blank-but-labeled failure this phase refuses; the panel draws
        // no image instead (DrawPreviewPanel gates on the same predicate,
        // PreviewReady()).
        if (!haveSprite && !haveFullscreen && !haveMesh)
            return;

        Arcane::GlobalParams globals;
        globals.time = static_cast<float>(m_animTime);
        globals.deltaTime = static_cast<float>(dt);
        globals.viewportWidth = static_cast<float>(m_graphPreviewSize);
        globals.viewportHeight = static_cast<float>(m_graphPreviewSize);

        Arcane::Batcher2D& b = *m_graphBatch;
        // Extent only, since ABI v15: the null command list + null framebuffer
        // that used to lead Begin were read by End() alone, and the NODE
        // drains this batch rather than this code calling End().
        b.Begin(m_graphPreviewSize, m_graphPreviewSize);
        // AFTER Begin, matching SubmitSceneToBatcher's own SetGlobals call --
        // so a future change to what Begin resets cannot silently drop this.
        b.SetGlobals(globals);

        // The checkerboard. On the fullscreen surface it is ALSO what
        // kSceneInput samples, which is what makes it the scene stand-in.
        const float extent = static_cast<float>(m_graphPreviewSize);
        const ShaderEditorSettings& preview = Arcane::Settings<ShaderEditorSettings>();
        const float cell = preview.previewCheckerCell;   // editor.shader.previewCheckerCell
        const glm::vec4 light(preview.previewCheckerLight.r, preview.previewCheckerLight.g,
                              preview.previewCheckerLight.b, preview.previewCheckerLight.a);
        for (int y = 0; y * cell < extent; ++y)
            for (int x = 0; x * cell < extent; ++x)
                if ((x + y) & 1)
                    b.Rect(glm::vec2(x * cell, y * cell), glm::vec2(cell, cell), light);

        if (haveSprite)
        {
            const float s = preview.previewCheckerSpriteScale * extent;
            b.QuadMaterial(m_graphSpriteMaterial,
                           glm::vec2((extent - s) * 0.5f, (extent - s) * 0.5f),
                           glm::vec2(s, s),
                           glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
        }

        Arcane::NriGraphContext::FrameDesc vp;
        vp.batch   = &b;
        vp.globals = &globals;

        // THE MESH SURFACE (T3-D6): the thumbnail's lit sphere, coloured from
        // the bound instance's CURRENT values -- an edit (or an instance
        // override) shows on the next frame, before any save. The albedo
        // resolves to a slot in THIS vehicle's bindless table.
        Arcane::MeshInstance sphereInstance;
        Arcane::MeshSceneDesc meshScene;
        if (haveMesh)
        {
            const MeshPreviewParams in = MeshPreviewInputs();
            std::uint32_t slot = 0xFFFFFFFFu;   // BindlessTable::kInvalidSlot: the flat baseColor path
            if (in.albedo.IsValid())
                slot = m_graphPreview->ResolveMeshAlbedoSlot(in.albedo);
            sphereInstance = MaterialPreviewSphereInstance(
                glm::vec4(in.baseColor[0], in.baseColor[1], in.baseColor[2], in.baseColor[3]), slot);
            meshScene = MaterialPreviewSphereScene(std::span<const Arcane::MeshInstance>(&sphereInstance, 1));
            vp.mesh = &meshScene;
        }

        // VIEW-ANY-INTERMEDIATE: the chain is TRUNCATED at the viewed pass,
        // so the last declared pass writes the output. The copy
        // is free of a rebuild -- PostChainNode's chain stamp is built from
        // the blob POINTERS, the sizes and the input lists, so a truncation
        // that does not move re-stamps identically and hits the cache.
        Arcane::PostChainDesc truncated;
        if (haveFullscreen)
        {
            const std::size_t last =
                m_viewPass < 0 ? m_graphPost.passes.size() - 1
                               : std::min(static_cast<std::size_t>(m_viewPass),
                                          m_graphPost.passes.size() - 1);
            if (last + 1 == m_graphPost.passes.size())
            {
                vp.post = &m_graphPost;
            }
            else
            {
                truncated = m_graphPost;
                truncated.passes.resize(last + 1);
                vp.post = &truncated;
            }
        }

        const Arcane::NriGraphContext::FrameOutcome outcome =
            m_graphPreview->RenderFrameOffscreen(vp);
        // Skipped is routine and unacted-on (a zero extent cannot happen here
        // -- the target is a fixed 512 -- so this is the "resize could not
        // create a replacement" corner, which this document never enters).
        // FAILED is reported ONCE and then the vehicle is dropped: a preview
        // that cannot record must not spend a frame's worth of validation
        // errors per frame for the rest of the session, and unlike the
        // editor's own frames a document preview failing is not a reason to
        // end the run.
        if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)
        {
            ARC_ERROR("ShaderEditorDocument '{}': the graph preview frame failed -- dropping this "
                      "document's preview vehicle", m_title);
            m_previewFrameFailed = true;   // Tick stops retrying; the next bind rebuilds (s3.2)
            DestroyGraphPreview();
        }
        else if (outcome == Arcane::NriGraphContext::FrameOutcome::Presented)
        {
            m_previewFrameFailed = false;  // the next good frame clears the latch
            m_meshPreviewPresented = haveMesh;
        }
    }

    bool ShaderEditorDocument::PreviewUploadsPending() const noexcept
    {
        return m_graphPreview && m_graphPreview->HasPendingUploads();
    }

    ShaderEditorDocument::MeshPreviewParams ShaderEditorDocument::MeshPreviewInputs() const
    {
        // The same two names MeshMaterialCache resolves off disk (and the
        // thumbnail harvester through it), read off the LIVE instance -- which
        // already layers the parent chain under this document's overrides
        // (PromotePendingInstance), so an instance inherits exactly what the
        // saved resolve would give it.
        MeshPreviewParams out;
        if (!m_instance)
            return out;
        Arcane::MatParamValue v;
        if (m_instance->GetParam("baseColor", v) && v.type == Arcane::MatParamType::Color)
            out.baseColor = { v.f[0], v.f[1], v.f[2], v.f[3] };
        if (m_instance->GetParam("albedo", v) && v.type == Arcane::MatParamType::Texture)
            out.albedo = v.tex;
        return out;
    }

    void ShaderEditorDocument::DestroyGraphPreview()
    {
        if (!m_graphPreview)
            return;

        // The CPU half goes immediately -- a Batcher2D owns no GPU object on
        // this arm (it is device-less by construction) and nothing outside this
        // document has ever seen it.
        m_graphSpriteMaterial = Arcane::Batcher2D::kInvalidMaterialId;
        m_graphSpriteStamp = nullptr;
        m_meshPreviewPresented = false;
        m_graphBatch.reset();
        Arcane::RenderOverlayNode* hud = std::exchange(m_previewHud, nullptr);

        // ===== THE VEHICLE IS RETIRED, NOT DESTROYED (see DocServices::
        // retireGraphPreview for the full reasoning) =====
        // This runs from ~ShaderEditorDocument, and a document is destroyed
        // INSIDE the editor's ImGui pass (phase 14) while the chrome frame
        // that replays this frame's draw lists is recorded at phase 19. Those
        // lists still name this output BY RAW POINTER. So the app takes it and
        // destroys it at the top of the next frame, with the invalidate.
        if (m_services.retireGraphPreview)
        {
            m_services.retireGraphPreview(std::move(m_graphPreview));
            return;
        }

        // ===== NO SINK: THE CROSS-CONTEXT INVALIDATE, OWED *BEFORE* =====
        // Reached only where no further frame will be recorded (the app drains
        // its retire list and then closes every document at shutdown, and a
        // [gpu] test that wires no retire sink).
        //
        // The chrome context's ImGuiNri caches per texture BY RAW POINTER --
        // and NRI does not ref-count, so the next allocation may land on the
        // address this one is about to vacate. A stale entry then serves a
        // descriptor + descriptor set naming a destroyed texture. That is
        // NriGraphContext.hpp's item (2), and no declaration order solves it:
        // the view belongs to the chrome context's lane and the texture to
        // this one, and nothing orders two graveyards. `Now` destroys the view
        // inside the call behind its own DeviceWaitIdle, so run BEFORE the
        // destroy below it provably dies while the texture is still alive.
        //
        // Unconditional and idempotent: a miss is routine (a preview that
        // never drew), and a null node is an early-out inside the hook.
        if (hud)
            (void)hud->InvalidateUserTextureNow(m_graphPreview->OffscreenOutput());   // the node recorded at creation (s3.2)
        m_graphPreview.reset();
    }

    bool ShaderEditorDocument::HasErrors() const
    {
        if (!m_parseErrors.empty())
            return true;
        for (const Arcane::ShaderDiag& d : m_diags)
            if (d.severity == Arcane::ShaderDiagSeverity::Error)
                return true;
        for (const PassJobs& pj : m_passJobs)
            for (const Arcane::ShaderDiag& d : pj.diags)
                if (d.severity == Arcane::ShaderDiagSeverity::Error)
                    return true;
        // Vertex-stage errors, filtered to the vertex body (same filter as
        // ForEachDiagnosticRow's vertex block). CompiledVertexSnippet: an
        // instance compiles its BASE's vertex stage (fix round 1).
        if (const std::string& vs = CompiledVertexSnippet(); !vs.empty())
        {
            const int vsLines = 1 + static_cast<int>(std::count(vs.begin(), vs.end(), '\n'));
            for (const Arcane::ShaderDiag& d : m_vsDiags)
            {
                const int rel = d.line - m_vsLineOffset;
                if (rel >= 1 && rel <= vsLines &&
                    d.severity == Arcane::ShaderDiagSeverity::Error)
                    return true;
            }
        }
        return false;
    }

    SaveGestureResult ShaderEditorDocument::RequestSave()
    {
        // Error-guarded save (UE's pre-apply guard shape): saving broken WIP is
        // allowed, but only through an explicit confirm. This guard used to sit
        // inside the toolbar's Save button; it lives here now so the Ctrl+S
        // route (the document's AND the Inspector page's) cannot walk past it.
        // The modal (DrawSaveWithErrorsConfirm) is drawn by the toolbar or the
        // material page, and still calls the unguarded Save on "Save Anyway".
        if (HasErrors())
        {
            m_confirmSaveWithErrors = true;
            return SaveGestureResult::Deferred;
        }
        return Save() ? SaveGestureResult::Saved : SaveGestureResult::Refused;
    }

    bool ShaderEditorDocument::Save()
    {
        m_data.snippet = m_snippet;
        // Review M1: before the first successful bind there is no instance to
        // harvest values from -- keep the loaded params instead of wiping them.
        if (m_instance && m_boundTemplate)
        {
            m_data.params.clear();
            for (const auto& [hash, value] : m_instance->Overrides())
                if (const Arcane::ParamDecl* d = m_boundTemplate->Find(hash))
                {
                    // An INSTANCE still bound to the pre-rename base harvests
                    // the OLD decl name -- translate so Save never writes an
                    // orphan back over the propagated file. (A base's own
                    // template already carries the new name.)
                    std::string pname = d->name;
                    if (IsInstance())
                        for (const auto& [oldName, newName] : m_paramRenames)
                            if (pname == oldName)
                                pname = newName;
                    m_data.params.emplace_back(std::move(pname), value);
                }
        }
        if (!Arcane::SaveMaterialAsset(m_path, m_data))
            return false;
        // Idempotent: heals assets opened from paths the registry has never seen
        // (created outside the editor flows, or before this session).
        if (m_services.runtime)
            m_services.runtime->RegisterCreatedAsset(m_path);
        m_dirty = false;
        m_paramsBaseDirty = false;
        m_savedParamSerial = m_instance ? m_instance->EffectiveSerial() : 0;
        if (m_services.onAssetSaved)
            m_services.onAssetSaved(m_data.id);
        return true;
    }

    bool ShaderEditorDocument::ParamsDirty() const
    {
        return m_paramsBaseDirty ||
               (m_instance && m_instance->EffectiveSerial() != m_savedParamSerial);
    }

    std::optional<ShaderEditorDocument::MeshMaterialMetadataState>
    ShaderEditorDocument::CaptureMeshMaterialMetadata() const
    {
        if (SurfaceOf(m_surface) != Arcane::MaterialSurface::Mesh)
            return std::nullopt;
        return MeshMaterialMetadataState{ m_data.blend, m_data.alphaCutoff, m_data.twoSided };
    }

    void ShaderEditorDocument::ApplyMeshMaterialMetadata(MeshMaterialMetadataState state)
    {
        if (SurfaceOf(m_surface) != Arcane::MaterialSurface::Mesh)
            return;

        if (state.alphaCutoff)
        {
            if (!std::isfinite(*state.alphaCutoff))
                *state.alphaCutoff = 0.5f;
            *state.alphaCutoff = std::clamp(*state.alphaCutoff, 0.0f, 1.0f);
        }

        if (m_data.blend == state.blend &&
            m_data.alphaCutoff == state.alphaCutoff &&
            m_data.twoSided == state.twoSided)
            return;

        m_data.blend = state.blend;
        m_data.alphaCutoff = state.alphaCutoff;
        m_data.twoSided = state.twoSided;
        m_dirty = true;
    }

    void ShaderEditorDocument::SetMeshMaterialMetadataWithUndo(MeshMaterialMetadataState state)
    {
        const std::optional<MeshMaterialMetadataState> before = CaptureMeshMaterialMetadata();
        if (!before)
            return;   // not a mesh surface: there is no metadata to edit
        ApplyMeshMaterialMetadata(std::move(state));
        PushMeshMaterialMetadataUndo(*before);
    }

    void ShaderEditorDocument::PushMeshMaterialMetadataUndo(const MeshMaterialMetadataState& before)
    {
        const std::optional<MeshMaterialMetadataState> after = CaptureMeshMaterialMetadata();
        if (!after || *after == before)
            return;   // nothing changed (or the clamp collapsed the request back) -- no step, redo intact
        if (!UndoStack())
            return;
        // Labelled by the field that changed, blend first when several did (a
        // blend switch is the edit the others ride along with).
        const char* label = before.blend != after->blend             ? "Edit Blend"
                          : before.alphaCutoff != after->alphaCutoff ? "Edit Alpha Cutoff"
                                                                     : "Edit Two Sided";
        UndoStack()->Push(std::make_unique<MeshMaterialMetadataCommand>(m_anchor, label, before, *after));
    }

    void ShaderEditorDocument::ApplyParamEdit(std::uint32_t nameHash, bool hasValue,
                                              const Arcane::MatParamValue& value)
    {
        if (!m_instance)
            return;
        // A param the current snippet dropped is rejected by Set -- the step
        // no-ops rather than corrupting an unrelated instance.
        if (hasValue)
            m_instance->Set(nameHash, value);
        else
            m_instance->ClearOverride(nameHash);
        // NO RE-BIND IS OWED on a TEXTURE param undo/redo: the preview's
        // texture params resolve by Guid, fresh, every frame through
        // NriTextureCache.
    }

    bool ShaderEditorDocument::PreviewReady() const
    {
        // Gated on the VEHICLE: without one (no seam, or the context failed
        // to create) there is nothing to be ready.
        if (!m_graphPreview)
            return false;
        if (SurfaceOf(m_surface) == Arcane::MaterialSurface::Mesh)
            return m_meshPreviewPresented;   // the sphere frame landed (T3-D6)
        return m_surface == 1
                   ? m_graphSpriteMaterial != Arcane::Batcher2D::kInvalidMaterialId
                   : (m_graphPost.templ && m_graphPost.instance &&
                      !m_graphPost.passes.empty());
    }

    PreviewStatus ShaderEditorDocument::ComputeStatus() const
    {
        PreviewStatusInputs in;
        in.notCompiledHere   = SurfaceOf(m_surface) == Arcane::MaterialSurface::Mesh;
        in.compilerAvailable = m_services.compiler && m_services.sources
                            && m_services.compiler->IsAvailable() && !m_submitRefused;
        in.jobsInFlight      = m_jobsInFlight;
        in.hasErrors         = HasErrors();
        in.deviceSeam        = m_services.chromeGraph && m_services.chromeGraph() != nullptr;
        in.vehicleFailed     = m_previewVehicleFailed;
        in.frameFailed       = m_previewFrameFailed;
        in.imageBound        = PreviewReady();
        return ComputePreviewStatus(in);
    }

    std::string& ShaderEditorDocument::ActiveSnippet()
    {
        if (m_editVertex)
            return m_data.vertexSnippet;   // the ONE vertex stage (doc-level)
        if (m_activePass > 0 &&
            m_activePass <= static_cast<int>(m_data.passes.size()))
            return m_data.passes[static_cast<std::size_t>(m_activePass) - 1].snippet;
        return m_snippet;
    }

    const std::string& ShaderEditorDocument::CompiledVertexSnippet() const
    {
        static const std::string kNone;
        const Arcane::MaterialAssetData* src = CompiledSource();
        return src ? src->vertexSnippet : kNone;
    }

    std::string ShaderEditorDocument::PassLabel(std::size_t pass) const
    {
        if (pass == 0)
            return "base";
        // CompiledSource: an instance's pass errors name its BASE's passes.
        const Arcane::MaterialAssetData* src = CompiledSource();
        if (src && pass <= src->passes.size())
        {
            const std::string& n = src->passes[pass - 1].name;
            if (!n.empty())
                return n;
        }
        return "pass " + std::to_string(pass);
    }

    void ShaderEditorDocument::Tick(double dt)
    {
        // Before any early return below: diagnostics are published even for a
        // document whose preview is not ready (a material that fails to compile
        // is exactly the case that has something to say).
        PublishDiagnostics();
        m_animTime += dt;
        // THE WHOLE RENDER PHASE. A missing vehicle simply means no preview
        // this Tick -- the same degraded-not-fatal outcome as any other
        // vehicle failure (see EnsureGraphPreviewContext).
        //
        // THE LATE-BOUND SEAM'S RETRY (s3.2): a document opened during boot
        // compiled and bound with no vehicle; PublishGraphPreview builds it and
        // re-registers the sprite binding the bind could not make. Not after a
        // creation refusal, and not after a frame failure (that rebuilds on
        // the next bind -- today's drop-and-rebuild-on-bind).
        // A MESH-surface material builds its vehicle here too (T3-D6): it has
        // no bind site (nothing compiles), and its preview -- the lit sphere,
        // drawn by the document tab (instance) or the page square (base) --
        // is a frame like any other surface's.
        if (!m_graphPreview && !m_previewVehicleFailed && !m_previewFrameFailed)
            PublishGraphPreview();
        if (m_graphPreview)
            RenderGraphPreview(dt);
    }

    void ShaderEditorDocument::Draw(bool& requestClose)
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard. It
        // covers the early return below (Begin refused: collapsed window or a
        // background tab, where no widget inside can report its deactivation).
        const EditGesture::ScopeGuard gestureGuard{ UndoStack(), m_gesture };

        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
        ImGuiWindowFlags flags = Dirty() ? ImGuiWindowFlags_UnsavedDocument : 0;
        if (!ImGui::Begin(m_windowLabel.c_str(), &open, flags))
        {
            // Collapsed or a background tab: not focused, and it must be said
            // out loud -- a stale true here would hand Ctrl+S to a document the
            // user cannot even see.
            m_windowFocused = false;
            ImGui::End();
            requestClose = !open;
            return;
        }
        // What Ctrl+S resolves against (DocumentHost::FocusedDoc).
        // RootAndChildWindows so the canvas, the text editor and every child
        // region inside the document still count as "in this document".
        m_windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        if (m_windowFocused) EditorActions::Get().MarkContextActive(ActionContext::Document);
        if (m_windowFocused && EditorActions::Get().Pressed("document.save"))
            RequestSave();

        DrawToolbar();

        // ONE column. The preview and the params editor moved OUT to the
        // Inspector's material page (DrawMaterialPageBody, drawn by whichever
        // Inspector instance shows this document), which is what retired this
        // window's right column and the horizontal "##splitmain" divider that
        // used to size it: with the right column gone there was nothing left
        // for a horizontal split to divide.
        // The page has no split either: its preview is a collapsible square (s5.3).
        if (!IsInstance())
        {
            if (m_activePass > static_cast<int>(m_data.passes.size()))
                m_activePass = 0;   // stale selection after an outside reload

            // ONE canvas area, TWO mutually exclusive views. The old stacked
            // split (a fixed 170px pass strip permanently above the graph) is
            // gone: it spent a fifth of the tab on a map the user needed only
            // between edits, and it put two node canvases on screen at once,
            // which read as one editor disagreeing with itself. UE shows one
            // graph at a time and navigates between levels
            // (BlueprintEditor.cpp:4439-4443); this is that shape.
            //
            // The chain overview only exists for fullscreen BASE materials --
            // sprite chains are refused and instances re-value the base's chain
            // -- so on a sprite surface the pass view is unreachable and the
            // breadcrumb has nowhere to go. Skip the bar entirely there rather
            // than draw a root crumb that does nothing.
            const bool chainAvailable = m_surface == 0;

            // MOUSE4/MOUSE5 = back/forward, the browser gesture. The SDL3
            // backend maps X1/X2 to ImGui buttons 3/4
            // (imgui_impl_sdl3.cpp:445-446) and ImGuiMouseButton_COUNT is 5
            // (imgui.h:2045), so these are real, tracked buttons -- and nothing
            // else in the editor binds them.
            //
            // GATED ON HOVER, not focus: it is a pointer gesture, so it belongs
            // to whatever the pointer is over, the same way it does in a
            // browser. RootAndChildWindows so the canvas and every child region
            // inside the document still count as "in this document"; without
            // the flags set, a popup over the window blocks it, which is what
            // we want while a context menu is open. The gate is what keeps the
            // buttons from navigating this document while the user is over the
            // Inspector or the viewport.
            //
            // Fullscreen base materials only -- the other surfaces have exactly
            // one view, so there is no history to walk.
            if (chainAvailable &&
                ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
            {
                if (ImGui::IsMouseClicked(ImGuiMouseButton(3)))
                    NavStep(-1);
                if (ImGui::IsMouseClicked(ImGuiMouseButton(4)))
                    NavStep(+1);
            }

            if (chainAvailable)
                DrawBreadcrumbBar();
            else
                m_inChainView = false;

            if (ChainViewShowing())
                DrawPassCanvas();
            // A mesh base material has no snippet, no graph and no pass
            // canvas to show here (Rebuild()'s own guard) -- falling through
            // to DrawSnippetEditor would hand the user a live text box that
            // edits `m_snippet`, which LoadMaterialAsset would then flag AND
            // STRIP on the very next reload (MaterialAsset.cpp's
            // KindIgnoresSnippetGraph diagnostic). Say so instead and point
            // at where authoring actually happens.
            else if (SurfaceOf(m_surface) == Arcane::MaterialSurface::Mesh)
            {
                ImGui::TextDisabled("mesh materials carry no shader source -- "
                                    "author baseColor / albedo in the Inspector's material page");
            }
            // The canvas serves whichever pass is active and graph-owned;
            // text-owned passes -- and the vertex stage -- get the text editor.
            // Both fill the rest of the tab themselves (each measures the
            // region at the point it opens).
            else if (ActiveGraphOwned() && !m_showGeneratedText && !m_editVertex)
                DrawGraphPanel();
            else
                DrawSnippetEditor();
        }
        else
        {
            // INSTANCE mode: an instance authors no source -- that belongs to
            // its base -- and its params now live in the Inspector's material
            // page, which would leave this tab empty. So the preview takes the
            // whole tab: an instance IS its values, and this tab OWNS its
            // preview (s5.3 amendment, user decision A): the page draws none
            // for an instance, so there is one preview, not two. The toolbar
            // above keeps the parent-chain affordances reachable; saving is
            // Ctrl+S, which needs no toolbar room at all.
            // A mesh surface's caption sits under the box (T3-D6).
            const float caption = SurfaceOf(m_surface) == Arcane::MaterialSurface::Mesh
                                      ? MeshPreviewCaptionHeight(ImGui::GetContentRegionAvail().x) : 0.0f;
            DrawPreviewPanel(ImVec2(0.0f, (std::max)(1.0f, ImGui::GetContentRegionAvail().y - caption)));
        }

        // Opened = selected; a click anywhere in the content (canvas
        // background, a node, the preview, the snippet text) re-selects the
        // material page (spec s3's one selection rule). AFTER the content: the
        // node-editor canvas draws in this window directly (the vendored
        // imgui-node-editor's BeginChild is commented out,
        // imgui_node_editor.cpp:1210-1214) and ImGuiEx::Canvas restores
        // io.MousePos at ed::End, so here the window is the hovered one and
        // the mouse is back in screen space. Only on this non-collapsed path:
        // a collapsed/background tab has no content to click.
        m_pageSel.NoteContentClick();

        ImGui::End();
        requestClose = !open;
    }

    void ShaderEditorDocument::DrawMaterialPageBody(PropertyGrid& grid)
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard.
        // The param rows below open gestures against m_gesture. The body
        // draws inside an Inspector instance window, AFTER the documents, and
        // on collapsed/refused frames too (InspectorWindows, s5.7) -- where no
        // widget inside can report its own deactivation, which is exactly
        // what this guard covers.
        const EditGesture::ScopeGuard gestureGuard{ UndoStack(), m_gesture };

        // The Inspector's Ctrl+S parks here too (RequestSaveFromInspector): the
        // page opens the confirm when the document window did not draw first.
        DrawSaveWithErrorsConfirm();

        // No title line (spec 2026-09-30 s4.3): the Inspector header's crumb
        // names this material ("<title> (Instance)" for an instance); the
        // body opens on the Preview section.
        // An INSTANCE's page has no Preview section (s5.3 amendment,
        // 2026-10-02, user decision A -- UE's Material Instance editor): its
        // document tab IS the preview, full-tab (Draw's INSTANCE mode), so
        // the page opens on the parameters instead of showing a second one.
        // The page child's height (s5.7), read before anything is laid out.
        const float pageHeight = ImGui::GetWindowHeight();
        if (!IsInstance() && grid.Section("Preview"))
        {
            // A square: the column's width, capped at a share of the page
            // (editor.inspector.materialPreviewFraction), centred. A mesh
            // surface gets the same square (T3-D6, the s5.3 amendment): its
            // lit-sphere preview, with the "not compiled here" caption below.
            const float availX = ImGui::GetContentRegionAvail().x;
            const float side = (std::max)(1.0f, (std::min)(availX, MaterialPreviewFraction() * pageHeight));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, (availX - side) * 0.5f));
            DrawPreviewPanel(ImVec2(side, side));
        }
        DrawRenderingSection(grid, UndoStack());
        DrawParamsSection(grid, UndoStack());
    }

    void ShaderEditorDocument::DrawToolbar()
    {
        // NO Save button: saving is Ctrl+S (routed to the focused document by
        // EditorAppFrame). The error guard that button carried did not go with
        // it -- it moved to RequestSave, which is what the shortcut runs.
        //
        // Losing the button cost the toolbar its guaranteed first item, which
        // the SameLine chain below was leaning on: an INSTANCE whose parent
        // chain has not resolved now draws nothing before the surface selector,
        // and a SameLine as a window's first call pulls the cursor up onto the
        // line above. Hence the explicit flag rather than an unconditional
        // SameLine.
        const PreviewStatus status = ComputeStatus();   // T1's model (s3.2): the toolbar and the toggle read one answer
        bool anyBefore = false;
        if (!IsInstance())
        {
            // Structural (snippet) controls are base-material-only; an instance
            // recompiles nothing -- it only re-values the parent's shader.
            ImGui::Checkbox("Live", &m_live);
            ImGui::SameLine();
            if (ImGui::Button("Compile"))
                RegenerateFromGraph();   // regenerates graph passes, then Rebuild
            if (ActiveGraphOwned())
            {
                // Read-only generated-code view (UE's HLSL window / SG's View
                // Generated Shader). No convert-out: graphs are THE authoring
                // tier; freeform HLSL lives in Custom nodes.
                ImGui::SameLine();
                ImGui::Checkbox("HLSL", &m_showGeneratedText);
                ImGui::SameLine();
                // "Output preview" (s5.2): gates DrawNodePreviewImage's
                // Output-node copy only, and is disabled while there is no
                // image to copy -- the tooltip says why.
                ImGui::BeginDisabled(!status.image);
                ImGui::Checkbox("Output preview", &m_showNodePreviews);
                ImGui::EndDisabled();
                if (!status.image &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Shows the material preview on the Output node. Unavailable: %s",
                                      NoPreviewReason(status).c_str());
            }
            // The vertex stage (%{VERTEX_BODY}): graph-owned materials author
            // it with the Vertex Output NODE and view it inside the HLSL
            // toggle (UE's shape: one graph, one read-only code viewer). Only
            // repair mode -- a graphless base -- hand-edits the text.
            if (!IsGraphOwned())
            {
                ImGui::SameLine();
                ImGui::Checkbox("Vertex", &m_editVertex);
            }
            else
                m_editVertex = false;
            anyBefore = true;   // the Live checkbox always draws
        }
        else if (!m_parentChain.empty())
        {
            ImGui::TextDisabled("instance of '%s'", m_parentChain.back().name.c_str());
            anyBefore = true;
        }
        if (anyBefore)
            ImGui::SameLine();
        ImGui::TextDisabled("Surface");   // s5.3: the combo names itself; it stays here, so a re-kind is still no step
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        // Preview-surface selector (Slice 8). On a base material this is a
        // STRUCTURAL edit: it re-kinds the asset (the surface is what the
        // material is FOR) and recompiles under the other template. Instances
        // preview under the switched surface without touching the base.
        // Pass chains are fullscreen-only: the selector LOCKS while extra
        // passes exist (no refusal path can lose data).
        //
        // ...AND IT LOCKS ON THE "mesh" KIND (F2a), for an unrelated reason: a
        // mesh material is not authored in this document at all. It carries no
        // snippet, no pass chain and no engine template to compile against
        // (MaterialSurface's own comment, Material/MaterialSource.hpp), so the
        // only thing a fullscreen/sprite picker could do to one is re-kind it
        // to something it is not -- and silently, since `kind` is otherwise
        // written back verbatim and nothing downstream would report the loss.
        // The pass-chain condition below does NOT already cover this: a mesh
        // material has neither `passes` nor `baseInputs`, so that test is false
        // for every one of them.
        const bool meshSurface = SurfaceOf(m_surface) == Arcane::MaterialSurface::Mesh;
        const bool surfaceLocked =
            meshSurface ||
            (!IsInstance() &&
             (!m_data.passes.empty() || !m_data.baseInputs.empty()));
        if (surfaceLocked)
            ImGui::BeginDisabled();
        int surface = m_surface;
        // "Mesh" appears in the item list ONLY when that is already what this
        // material IS: the combo has to be able to NAME the current index or it
        // draws a blank preview, but offering Mesh as a DESTINATION would be
        // authoring a mesh material, which is not this document's job.
        const bool surfacePicked = ImGui::Combo(
            "##surface", &surface,
            meshSurface ? "Fullscreen\0Sprite\0Mesh\0" : "Fullscreen\0Sprite\0");
        if (surfaceLocked)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(meshSurface
                    ? "\"mesh\" materials are not authored here -- they carry no "
                      "snippet and no template, and re-kinding one would silently "
                      "retarget the asset"
                    : "pass chains are fullscreen-only -- remove the "
                      "extra passes to change the surface");
        }
        if (surfacePicked && surface != m_surface)
        {
            m_surface = surface;
            if (!IsInstance())
            {
                // Through the shared map, so the kind written back always
                // round-trips to the index the picker was showing.
                m_data.kind = KindForSurfaceIndex(m_surface);
                m_dirty = true;
            }
            // Graph docs must re-CODEGEN, not just restitch -- the surface
            // gates node validity (VertexColor/SpriteTexture are sprite-only).
            RegenerateFromGraph();
        }
        ImGui::SameLine();
        // Two statuses in one line, compile then preview (s5.2, 9.8): never
        // "compiling..." for a finished compile that simply has no device.
        const std::string statusText = ToolbarStatusText(status);
        if (status.compile == CompileStatus::Errors)
            ImGui::TextColored(Theme::kError, "%s", statusText.c_str());
        else if (status.compile == CompileStatus::CompilerUnavailable)
            ImGui::TextColored(Theme::kAmber, "%s", statusText.c_str());
        else
            ImGui::TextDisabled("%s", statusText.c_str());
        ImGui::Separator();
        DrawSaveWithErrorsConfirm();
    }

    void ShaderEditorDocument::DrawSaveWithErrorsConfirm()
    {
        if (m_confirmSaveWithErrors)
        {
            ImGui::OpenPopup("Save With Errors?##matdoc");
            m_confirmSaveWithErrors = false;
        }
        if (ImGui::BeginPopupModal("Save With Errors?##matdoc", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            // "see Problems" because the rows no longer live in this window --
            // PublishDiagnostics routes them to the editor-wide Problems panel,
            // matching the shell's other deferrals.
            ImGui::TextUnformatted("This material has compile errors (see the "
                                   "Problems panel). Save anyway?");
            ImGui::Separator();
            if (ImGui::Button("Save Anyway"))
            {
                Save();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

    }

    // ---------------------------------------------- node page selection (s5.1.1)
    const Arcane::GraphNode* ShaderEditorDocument::FindGraphNode(std::size_t pass, std::uint32_t id) const
    {
        if (IsInstance() || pass > m_data.passes.size())
            return nullptr;
        const std::optional<Arcane::MaterialGraph>& g =
            pass == 0 ? m_data.graph : m_data.passes[pass - 1].graph;
        return g ? g->FindNode(id) : nullptr;
    }

    Arcane::GraphNode* ShaderEditorDocument::FindGraphNode(std::size_t pass, std::uint32_t id)
    {
        return const_cast<Arcane::GraphNode*>(std::as_const(*this).FindGraphNode(pass, id));
    }

    bool ShaderEditorDocument::Resolves(std::string_view key) const
    {
        if (m_pageSel.Resolves(key))
            return true;
        const std::optional<NodeKey> k = ParseNodeKey(key);
        return k && FindGraphNode(k->pass, k->id) != nullptr;
    }

    std::string ShaderEditorDocument::SelectionKey() const
    {
        // Chain-overview pass nodes keep the material page; the mirror
        // survives the overview, so leaving it restores the node page with no
        // event. An undo that deleted the node falls back here too.
        if (m_nodeSel && !ChainViewShowing())
            if (std::string k = FormatNodeKey(*m_nodeSel); Resolves(k))
                return k;
        return m_pageSel.SelectionKey();
    }

    bool ShaderEditorDocument::RestoreSelection(std::string_view key)
    {
        if (!Resolves(key))
            return false;
        if (m_pageSel.Resolves(key))
        {
            m_nodeSel.reset();
            m_nodeSelRequest = NodeSelRequest{ NodeSelRequest::Clear, 0 };
            return true;
        }
        const NodeKey k = *ParseNodeKey(key);
        // Resolves range-checked the pass, so the cast cannot clamp. EnterPass
        // leaves the overview and records the document's own navigation.
        if (static_cast<int>(k.pass) != m_activePass || ChainViewShowing())
            EnterPass(static_cast<int>(k.pass));
        // IMMEDIATELY: InspectorHost::TryLand re-reads SelectionKey() right
        // after this returns (Panels/InspectorHost.cpp:240-245).
        m_nodeSel = k;
        m_nodeSelRequest = NodeSelRequest{ NodeSelRequest::Select, k.id };
        return true;
    }

    bool ShaderEditorDocument::SelectByPath(std::string_view path)
    {
        // StageFinalize runs this before the first draw: the key is valid at
        // once (checked against the data), the request lands on the first
        // canvas frame.
        const std::optional<NodeKey> k =
            ParseNodeSelectPath(path, static_cast<std::size_t>(std::max(0, m_activePass)));
        if (!k || !RestoreSelection(FormatNodeKey(*k)))
            return false;
        ++m_pageSel.epoch;
        return true;
    }

    InspectorPage* ShaderEditorDocument::PageFor(std::string_view key)
    {
        if (m_pageSel.Resolves(key))
            return &m_page;
        if (const std::optional<NodeKey> k = ParseNodeKey(key); k && FindGraphNode(k->pass, k->id))
        {
            m_nodePage.SetTarget(k->pass, k->id);
            return &m_nodePage;
        }
        return nullptr;
    }

    void ShaderEditorDocument::SelectMaterialFromCrumb()
    {
        m_nodeSel.reset();
        m_nodeSelRequest = NodeSelRequest{ NodeSelRequest::Clear, 0 };
        ++m_pageSel.epoch;
    }

    std::vector<InspectorCrumb> ShaderEditorDocument::NodeInspectorPage::Breadcrumb() const
    {
        // node page s5.1.3. Landings on a node select it through the canvas
        // request and NEVER call NavigateToSelection.
        ShaderEditorDocument& d = m_doc;
        std::vector<InspectorCrumb> crumbs;
        // The material crumb's label is the material page's own (s4.3); a
        // node page never exists on an instance, so it reads m_title.
        crumbs.push_back({ d.m_title, [doc = &d] { doc->SelectMaterialFromCrumb(); },
                           std::string{ "material" } });
        if (d.ChainMode())   // every pass, base included
            crumbs.push_back({ d.PassLabel(m_pass),
                               [doc = &d, pass = m_pass]
                               {
                                   doc->EnterPass(static_cast<int>(pass));
                                   doc->SelectMaterialFromCrumb();
                               },
                               std::nullopt });
        const Arcane::GraphNode* n = d.FindGraphNode(m_pass, m_id);
        crumbs.push_back({ n ? std::string(Arcane::GraphNodeInfo(n->type).display) : std::string("Node"),
                           [] {}, FormatNodeKey({ m_pass, m_id }) });
        return crumbs;
    }

    void ShaderEditorDocument::DrawNodePageBody(PropertyGrid& grid, std::size_t pass, std::uint32_t id)
    {
        // FIRST local, destructs LAST (EditGesture::ScopeGuard): the page
        // draws on collapsed and background frames too, like
        // DrawMaterialPageBody.
        // UndoStack() (T1-B12) is the null-tolerant resolver read: a default
        // DocServices{} -- every headless page test -- holds an EMPTY
        // std::function, and calling m_services.undo() directly would throw
        // std::bad_function_call on the first page draw.
        const EditGesture::ScopeGuard gestureGuard{ UndoStack(), m_gesture };
        DrawSaveWithErrorsConfirm();
        // In normal ImGui space, so "Edit HLSL..." and rename propagation work
        // while the canvas is hidden (s5.1.7). Before the node's id scope:
        // the popup ids must not depend on which node is shown.
        DrawGraphModals();
        // Re-resolved EVERY call (never a GraphNode* across frames): create
        // and paste reallocate `nodes`. A gone node draws its one read-only
        // line inside a Rows table -- a row outside one would hit
        // ImGui::TableNextRow with no current table.
        const Arcane::GraphNode* n = FindGraphNode(pass, id);
        if (!n)
        {
            PropertyGrid::Rows rows(grid, "##nodegone");
            if (rows)
                grid.ReadOnlyRow("Node", "This node no longer exists");
            return;
        }
        const NodePageIdScope idScope{ pass, id };
        // The sections. Discrete edits queue (DeferNodeEdit); numeric
        // write-through stays inline (s5.1.5). `n` is read by the header
        // only: every later section re-resolves.
        m_nodePageDrawing = true;
        DrawNodePageHeader(*n);
        DrawNodePageInputs(grid, pass, id);
        DrawNodePageSettings(grid, pass, id);
        DrawNodePageOutputs(grid, pass, id);
        DrawNodePageErrors(grid, pass, id);
        m_nodePageDrawing = false;
        // The queued discrete edits, in order, after the last row (a Remove
        // Pin mid-loop would otherwise invalidate the loop).
        std::vector<std::function<void()>> edits = std::move(m_nodePageEdits);
        m_nodePageEdits.clear();
        for (std::function<void()>& edit : edits)
            edit();
    }

    void ShaderEditorDocument::DrawNodePageHeader(const Arcane::GraphNode& n)
    {
        // The crumb leaf's one sanctioned repeat (s5.1.4): it shares the chip's line.
        const Arcane::GraphNodeTypeInfo& info = Arcane::GraphNodeInfo(n.type);
        const char* category = Arcane::GraphNodeCategoryName(info.category);
        const float padX = ImGui::GetStyle().FramePadding.x;
        ImGui::AlignTextToFramePadding();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 text = ImGui::CalcTextSize(category);
        const float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(
            start, ImVec2(start.x + text.x + padX * 2.0f, start.y + h),
            ImGui::GetColorU32(GraphCategoryHeaderColor(info.category)), h * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(start.x + padX, start.y));
        ImGui::PushStyleColor(ImGuiCol_Text, NodeTitleText());
        ImGui::TextUnformatted(category);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, padX * 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::kTextDim);
        ImGui::TextUnformatted(info.display);
        if (info.description && info.description[0] != '\0')
            ImGui::TextWrapped("%s", info.description);
        ImGui::PopStyleColor();
    }

    void ShaderEditorDocument::DrawNodePageInputs(PropertyGrid& grid, std::size_t pass, std::uint32_t id)
    {
        const Arcane::GraphNode* n = FindGraphNode(pass, id);
        if (!n || Arcane::GraphNodeInputCount(*n) == 0)
            return;   // UV, Time, the Const nodes, Param, Comment, Vertex Color
        if (!grid.Section("Inputs"))
            return;
        PropertyGrid::Rows rows(grid, "##inputs");
        if (!rows)
            return;
        // Once for the section's rows (T3-D1): the type chips' resolution.
        const int resolved = WidthsOf(Arcane::ResolveGraphNodeWidths(*GraphOptAt(pass)), id).inputs;
        const std::uint32_t count = Arcane::GraphNodeInputCount(*n);   // pin edits are queued: stable here
        for (std::uint32_t pin = 0; pin < count; ++pin)
        {
            ImGui::PushID(static_cast<int>(pin));
            DrawNodePageInputRow(grid, pass, id, pin, resolved);
            ImGui::PopID();
        }
    }

    void ShaderEditorDocument::DrawNodePageInputRow(PropertyGrid& grid, std::size_t pass, std::uint32_t id,
                                                    std::uint32_t pin, int resolvedInputs)
    {
        const Arcane::GraphNode* n = FindGraphNode(pass, id);
        if (!n)
            return;
        const Arcane::MaterialGraph& g = *GraphOptAt(pass);   // FindGraphNode range-checked `pass`
        const Arcane::GraphPinDesc desc = Arcane::GraphNodeInputPin(*n, pin);
        const std::string label = desc.name;   // a Custom pin's name points into the node: copy
        // The row's type chip (T3-D1): the pin's dot + type word, resolved
        // exactly as the canvas paints it.
        const Arcane::GraphLink* wire = nullptr;
        for (const Arcane::GraphLink& l : g.links)
            if (l.toNode == id && l.toPin == pin)
                wire = &l;   // last wins, as codegen reads it
        RowDecor typeChip;
        typeChip.lead = [paint = PinPaintFor(desc.width, resolvedInputs), wired = wire != nullptr,
                         type = PinTypeText(desc.width, resolvedInputs)] { return PinTypeChip(paint, wired, type); };
        if (wire)
        {
            grid.SetNextRowDecor(typeChip);
            grid.ReadOnlyRow(label.c_str(), "<- " + WireSourceText(g, *wire));
            return;
        }
        const Arcane::GraphPinNeutral neutral = Arcane::GraphPinNeutralDefault(*n, pin);
        if (!Arcane::GraphPinAcceptsLiteral(*n, pin))
        {
            grid.SetNextRowDecor(typeChip);
            grid.ReadOnlyRow(label.c_str(), "default: " + FormatPinNeutral(neutral));
            return;
        }

        // ---- The live literal row (s5.1.5) ----
        const int lanes = Arcane::GraphPinLiteralLanes(desc.width);
        const Arcane::GraphPinLiteral* lit = n->FindPinLiteral(pin);
        // The neutral SPLATTED to this pin's lanes through SeedPinNeutral (the
        // one seed canvas and page share): Tiling & Offset's width-1 `tiling`
        // neutral (lanes 1, v {1}) must read (1, 1), not (1, 0). Zero for
        // Expression/Passthrough.
        float neutralV[4];
        SeedPinNeutral(neutral, lanes, neutralV);
        float shown[4] = {};
        std::memcpy(shown, lit ? lit->v : neutralV, sizeof(shown));
        // An Expression neutral (Panner uv) prints AS ITSELF: a format with no
        // conversion is printed verbatim (the canvas trick, imgui_widgets.cpp:2496).
        const char* format = (!lit && neutral.kind == Arcane::GraphPinNeutralKind::Expression) ? neutral.hlsl : "%.3f";
        float local[4];
        std::memcpy(local, shown, sizeof(local));
        RowDecor decor = std::move(typeChip);
        decor.reset = true;                  // the slot is always reserved: values stay aligned
        decor.resetActive = lit != nullptr;  // drawn only while a literal exists
        grid.SetNextRowDecor(decor);
        if (lanes == 1)
            (void)grid.FloatRow(label.c_str(), local[0], 0.01f, std::nullopt, format);
        else
            (void)grid.VecRow(label.c_str(), local, lanes, 0.01f, std::nullopt, format);
        const RowEvents ev = grid.LastRowEvents();
        Arcane::CommandStack* stack = UndoStack();
        // The snapshot is taken BEFORE this frame's write, so it is pre-edit on the
        // activation frame (the canvas pin-literal ordering).
        // Latched on ANY activation: BeginOnActivate skips both callbacks when the
        // stack is null (Play, s3.3), and a stale flag would erase an existing
        // literal on Esc or keep a new one.
        if (ImGui::IsItemActivated())
            m_nodePageLiteralExisted = lit != nullptr;
        EditGesture::BeginOnActivate(stack, m_gesture, [] { return std::string("Pin Value"); },
                                     [&] { return GraphEditBuilder("Pin Value", pass); });
        if (ev.resetClicked)
            DeferNodeEdit([this, pass, id, pin]
            {
                (void)RunNodeEdit("Reset Pin Value", pass, id, [pin](Arcane::GraphNode& node, Arcane::MaterialGraph&)
                {
                    ErasePinLiteral(node, pin);
                });
            });
        bool differs = false;
        for (int i = 0; i < lanes; ++i)
            differs = differs || local[i] != shown[i];
        if (differs)
            if (Arcane::GraphNode* w = FindGraphNode(pass, id))
            {
                bool onNeutral = neutral.kind == Arcane::GraphPinNeutralKind::Constant;
                for (int i = 0; i < lanes && onNeutral; ++i)
                    onNeutral = local[i] == neutralV[i];
                // A cancelled gesture, or one dragged back onto the neutral, never
                // leaves a NEW literal behind; an existing one updates in place.
                if (!m_nodePageLiteralExisted && (ev.cancelled || onNeutral))
                    ErasePinLiteral(*w, pin);
                else
                    SetPinLiteral(*w, pin, lanes, local);
                NoteGraphValueEdited();
            }
        EditGesture::EndAfterRow(stack, m_gesture, ev.cancelled);
    }

    void ShaderEditorDocument::LiveNodeFloats(PropertyGrid& grid, const char* label, const char* undoLabel,
                                              std::size_t pass, std::uint32_t id, int lanes,
                                              Arcane::FunctionRef<float*(Arcane::GraphNode&)> field)
    {
        Arcane::GraphNode* n = FindGraphNode(pass, id);
        if (!n)
            return;
        float local[4] = {};
        std::memcpy(local, field(*n), sizeof(float) * static_cast<std::size_t>(lanes));
        if (lanes == 1)
            (void)grid.FloatRow(label, local[0], 0.01f, std::nullopt, "%.3f");
        else
            (void)grid.VecRow(label, local, lanes, 0.01f, std::nullopt, "%.3f");
        const bool cancelled = grid.LastRowEvents().cancelled;
        Arcane::CommandStack* stack = UndoStack();
        // The snapshot is taken BEFORE this frame's write: pre-edit on the activation frame.
        EditGesture::BeginOnActivate(stack, m_gesture, [&] { return std::string(undoLabel); },
                                     [&] { return GraphEditBuilder(undoLabel, pass); });
        if (Arcane::GraphNode* w = FindGraphNode(pass, id))
        {
            float* dst = field(*w);
            bool differs = false;
            for (int i = 0; i < lanes; ++i)
                differs = differs || dst[i] != local[i];
            if (differs)   // includes the seed an Esc restored: written back, the graph compares equal, no step
            {
                std::memcpy(dst, local, sizeof(float) * static_cast<std::size_t>(lanes));
                NoteGraphValueEdited();
            }
        }
        EditGesture::EndAfterRow(stack, m_gesture, cancelled);
    }

    void ShaderEditorDocument::LiveNodeColor(PropertyGrid& grid, const char* label, const char* undoLabel,
                                             const char* popupLabel, std::size_t pass, std::uint32_t id, bool hdr,
                                             Arcane::FunctionRef<float*(Arcane::GraphNode&)> field)
    {
        Arcane::GraphNode* n = FindGraphNode(pass, id);
        if (!n)
            return;
        float local[4];
        std::memcpy(local, field(*n), sizeof(local));
        ImGuiID popupId = 0;
        (void)grid.ColorRow(label, local, &popupId, hdr);
        const bool cancelled = grid.LastRowEvents().cancelled;
        Arcane::CommandStack* stack = UndoStack();
        EditGesture::BeginOnActivate(stack, m_gesture, [&] { return std::string(undoLabel); },
                                     [&] { return GraphEditBuilder(undoLabel, pass); });
        if (popupId != 0)   // snapshot before this frame's write: pre-edit on the opening frame
            EditGesture::BeginOnPopupOpen(stack, m_gesture, popupId, [&] { return std::string(popupLabel); },
                                          [&] { return GraphEditBuilder(popupLabel, pass); });
        if (Arcane::GraphNode* w = FindGraphNode(pass, id); w && std::memcmp(field(*w), local, sizeof(local)) != 0)
        {
            std::memcpy(field(*w), local, sizeof(local));
            NoteGraphValueEdited();
        }
        EditGesture::EndAfterRow(stack, m_gesture, cancelled);
        if (popupId != 0)
            EditGesture::EndOnPopupClose(stack, m_gesture, popupId);
    }

    void ShaderEditorDocument::NodeTextRow(PropertyGrid& grid, const char* label, std::string_view current,
                                           std::size_t pass, std::uint32_t id, NodeTextField field, std::uint32_t pin)
    {
        // CommitOrphans can fire this AFTER the selection moved or the document
        // closed (InspectorWindows.cpp:337-340): never read the page's target or `this`.
        std::weak_ptr<ShaderEditorDocument*> anchor = m_anchor;
        (void)grid.TextRow(label, current, [anchor, pass, id, field, pin](std::string text)
        {
            const std::shared_ptr<ShaderEditorDocument*> doc = anchor.lock();
            if (!doc || !*doc)
                return;
            (*doc)->DeferNodeEdit([anchor, pass, id, field, pin, text = std::move(text)]
            {
                const std::shared_ptr<ShaderEditorDocument*> live = anchor.lock();
                if (live && *live)
                    (*live)->CommitNodeText(pass, id, field, pin, text);
            });
        });
    }

    void ShaderEditorDocument::CommitNodeText(std::size_t pass, std::uint32_t id, NodeTextField field,
                                              std::uint32_t pin, const std::string& text)
    {
        // No name validation here (drafting pick 9.28 #22): codegen is the one
        // validator, and its verdict shows in Errors.
        Arcane::GraphNode* n = FindGraphNode(pass, id);
        if (!n)
            return;   // the node died: drop the edit
        switch (field)
        {
            case NodeTextField::ParamName:
            {
                const std::string oldName = n->paramName;
                if (RunNodeEdit("Rename Param", pass, id, [&text](Arcane::GraphNode& node, Arcane::MaterialGraph&) { node.paramName = text; }))
                    BeginParamRename(oldName, std::string(text));   // copies: it re-scans every pass's nodes
                return;
            }
            case NodeTextField::SwizzleMask:
                (void)RunNodeEdit("Edit Swizzle", pass, id, [&text](Arcane::GraphNode& node, Arcane::MaterialGraph&) { node.swizzleMask = text; });
                return;
            case NodeTextField::CommentText:
                (void)RunNodeEdit("Edit Comment", pass, id, [&text](Arcane::GraphNode& node, Arcane::MaterialGraph&) { node.paramName = text; },
                                  /*recompile*/ false);
                return;
            case NodeTextField::CustomPinName:
                // Links address pins by index: nothing re-wires, and the HLSL body is never rewritten.
                (void)RunNodeEdit("Rename Pin", pass, id, [&text, pin](Arcane::GraphNode& node, Arcane::MaterialGraph&)
                                  { if (pin < node.customPins.size()) node.customPins[pin].name = text; });
                return;
        }
    }

    void ShaderEditorDocument::DrawNodePageSettings(PropertyGrid& grid, std::size_t pass, std::uint32_t id)
    {
        using GT = Arcane::GraphNodeType;
        const Arcane::GraphNode* n = FindGraphNode(pass, id);
        if (!n)
            return;
        switch (n->type)
        {
            case GT::ConstFloat: case GT::ConstFloat2: case GT::ConstFloat4: case GT::ConstColor:
            case GT::Param: case GT::TextureSample: case GT::Swizzle: case GT::PassInput:
            case GT::Panner: case GT::Custom: case GT::Comment:
                break;
            default:
                return;   // pin literals only: no Settings section
        }
        if (!grid.Section("Settings"))
            return;
        static constexpr const char* kWidthNames[] = { "float", "float2", "float4" };
        ARC_CONSTANT("shader contract: the float / float2 / float4 widths a Custom node pin can take")
        static constexpr int kWidths[] = { 1, 2, 4 };
        const auto widthIndex = [](int w) { return w == 1 ? 0 : w == 2 ? 1 : 2; };
        // Combos, checkboxes and buttons queue ONE discrete step each (s5.1.4 step 5).
        const auto discrete = [this, pass, id](const char* label, std::function<void(Arcane::GraphNode&)> set)
        {
            DeferNodeEdit([this, pass, id, label, set = std::move(set)]
            { (void)RunNodeEdit(label, pass, id, [&set](Arcane::GraphNode& node, Arcane::MaterialGraph&) { set(node); }); });
        };

        if (n->type == GT::Custom)
        {
            if (grid.SubSection("Pins"))
            {
                {
                    PropertyGrid::Rows rows(grid, "##pins");
                    if (rows)
                    {
                        const std::uint32_t count = static_cast<std::uint32_t>(n->customPins.size());
                        for (std::uint32_t k = 0; k < count; ++k)
                        {
                            const Arcane::GraphNode* cur = FindGraphNode(pass, id);
                            if (!cur || k >= cur->customPins.size())
                                break;
                            ImGui::PushID(static_cast<int>(k));
                            NodeTextRow(grid, "Name", cur->customPins[k].name, pass, id, NodeTextField::CustomPinName, k);
                            ImGui::SetItemTooltip("Renaming does not edit the HLSL body");
                            if (const int picked = grid.ComboRow("Width", kWidthNames, 3, widthIndex(cur->customPins[k].width)); picked >= 0)
                                discrete("Pin Width", [k, w = kWidths[picked]](Arcane::GraphNode& node)
                                         { if (k < node.customPins.size()) node.customPins[k].width = w; });
                            static constexpr const char* kRemove[] = { "Remove" };
                            if (grid.ButtonRow("", kRemove, 1) == 0)
                                DeferNodeEdit([this, pass, id, k] { (void)RemoveCustomPin(pass, id, k); });
                            ImGui::PopID();
                        }
                        static constexpr const char* kAdd[] = { "Add Pin" };
                        if (grid.ButtonRow("", kAdd, 1) == 0)
                            DeferNodeEdit([this, pass, id] { (void)AddCustomPin(pass, id); });
                    }
                }
                grid.EndSubSection();
            }
        }

        PropertyGrid::Rows rows(grid, "##settings");
        if (!rows)
            return;
        n = FindGraphNode(pass, id);
        if (!n)
            return;
        switch (n->type)
        {
            case GT::ConstFloat:
            case GT::ConstFloat2:
            case GT::ConstFloat4:
                LiveNodeFloats(grid, "Value", "Edit Value", pass, id,
                               n->type == GT::ConstFloat ? 1 : n->type == GT::ConstFloat2 ? 2 : 4,
                               [](Arcane::GraphNode& node) { return node.value; });
                break;
            case GT::ConstColor:   // hdr: a ConstColor feeds raw maths and may exceed 1
                LiveNodeColor(grid, "Color", "Edit Value", "Edit Color", pass, id, /*hdr*/ true,
                              [](Arcane::GraphNode& node) { return node.value; });
                break;
            case GT::Param:
            {
                NodeTextRow(grid, "Name", n->paramName, pass, id, NodeTextField::ParamName);
                static constexpr const char* kTypeNames[] = { "float", "float2", "float4", "color" };
                static constexpr Arcane::MatParamType kTypes[] = { Arcane::MatParamType::Float, Arcane::MatParamType::Float2,
                                                                   Arcane::MatParamType::Float4, Arcane::MatParamType::Color };
                int typeIdx = 0;
                for (int t = 0; t < 4; ++t)
                    if (kTypes[t] == n->paramType)
                        typeIdx = t;
                if (const int picked = grid.ComboRow("Type", kTypeNames, 4, typeIdx); picked >= 0)
                    discrete("Param Type", [t = kTypes[picked]](Arcane::GraphNode& node) { node.paramType = t; node.paramDefault.type = t; });
                // A Texture-typed Param loads (codegen diagnoses it) but has no
                // lanes: ComponentCount(Texture) == 0 would reach VecRow's n >= 2
                // assert. Read-only, so nothing writes paramDefault.f on it; the
                // Type combo (index 0 'float') is the repair path.
                if (n->paramType == Arcane::MatParamType::Texture)
                    grid.ReadOnlyRow("Default", "n/a (texture is invalid on a Param)");
                else if (n->paramType == Arcane::MatParamType::Color)
                    LiveNodeColor(grid, "Default", "Param Default", "Param Default", pass, id, /*hdr*/ false,
                                  [](Arcane::GraphNode& node) { return node.paramDefault.f; });
                else
                    LiveNodeFloats(grid, "Default", "Param Default", pass, id,
                                   static_cast<int>(Arcane::ComponentCount(n->paramType)),
                                   [](Arcane::GraphNode& node) { return node.paramDefault.f; });
                n = FindGraphNode(pass, id);
                if (!n)
                    break;
                bool ranged = n->hasRange;
                if (grid.CheckboxRow("Range", ranged))
                    discrete("Param Range", [ranged](Arcane::GraphNode& node) { node.hasRange = ranged; });
                if (n->hasRange)
                {
                    LiveNodeFloats(grid, "Min", "Param Range", pass, id, 1, [](Arcane::GraphNode& node) { return &node.rangeMin; });
                    LiveNodeFloats(grid, "Max", "Param Range", pass, id, 1, [](Arcane::GraphNode& node) { return &node.rangeMax; });
                }
                break;
            }
            case GT::TextureSample:
                NodeTextRow(grid, "Texture Param", n->paramName, pass, id, NodeTextField::ParamName);
                break;
            case GT::Swizzle:
                NodeTextRow(grid, "Mask", n->swizzleMask, pass, id, NodeTextField::SwizzleMask);
                break;
            case GT::PassInput:
            {
                static constexpr const char* kSlots[] = { "in0", "in1", "in2", "in3" };
                static_assert(std::size(kSlots) == Arcane::kMaxPassInputs);
                if (const int picked = grid.ComboRow("Slot", kSlots, 4, static_cast<int>(n->passInputSlot % Arcane::kMaxPassInputs)); picked >= 0)
                    discrete("Input Slot", [picked](Arcane::GraphNode& node) { node.passInputSlot = static_cast<std::uint32_t>(picked); });
                break;
            }
            case GT::Panner:
            {
                bool frac = n->pannerFractional;
                if (grid.CheckboxRow("Fractional", frac))
                    discrete("Panner Fraction", [frac](Arcane::GraphNode& node) { node.pannerFractional = frac; });
                break;
            }
            case GT::Custom:
            {
                if (const int picked = grid.ComboRow("Output", kWidthNames, 3, widthIndex(n->customOutWidth)); picked >= 0)
                    discrete("Output Width", [w = kWidths[picked]](Arcane::GraphNode& node) { node.customOutWidth = w; });
                std::string_view first = n->customBody;
                first = first.substr(0, first.find('\n'));
                if (!first.empty() && first.back() == '\r')
                    first.remove_suffix(1);
                grid.ReadOnlyRow("Body", first);
                if (!n->customBody.empty())
                    ImGui::SetItemTooltip("%s", n->customBody.c_str());   // last-wins over the ellipsis tooltip
                static constexpr const char* kEdit[] = { "Edit HLSL..." };
                if (grid.ButtonRow("", kEdit, 1) == 0)
                    RequestBodyEdit(pass, id);   // DrawGraphModals opens it, canvas drawn or not
                break;
            }
            case GT::Comment:   // size is not exposed (s5.1.9)
                NodeTextRow(grid, "Text", n->paramName, pass, id, NodeTextField::CommentText);
                break;
            default:
                break;
        }
    }

    void ShaderEditorDocument::DrawNodePageOutputs(PropertyGrid& grid, std::size_t pass, std::uint32_t id)
    {
        const Arcane::GraphNode* n = FindGraphNode(pass, id);
        if (!n || Arcane::GraphNodeOutputCount(*n) == 0)
            return;   // Output, Vertex Output, Comment
        if (!grid.Section("Outputs"))
            return;
        PropertyGrid::Rows rows(grid, "##outputs");
        if (!rows)
            return;
        const Arcane::MaterialGraph& g = *GraphOptAt(pass);   // FindGraphNode range-checked `pass`
        const int resolved = WidthsOf(Arcane::ResolveGraphNodeWidths(g), id).outputs;
        for (std::uint32_t pin = 0; pin < Arcane::GraphNodeOutputCount(*n); ++pin)
        {
            const Arcane::GraphPinDesc desc = Arcane::GraphNodeOutputPin(*n, pin);   // Custom: customOutWidth
            std::string targets;
            for (const Arcane::GraphLink& l : g.links)
            {
                if (l.fromNode != id || l.fromPin != pin)
                    continue;
                const Arcane::GraphNode* dst = g.FindNode(l.toNode);
                if (!dst || l.toPin >= Arcane::GraphNodeInputCount(*dst))
                    continue;
                if (!targets.empty())
                    targets += ", ";
                targets += std::string(Arcane::GraphNodeInfo(dst->type).display) + "." +
                           Arcane::GraphNodeInputPin(*dst, l.toPin).name;
            }
            // The type word rides the row's type chip (the dot, then the word),
            // so the value text after it carries only the wiring.
            RowDecor typeChip;
            typeChip.lead = [paint = PinPaintFor(desc.width, resolved), wired = !targets.empty(),
                             type = PinTypeText(desc.width, resolved)] { return PinTypeChip(paint, wired, type); };
            grid.SetNextRowDecor(typeChip);
            ImGui::PushID(static_cast<int>(pin));
            grid.ReadOnlyRow(desc.name, targets.empty() ? std::string("(unused)") : "-> " + targets);
            ImGui::PopID();
        }
    }

    void ShaderEditorDocument::DrawNodePageErrors(PropertyGrid& grid, std::size_t pass, std::uint32_t id)
    {
        // Graph-level errors (nodeId 0) stay on the material page and in Problems.
        std::vector<std::string> lines;
        if (pass < m_passGraphErrors.size())
            for (const Arcane::GraphError& e : m_passGraphErrors[pass])
                if (e.nodeId == id)
                    lines.push_back(e.message);
        ForEachNodeDiagnostic(pass, id, [&](std::string_view m) { lines.emplace_back(m); });
        if (lines.empty())
            return;
        const std::string label = "Errors (" + std::to_string(lines.size()) + ")###errors";   // stable id across N
        if (!grid.Section(label.c_str()))
            return;
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::kError);
        for (const std::string& line : lines)
            ImGui::TextWrapped("%s", line.c_str());
        ImGui::PopStyleColor();
    }

    void ShaderEditorDocument::ForEachNodeDiagnostic(std::size_t pass, std::uint32_t nodeId,
                                                     const std::function<void(std::string_view)>& fn) const
    {
        ForEachPassErrorDiag(pass, [&](std::uint32_t id, std::string_view m) { if (id == nodeId) fn(m); });
    }

    void ShaderEditorDocument::DrawSnippetEditor()
    {
        // Graph-owned + HLSL toggle = UE's code viewer: ONE read-only window
        // with everything this pass generates -- the pixel body, plus the
        // material's vertex body under the base.
        const bool generatedView =
            ActiveGraphOwned() && m_showGeneratedText && !m_editVertex;

        // Say which buffer this is (the pass canvas / repair toggles select it).
        if (m_editVertex)
            ImGui::TextDisabled("repair mode: vertex stage (hand-edit; graph-owned "
                                "materials author this with a Vertex Output node)");
        else if (!ActiveGraphOwned())
            ImGui::TextDisabled("repair mode: no graph on this %s -- hand-edit "
                                "the snippet, or revert the file",
                                m_activePass == 0 ? "material" : "pass");
        else if (ChainMode())
            ImGui::TextDisabled("%s (generated)",
                                PassLabel(static_cast<std::size_t>(m_activePass)).c_str());
        if (m_jumpToLine > 0)
        {
            m_callbackJumpLine = m_jumpToLine;
            m_jumpToLine = 0;
            m_focusSnippet = true;
        }

        if (m_focusSnippet)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusSnippet = false;
        }
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput |
                                    ImGuiInputTextFlags_CallbackResize |
                                    ImGuiInputTextFlags_CallbackAlways;
        // Text is a VIEW of generated output, never an editing surface --
        // edits would be silently stomped by the next regeneration; freeform
        // HLSL goes through the Custom (HLSL) node. The ONE exception is the
        // disaster case: a material whose graph is corrupt/missing loads
        // graphless, and hand-editing its snippet is the in-tool repair.
        if (m_editVertex ? IsGraphOwned() : ActiveGraphOwned())
            flags |= ImGuiInputTextFlags_ReadOnly;
        // Per-buffer widget identity: switching passes (or to the vertex
        // stage) swaps the buffer under the widget, which must not inherit
        // the previous buffer's edit state.
        ImGui::PushID(m_editVertex ? -2 : m_activePass);
        std::string* buf = &ActiveSnippet();
        if (generatedView)
        {
            m_generatedView = *buf;
            if (m_activePass == 0 && !m_data.vertexSnippet.empty())
            {
                m_generatedView += "\n// ---- vertex stage (Vertex Output node) ----\n";
                m_generatedView += m_data.vertexSnippet;
            }
            buf = &m_generatedView;
        }
        // +1 capacity: ImGui writes the terminator into the buffer it is given.
        // Height measured HERE, after the optional mode banner above -- the
        // input takes exactly what is left of the column.
        if (ImGui::InputTextMultiline("##snippet", buf->data(), buf->capacity() + 1,
                                      ImVec2(-1.0f, ImGui::GetContentRegionAvail().y),
                                      flags,
                                      &SnippetCallbackForwarder::Callback, this))
        {
            m_dirty = true;
            if (m_live)
                Rebuild();
        }
        ImGui::PopID();
    }

    bool ShaderEditorDocument::PassWireWouldCycle(std::uint32_t source,
                                                  std::uint32_t consumer) const
    {
        // Adding consumer.inputs += source cycles iff `consumer` is already
        // upstream of `source` (walk source's input ancestry).
        if (source == consumer)
            return true;
        std::vector<std::uint32_t> stack{ source };
        std::unordered_set<std::uint32_t> seen;
        while (!stack.empty())
        {
            const std::uint32_t c = stack.back();
            stack.pop_back();
            if (c == consumer)
                return true;
            if (!seen.insert(c).second || c == 0)
                continue;
            if (c - 1 < m_data.passes.size())
                for (std::uint32_t in : m_data.passes[c - 1].inputs)
                    stack.push_back(in);
        }
        return false;
    }

    bool ShaderEditorDocument::TopoSortPasses()
    {
        // Stable Kahn over chain indices (base = 0 is always first). Positions
        // ride each MaterialPass; active/view indices remap.
        const std::size_t n = m_data.passes.size();
        if (n == 0)
            return true;
        std::vector<std::uint32_t> order;   // new sequence of OLD chain indices
        std::vector<bool> placed(n + 1, false);
        placed[0] = true;
        bool progress = true;
        while (order.size() < n && progress)
        {
            progress = false;
            for (std::uint32_t c = 1; c <= n; ++c)
            {
                if (placed[c])
                    continue;
                bool ready = true;
                for (std::uint32_t in : m_data.passes[c - 1].inputs)
                    ready = ready &&
                            (in == Arcane::kSceneInput || (in <= n && placed[in]));
                if (!ready)
                    continue;
                placed[c] = true;
                order.push_back(c);
                progress = true;
            }
        }
        if (order.size() < n)
            return false;   // cycle -- the canvas refuses these at wire time

        std::vector<std::uint32_t> remap(n + 1, 0);
        for (std::size_t i = 0; i < order.size(); ++i)
            remap[order[i]] = static_cast<std::uint32_t>(i) + 1;
        std::vector<Arcane::MaterialPass> sorted;
        sorted.reserve(n);
        for (std::uint32_t old : order)
            sorted.push_back(std::move(m_data.passes[old - 1]));
        for (Arcane::MaterialPass& p : sorted)
            for (std::uint32_t& in : p.inputs)
                if (in != Arcane::kSceneInput)   // the scene is not a pass
                    in = remap[in];
        m_data.passes = std::move(sorted);
        if (m_activePass > 0 && m_activePass <= static_cast<int>(n))
            m_activePass = static_cast<int>(remap[static_cast<std::uint32_t>(m_activePass)]);
        if (m_viewPass > 0 && m_viewPass <= static_cast<int>(n))
            m_viewPass = static_cast<int>(remap[static_cast<std::uint32_t>(m_viewPass)]);
        // The nav history stores pass INDICES, so it rides the same remap --
        // otherwise a rewire that re-sorts the chain would silently re-point
        // every recorded entry at whatever pass slid into its slot.
        for (ViewEntry& e : m_navHistory)
            if (!e.chainView && e.pass > 0 && e.pass <= static_cast<int>(n))
                e.pass = static_cast<int>(remap[static_cast<std::uint32_t>(e.pass)]);
        return true;
    }

    void ShaderEditorDocument::EnterPass(int chainIndex)
    {
        m_activePass = std::clamp(chainIndex, 0,
                                  static_cast<int>(m_data.passes.size()));
        m_inChainView = false;
        NavRecord();
    }

    void ShaderEditorDocument::NavRecord()
    {
        const ViewEntry now{ m_inChainView, m_activePass };
        const bool haveCurrent =
            m_navIndex >= 0 && m_navIndex < static_cast<int>(m_navHistory.size());
        // Re-navigating to where we already are is not a history event -- it
        // would otherwise let a repeatedly-clicked crumb push the forward
        // branch off the end one entry at a time.
        if (haveCurrent && m_navHistory[static_cast<std::size_t>(m_navIndex)] == now)
            return;

        // Truncate the forward branch, then append: the standard rule, and the
        // reason a new jump abandons whatever forward history existed.
        m_navHistory.resize(static_cast<std::size_t>(m_navIndex + 1));
        m_navHistory.push_back(now);
        const int navMax = Arcane::Settings<ShaderEditorSettings>().navHistoryMax;
        while (static_cast<int>(m_navHistory.size()) > navMax)   // while: the cap may have shrunk live
            m_navHistory.erase(m_navHistory.begin());
        m_navIndex = static_cast<int>(m_navHistory.size()) - 1;
    }

    bool ShaderEditorDocument::NavStep(int dir)
    {
        const int count = static_cast<int>(m_navHistory.size());
        for (int i = m_navIndex + dir; i >= 0 && i < count; i += dir)
        {
            const ViewEntry& e = m_navHistory[static_cast<std::size_t>(i)];
            // SKIP a stale entry rather than land on it. Passes get deleted and
            // re-sorted under a history that was recorded before either; the
            // re-sort is remapped in TopoSortPasses, but a DELETE has no
            // meaningful remap, so those entries simply stop being destinations.
            // Walking past them keeps one delete from turning Back into a
            // dead key.
            if (!e.chainView &&
                (e.pass < 0 || e.pass > static_cast<int>(m_data.passes.size())))
                continue;
            m_navIndex = i;
            m_inChainView = e.chainView;
            m_activePass = e.chainView ? m_activePass : e.pass;
            return true;
        }
        return false;
    }

    float ShaderEditorDocument::DrawBreadcrumbBar()
    {
        // One line, monochrome, no frame -- the editor's chrome language
        // (EditorTheme.hpp): this is a location readout, not a toolbar. UE's is
        // the same shape, a trail of text buttons with ">" separators
        // (SGraphTitleBar.cpp:240-265).
        //
        // MODELLED AS DERIVED STATE, like UE's. Its crumbs are recomputed from
        // (material, m_activePass) every frame rather than pushed and popped as
        // the user navigates -- UE rebuilds its trail from the graph's outer
        // chain on every refresh for the same reason. There is no history to
        // desynchronise from the document that way.
        const float startY = ImGui::GetCursorPosY();

        // Root crumb: the material itself == the chain overview. A plain text
        // button so the row stays flat; only the CURRENT crumb is full-strength
        // text, the clickable ancestor is dimmed until hovered, which is the
        // usual breadcrumb reading.
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::kPanelRaised);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::kButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              m_inChainView ? Theme::kText : Theme::kTextDim);
        // Stable ID suffix: a material whose NAME contained "##" would
        // otherwise redefine the button's identity (and hide the rest of the
        // label), and the name is user-authored.
        const std::string rootCrumb = m_title + "###bcroot";
        if (ImGui::SmallButton(rootCrumb.c_str()))
        {
            m_inChainView = true;   // always reachable -- see the member comment
            NavRecord();            // explicit navigation: it goes in history
        }
        ImGui::PopStyleColor(4);

        if (!m_inChainView)
        {
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::TextColored(Theme::kTextDim, ">");
            ImGui::SameLine(0.0f, 6.0f);
            // The leaf crumb is where you are, so it is text rather than a
            // button: clicking it would navigate nowhere.
            ImGui::TextColored(Theme::kText, "%s",
                               PassLabel(static_cast<std::size_t>(
                                   (std::max)(0, m_activePass))).c_str());
        }

        ImGui::Separator();
        return ImGui::GetCursorPosY() - startY;
    }

    void ShaderEditorDocument::DrawPassCanvas()
    {
        // The pass DAG as a canvas (replaces the pass bar): chain index c is
        // node id c+1, the Output node is kPassOutputNodeId, and the WIRES ARE
        // THE DATA -- a link into input pin s of a pass IS inputs[s]. Every
        // structural gesture (wire/unwire/add/remove/reorder/rename) is ONE
        // undo step (whole pass-list before/after through PassListCommand).
        //
        // NO RENDERING LOD HERE, deliberately, and the facelift did not change
        // that. The graph canvas degrades with zoom (NodeLOD) because its nodes
        // carry pin labels, inline literals, payload widgets and a per-node
        // compiled thumbnail -- a stack deep enough to be worth shedding. A
        // pass node is a title, at most five pins and one thumbnail, and the
        // thumbnail is an ALREADY-RENDERED chain intermediate rather than
        // something compiled for the node, so hiding it reclaims no GPU work.
        // Every tier below DefaultDetail would degrade to roughly what this
        // already draws. UE's own tiers bottom out the same way -- MediumDetail
        // and up degrade NOTHING even in the Blueprint graph (SNodePanel.h:
        // 70-90 calls MediumDetail "still drawn", and no consumer in the engine
        // tests for FullyZoomedIn at all).
        //
        // Shared with the graph canvas: the zoom TABLE (navigation feel should
        // not differ per canvas), the node/canvas STYLE, the shader backdrop,
        // title bands, port dots and wire rendering. Only the LOD tiers are not.
        if (!m_passCanvasCtx)
        {
            ed::Config cfg;
            cfg.SettingsFile = nullptr;
            // Same stops as the graph canvas: the zoom TABLE is a navigation
            // feel and belongs on every canvas in the editor. (The LOD tiers
            // built on top of it are not -- see DrawPassCanvas's note below.)
            ApplyZoomLevels(cfg);
            // UE's modifiers, as on the graph canvas (editor.graph.shiftAddsToSelection).
            cfg.ShiftAddsToSelection = Settings<GraphCanvasSettings>().shiftAddsToSelection;
            m_passCanvasCtx = ed::CreateEditor(&cfg);
            // Same node/canvas styling as the material graph, including the
            // switch that kills the vendored grid so the shader backdrop below
            // is the only one. Per-context state, so a rebuilt context
            // re-applies it.
            ed::SetCurrentEditor(m_passCanvasCtx);
            ApplyGraphCanvasStyle(ShaderCanvasStyleDesc());
            ed::SetCurrentEditor(nullptr);
            m_passCanvasSeeded = false;
            m_passFitOnSeed = true;
        }
        const std::size_t total = 1 + m_data.passes.size();
        auto nodeOf = [](std::size_t chain) { return static_cast<std::uint32_t>(chain) + 1; };

        ed::SetCurrentEditor(m_passCanvasCtx);
        RefreshGraphCanvasStyle(ShaderCanvasStyleDesc());   // a Live theme change reaches the open canvas
        // Its OWN grid instance -- the phase is per-canvas state, so sharing
        // one with the graph canvas would hand each the other's accumulated
        // pan/zoom on every breadcrumb trip (see DrawCanvasBackdrop).
        DrawCanvasBackdrop(m_passGridPhase);
        // Fills the region: this canvas IS the view now, not a strip over one.
        ed::Begin("##passcanvas", ImVec2(0.0f, ImGui::GetContentRegionAvail().y));

        // Wire anchors are per-frame and per-canvas; the link loop below reads
        // what this frame's pin rows write.
        m_pinPivots.clear();

        const bool seededThisFrame = !m_passCanvasSeeded;
        if (seededThisFrame)
        {
            // Never-laid-out data (all zeros, incl. pre-canvas files): a simple
            // left-to-right row.
            bool anyPos = m_data.chainBaseX != 0.0f || m_data.chainBaseY != 0.0f ||
                          m_data.chainOutX != 0.0f || m_data.chainOutY != 0.0f;
            for (const Arcane::MaterialPass& p : m_data.passes)
                anyPos = anyPos || p.posX != 0.0f || p.posY != 0.0f;
            const ShaderChainLayoutSettings& layout = Arcane::Settings<ShaderChainLayoutSettings>();   // editor.shader.chainLayout.*
            if (!anyPos)
            {
                m_data.chainBaseX = layout.originX;
                m_data.chainBaseY = layout.originY;
                for (std::size_t k = 0; k < m_data.passes.size(); ++k)
                {
                    m_data.passes[k].posX = layout.originX + layout.pitchX * static_cast<float>(k + 1);
                    m_data.passes[k].posY = layout.originY;
                }
                m_data.chainOutX = layout.originX + layout.pitchX * static_cast<float>(total);
                m_data.chainOutY = layout.originY;
            }
            // The Scene source sits left of the base by default (also heals
            // pre-scene files whose chainPos lacks it).
            if (m_data.chainSceneX == 0.0f && m_data.chainSceneY == 0.0f)
            {
                m_data.chainSceneX = m_data.chainBaseX + layout.sceneOffsetX;
                m_data.chainSceneY = m_data.chainBaseY + layout.sceneOffsetY;
            }
            ed::SetNodePosition(nodeOf(0), ImVec2(m_data.chainBaseX, m_data.chainBaseY));
            for (std::size_t k = 0; k < m_data.passes.size(); ++k)
                ed::SetNodePosition(nodeOf(k + 1),
                                    ImVec2(m_data.passes[k].posX, m_data.passes[k].posY));
            ed::SetNodePosition(kPassOutputNodeId,
                                ImVec2(m_data.chainOutX, m_data.chainOutY));
            ed::SetNodePosition(kPassSceneNodeId,
                                ImVec2(m_data.chainSceneX, m_data.chainSceneY));
            m_passCanvasSeeded = true;
            // Fit a FRESH view only (the context was just made, or the file
            // reloaded), never the re-seed after a structural edit or a
            // pass-list undo/redo: the user's view survives those (T3-D3).
            if (m_passFitOnSeed)
            {
                m_passFitPending.Arm();
                m_passFitOnSeed = false;
            }
        }

        // ---- nodes
        for (std::size_t c = 0; c < total; ++c)
        {
            const std::uint32_t nodeId = nodeOf(c);

            // Same off-screen cull as the graph canvas: submit the node and its
            // pins so links and framing still see it, skip the content -- which
            // here includes the 72x72 chain-intermediate Image, the per-node
            // ImGui draw this canvas pays most for.
            if (NodeCulled(nodeId))
            {
                const ImVec2 size = ed::GetNodeSize(ed::NodeId(nodeId));
                ed::BeginNode(ed::NodeId(nodeId));
                ImGui::PushID(static_cast<int>(nodeId));
                const float startY = ImGui::GetCursorPosY();
                const std::vector<std::uint32_t>& culledInputs =
                    c >= 1 ? m_data.passes[c - 1].inputs : m_data.baseInputs;
                const std::size_t pinCount =
                    (std::min)(culledInputs.size() + 1,
                               static_cast<std::size_t>(Arcane::kMaxPassInputs));
                for (std::size_t s = 0; s < pinCount; ++s)
                {
                    const ed::PinId id = InPin(nodeId, static_cast<std::uint32_t>(s));
                    ed::BeginPin(id, ed::PinKind::Input);
                    SetPinPivot(id.Get(), ImGui::GetCursorScreenPos());
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));
                    ed::EndPin();
                    ImGui::SameLine(0.0f, 0.0f);
                }
                ed::BeginPin(OutPin(nodeId, 0), ed::PinKind::Output);
                SetPinPivot(OutPin(nodeId, 0).Get(), ImGui::GetCursorScreenPos());
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ed::EndPin();
                // Pad out to the remembered footprint MINUS what the pin row
                // already advanced, exactly as the graph canvas's stand-in
                // does: a FIXED POINT. Padding the full height on top of the
                // pin row's line advance grew the node by one item spacing per
                // culled draw, and GetContentBounds (F, the s4.5 fit) then
                // framed phantom bounds once the view came back.
                const float usedY = ImGui::GetCursorPosY() - startY;
                const float wantY = size.y - 2.0f * NodePadY();
                ImGui::Dummy(ImVec2((std::max)(0.0f, size.x - 2.0f * NodePadX()),
                                    (std::max)(0.0f, wantY - usedY)));
                ImGui::PopID();
                ed::EndNode();
                continue;
            }

            ed::BeginNode(ed::NodeId(nodeId));
            ImGui::PushID(static_cast<int>(nodeId));

            bool passError = false;
            if (c < m_passJobs.size())
                for (const Arcane::ShaderDiag& d : m_passJobs[c].diags)
                    passError = passError ||
                                d.severity == Arcane::ShaderDiagSeverity::Error;
            // "> " marks the pass the editor is aimed at (the one Enter would
            // return you to); the eye marks the preview truncation point, which
            // is a SEPARATE piece of state and now needs its own indicator --
            // it lost double-click to Enter, so the context menu is its only
            // control and the node has to show what that control did.
            const bool isPreviewCut =
                (m_viewPass < 0 && c == total - 1) || m_viewPass == static_cast<int>(c);
            const std::string title =
                (m_activePass == static_cast<int>(c) ? "> " : "") + PassLabel(c) +
                (isPreviewCut ? "  " ICON_LC_EYE : "");
            if (passError)
                ImGui::TextColored(NodeBadgeText(), "(!) %s", title.c_str());
            else
                ImGui::TextColored(NodeTitleText(), "%s", title.c_str());
            // Band bottom + the body gap under it, same treatment and same
            // reasoning as a graph node (DrawNodeTitleBand).
            const float headerMaxY = ImGui::GetItemRectMax().y;
            {
                const float fill = (NodePadY() + NodeHeaderGap()) -
                                   2.0f * ImGui::GetStyle().ItemSpacing.y;
                if (fill > 0.0f)
                    ImGui::Dummy(ImVec2(0.0f, fill));
            }
            // Last frame's measured width; output rows right-align to it. Its
            // own map: pass-canvas node ids (chain index + 1) and graph node
            // ids are unrelated counters that would collide in one.
            const auto passWidthIt = m_passNodeWidths.find(nodeId);
            const float passContentW =
                passWidthIt == m_passNodeWidths.end()
                    ? 0.0f
                    : passWidthIt->second - 2.0f * NodePadX();

            // Extra passes rename in-node (StableTextEdit's stable-buffer
            // commit; one undo step on deactivate-after-edit -- renames are not
            // structural, so the step pushes here rather than riding the
            // structural block).
            if (c >= 1)
            {
                Arcane::MaterialPass& pass = m_data.passes[c - 1];
                StableTextEdit("##passname", m_textEdit,
                               TextKey(TextEditKind::PassName, c),
                               pass.name, Arcane::Settings<GraphCanvasSettings>().passNameFieldWidth,
                               [&](const char* text)
                               {
                                   PassListState before = CapturePassListState();
                                   pass.name = text;
                                   m_dirty = true;
                                   PushPassUndo("Rename Pass", std::move(before));
                               });
            }

            // Input pins: one per wired slot + a spare that accepts a new
            // wire. The BASE has pins too -- its slots are its scene inputs
            // (the base may read ONLY the Scene source; enforced at connect).
            const std::vector<std::uint32_t>& nodeInputs =
                c >= 1 ? m_data.passes[c - 1].inputs : m_data.baseInputs;
            for (std::size_t s = 0; s < nodeInputs.size(); ++s)
            {
                ed::BeginPin(InPin(nodeId, static_cast<std::uint32_t>(s)),
                             ed::PinKind::Input);
                // A wired slot is always connected by construction -- the slot
                // list IS the wire list -- so the dot is always filled here.
                const ImVec2 dot = DrawPinDot(PinTextureColor(), true);
                SetPinPivot(InPin(nodeId, static_cast<std::uint32_t>(s)).Get(),
                            ImVec2(dot.x - PinDotRadius(), dot.y));
                ImGui::SameLine();
                ImGui::Text("in%zu", s);
                ed::EndPin();
            }
            if (nodeInputs.size() < Arcane::kMaxPassInputs)
            {
                const std::uint32_t sparePin =
                    static_cast<std::uint32_t>(nodeInputs.size());
                ed::BeginPin(InPin(nodeId, sparePin), ed::PinKind::Input);
                // The spare accepts the NEXT wire and has none yet, so it draws
                // hollow -- the same "nothing attached" reading the graph
                // canvas gives an unwired input.
                const ImVec2 dot = DrawPinDot(PinTextureColor(), false);
                SetPinPivot(InPin(nodeId, sparePin).Get(),
                            ImVec2(dot.x - PinDotRadius(), dot.y));
                ImGui::SameLine();
                ImGui::TextDisabled("+");
                ed::EndPin();
            }

            // Live thumbnail: pass 0 (the base) shows the tonemapped
            // preview. Passes 1+ show none -- NriGraphContext has no per-pass
            // intermediate readback, so there is nothing to draw for them.
            ImTextureID thumbId = c == 0 ? PreviewImageOf().id : 0;
            if (thumbId)
            {
                const float thumbPx = Arcane::Settings<ShaderEditorSettings>().passThumbPx;   // editor.shader.passThumbPx
                ImGui::Image(thumbId, ImVec2(thumbPx, thumbPx));
            }

            {
                const float rowW = ImGui::CalcTextSize("out").x +
                                   ImGui::GetStyle().ItemSpacing.x +
                                   PinDotRadius() * 2.0f;
                RightAlignRow(passContentW, rowW);
                ed::BeginPin(OutPin(nodeId, 0), ed::PinKind::Output);
                ImGui::TextUnformatted("out");
                ImGui::SameLine();
                // An output fans out, so "connected" is whether anything
                // downstream lists this chain index (the Output node's implicit
                // read of the tail counts -- that is what the final wire is).
                bool fanout = (c == total - 1);
                for (const Arcane::MaterialPass& p : m_data.passes)
                    for (std::uint32_t in : p.inputs)
                        fanout = fanout || in == static_cast<std::uint32_t>(c);
                const ImVec2 dot = DrawPinDot(PinTextureColor(), fanout);
                SetPinPivot(OutPin(nodeId, 0).Get(),
                            ImVec2(dot.x + PinDotRadius(), dot.y));
                ed::EndPin();
            }

            ImGui::PopID();
            ed::EndNode();
            const ImVec2 passSize = DrawNodeTitleBand(nodeId, headerMaxY);
            if (passSize.x > 0.0f)
                m_passNodeWidths[nodeId] = passSize.x;
        }

        // The Scene source: the EXTERNAL scene color (bound by the runtime
        // post hook; the checkerboard stand-in in the preview). Output pin
        // only; wiring it writes the kSceneInput sentinel.
        ed::BeginNode(ed::NodeId(kPassSceneNodeId));
        ImGui::TextColored(NodeTitleText(), "Scene");
        const float sceneHeaderY = ImGui::GetItemRectMax().y;
        {
            const float fill = (NodePadY() + NodeHeaderGap()) -
                               2.0f * ImGui::GetStyle().ItemSpacing.y;
            if (fill > 0.0f)
                ImGui::Dummy(ImVec2(0.0f, fill));
        }
        // THIS NODE DRAWS NO IMAGE. It is a pin and a label; wiring it to
        // something live would be a behaviour change, not a fix -- the same
        // treatment the pass-canvas Output node below carries.
        ed::BeginPin(OutPin(kPassSceneNodeId, 0), ed::PinKind::Output);
        ImGui::TextUnformatted("scene");
        ImGui::SameLine();
        {
            // Connected when any consumer wired the sentinel.
            bool used = false;
            for (std::uint32_t in : m_data.baseInputs)
                used = used || in == Arcane::kSceneInput;
            for (const Arcane::MaterialPass& p : m_data.passes)
                for (std::uint32_t in : p.inputs)
                    used = used || in == Arcane::kSceneInput;
            const ImVec2 dot = DrawPinDot(PinTextureColor(), used);
            SetPinPivot(OutPin(kPassSceneNodeId, 0).Get(),
                        ImVec2(dot.x + PinDotRadius(), dot.y));
        }
        ed::EndPin();
        ed::EndNode();
        DrawNodeTitleBand(kPassSceneNodeId, sceneHeaderY);

        // The Output node: shows the final image; its wire marks the LAST pass
        // (execution order's tail = what single-material consumers see).
        ed::BeginNode(ed::NodeId(kPassOutputNodeId));
        ImGui::TextColored(NodeTitleText(), "Output");
        const float outHeaderY = ImGui::GetItemRectMax().y;
        {
            const float fill = (NodePadY() + NodeHeaderGap()) -
                               2.0f * ImGui::GetStyle().ItemSpacing.y;
            if (fill > 0.0f)
                ImGui::Dummy(ImVec2(0.0f, fill));
        }
        ed::BeginPin(InPin(kPassOutputNodeId, 0), ed::PinKind::Input);
        {
            // Always fed: the final wire is the chain's tail by construction.
            const ImVec2 dot = DrawPinDot(PinTextureColor(), true);
            SetPinPivot(InPin(kPassOutputNodeId, 0).Get(),
                        ImVec2(dot.x - PinDotRadius(), dot.y));
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("final");
        ed::EndPin();
        // THIS NODE DRAWS NO THUMBNAIL. There is no per-pass intermediate
        // to read; wiring it to PreviewImageOf() would be a behaviour change
        // rather than a fix, and is deliberately not done here.
        ed::EndNode();
        DrawNodeTitleBand(kPassOutputNodeId, outHeaderY);

        // ---- links (derived from the data each frame; ids = list index + 1).
        // Sentinel entries draw from the Scene source; the base (c == 0) only
        // ever has those.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> linkSlots;   // consumer, slot
        for (std::size_t c = 0; c < total; ++c)
        {
            const std::vector<std::uint32_t>& ins =
                c >= 1 ? m_data.passes[c - 1].inputs : m_data.baseInputs;
            for (std::size_t s = 0; s < ins.size(); ++s)
            {
                const std::uint32_t src = ins[s];
                linkSlots.emplace_back(static_cast<std::uint32_t>(c),
                                       static_cast<std::uint32_t>(s));
                const ed::PinId fromPin = src == Arcane::kSceneInput
                                              ? OutPin(kPassSceneNodeId, 0)
                                              : OutPin(nodeOf(src), 0);
                const ed::PinId toPin =
                    InPin(nodeOf(c), static_cast<std::uint32_t>(s));
                DrawPassWire(linkSlots.size(), fromPin.Get(), toPin.Get());
            }
        }
        DrawPassWire(kPassOutputLinkId, OutPin(nodeOf(total - 1), 0).Get(),
                     InPin(kPassOutputNodeId, 0).Get());

        // ---- wire edits
        // Structural gestures land on the undo stack as whole pass-list
        // before/after (the before captures lazily at the FIRST mutation of
        // the frame; the push rides the structural block at the end).
        bool structural = false;
        std::optional<PassListState> passBefore;
        const char* passEditLabel = "Edit Passes";
        auto capturePassBefore = [&](const char* label)
        {
            if (!passBefore)
                passBefore = CapturePassListState();
            passEditLabel = label;
        };
        {
        // The bracket is a scope object: ed::EndCreate() runs at the closing
        // brace whatever this block does. See Widgets/CanvasEditScope.hpp for
        // the rule and the crash it prevents.
        const CanvasCreateScope create;
        if (create)
        {
            ed::PinId aId, bId;
            if (ed::QueryNewLink(&aId, &bId))
            {
                const DecodedPin a = DecodePin(aId);
                const DecodedPin b = DecodePin(bId);
                const DecodedPin& out = a.isInput ? b : a;
                const DecodedPin& in = a.isInput ? a : b;
                const bool sceneSource = out.node == kPassSceneNodeId;
                bool valid = a.valid && b.valid && a.isInput != b.isInput &&
                             out.node != kPassOutputNodeId &&
                             (sceneSource ||
                              (out.node >= 1 && out.node <= total));
                const std::uint32_t source =
                    sceneSource ? Arcane::kSceneInput : out.node - 1;
                if (valid && in.node == kPassOutputNodeId)
                {
                    // Make `source` the final pass: legal only when nothing
                    // reads it (a consumer must execute after it). The Scene
                    // is a source, never the final image.
                    bool hasDependent = false;
                    for (const Arcane::MaterialPass& p : m_data.passes)
                        for (std::uint32_t pin : p.inputs)
                            hasDependent = hasDependent || pin == source;
                    if (sceneSource || source == 0 || source == total - 1 ||
                        hasDependent)
                        ed::RejectNewItem();
                    else if (ed::AcceptNewItem())
                    {
                        capturePassBefore("Reorder Passes");
                        Arcane::MaterialPass moved =
                            std::move(m_data.passes[source - 1]);
                        m_data.passes.erase(m_data.passes.begin() +
                                            static_cast<std::ptrdiff_t>(source - 1));
                        m_data.passes.push_back(std::move(moved));
                        for (Arcane::MaterialPass& p : m_data.passes)
                            for (std::uint32_t& pin : p.inputs)
                            {
                                if (pin == Arcane::kSceneInput)
                                    continue;   // the scene is not a pass
                                pin = pin == source
                                          ? static_cast<std::uint32_t>(total - 1)
                                          : pin > source ? pin - 1 : pin;
                            }
                        if (m_activePass == static_cast<int>(source))
                            m_activePass = static_cast<int>(total - 1);
                        else if (m_activePass > static_cast<int>(source))
                            --m_activePass;
                        m_viewPass = -1;
                        structural = true;
                    }
                }
                else
                {
                    // The BASE (in.node == 1) accepts ONLY the Scene source;
                    // scene wires skip the cycle check (the scene is external,
                    // it cannot depend on any pass).
                    const std::uint32_t consumer = in.node - 1;
                    valid = valid && in.node >= 1 && in.node <= total &&
                            (consumer > 0 || sceneSource);
                    if (valid)
                    {
                        const std::vector<std::uint32_t>& ins =
                            consumer == 0 ? m_data.baseInputs
                                          : m_data.passes[consumer - 1].inputs;
                        valid = in.pin <= ins.size() &&
                                in.pin < Arcane::kMaxPassInputs &&
                                (sceneSource ||
                                 !PassWireWouldCycle(source, consumer));
                    }
                    if (!valid)
                        ed::RejectNewItem();
                    else if (ed::AcceptNewItem())
                    {
                        capturePassBefore("Wire Pass");
                        std::vector<std::uint32_t>& ins =
                            consumer == 0 ? m_data.baseInputs
                                          : m_data.passes[consumer - 1].inputs;
                        if (in.pin < ins.size())
                            ins[in.pin] = source;   // silent replace
                        else
                            ins.push_back(source);  // the spare pin
                        TopoSortPasses();
                        structural = true;
                    }
                }
            }
        }
        }   // ~CanvasCreateScope -> ed::EndCreate()

        if (ed::IsActive() && !ImGui::GetIO().WantTextInput && EditorActions::Get().Pressed("graph.delete"))
            DeleteCanvasSelection();

        // ---- deletions: links = unwire a slot; nodes = remove the pass
        std::vector<std::pair<std::uint32_t, std::uint32_t>> unwire;
        std::vector<std::uint32_t> removePasses;   // chain indices
        {
        const CanvasDeleteScope del;   // ed::EndDelete() at the closing brace
        if (del)
        {
            ed::LinkId lid;
            while (ed::QueryDeletedLink(&lid))
            {
                const std::size_t idx = static_cast<std::size_t>(lid.Get()) - 1;
                if (lid.Get() == kPassOutputLinkId || idx >= linkSlots.size())
                    ed::RejectDeletedItem();   // the final wire is structural
                else if (ed::AcceptDeletedItem())
                    unwire.push_back(linkSlots[idx]);
            }
            ed::NodeId nid;
            while (ed::QueryDeletedNode(&nid))
            {
                const std::uint32_t id = static_cast<std::uint32_t>(nid.Get());
                if (id < 2 || id > total)   // base + Output are fixed
                {
                    ed::RejectDeletedItem();
                    continue;
                }
                if (ed::AcceptDeletedItem())
                    removePasses.push_back(id - 1);
            }
        }
        }   // ~CanvasDeleteScope -> ed::EndDelete()

        if (!unwire.empty() || !removePasses.empty())
        {
            capturePassBefore(removePasses.empty() ? "Unwire Pass" : "Remove Pass");
            // Unwire first (descending slot so indices stay valid), then remove
            // passes (descending chain index), fixing every reference.
            std::sort(unwire.rbegin(), unwire.rend());
            for (const auto& [consumer, slot] : unwire)
            {
                std::vector<std::uint32_t>& ins =
                    consumer == 0 ? m_data.baseInputs
                                  : m_data.passes[consumer - 1].inputs;
                if (slot < ins.size())
                    ins.erase(ins.begin() + slot);
            }
            std::sort(removePasses.rbegin(), removePasses.rend());
            for (std::uint32_t r : removePasses)
            {
                m_data.passes.erase(m_data.passes.begin() +
                                    static_cast<std::ptrdiff_t>(r - 1));
                for (Arcane::MaterialPass& p : m_data.passes)
                {
                    std::erase(p.inputs, r);
                    for (std::uint32_t& in : p.inputs)
                        if (in != Arcane::kSceneInput && in > r)
                            --in;
                }
                if (m_activePass >= static_cast<int>(r))
                    --m_activePass;
            }
            m_activePass = std::clamp(m_activePass, 0,
                                      static_cast<int>(m_data.passes.size()));
            m_viewPass = -1;
            structural = true;
        }

        // Frame the pass canvas with the current graph chord.
        if (ImGui::IsWindowHovered() || ed::IsActive()) EditorActions::Get().MarkContextActive(ActionContext::Graph);
        if (ImGui::IsWindowHovered() && !ImGui::GetIO().WantTextInput && EditorActions::Get().Pressed("graph.frameSelected"))
        {
            if (ed::GetSelectedObjectCount() > 0)
                ed::NavigateToSelection(true);
            else
                ed::NavigateToContent();
            m_passFitPending.Disarm();   // the user's own frame wins over a pending fit
        }

        // The pass canvas's twin of the graph canvas's fit-on-open (s4.5),
        // through the same self-confirming latch (see the graph canvas).
        if (m_passFitPending.Update(ed::GetScreenSize(), ImGui::GetTime(), 0.0f, !seededThisFrame) &&
            !GraphFitToContent(GraphFitZoomRangeFromCVars(), 0.0f))
            m_passFitPending.Disarm();   // nothing to fit

        // ---- double-click ENTERS a pass (UE's collapsed-graph gesture).
        //
        // Selection is now pure selection: it no longer writes m_activePass.
        // It used to, because the pass canvas and the pass's graph were on
        // screen together and single-click was the only way to re-aim the lower
        // half. With one canvas at a time that reading is gone -- selecting a
        // node to read its name, or marquee-selecting to drag two of them, must
        // not silently re-aim the editor at a different pass.
        //
        // ENTER is what changes m_activePass, and it is deliberately the same
        // gesture UE uses to descend into a collapsed graph: double-click ->
        // swap the displayed graph into the SAME view rather than open a second
        // one (BlueprintEditor.cpp:4439-4443 picks NavigatingCurrentDocument
        // when the graph's outer is a UK2Node -- the collapsed-graph case --
        // and the material editor mirrors it at MaterialEditor.cpp:6995-6998).
        //
        // Scene and Output are fixed chrome, not passes: they have nothing to
        // descend into, so they no-op. Their double-click used to mean "preview
        // the final image"; that meaning moved to the context menu with the
        // rest of the preview-truncation controls (see below).
        {
            const std::uint32_t dbl =
                static_cast<std::uint32_t>(ed::GetDoubleClickedNode().Get());
            if (dbl >= 1 && dbl <= total)
                EnterPass(static_cast<int>(dbl - 1));
        }

        // ---- context menus (Suspend: popups live in screen space)
        {
            const CanvasPopupScope canvasPopup;   // ed::Suspend/Resume, see the header
            ed::NodeId ctxNode;
            if (ed::ShowNodeContextMenu(&ctxNode))
            {
                m_passCtxNode = static_cast<std::uint32_t>(ctxNode.Get());
                ImGui::OpenPopup("##passnodemenu");
            }
            else if (ed::ShowBackgroundContextMenu())
            {
                const ImVec2 p = ImGui::GetMousePos();
                m_passPopupX = p.x;
                m_passPopupY = p.y;
                ImGui::OpenPopup("##passbgmenu");
            }
            if (ImGui::BeginPopup("##passnodemenu"))
            {
                const std::uint32_t id = m_passCtxNode;
                if (id == kPassOutputNodeId)
                {
                    if (ImGui::MenuItem("Preview Final", nullptr, m_viewPass < 0))
                        m_viewPass = -1;
                }
                else if (id == kPassSceneNodeId)
                {
                    ImGui::TextDisabled("the scene color (bound by the runtime "
                                        "post hook; checkerboard here)");
                }
                else if (id >= 1 && id <= total)
                {
                    // ENTER first: it is the primary gesture now, and a menu
                    // that omitted it would leave double-click undiscoverable.
                    if (ImGui::MenuItem("Edit This Pass"))
                        EnterPass(static_cast<int>(id - 1));
                    ImGui::Separator();
                    // Preview truncation. This menu is its ONLY control since
                    // double-click became Enter, so the item is checkable --
                    // it has to report the state as well as set it (the node's
                    // eye marker is the other half of that).
                    const bool cutHere =
                        (m_viewPass < 0 && id == total) ||
                        m_viewPass == static_cast<int>(id - 1);
                    if (ImGui::MenuItem("Preview Up To Here", nullptr, cutHere))
                        m_viewPass = id == total ? -1 : static_cast<int>(id - 1);
                    if (id >= 2 && ImGui::MenuItem("Remove Pass"))
                    {
                        capturePassBefore("Remove Pass");
                        const std::uint32_t r = id - 1;
                        m_data.passes.erase(m_data.passes.begin() +
                                            static_cast<std::ptrdiff_t>(r - 1));
                        for (Arcane::MaterialPass& p : m_data.passes)
                        {
                            std::erase(p.inputs, r);
                            for (std::uint32_t& in : p.inputs)
                                if (in != Arcane::kSceneInput && in > r)
                                    --in;
                        }
                        if (m_activePass >= static_cast<int>(r))
                            --m_activePass;
                        m_activePass = std::clamp(
                            m_activePass, 0, static_cast<int>(m_data.passes.size()));
                        m_viewPass = -1;
                        structural = true;
                    }
                }
                ImGui::EndPopup();
            }
            if (ImGui::BeginPopup("##passbgmenu"))
            {
                if (ImGui::MenuItem("Add Pass"))
                {
                    capturePassBefore("Add Pass");
                    Arcane::MaterialPass p;
                    p.name = "pass " + std::to_string(m_data.passes.size() + 1);
                    // UE model: new passes are GRAPH-owned. Starter = Pass
                    // Input (slot 0) wired to Output -- a visible passthrough,
                    // never an empty canvas. The snippet regenerates from it.
                    Arcane::MaterialGraph pg;
                    Arcane::GraphNode out;
                    out.id = 1;
                    out.type = Arcane::GraphNodeType::Output;
                    out.posX = 360.0f;
                    out.posY = 120.0f;
                    Arcane::GraphNode in;
                    in.id = 2;
                    in.type = Arcane::GraphNodeType::PassInput;
                    in.posX = 100.0f;
                    in.posY = 120.0f;
                    pg.nodes = { out, in };
                    Arcane::GraphLink link;
                    link.fromNode = 2;
                    link.toNode = 1;
                    pg.links.push_back(link);
                    pg.nextId = 3;
                    p.graph = std::move(pg);
                    // Default wiring: read the current final (linear extend).
                    p.inputs = { static_cast<std::uint32_t>(total - 1) };
                    const ImVec2 canvasPos =
                        ed::ScreenToCanvas(ImVec2(m_passPopupX, m_passPopupY));
                    p.posX = canvasPos.x;
                    p.posY = canvasPos.y;
                    m_data.passes.push_back(std::move(p));
                    m_activePass = static_cast<int>(m_data.passes.size());
                    structural = true;
                }
                ImGui::EndPopup();
            }
        }

        // ---- position readback (skip the seed frame)
        if (!seededThisFrame && !structural)
        {
            auto readback = [&](std::uint32_t nodeId, float& x, float& y)
            {
                const ImVec2 p = ed::GetNodePosition(ed::NodeId(nodeId));
                if (p.x != x || p.y != y)
                {
                    x = p.x;
                    y = p.y;
                    m_dirty = true;
                }
            };
            readback(nodeOf(0), m_data.chainBaseX, m_data.chainBaseY);
            for (std::size_t k = 0; k < m_data.passes.size(); ++k)
                readback(nodeOf(k + 1), m_data.passes[k].posX, m_data.passes[k].posY);
            readback(kPassOutputNodeId, m_data.chainOutX, m_data.chainOutY);
            readback(kPassSceneNodeId, m_data.chainSceneX, m_data.chainSceneY);
        }

        ed::End();
        ed::SetCurrentEditor(nullptr);

        if (structural)
        {
            // Indices moved under the canvas: re-seed node ids from the data
            // next frame, then re-CODEGEN (rewires change which PassInput
            // slots are valid) and recompile the chain.
            m_passCanvasSeeded = false;
            m_dirty = true;
            if (m_live)
                RegenerateFromGraph();
            if (passBefore)
                PushPassUndo(passEditLabel, std::move(*passBefore));
        }
    }

    void ShaderEditorDocument::ForEachDiagnosticRow(
        Arcane::FunctionRef<void(const DiagnosticRow&)> fn)
    {
        // Graph-level codegen errors first, for EVERY pass's graph -- these are
        // the ones the canvas also badges (NodeBadged/RebuildDiagBadges).
        for (std::size_t c = 0; c < m_passGraphErrors.size(); ++c)
        {
            for (const Arcane::GraphError& e : m_passGraphErrors[c])
            {
                std::string row = m_data.passes.empty() ? std::string("graph: ")
                                                        : PassLabel(c) + " graph: ";
                DiagnosticRow out;
                if (e.nodeId != 0)
                {
                    std::optional<Arcane::MaterialGraph>& g = GraphOptAt(c);
                    const Arcane::GraphNode* n = g ? g->FindNode(e.nodeId) : nullptr;
                    row += (n ? std::string(Arcane::GraphNodeInfo(n->type).display)
                              : std::string("node")) +
                           " #" + std::to_string(e.nodeId) + ": ";
                    out.nodeId = e.nodeId;
                }
                out.isError = true;
                out.message = row + e.message;
                fn(out);
            }
        }
        // Stitch/parse failures (instance parent-chain resolution included).
        for (const std::string& e : m_parseErrors)
        {
            DiagnosticRow row;
            row.isError = true;
            row.message = "parse: " + e;
            fn(row);
        }
        // Vertex-stage rows: ONLY diags whose line falls inside the vertex
        // body -- both stages compile the same TU, so pixel-body errors appear
        // in the vs result too and are already carried by the compile rows
        // below. Same filter as HasErrors (the COMPILED vertex stage: an
        // instance's is its base's).
        if (const std::string& vs = CompiledVertexSnippet(); !vs.empty())
        {
            const int vsLines = 1 + static_cast<int>(std::count(vs.begin(), vs.end(), '\n'));
            for (const Arcane::ShaderDiag& d : m_vsDiags)
            {
                const int rel = d.line - m_vsLineOffset;
                if (rel < 1 || rel > vsLines)
                    continue;
                const bool isError = d.severity == Arcane::ShaderDiagSeverity::Error;
                DiagnosticRow row;
                row.isError = isError;
                row.line    = rel;
                row.message = "vertex: " + std::string(isError ? "error" : "warning") +
                              "(" + std::to_string(rel) + "): " + d.message;
                fn(row);
            }
        }
        // Compile diags. Diag lines arrive in STITCHED-source space; the pass's
        // line offset maps them back into the buffer the designer sees.
        auto compileRow = [&](const Arcane::ShaderDiag& d, int offset,
                              const std::string& prefix)
        {
            const bool isError = d.severity == Arcane::ShaderDiagSeverity::Error;
            const int line = d.line > offset ? d.line - offset : 1;
            DiagnosticRow row;
            row.isError = isError;
            row.line    = line;
            row.message = prefix + (isError ? "error" : "warning") +
                          "(" + std::to_string(line) + "): " + d.message;
            fn(row);
        };
        if (CompilesAsChain())
        {
            // A chain compile owns them per pass; m_diags only MIRRORS pass 0
            // there (for the badges), so the single-path loop must not also
            // run. CompilesAsChain, not the authoring ChainMode: an instance of
            // a chain base compiles per pass too, and a failing pass 1+ must
            // still reach the Problems pane (fix round 1).
            for (std::size_t p = 0; p < m_passJobs.size(); ++p)
            {
                const int offset = p < m_passLineOffsets.size() ? m_passLineOffsets[p] : 0;
                for (const Arcane::ShaderDiag& d : m_passJobs[p].diags)
                    compileRow(d, offset, PassLabel(p) + ": ");
            }
            return;
        }
        for (const Arcane::ShaderDiag& d : m_diags)
            compileRow(d, m_snippetLineOffset, std::string());
    }

    void ShaderEditorDocument::PublishDiagnostics()
    {
        // Publication groups make this idempotent: republishing an identical set
        // replaces it with itself. The old FNV-1a signature gate and its
        // synthetic "went clean" log line existed only because the console is
        // append-only, and are gone with it.
        std::vector<Arcane::Diagnostic> out;
        ForEachDiagnosticRow([&](const DiagnosticRow& row)
        {
            Arcane::Diagnostic d;
            d.severity = row.isError ? Arcane::DiagSeverity::Error
                                     : Arcane::DiagSeverity::Warning;
            d.scope    = Arcane::DiagScope::Material;
            d.code     = row.isError ? "material.compile.error" : "material.compile.warning";
            d.message  = m_title + ": " + row.message;
            if (row.nodeId != 0)
                d.locator = Arcane::DiagLocator::GraphNode(m_data.id, row.nodeId);
            else if (row.line != 0)
                d.locator = Arcane::DiagLocator::File(m_path.string(), row.line);
            out.push_back(std::move(d));
        });
        // An empty `out` retracts this document's rows -- the clean transition.
        Arcane::Diagnostics::Publish(DiagnosticKey(), out);
    }

    std::string ShaderEditorDocument::DiagnosticKey() const
    {
        // KEY OWNERSHIP: "material:<guid>" -- this document's own live compile
        // rows (published a few lines up, in the lambda above). NEVER reuse
        // this key for anything but this document's own compile output.
        // LoadMaterialAsset (MaterialAsset.cpp) publishes its dropped-entry
        // diagnostics under the DISTINCT "material-load:<path>" key precisely
        // so a background/file-watcher reload of this same asset can never
        // silently wipe these compile rows out from under an open document --
        // see MaterialAsset.cpp's reciprocal comment at LoadMaterialAsset's
        // `diagnostics` declaration and its final Diagnostics::Publish call.
        return "material:" + m_data.id.ToString();
    }

    // The preview image: the document's offscreen output, and 0 for "nothing
    // to draw" (no vehicle, or PreviewReady() says there is nothing bound
    // yet).
    ShaderEditorDocument::PreviewImage ShaderEditorDocument::PreviewImageOf() const
    {
        if (!PreviewReady() || !m_graphPreview)
            return {};
        return { static_cast<ImTextureID>(GraphPreviewTextureId()),
                 static_cast<float>(m_graphPreviewSize) };
    }

    void ShaderEditorDocument::DrawPreviewPanel(ImVec2 size)
    {
        // A mesh surface is never compiled here, but it IS previewed (T3-D6,
        // the s5.3 amendment): the thumbnail's lit sphere in the CURRENT
        // params, in the same box, with the old one-line note as its caption
        // (the caller leaves room for it: kMeshPreviewCaption).
        const bool mesh = SurfaceOf(m_surface) == Arcane::MaterialSurface::Mesh;
        ImGui::BeginChild("##preview", size, ImGuiChildFlags_Borders);
        const PreviewImage image = PreviewImageOf();
        if (image.id != 0)
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const PreviewFit fit = FitPreviewImage(avail.x, avail.y, image.extent);
            const ImVec2 at = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(at.x + fit.x, at.y + fit.y));
            ImGui::Image(image.id, ImVec2(fit.side, fit.side));
        }
        else if (mesh)
        {
            // PreviewBoxText's NotCompiledHere line names the IMPORTED mesh;
            // a mesh material's box names why its sphere is missing instead.
            const PreviewStatus st = ComputeStatus();
            CenteredTextDisabled(st.preview != PreviewAvailability::Ready
                                     ? "No preview -- " + NoPreviewReason(st)
                                     : std::string("Preview pending -- nothing rendered yet"));
        }
        else
        {
            CenteredTextDisabled(PreviewBoxText(ComputeStatus()));
        }
        ImGui::EndChild();
        if (mesh)
        {
            // Wrapped: the page column is narrow (MeshPreviewCaptionHeight
            // measures the same wrap for the full-tab reservation).
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", kMeshPreviewCaption);
            ImGui::PopTextWrapPos();
        }
    }

    // ------------------------------------------------------------ graph mode
    std::optional<Arcane::MaterialGraph>& ShaderEditorDocument::GraphOptAt(std::size_t pass)
    {
        if (pass > 0 && pass <= m_data.passes.size())
            return m_data.passes[pass - 1].graph;
        return m_data.graph;
    }

    std::optional<Arcane::MaterialGraph>& ShaderEditorDocument::ActiveGraphOpt()
    {
        return GraphOptAt(static_cast<std::size_t>(std::max(0, m_activePass)));
    }

    void ShaderEditorDocument::RegenerateFromGraph()
    {
        const std::size_t total = 1 + m_data.passes.size();
        m_passGraphErrors.assign(total, std::vector<Arcane::GraphError>{});
        m_passLineNodeIds.assign(total, std::vector<std::uint32_t>{});
        bool anyError = false;
        for (std::size_t c = 0; c < total; ++c)
        {
            std::optional<Arcane::MaterialGraph>& g = GraphOptAt(c);
            if (!g)
                continue;
            // The wired-slot context: PassInput nodes may only read slots the
            // pass canvas actually wired (the base's slots are its scene
            // inputs).
            const std::uint32_t avail = static_cast<std::uint32_t>(
                c == 0 ? m_data.baseInputs.size()
                       : m_data.passes[c - 1].inputs.size());
            Arcane::GraphCodegenResult r = Arcane::GenerateGraphSnippet(
                *g, SurfaceOf(m_surface), avail, /*passGraph=*/c > 0);
            if (!r.Ok())
            {
                m_passGraphErrors[c] = std::move(r.errors);
                anyError = true;
                continue;
            }
            m_passLineNodeIds[c] = std::move(r.lineNodeIds);
            (c == 0 ? m_snippet : m_data.passes[c - 1].snippet) = std::move(r.snippet);
            // The base graph OWNS the vertex stage: its Vertex Output node
            // generates the body (absent node = empty = passthrough).
            if (c == 0)
                m_data.vertexSnippet = std::move(r.vertexSnippet);
        }
        if (!anyError)
            Rebuild();   // any codegen error keeps last-good bound; badges show why
    }

    void ShaderEditorDocument::ApplyGraphState(std::size_t pass,
                                               std::optional<Arcane::MaterialGraph> state)
    {
        if (pass > m_data.passes.size())
            return;   // the pass was removed since the step was pushed
        GraphOptAt(pass) = std::move(state);
        if (static_cast<int>(pass) == m_activePass)
            m_graphPositionsApplied = false;   // re-seed the canvas from the data
        m_dirty = true;
        RegenerateFromGraph();
    }

    void ShaderEditorDocument::PushGraphUndo(const char* label,
                                             std::optional<Arcane::MaterialGraph> before)
    {
        PushGraphUndo(label, std::move(before),
                      static_cast<std::size_t>(std::max(0, m_activePass)));
    }

    void ShaderEditorDocument::PushGraphUndo(const char* label,
                                             std::optional<Arcane::MaterialGraph> before,
                                             std::size_t pass)
    {
        // GraphOptAt range-checks (an out-of-range pass falls back to the base,
        // exactly as ActiveGraphOpt's own clamp does), and ApplyGraphState
        // refuses a pass that has since been removed -- so an OUT-OF-RANGE
        // pinned index degrades to an inert step rather than a misdirected
        // write. An index that is still IN RANGE but now names a DIFFERENT
        // pass (the list was reordered or an earlier pass removed) is not
        // covered by either check and would write to that other pass: a
        // pre-existing GraphEditCommand weakness (every step stores a bare
        // index), not one this bracket introduces.
        if (UndoStack())
            UndoStack()->Push(std::make_unique<GraphEditCommand>(
                m_anchor, label, pass, std::move(before), GraphOptAt(pass)));
    }

    // -------------------------------- node-edit plumbing (s5.1.4/5.1.5)
    std::function<void()> ShaderEditorDocument::GraphEditBuilder(const char* label, std::size_t pass)
    {
        // Whole-graph before AND the pass it belongs to, both pinned at
        // activation. The command builds at CLOSE from this plus whatever the
        // gesture did -- which is why an abandoned drag lands on the stack
        // instead of vanishing. Pinning the PASS is what keeps that safe: a
        // close can land after the active pass moved (a ctrl+click text entry
        // parks a gesture without deactivating it, the pass canvas is submitted
        // before the graph panel, and the abandoned close runs later still at
        // the ScopeGuard), and a node page may be pinned to a pass other than
        // the active one. A command pairing pass B's index with pass A's
        // `before` would have Undo overwrite B's graph with A's.
        //
        // NO-OP GUARD: the close runs on EVERY close path, including the
        // abandonment ones (stale-close, collapsed window, document teardown)
        // where the gesture never edited anything. Pushing there would leave a
        // junk step whose before == after AND clear the redo stack
        // (CommandStack.cpp:70) -- a generic Push is its own transaction, so it
        // never meets Commit's empty-transaction drop at :61-62. So compare
        // first; an EDITED gesture still differs and still pushes one step.
        return [this, label = std::string(label), pass, before = GraphOptAt(pass)]() mutable
        {
            if (GraphOptEqual(before, GraphOptAt(pass)))
                return;   // nothing changed -- no step, redo intact
            PushGraphUndo(label.c_str(), std::move(before), pass);
        };
    }

    bool ShaderEditorDocument::CanvasDragEscape(const float (&pre)[4], bool preExisted, float* values,
                                                int lanes, bool* existed)
    {
        // LastItemData.ID: the scalar drag's own id, or -- for DragFloat2/4,
        // which close a group -- the live component's (EndGroup forwards the
        // ActiveId), the same id on every frame of one drag.
        const std::uint32_t item = ImGui::GetItemID();
        if (ImGui::IsItemActivated())
        {
            m_canvasDragSeed.item = item;
            std::memcpy(m_canvasDragSeed.v, pre, sizeof(pre));
            m_canvasDragSeed.existed = preExisted;
        }
        if (m_canvasDragSeed.item == 0 || m_canvasDragSeed.item != item)
            return false;
        if (!ImGui::IsItemActive())
        {
            m_canvasDragSeed = {};   // the gesture ended its own way
            return false;
        }
        // Mid-DRAG only: the text-entry mode (Ctrl+click / double-click) has no
        // button down, and its InputText reverts on Esc by itself.
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !EditorActions::Get().Pressed("ui.cancel"))
            return false;
        std::memcpy(values, m_canvasDragSeed.v, sizeof(float) * static_cast<std::size_t>(lanes));
        if (existed)
            *existed = m_canvasDragSeed.existed;
        m_canvasDragSeed = {};
        ImGui::ClearActiveID();
        return true;
    }

    void ShaderEditorDocument::NoteGraphValueEdited()
    {
        m_dirty = true;
        if (m_live)
            RegenerateFromGraph();
    }

    bool ShaderEditorDocument::RunNodeEdit(const char* label, std::size_t pass, std::uint32_t id,
                                           Arcane::FunctionRef<void(Arcane::GraphNode&, Arcane::MaterialGraph&)> mutate,
                                           bool recompile)
    {
        Arcane::GraphNode* n = FindGraphNode(pass, id);   // range-checks the pass FIRST
        if (!n)
            return false;
        std::optional<Arcane::MaterialGraph> before = GraphOptAt(pass);
        mutate(*n, *GraphOptAt(pass));
        if (GraphOptEqual(before, GraphOptAt(pass)))
            return false;
        if (recompile)
            NoteGraphValueEdited();
        else
            m_dirty = true;   // annotation only (Comment text): no recompile
        PushGraphUndo(label, std::move(before), pass);
        return true;
    }

    void ShaderEditorDocument::DeferNodeEdit(std::function<void()> fn)
    {
        if (m_nodePageDrawing)
            m_nodePageEdits.push_back(std::move(fn));
        else
            fn();
    }

    bool ShaderEditorDocument::AddCustomPin(std::size_t pass, std::uint32_t id)
    {
        const Arcane::GraphNode* n = FindGraphNode(pass, id);
        if (!n || n->type != Arcane::GraphNodeType::Custom)
            return false;
        return RunNodeEdit("Add Pin", pass, id, [](Arcane::GraphNode& node, Arcane::MaterialGraph&)
        {
            Arcane::GraphCustomPin p;
            for (std::uint32_t k = 1;; ++k)
            {
                p.name = "p" + std::to_string(k);
                bool taken = false;
                for (const Arcane::GraphCustomPin& other : node.customPins)
                    taken = taken || other.name == p.name;
                if (!taken)
                    break;
            }
            node.customPins.push_back(std::move(p));
        });
    }

    bool ShaderEditorDocument::RemoveCustomPin(std::size_t pass, std::uint32_t id, std::uint32_t pin)
    {
        const Arcane::GraphNode* n = FindGraphNode(pass, id);
        if (!n || n->type != Arcane::GraphNodeType::Custom || pin >= n->customPins.size())
            return false;
        return RunNodeEdit("Remove Pin", pass, id, [id, pin](Arcane::GraphNode& node, Arcane::MaterialGraph& g)
        {
            // Drop the pin's links, re-index links to later pins (toPin is a
            // bare index into this node's pin list).
            std::erase_if(g.links, [&](const Arcane::GraphLink& l) { return l.toNode == id && l.toPin == pin; });
            for (Arcane::GraphLink& l : g.links)
                if (l.toNode == id && l.toPin > pin)
                    --l.toPin;
            node.customPins.erase(node.customPins.begin() + pin);
            // Literals index pins exactly like links: same drop + re-index, or a
            // removed pin's value resurfaces on whatever pin slid into its index
            // (silently, since the reader only range-checks).
            std::erase_if(node.pinLiterals, [&](const Arcane::GraphPinLiteral& pl) { return pl.pin == pin; });
            for (Arcane::GraphPinLiteral& pl : node.pinLiterals)
                if (pl.pin > pin)
                    --pl.pin;
        });
    }

    // --------------------------------------------- external file changes
    void ShaderEditorDocument::ReloadFromDisk()
    {
        auto data = Arcane::LoadMaterialAsset(m_path);
        if (!data)
        {
            ARC_WARN("'{}': reload from disk failed -- keeping the in-memory copy",
                     m_title);
            return;
        }
        m_data = std::move(*data);
        m_snippet = m_data.snippet;
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + (m_data.IsInstance() ? " (Instance)###matdoc_"
                                                       : " (Material)###matdoc_") +
                        m_data.id.ToString();
        m_dirty = false;
        m_paramsBaseDirty = false;
        m_savedParamSerial = 0;
        m_instance.reset();      // the file's values are truth again (rebind reapplies)
        m_paramRenames.clear();
        m_parentChain.clear();
        m_parseErrors.clear();
        m_activePass = 0;
        m_viewPass = -1;
        m_passCanvasSeeded = false;
        m_passFitOnSeed = true;   // the reloaded file is a fresh view (the graph canvas rebuilds its context)
        m_graphPositionsApplied = false;
        m_graphShownPass = -1;
        if (!IsInstance() || ResolveParentChain())
        {
            const std::string& kind =
                IsInstance() && !m_parentChain.empty() ? m_parentChain.back().kind
                                                       : m_data.kind;
            m_surface = SurfaceIndexOf(Arcane::MaterialSurfaceForKind(kind));
            RegenerateFromGraph();
        }
    }

    bool ShaderEditorDocument::DependsOn(const Arcane::Guid& id) const
    {
        for (const Arcane::MaterialAssetData& p : m_parentChain)
            if (p.id == id)
                return true;
        return false;
    }

    void ShaderEditorDocument::RefreshParentChain()
    {
        if (!IsInstance())
            return;
        m_parseErrors.clear();
        if (ResolveParentChain())
            Rebuild();   // failure keeps last-good bound + the errors visible
    }

    // --------------------------------------------- assisted param rename
    void ShaderEditorDocument::BeginParamRename(const std::string& oldName,
                                                const std::string& newName)
    {
        if (IsInstance() || oldName.empty() || newName.empty() || oldName == newName)
            return;
        // Sole-declarer guard: if ANOTHER node (any pass's graph) still
        // declares the old name, this edit was a decl SPLIT, not a rename --
        // the shared declaration lives on, nothing orphans.
        for (std::size_t c = 0; c <= m_data.passes.size(); ++c)
        {
            const std::optional<Arcane::MaterialGraph>& opt = GraphOptAt(c);
            if (!opt)
                continue;
            for (const Arcane::GraphNode& gn : opt->nodes)
                if ((gn.type == Arcane::GraphNodeType::Param ||
                     gn.type == Arcane::GraphNodeType::TextureSample) &&
                    gn.paramName == oldName)
                    return;
        }

        // Local fix (unconditional): this document's own saved value follows
        // the rename; the live override migrates at the next rebind through
        // the pending-rename translation.
        RekeySavedParam(m_data.params, oldName, newName);
        m_paramRenames.emplace_back(oldName, newName);

        // Discovery: every registered instance whose parent chain reaches
        // THIS base (any depth, cycle-guarded) and whose saved params carry
        // the old name. GUID identity is what makes this possible at all --
        // Unity structurally cannot find these files.
        m_renameTargets.clear();
        const Arcane::Project* project =
            m_services.runtime ? m_services.runtime->CurrentProject() : nullptr;
        if (!project)
            return;
        for (const AssetEntry& e : BuildAssetEntries(project->Registry()))
        {
            if (e.kind != AssetKind::Material || e.guid == m_data.id)
                continue;
            const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(e.guid));
            if (!path)
                continue;
            const auto data = Arcane::LoadMaterialAsset(*path);
            if (!data || !data->IsInstance())
                continue;

            bool reaches = false;
            Arcane::Guid cursor = data->parent;
            std::vector<Arcane::Guid> visited{ data->id };
            while (cursor.IsValid())
            {
                if (cursor == m_data.id)
                {
                    reaches = true;
                    break;
                }
                bool seen = false;
                for (const Arcane::Guid& v : visited)
                    seen = seen || v == cursor;
                if (seen)
                    break;   // cycle -- refuse quietly
                visited.push_back(cursor);
                const auto hopPath = project->ResolveAsset(Arcane::AssetId::FromGuid(cursor));
                if (!hopPath)
                    break;
                const auto hop = Arcane::LoadMaterialAsset(*hopPath);
                if (!hop)
                    break;
                cursor = hop->parent;
            }
            if (!reaches)
                continue;
            bool carries = false;
            for (const auto& [pname, pvalue] : data->params)
                carries = carries || pname == oldName;
            if (!carries)
                continue;
            m_renameTargets.push_back(
                { e.guid, *path, data->name.empty() ? e.name : data->name });
        }
        if (m_renameTargets.empty())
            return;   // nothing to propagate -- no modal
        m_renameOld = oldName;
        m_renameNew = newName;
        m_renameRequest = true;
    }

    void ShaderEditorDocument::PatchParamRename(const std::string& oldName,
                                                const std::string& newName)
    {
        // The base's propagation rewrote this instance's FILE; re-key the open
        // document's memory to match (unsaved edits stay untouched) and queue
        // the override migration for whenever the renamed base rebinds here.
        RekeySavedParam(m_data.params, oldName, newName);
        m_paramRenames.emplace_back(oldName, newName);
    }

    std::uint32_t ShaderEditorDocument::TranslateOverrideHash(
        std::uint32_t hash, const Arcane::MaterialTemplate& templ) const
    {
        // A pending rename migrates an override across the template boundary
        // the moment it MATERIALIZES (the target declares new, not old) -- and
        // in reverse when an undo rolled the template back. Inert otherwise,
        // so a stale pair can never mistranslate.
        for (const auto& [oldName, newName] : m_paramRenames)
        {
            const std::uint32_t oh = Arcane::HashParamName(oldName);
            const std::uint32_t nh = Arcane::HashParamName(newName);
            if (hash == oh && templ.Find(nh) && !templ.Find(oh))
                hash = nh;
            else if (hash == nh && templ.Find(oh) && !templ.Find(nh))
                hash = oh;
        }
        return hash;
    }

    ShaderEditorDocument::PassListState ShaderEditorDocument::CapturePassListState() const
    {
        return { m_data.passes, m_data.baseInputs, m_activePass, m_viewPass };
    }

    void ShaderEditorDocument::ApplyPassListState(PassListState state)
    {
        if (IsInstance())
            return;   // instances never carry passes
        m_data.passes = std::move(state.passes);
        m_data.baseInputs = std::move(state.baseInputs);
        const int count = static_cast<int>(m_data.passes.size());
        m_activePass = std::clamp(state.activePass, 0, count);
        m_viewPass = std::clamp(state.viewPass, -1, count);
        // Chain indices moved under BOTH canvases: re-seed the pass canvas,
        // and force the graph canvas through its pass-switch path (positions,
        // selection, badges, thumbnails).
        m_passCanvasSeeded = false;
        m_graphPositionsApplied = false;
        m_graphShownPass = -1;
        m_dirty = true;
        RegenerateFromGraph();
    }

    void ShaderEditorDocument::PushPassUndo(const char* label, PassListState before)
    {
        if (UndoStack())
            UndoStack()->Push(std::make_unique<PassListCommand>(
                m_anchor, label, std::move(before), CapturePassListState()));
    }

    GraphPinPaint ShaderEditorDocument::GraphPinPaintOn(const Arcane::GraphNode& node, std::uint32_t pin,
                                                        bool input) const
    {
        const Arcane::GraphPinDesc desc = input ? Arcane::GraphNodeInputPin(node, pin)
                                                : Arcane::GraphNodeOutputPin(node, pin);
        const Arcane::GraphNodeWidths w = WidthsOf(m_canvasWidths, node.id);
        return PinPaintFor(desc.width, input ? w.inputs : w.outputs);
    }

    GraphPinPaint ShaderEditorDocument::CanvasPinPaint(std::uint32_t id, std::uint32_t pin, bool input) const
    {
        const Arcane::MaterialGraph* g = PassGraph(static_cast<std::size_t>(std::max(0, m_activePass)));
        const Arcane::GraphNode* n = g ? g->FindNode(id) : nullptr;
        if (!n || pin >= (input ? Arcane::GraphNodeInputCount(*n) : Arcane::GraphNodeOutputCount(*n)))
            return {};
        return GraphPinPaintOn(*n, pin, input);
    }

    std::optional<ShaderEditorDocument::CanvasWireTint>
    ShaderEditorDocument::CanvasWireTintOf(std::size_t linkIndex) const
    {
        if (linkIndex >= m_wireTints.size())
            return std::nullopt;
        return m_wireTints[linkIndex];
    }

    std::string ShaderEditorDocument::CanvasPinTooltip(const Arcane::MaterialGraph& graph, const Arcane::GraphNode& node,
                                                       std::uint32_t pin, bool input) const
    {
        const Arcane::GraphPinDesc desc = input ? Arcane::GraphNodeInputPin(node, pin)
                                                : Arcane::GraphNodeOutputPin(node, pin);
        const Arcane::GraphNodeWidths w = WidthsOf(m_canvasWidths, node.id);
        std::string source;
        int fanout = 0;
        for (const Arcane::GraphLink& l : graph.links)
        {
            if (input && l.toNode == node.id && l.toPin == pin)
                source = WireSourceText(graph, l);   // last wins, as codegen reads it
            if (!input && l.fromNode == node.id && l.fromPin == pin)
                ++fanout;
        }
        return PinTooltipText(desc.name, desc.width, input ? w.inputs : w.outputs, input, source, fanout);
    }

    bool ShaderEditorDocument::NodeBadged(std::uint32_t nodeId) const
    {
        const std::size_t c = static_cast<std::size_t>(std::max(0, m_activePass));
        if (c < m_passGraphErrors.size())
            for (const Arcane::GraphError& e : m_passGraphErrors[c])
                if (e.nodeId == nodeId)
                    return true;
        for (std::uint32_t id : m_diagBadgeNodes)
            if (id == nodeId)
                return true;
        return false;
    }

    void ShaderEditorDocument::RebuildDiagBadges()
    {
        // Compile-diag badges for the ACTIVE pass's canvas (single-path docs
        // are pass 0 throughout); graph-level lines (node 0) badge nothing.
        m_diagBadgeNodes.clear();
        ForEachPassErrorDiag(static_cast<std::size_t>(std::max(0, m_activePass)),
                             [&](std::uint32_t id, std::string_view)
                             {
                                 if (id != 0)
                                     m_diagBadgeNodes.push_back(id);
                             });
    }

    void ShaderEditorDocument::ForEachPassErrorDiag(
        std::size_t pass, const std::function<void(std::uint32_t nodeId, std::string_view message)>& fn) const
    {
        // That pass's diags, line offset, and line map.
        if (pass >= m_passLineNodeIds.size() || m_passLineNodeIds[pass].empty())
            return;
        const std::vector<Arcane::ShaderDiag>* diags = &m_diags;
        int offset = m_snippetLineOffset;
        if (CompilesAsChain() && pass < m_passJobs.size())   // the compile predicate (per-pass diags)
        {
            diags = &m_passJobs[pass].diags;
            if (pass < m_passLineOffsets.size())
                offset = m_passLineOffsets[pass];
        }
        const std::vector<std::uint32_t>& lineMap = m_passLineNodeIds[pass];
        for (const Arcane::ShaderDiag& d : *diags)
        {
            if (d.severity != Arcane::ShaderDiagSeverity::Error)
                continue;
            // stitched line -> snippet line -> statement's node (the line map).
            const int snippetLine = d.line - offset;
            const std::size_t idx = static_cast<std::size_t>(snippetLine) - 1;
            if (snippetLine >= 1 && idx < lineMap.size())
                fn(lineMap[idx], d.message);
        }
    }

    // ---------------------------------- node preview thumbnails ------------
    // THERE ARE NONE, and the gap is named rather than left to be discovered:
    // the shader graph canvas draws no per-node thumbnail. Doing so would need
    // one render target and one pass PER NODE (see THE PREVIEW above).
    //
    // The MATERIAL's own preview -- the material page's image, the pass-canvas
    // base thumbnail, and the Output node's own image below -- all render for
    // real through the document's offscreen graph context.
    //
    // WHAT SURVIVES, UNCHANGED FROM BEFORE THIS TASK (fix round 1 restored
    // the toggle a first pass wrongly deleted): DrawNodePreviewImage below,
    // gated by m_showNodePreviews exactly as it was at 2ab107dd. That flag is
    // NOT dead -- it also gates this function's one live branch, the Output
    // node's real material preview, which is why the checkbox that sets it
    // stays on the toolbar.

    void ShaderEditorDocument::DrawNodePreviewImage(const Arcane::GraphNode& n, float width)
    {
        if (!m_showNodePreviews)
            return;
        // SG parity: the thumbnail is square and spans the node, sitting below
        // the port rows. `width` is last frame's measured content width -- a
        // node drawing for the first time has none and gets the floor, which is
        // also what keeps a narrow node from collapsing the thumbnail.
        const float thumbMin = Arcane::Settings<GraphCanvasSettings>().nodePreviewMinPx;
        const float kThumbDraw = width > thumbMin ? width : thumbMin;
        // The Output node shows the material's own preview -- the pass
        // canvas's base-node convention. It is the ONLY node with a preview:
        // there is no per-node compile/record machinery.
        if (n.type == Arcane::GraphNodeType::Output)
        {
            if (const ImTextureID id = PreviewImageOf().id)
                ImGui::Image(id, ImVec2(kThumbDraw, kThumbDraw));
        }
    }

    void ShaderEditorDocument::DrawGraphPanel()
    {
        if (!ActiveGraphOwned())
            return;
        // One context per SHOWN graph: node ids are only unique per graph and
        // the context keeps per-id state (a Comment's group record re-types
        // the id), so a pass switch rebuilds the context wholesale -- which
        // also drops the selection and stale view for free.
        const bool switchedPass = m_graphShownPass != m_activePass;
        if (switchedPass && m_graphCtx)
        {
            ed::DestroyEditor(m_graphCtx);
            m_graphCtx = nullptr;
        }
        if (!m_graphCtx)
        {
            ed::Config cfg;
            cfg.SettingsFile = nullptr;   // layout persists in the .arcmat, not an ini
            ApplyZoomLevels(cfg);         // UE's 20 stops by default (editor.graph.zoomLevels)
            // UE's selection modifiers (SNodePanel.cpp:194-212, MarqueeOperation.h:
            // 50-68): Shift+click and Shift+drag ADD. Upstream's Shift+drag selects
            // only groups (Comments), which read on the desk as a broken marquee.
            // editor.graph.shiftAddsToSelection (S6-34; true by default).
            cfg.ShiftAddsToSelection = Settings<GraphCanvasSettings>().shiftAddsToSelection;
            m_graphCtx = ed::CreateEditor(&cfg);
            // The style is per-context state, so a rebuilt context re-applies
            // it -- including the switch that kills the vendored grid.
            ed::SetCurrentEditor(m_graphCtx);
            ApplyGraphCanvasStyle(ShaderCanvasStyleDesc());
            ed::SetCurrentEditor(nullptr);
        }
        if (switchedPass)
        {
            m_graphShownPass = m_activePass;
            m_graphPositionsApplied = false;
            m_nodeWidths.clear();   // ids are only unique per graph
            RebuildDiagBadges();
        }
        Arcane::MaterialGraph& g = *ActiveGraphOpt();

        ed::SetCurrentEditor(m_graphCtx);
        RefreshGraphCanvasStyle(ShaderCanvasStyleDesc());   // a Live theme change reaches the open canvas
        DrawCanvasBackdrop(m_gridPhase);
        // The canvas's SCREEN rect, for the pin legend (screen space, T3-D1).
        const ImVec2 canvasMin  = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        // Nothing is drawn above the canvas inside this function, so the
        // remaining region IS the canvas's height.
        ed::Begin("##graphcanvas", ImVec2(0.0f, ImGui::GetContentRegionAvail().y));
        if (switchedPass)
            ed::ClearSelection();

        const bool seededThisFrame = !m_graphPositionsApplied;
        if (seededThisFrame)
        {
            for (const Arcane::GraphNode& n : g.nodes)
            {
                ed::SetNodePosition(n.id, ImVec2(n.posX, n.posY));
                if (n.type == Arcane::GraphNodeType::Comment)
                    ed::SetGroupSize(n.id, ImVec2((std::max)(80.0f, n.value[0]),
                                                  (std::max)(60.0f, n.value[1])));
            }
            m_graphPositionsApplied = true;
            // s4.5's fit belongs to a FRESH view: the open, a pass switch, a
            // reload or a pass-list undo -- each rebuilt the context above, so
            // there is no view to keep. An undo/redo of a graph edit
            // (ApplyGraphState) re-seeds positions too, but on the SAME context,
            // and the user's zoom and scroll must survive it (T3-D3).
            if (switchedPass)
                m_fitPending.Arm();
        }

        // ---- Rendering LOD: ONE read of the zoom, ONE tier, per frame ----
        // Read inside Begin/End on purpose: this is the view the editor
        // installed for THIS frame's submission (imgui_node_editor.cpp:1258),
        // so the tier and the geometry it degrades agree exactly. The grid's
        // read above is the same number -- the navigate action only re-derives
        // the view during End -- but it is taken before Begin because
        // ScreenToCanvas has to be, so the two calls stay separate.
        const NodeLOD lod = NodeLODForScale(GraphViewScale());
        // Culled-node set for THIS submission: refilled below. Currently
        // WRITE-ONLY -- nothing reads it (see its declaration in the header).
        m_culledGraphNodes.clear();

        // Wire anchors are per-frame: nodes move, the view moves, and a pin
        // that stops being submitted must stop having an anchor (see
        // DrawGradientWire's miss path). Cleared here, refilled by the pin
        // rows below, consumed by the link loop after them.
        m_pinPivots.clear();
        m_wireTints.clear();

        // The dynamic-width resolution codegen emits from, ONCE per frame for
        // this graph (T3-D1): every pin dot, wire end and tooltip below reads it.
        m_canvasWidths = Arcane::ResolveGraphNodeWidths(g);
        m_pinTip = {};
        // The pin legend's click target, BEFORE any node: ImGui hands hover to
        // the first item submitted over a point, and the canvas's own hit areas
        // come later, in ed::End -- so a click on the legend folds it instead
        // of reaching the graph (ShaderGraphPinLegend.hpp). Screen space.
        {
            const CanvasPopupScope screenSpace;
            m_pinLegendHovered = GraphPinLegendInteract(canvasMin, canvasSize);
        }

        for (Arcane::GraphNode& n : g.nodes)
            DrawGraphNode(n, lod);

        // Links: ids are the vector index + 1, stable within this frame (the
        // deletion pass collects indices and erases after the queries).
        for (std::size_t i = 0; i < g.links.size(); ++i)
        {
            const Arcane::GraphLink& l = g.links[i];
            // A wire now carries BOTH its endpoints' types: source colour at
            // the tail, destination colour at the head. That makes an adapting
            // connection (float -> float4, or anything into a dynamic pin)
            // legible as a transition rather than as a wire that lies about one
            // of its ends. A dangling endpoint (should not survive an edit, but
            // the draw must not depend on that) falls back to the neutral
            // dynamic colour.
            //
            // Each end takes its PIN's paint colour (T3-D1): a resolved dynamic
            // pin is the width it resolved to, the same colour as its dot.
            const Arcane::GraphNode* src = g.FindNode(l.fromNode);
            const bool srcPinValid =
                src && l.fromPin < Arcane::GraphNodeOutputCount(*src);
            const ImVec4 srcTint =
                srcPinValid ? GraphPinPaintOn(*src, l.fromPin, /*input*/ false).color
                            : PinDynamicColor();

            const Arcane::GraphNode* dst = g.FindNode(l.toNode);
            const bool dstPinValid =
                dst && l.toPin < Arcane::GraphNodeInputCount(*dst);
            const ImVec4 dstTint =
                dstPinValid ? GraphPinPaintOn(*dst, l.toPin, /*input*/ true).color
                            : PinDynamicColor();

            const ed::LinkId linkId(i + 1);
            const ed::PinId fromPin = OutPin(l.fromNode, l.fromPin);
            const ed::PinId toPin   = InPin(l.toNode, l.toPin);

            // Interaction only -- transparent, so the library tessellates
            // nothing (see kGraphLinkChannel). The thickness is the real one:
            // it is still the hit radius.
            ed::Link(linkId, fromPin, toPin, ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
                     GraphWireThickness());

            // GetHoveredLink reports 0 while any action is running (the
            // m_CurrentAction guard, imgui_node_editor.cpp:1280), so a wire
            // does not flicker bright while it is being dragged past.
            const bool emphasize = ed::IsLinkSelected(linkId) ||
                                   ed::GetHoveredLink() == linkId;
            m_wireTints.push_back({ srcTint, dstTint });   // index i: the CanvasWireTintOf seam
            DrawGradientWire(fromPin.Get(), toPin.Get(), srcTint, dstTint,
                             emphasize);
        }

        // The hovered pin's tooltip (T3-D1): name, type word, wiring. The pin
        // rows recorded the hover; the tooltip opens here, once, in screen
        // space (CanvasPopupScope).
        if (m_pinTip.valid)
            if (const Arcane::GraphNode* tipNode = g.FindNode(m_pinTip.node))
            {
                const std::string tip = CanvasPinTooltip(g, *tipNode, m_pinTip.pin, m_pinTip.isInput);
                const CanvasPopupScope screenSpace;
                ImGui::SetTooltip("%s", tip.c_str());
            }

        HandleGraphEdits();

        // Node page selection mirror, WRITE half (s5.1.1): the only place the
        // document writes `ed` selection besides the Problems locator below.
        // Here, because this frame's nodes now exist (SelectNode resolves
        // through the context's node list) and ed::End has not yet run this
        // frame's selection actions. Never NavigateToSelection: history,
        // crumb and restore landings select without framing (s5.1.3).
        if (m_nodeSelRequest)
        {
            if (m_nodeSelRequest->op == NodeSelRequest::Select)
                ed::SelectNode(ed::NodeId(m_nodeSelRequest->id));
            else
                ed::ClearSelection();
            m_nodeSelApplying = true;
            m_nodeSelRequest.reset();
        }

        // Requested focus: select + frame the offending node. Written by
        // RequestFocusGraphNode (Task 5, the Problems panel's GraphNode
        // locator) -- the errors panel's rows were its only writer before
        // that panel was removed; console lines are not clickable.
        //
        // Both this focus and the s4.5 fit below go through a CanvasNavLatch
        // (Widgets/GraphFit.hpp): the node editor's Begin answers a canvas
        // RESIZE by re-centring the view it showed LAST draw
        // (imgui_node_editor.cpp:1221-1254), which discards any navigation
        // still in flight -- and a freshly opened document's canvas changes
        // width over its first draws (a scrollbar comes and goes while the
        // layout settles), as can any dock/splitter drag. The latch re-issues
        // the navigation on a draw that sees a new size and disarms once the
        // size held across it (and, for this animated focus, the animation's
        // ScrollDuration elapsed). Not on the seed draw: no node is measured.
        const ImVec2 graphCanvasSize = ed::GetScreenSize();
        const double graphCanvasNow = ImGui::GetTime();
        if (m_focusNode != 0 &&
            m_focusPending.Update(graphCanvasSize, graphCanvasNow, ed::GetStyle().ScrollDuration, !seededThisFrame))
        {
            ed::SelectNode(ed::NodeId(m_focusNode));
            ed::NavigateToSelection(true);
        }
        if (m_focusPending.Pending())
            m_fitPending.Disarm();   // a Problems focus outranks the open fit
        else
            m_focusNode = 0;

        // Frame the graph selection, or everything when nothing is selected.
        if (ImGui::IsWindowHovered() || ed::IsActive()) EditorActions::Get().MarkContextActive(ActionContext::Graph);
        if (ImGui::IsWindowHovered() && !ImGui::GetIO().WantTextInput && EditorActions::Get().Pressed("graph.frameSelected"))
        {
            if (ed::GetSelectedObjectCount() > 0)
                ed::NavigateToSelection(true);
            else
                ed::NavigateToContent();
            // The user's own frame wins over any pending automatic one.
            m_fitPending.Disarm();
            m_focusPending.Disarm();
            m_focusNode = 0;
        }

        // Frame-to-fit on open (s4.5): armed by a fresh-view seed above, issued on the
        // first later draw and re-issued until it LANDED (the latch, see the
        // focus block above), AFTER the node loop -- every node is live and
        // carries the size it measured last frame (NodeCulled exempts
        // unmeasured nodes). Capped by editor.graph.fitMaxZoom and floored by
        // editor.graph.fitMinZoom (a graph too big for the floor frames its
        // centre); the selection is never touched. F above stays the uncapped
        // frame-selection / frame-all. Duration 0: it lands at the next Begin,
        // so settle 0.
        if (m_fitPending.Update(graphCanvasSize, graphCanvasNow, 0.0f, !seededThisFrame) &&
            !GraphFitToContent(GraphFitZoomRangeFromCVars(), 0.0f))
            m_fitPending.Disarm();   // nothing to fit

        // Node context menu -> alignment over the current selection.
        {
            const CanvasPopupScope canvasPopup;   // ed::Suspend/Resume, see the header
            ed::NodeId ctxNode;
            if (ed::ShowNodeContextMenu(&ctxNode))
                ImGui::OpenPopup("##graphnodemenu");
            if (ImGui::BeginPopup("##graphnodemenu"))
            {
                std::vector<ed::NodeId> sel(
                    static_cast<std::size_t>(std::max(0, ed::GetSelectedObjectCount())));
                const int count = sel.empty() ? 0
                    : ed::GetSelectedNodes(sel.data(), static_cast<int>(sel.size()));
                std::vector<Arcane::GraphNode*> picked;
                for (int i = 0; i < count; ++i)
                    if (Arcane::GraphNode* node = g.FindNode(static_cast<std::uint32_t>(
                            sel[static_cast<std::size_t>(i)].Get())))
                        picked.push_back(node);
                const bool can = picked.size() >= 2;

                // One undo step per alignment; positions write BOTH the canvas
                // and the data, so the later readback sees no delta.
                auto align = [&](const char* label, auto&& place)
                {
                    if (!ImGui::MenuItem(label, nullptr, false, can))
                        return;
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    for (Arcane::GraphNode* node : picked)
                    {
                        const ImVec2 size = ed::GetNodeSize(ed::NodeId(node->id));
                        ImVec2 p(node->posX, node->posY);
                        place(p, size);
                        node->posX = p.x;
                        node->posY = p.y;
                        ed::SetNodePosition(ed::NodeId(node->id), p);
                    }
                    m_dirty = true;
                    PushGraphUndo(label, std::move(before));
                };

                float minX = FLT_MAX, minY = FLT_MAX, maxR = -FLT_MAX, maxB = -FLT_MAX;
                float sumCX = 0.0f, sumCY = 0.0f;
                for (const Arcane::GraphNode* node : picked)
                {
                    const ImVec2 size = ed::GetNodeSize(ed::NodeId(node->id));
                    minX = (std::min)(minX, node->posX);
                    minY = (std::min)(minY, node->posY);
                    maxR = (std::max)(maxR, node->posX + size.x);
                    maxB = (std::max)(maxB, node->posY + size.y);
                    sumCX += node->posX + size.x * 0.5f;
                    sumCY += node->posY + size.y * 0.5f;
                }
                const float n = picked.empty() ? 1.0f : static_cast<float>(picked.size());
                const float avgCX = sumCX / n, avgCY = sumCY / n;

                align("Align Left",   [&](ImVec2& p, const ImVec2&)  { p.x = minX; });
                align("Align Right",  [&](ImVec2& p, const ImVec2& s){ p.x = maxR - s.x; });
                align("Align Top",    [&](ImVec2& p, const ImVec2&)  { p.y = minY; });
                align("Align Bottom", [&](ImVec2& p, const ImVec2& s){ p.y = maxB - s.y; });
                align("Center Column",[&](ImVec2& p, const ImVec2& s){ p.x = avgCX - s.x * 0.5f; });
                align("Center Row",   [&](ImVec2& p, const ImVec2& s){ p.y = avgCY - s.y * 0.5f; });
                if (!can)
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("select 2+ nodes to align");
                }
                ImGui::EndPopup();
            }
        }

        // Background context menu -> create node (Suspend: popups live in
        // normal ImGui space, not canvas space).
        ed::Suspend();
        if (ed::ShowBackgroundContextMenu())
        {
            m_wireActive = false;   // plain create -- no wire to connect
            const ImVec2 p = ImGui::GetMousePos();
            m_graphPopupX = p.x;
            m_graphPopupY = p.y;
            ImGui::OpenPopup("##graphcreate");
        }
        if (m_wireCreateRequest)
        {
            m_wireCreateRequest = false;
            m_wireActive = true;
            const ImVec2 p = ImGui::GetMousePos();
            m_graphPopupX = p.x;
            m_graphPopupY = p.y;
            ImGui::OpenPopup("##graphcreate");
        }

        // The body editor and the rename consent modal (s5.1.7); still inside
        // this Suspend, where they sat before the hoist.
        DrawGraphModals();

        if (ImGui::BeginPopup("##graphcreate"))
        {
            // The searcher: type-to-filter, Enter creates the first match.
            if (ImGui::IsWindowAppearing())
            {
                m_createSearch[0] = '\0';
                ImGui::SetKeyboardFocusHere();
            }
            const bool enter = ImGui::InputTextWithHint(
                "##nodesearch", "Search...", m_createSearch, sizeof(m_createSearch),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Separator();

            const Arcane::GraphNodeTypeInfo* chosen = nullptr;
            const Arcane::GraphNodeTypeInfo* first = nullptr;
            // CONTEXT eligibility (surface, pass context, wire side) -- asked
            // in one place so the flat and the categorised pass below cannot
            // drift apart. The search filter is deliberately NOT in here: the
            // two passes differ precisely in whether they apply it.
            auto eligible = [&](const Arcane::GraphNodeTypeInfo& info)
            {
                if (info.type == Arcane::GraphNodeType::Output)
                    return false;   // exactly one, seeded at creation, undeletable
                const bool spriteOnly = info.type == Arcane::GraphNodeType::VertexColor ||
                                        info.type == Arcane::GraphNodeType::SpriteTexture;
                if (spriteOnly && m_surface != 1)
                    return false;
                // Pass Input samples wired input slots -- available wherever
                // the active pass has any (the base's are its scene wires).
                if (info.type == Arcane::GraphNodeType::PassInput &&
                    m_activePass == 0 && m_data.baseInputs.empty())
                    return false;
                // The vertex context lives on the BASE graph, at most once.
                if (info.type == Arcane::GraphNodeType::VertexOutput &&
                    (m_activePass != 0 ||
                     [&g] {
                         for (const Arcane::GraphNode& n : g.nodes)
                             if (n.type == Arcane::GraphNodeType::VertexOutput)
                                 return true;
                         return false;
                     }()))
                    return false;
                // Wire-invoked: only types with a pin on the wire's far side.
                // Every pin is numeric (adaptation absorbs widths), so
                // compatibility is purely structural. A fresh Custom node has
                // no inputs, so it only appears for input-side drags.
                if (m_wireActive)
                {
                    const bool hasFarPin = m_wireIsInput ? !info.outputs.empty()
                                                         : !info.inputs.empty();
                    if (!hasFarPin)
                        return false;
                }
                return true;
            };
            auto emit = [&](const Arcane::GraphNodeTypeInfo& info)
            {
                if (!first)
                    first = &info;   // Enter creates this one
                if (ImGui::MenuItem(info.display))
                    chosen = &info;
            };
            // A SEARCH FLATTENS the list: with a filter typed, headings would
            // strand one or two items under each and push the first match --
            // what Enter creates -- further down. Unfiltered, ~49 types need
            // the structure, so they group by GraphNodeCategory in ENUM order
            // (Uncategorized, Input, Math, Vector, Procedural, Output,
            // Utility). Empty categories draw no heading, so a sprite-only or
            // wire-invoked popup never shows a bare title -- and Uncategorized
            // is empty for any healthy table, which is why the menu looks the
            // same as it did before that enumerator existed. A row that ever
            // lands there gets a heading saying so, rather than hiding among
            // the Input nodes.
            if (m_createSearch[0] != '\0')
            {
                for (const Arcane::GraphNodeTypeInfo& info : Arcane::AllGraphNodeInfos())
                    if (eligible(info) && ContainsInsensitive(info.display, m_createSearch))
                        emit(info);
            }
            else
            {
                // The bound comes from the TABLE, not from the last
                // enumerator: a category appended after Utility then gets its
                // own heading here instead of silently hiding its nodes.
                int maxCat = 0;
                for (const Arcane::GraphNodeTypeInfo& info : Arcane::AllGraphNodeInfos())
                    maxCat = std::max(maxCat, static_cast<int>(info.category));
                for (int c = 0; c <= maxCat; ++c)
                {
                    const auto cat = static_cast<Arcane::GraphNodeCategory>(c);
                    bool headed = false;
                    for (const Arcane::GraphNodeTypeInfo& info : Arcane::AllGraphNodeInfos())
                    {
                        if (info.category != cat || !eligible(info))
                            continue;
                        if (!headed)
                        {
                            ImGui::SeparatorText(Arcane::GraphNodeCategoryName(cat));
                            headed = true;
                        }
                        emit(info);
                    }
                }
            }
            if (enter && first)
            {
                chosen = first;
                ImGui::CloseCurrentPopup();
            }
            if (chosen)
            {
                const Arcane::GraphNodeTypeInfo& info = *chosen;
                std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                Arcane::GraphNode n;
                n.id = g.MintId();
                n.type = info.type;
                if (info.type == Arcane::GraphNodeType::ConstColor ||
                    info.type == Arcane::GraphNodeType::ConstFloat4)
                {
                    n.value[0] = n.value[1] = n.value[2] = n.value[3] = 1.0f;
                }
                if (info.type == Arcane::GraphNodeType::Param ||
                    info.type == Arcane::GraphNodeType::TextureSample)
                {
                    // Unique default name ("Param3"/"Tex3") -- dup names are a
                    // shared decl, which a fresh node should not silently join.
                    const char* stem =
                        info.type == Arcane::GraphNodeType::Param ? "Param" : "Tex";
                    std::string name;
                    for (std::uint32_t k = n.id;; ++k)
                    {
                        name = stem + std::to_string(k);
                        bool taken = false;
                        for (const Arcane::GraphNode& other : g.nodes)
                            taken = taken || other.paramName == name;
                        if (!taken)
                            break;
                    }
                    n.paramName = name;
                    n.paramType = info.type == Arcane::GraphNodeType::Param
                                      ? Arcane::MatParamType::Float
                                      : Arcane::MatParamType::Texture;
                }
                if (info.type == Arcane::GraphNodeType::Custom)
                {
                    // Magenta until written -- the classic "custom shader
                    // pending" placeholder. Bodies can read params/Time
                    // directly (they land after the cbuffer declarations).
                    n.customBody = "return float4(1.0, 0.0, 1.0, 1.0);";
                    n.customOutWidth = 4;
                }
                if (info.type == Arcane::GraphNodeType::Comment)
                {
                    n.paramName = "Comment";
                    n.value[0] = 280.0f;   // starter box size
                    n.value[1] = 160.0f;
                    ed::SetGroupSize(n.id, ImVec2(n.value[0], n.value[1]));
                }
                const ImVec2 canvasPos =
                    ed::ScreenToCanvas(ImVec2(m_graphPopupX, m_graphPopupY));
                n.posX = canvasPos.x;
                n.posY = canvasPos.y;
                ed::SetNodePosition(n.id, canvasPos);
                const std::uint32_t newId = n.id;
                g.nodes.push_back(std::move(n));
                // Wire-invoked: auto-connect the first far-side pin. Input
                // drags replace silently, exactly like a hand-drawn wire; a
                // fresh node's single edge can never cycle.
                if (m_wireActive && g.FindNode(m_wireNode))
                {
                    Arcane::GraphLink l;
                    if (m_wireIsInput)
                    {
                        std::erase_if(g.links, [&](const Arcane::GraphLink& x)
                                      { return x.toNode == m_wireNode &&
                                               x.toPin == m_wirePin; });
                        l.fromNode = newId;
                        l.fromPin = 0;
                        l.toNode = m_wireNode;
                        l.toPin = m_wirePin;
                    }
                    else
                    {
                        l.fromNode = m_wireNode;
                        l.fromPin = m_wirePin;
                        l.toNode = newId;
                        l.toPin = 0;
                    }
                    g.links.push_back(l);
                }
                m_dirty = true;
                if (m_live)
                    RegenerateFromGraph();
                PushGraphUndo("Add Node", std::move(before));
            }
            ImGui::EndPopup();
        }
        else
            m_wireActive = false;   // popup closed without a pick
        ed::Resume();

        // Position readback (skipped the seeding frame -- the canvas would
        // report pre-seed positions). Pure moves dirty the asset, no recompile.
        if (!seededThisFrame)
        {
            for (Arcane::GraphNode& n : g.nodes)
            {
                const ImVec2 p = ed::GetNodePosition(ed::NodeId(n.id));
                if (p.x != n.posX || p.y != n.posY)
                {
                    n.posX = p.x;
                    n.posY = p.y;
                    m_dirty = true;
                }
            }
        }

        ed::End();

        // READ half, AFTER ed::End (drafting pick, 9.28 #20): the library runs
        // this frame's click/marquee selection actions inside End
        // (imgui_node_editor.cpp:1353) against the snapshot Begin took
        // (:1266-1269), and HasSelectionChanged compares the two (:1850-1853)
        // -- a read before End would see last frame's selection and never a
        // click. GetSelectedNodes caps at the buffer
        // (imgui_node_editor_api.cpp:22-36), so 2 means "two or more".
        {
            ed::NodeId selected[2];
            const int count = ed::GetSelectedNodes(selected, 2);
            const CanvasSelectionRead read = ReadCanvasSelection(
                count, count > 0 ? static_cast<std::uint32_t>(selected[0].Get()) : 0u,
                static_cast<std::size_t>(std::max(0, m_activePass)),
                ed::HasSelectionChanged(), m_nodeSelApplying);
            m_nodeSel = read.sel;
            if (read.event)
                ++m_pageSel.epoch;   // click, Ctrl-click, marquee, background clear, paste, the locator
            m_nodeSelApplying = false;
        }
        ed::SetCurrentEditor(nullptr);

        // The pin legend's PAINT, after ed::End: screen space, above every node.
        DrawGraphPinLegend(canvasMin, canvasSize, m_pinLegendHovered);
    }

    void ShaderEditorDocument::DrawGraphModals()
    {
        // Custom-node body editor: a MODAL in screen space -- the in-node
        // widget can only be a preview (child windows drift under the canvas
        // transform). Bound to m_bodyEditPass, so Apply writes the pass the
        // request named, not whatever the canvas shows. ONE undo step.
        if (m_bodyEditRequest != 0)
        {
            if (const Arcane::GraphNode* n = FindGraphNode(m_bodyEditPass, m_bodyEditRequest))
            {
                m_bodyEditNode = m_bodyEditRequest;
                std::snprintf(m_bodyBuf, sizeof(m_bodyBuf), "%s", n->customBody.c_str());
                ImGui::OpenPopup("Edit HLSL##graphbody");
            }
            m_bodyEditRequest = 0;
        }
        ImGui::SetNextWindowSize(ImVec2(560.0f, 380.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Edit HLSL##graphbody", nullptr))
        {
            ImGui::TextDisabled("Function body. Inputs arrive as the node's pins; params "
                                "and Time are directly visible. End with a return.");
            ImGui::InputTextMultiline("##bodyedit", m_bodyBuf, sizeof(m_bodyBuf),
                                      ImVec2(-1.0f, ImGui::GetContentRegionAvail().y - 34.0f),
                                      ImGuiInputTextFlags_AllowTabInput);
            if (ImGui::Button("Apply"))
            {
                if (Arcane::GraphNode* n = FindGraphNode(m_bodyEditPass, m_bodyEditNode);
                    n && n->customBody != m_bodyBuf)
                {
                    std::optional<Arcane::MaterialGraph> before = GraphOptAt(m_bodyEditPass);
                    n->customBody = m_bodyBuf;
                    m_dirty = true;
                    if (m_live)
                        RegenerateFromGraph();
                    PushGraphUndo("Edit HLSL Body", std::move(before), m_bodyEditPass);
                }
                m_bodyEditNode = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                m_bodyEditNode = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        // Assisted param rename: the consent modal (cross-FILE writes are not
        // undoable -- this gate is their structural-edit standing). Needs no
        // pass: BeginParamRename already walked every pass.
        if (m_renameRequest)
        {
            m_renameRequest = false;
            ImGui::OpenPopup("Rename Param Everywhere?##prename");
        }
        if (ImGui::BeginPopupModal("Rename Param Everywhere?##prename", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Renamed '%s' -> '%s'.", m_renameOld.c_str(), m_renameNew.c_str());
            ImGui::Text("%zu instance file(s) carry a saved value under the old name:",
                        m_renameTargets.size());
            const std::size_t listMax =
                static_cast<std::size_t>(Arcane::Settings<ShaderEditorSettings>().renameListMax);
            for (std::size_t i = 0; i < m_renameTargets.size() && i < listMax; ++i)
                ImGui::BulletText("%s", m_renameTargets[i].name.c_str());
            if (m_renameTargets.size() > listMax)
                ImGui::TextDisabled("...and %zu more", m_renameTargets.size() - listMax);
            ImGui::TextDisabled("Files that already have a '%s' value keep it; the "
                                "old entry drops.", m_renameNew.c_str());
            ImGui::Separator();
            if (ImGui::Button("Rename everywhere"))
            {
                for (const RenameTarget& t : m_renameTargets)
                {
                    auto data = Arcane::LoadMaterialAsset(t.path);
                    if (!data)
                    {
                        ARC_ERROR("param rename: '{}' failed to load -- skipped",
                                  t.path.generic_string());
                        continue;
                    }
                    RekeySavedParam(data->params, m_renameOld, m_renameNew);
                    if (!Arcane::SaveMaterialAsset(t.path, *data))
                    {
                        ARC_ERROR("param rename: '{}' failed to save -- skipped",
                                  t.path.generic_string());
                        continue;
                    }
                    if (m_services.onAssetSaved)
                        m_services.onAssetSaved(t.id);   // sprite-cache invalidate
                    if (m_services.onParamRenamed)
                        m_services.onParamRenamed(t.id, m_renameOld, m_renameNew);
                }
                m_renameTargets.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Just here"))
            {
                m_renameTargets.clear();   // today's behavior: the wart, chosen
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void ShaderEditorDocument::DrawGraphNode(Arcane::GraphNode& n, NodeLOD canvasLod)
    {
        if (n.type == Arcane::GraphNodeType::Comment)
        {
            // Comment/group box (UE comment, SG group): the editor's NATIVE
            // group node, so dragging the box carries contained nodes. The
            // size persists exactly: ed::Group records its bounds from the
            // Dummy it draws, so GetItemRectSize right after IS the live
            // (possibly user-resized) box.
            //
            // EXEMPT FROM THE LOD, and that is UE's rule, not a shortcut: a
            // comment's own bubble uses InvertLODCulling
            // (SGraphNodeComment.cpp:238), so SCommentBubble::IsBubbleVisible
            // shows it exactly when `CurrLOD <= MediumDetail`
            // (SCommentBubble.cpp:387-396) -- comment text is the thing UE
            // turns ON as you zoom out, because it is what you are navigating
            // BY. Degrading it here would delete the map.
            ed::BeginNode(ed::NodeId(n.id));
            ImGui::PushID(static_cast<int>(n.id));
            StableTextEdit("##ctitle", m_textEdit, TextKey(TextEditKind::Comment, n.id),
                           n.paramName, (std::max)(120.0f, n.value[0] - 16.0f),
                           [&](const char* text)
                           {
                               std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                               n.paramName = text;
                               m_dirty = true;   // annotation only -- no recompile
                               PushGraphUndo("Edit Comment", std::move(before));
                           });
            ed::Group(ImVec2((std::max)(80.0f, n.value[0]),
                             (std::max)(60.0f, n.value[1])));
            const ImVec2 gs = ImGui::GetItemRectSize();
            if (std::abs(gs.x - n.value[0]) > 0.5f || std::abs(gs.y - n.value[1]) > 0.5f)
            {
                n.value[0] = gs.x;
                n.value[1] = gs.y;
                m_dirty = true;   // resize has move standing (dirty, no undo step)
            }
            ImGui::PopID();
            ed::EndNode();
            return;
        }

        const Arcane::GraphNodeTypeInfo& info = Arcane::GraphNodeInfo(n.type);

        // ---- Effective tier for THIS node ----
        // UE's rename guard, ported onto the same risk it guards: a node whose
        // title is in edit mode refuses the low-detail swap
        // (SGraphNode.cpp:1596-1607 -- the `&& !InlineEditableText->IsInEditMode()`
        // term). It matters MORE here than it does in UE. StableTextEdit parks
        // the typed text in m_textEdit and only commits on a
        // deactivate-after-edit it can SEE (EditorWidgets.cpp:479-488); a field
        // that stops being submitted mid-edit never reports one, so the typed
        // text would be silently dropped when the node came back. Wheel-zoom
        // while a name field has focus is exactly that gesture.
        //
        // The KIND is checked, not just the id: m_textEdit is one buffer shared
        // with the pass canvas (ShaderEditorDocument.hpp:644-654), and
        // a PassName key carries a chain INDEX that can collide with a node id.
        //
        // Value drags need no guard -- an abandoned gesture is closed by
        // EditGesture's ScopeGuard, which pushes whatever the drag did
        // (EditGesture.cpp:90-97, armed at :1347 above).
        NodeLOD lod = canvasLod;
        if (lod < NodeLOD::DefaultDetail)
        {
            const std::uint64_t kindBits = m_textEdit.activeKey >> 56;
            const std::uint64_t idBits   = m_textEdit.activeKey & ((1ull << 56) - 1);
            if ((kindBits == static_cast<std::uint64_t>(TextEditKind::NodeName) ||
                 kindBits == static_cast<std::uint64_t>(TextEditKind::Swizzle)) &&
                idBits == n.id)
                lod = NodeLOD::DefaultDetail;
        }

        // The three degradation switches, in UE's own comparison form (the enum
        // ascends with detail, so a degradation is `lod <= Tier`). UE has only
        // two real thresholds across its whole graph editor -- `<= LowestDetail`
        // for structural widget swaps and `<= LowDetail` for text and small
        // controls -- and these are those two, plus one for the thumbnail:
        //
        //   showPinRows  (> LowestDetail)  pin rows exist at all. UE's harshest
        //       swap replaces a node's ENTIRE content area with a spacer at
        //       `<= LowestDetail` (SAnimationGraphNode.cpp:244-272); this is
        //       that, and the node reads as a labelled colored block.
        //   showPinText  (> LowDetail)     pin labels, inline pin literals and
        //       the per-type payload widgets. UE drops exactly this class at
        //       `<= LowDetail`: pin label + default-value widget, keeping the
        //       pin ICON (SGraphPin.cpp:354-364, :1463-1479); the "+ Add pin"
        //       button (SGraphNode.cpp:1681-1692); description text and badges
        //       (SGraphNodeAI.cpp:103-108).
        //
        // THE THUMBNAIL IS NO LONGER ONE OF THEM. It used to be gated at
        // `>= DefaultDetail`; that gate is gone, and what replaced it is the
        // off-screen cull above. See the note beside showPinText for why, and
        // NodeCulled for the mechanism -- both are now UE's arrangement rather
        // than an invention of ours.
        const bool showPinRows = lod > NodeLOD::LowestDetail;
        const bool showPinText = lod > NodeLOD::LowDetail;
        // NO ZOOM GATE ON THE PREVIEW, deliberately, and this is the one place
        // the port now MATCHES UE rather than merely citing it. UE's material
        // expression preview has no LOD term at all: its visibility is
        // bHidePreviewWindow/bCollapsed and nothing else
        // (SGraphNodeMaterialBase.cpp:695-700), and what bounds its cost is
        // off-screen CULLING -- the preview's GPU work is issued from
        // OnDrawViewport (:167-191), so an unpainted node submits no draw
        // element. We had invented a `lod >= DefaultDetail` gate with no UE
        // counterpart, and it was strictly more aggressive than anything UE
        // does: previews died at 0.675 while UE still renders them at 0.10.
        // Now culling is the bound, exactly as upstream, and a visible node
        // shows its thumbnail at every zoom.
        //
        // The tiers stay for what UE actually spends them on -- titles, pin
        // text and chrome (see the consumer list on NodeLOD).

        // The node's measured width from the LAST frame (see m_nodeWidths):
        // output rows right-align to it and the preview spans it. Zero on a
        // node's first frame, which both consumers treat as "no alignment".
        const auto widthIt = m_nodeWidths.find(n.id);
        const float contentW = widthIt == m_nodeWidths.end()
                                   ? 0.0f
                                   : widthIt->second - 2.0f * NodePadX();

        // ---- OFF-SCREEN CULL (UE's mechanism, ported) ----
        // The node is still SUBMITTED -- BeginNode/EndNode, and every pin --
        // because ed::Link refuses a link whose endpoint pin was not live this
        // frame (DoLink bails at imgui_node_editor.cpp:1639-1640, m_IsLive set
        // only by BeginPin at :5366), and because the editor's selection,
        // hit-testing and framing all read the node's rect. What is skipped is
        // the CONTENT: no title, no band, no pin rows, no widgets, no preview.
        //
        // The stand-in reproduces the node's LAST MEASURED SIZE rather than
        // collapsing to nothing. That is not cosmetic: F/frame-all computes
        // content bounds from live node rects (NavigateToContent ->
        // GetContentBounds), so a zero-size stand-in would shrink the framing
        // rect and pull the view onto whatever happens to be on screen. Marquee
        // select reads the same rects, so preserving them also keeps
        // box-selecting across off-screen nodes working. Reproducing the size
        // makes it a fixed point too -- the stand-in measures back to the number
        // it was built from, so a culled node's footprint never drifts.
        if (NodeCulled(n.id))
        {
            const ImVec2 size = ed::GetNodeSize(ed::NodeId(n.id));
            ed::BeginNode(ed::NodeId(n.id));
            ImGui::PushID(static_cast<int>(n.id));
            const float startY = ImGui::GetCursorPosY();
            for (std::uint32_t pin = 0; pin < Arcane::GraphNodeInputCount(n); ++pin)
            {
                ed::BeginPin(InPin(n.id, pin), ed::PinKind::Input);
                SetPinPivot(InPin(n.id, pin).Get(), ImGui::GetCursorScreenPos());
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ed::EndPin();
                ImGui::SameLine(0.0f, 0.0f);
            }
            for (std::uint32_t pin = 0; pin < Arcane::GraphNodeOutputCount(n); ++pin)
            {
                ed::BeginPin(OutPin(n.id, pin), ed::PinKind::Output);
                SetPinPivot(OutPin(n.id, pin).Get(), ImGui::GetCursorScreenPos());
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ed::EndPin();
                ImGui::SameLine(0.0f, 0.0f);
            }
            ImGui::Dummy(ImVec2(0.0f, 0.0f));   // terminate the SameLine chain
            // Pad out to the remembered footprint (node size minus the padding
            // the editor adds back around the content).
            const float usedY = ImGui::GetCursorPosY() - startY;
            const float wantY = size.y - 2.0f * NodePadY();
            ImGui::Dummy(ImVec2((std::max)(0.0f, size.x - 2.0f * NodePadX()),
                                (std::max)(0.0f, wantY - usedY)));
            ImGui::PopID();
            ed::EndNode();
            m_culledGraphNodes.insert(n.id);
            return;
        }

        ed::BeginNode(ed::NodeId(n.id));
        ImGui::PushID(static_cast<int>(n.id));

        // Title row. The BAND behind it is a rectangle drawn after ed::EndNode
        // (it spans the node's final width, which does not exist yet); this is
        // only the text, and headerMaxY is the band's bottom edge.
        //
        // NOT LOD-GATED, at either end. The title is the last thing a block
        // has left to be identified by, and the "(!)" prefix is the ERROR
        // BADGE -- the thing you zoom out to FIND. UE never LODs its error
        // reporting out either: SetupErrorReporting's widgets go into the node
        // unconditionally (SGraphNode.cpp:1001-1013), with no
        // SLevelOfDetailBranchNode around them and no LOD term in their
        // visibility, and the same holds for the panel's overlay badges
        // (SGraphPanel.cpp:414-466). (UE does swap the title itself for a flat
        // colored border at <= LowestDetail, SGraphNode.cpp:941-953 -- but its
        // low-detail node still has a body and pin icons to be read by. Ours
        // collapses to the band, so the band has to carry the identity.)
        if (NodeBadged(n.id))
            ImGui::TextColored(NodeBadgeText(), "(!) %s", info.display);
        else
            ImGui::TextColored(NodeTitleText(), "%s", info.display);
        const float headerMaxY = ImGui::GetItemRectMax().y;

        // Reserve the gap under the band (NodeHeaderGap()). Solved rather than
        // guessed, because ImGui's automatic spacing is already in play at both
        // ends of the dummy: the next real item lands at
        // headerMaxY + 2*ItemSpacing.y + fill, and it needs to land at the
        // band's bottom edge (headerMaxY + NodePadY()) plus the gap.
        //
        // Clamped at zero: a theme with generous ItemSpacing may already place
        // the row far enough down, and a negative dummy would be nonsense.
        //
        // GATED ON showPinRows, i.e. skipped at LowestDetail. There the band IS
        // the node -- the pin rows are gone and the node's whole height is the
        // header -- so reserving body space below it would open an empty strip
        // under the bar with nothing to put in it, and would inflate the block's
        // height for no reading. The band-only tier wants no gap at all.
        if (showPinRows)
        {
            const float fill = (NodePadY() + NodeHeaderGap()) -
                               2.0f * ImGui::GetStyle().ItemSpacing.y;
            if (fill > 0.0f)
                ImGui::Dummy(ImVec2(0.0f, fill));
        }

        // Gesture helpers (used by pin rows AND payload widgets below). Value
        // drags bracket a whole-graph gesture through EditGesture (before on
        // activation, one undo step at close); a popup-widget has to leave the
        // canvas's transformed space first (CanvasPopupScope), so node TYPES use
        // cycle buttons rather than combos. The label rides the OPEN call
        // because the transaction carries it -- CommandStack::Commit stamps the
        // step with Begin's label, not the pushed command's.
        //
        // ONE builder serves BOTH boundaries below: a value drag closes on
        // widget deactivation and the ConstColor popup closes on the popup going
        // away, but the step they owe the stack is identical. Neither may park
        // an EMPTY builder: this document is not registry-backed, so nothing
        // snapshots into the transaction and Commit's empty-transaction drop
        // (CommandStack.cpp:61-62) would swallow the whole thing -- the edit
        // applied live and recorded nowhere.
        auto buildGraphEdit = [&](const char* label) -> std::function<void()>
        {
            // GraphEditBuilder pins the pass AND `before` at activation and
            // skips the push when the close finds nothing changed (the
            // abandonment paths: stale-close, collapsed window, teardown).
            return GraphEditBuilder(label, static_cast<std::size_t>((std::max)(0, m_activePass)));
        };
        auto gestureBegin = [&](const char* label)
        {
            EditGesture::BeginOnActivate(UndoStack(), m_gesture,
                [&] { return std::string(label); },
                [&] { return buildGraphEdit(label); });
        };
        // The close is EditGesture::EndAfterRow at each drag site (not a lambda):
        // an Esc revert (CanvasDragEscape) closes as cancelled, at the row.
        // The popup pair, keyed on a popup id instead of the last submitted
        // item -- a hand-rolled popup's edits come from FOREIGN widgets, so
        // IsItemActivated() never fires for it (EditGesture.hpp:121-133).
        auto popupGestureBegin = [&](const char* label, std::uint32_t popupId)
        {
            EditGesture::BeginOnPopupOpen(UndoStack(), m_gesture, popupId,
                                          [&] { return std::string(label); },
                                          [&] { return buildGraphEdit(label); });
        };
        auto popupGestureEnd = [&](std::uint32_t popupId)
        {
            EditGesture::EndOnPopupClose(UndoStack(), m_gesture, popupId);
        };
        auto valueEdited = [&] { NoteGraphValueEdited(); };

        // An inline literal is hidden while a wire feeds the pin. One edge per
        // input is a canvas invariant (HandleGraphEdits replaces silently), so
        // a single scan answers it.
        const Arcane::MaterialGraph& graph = *ActiveGraphOpt();
        auto pinWired = [&](std::uint32_t pin)
        {
            for (const Arcane::GraphLink& l : graph.links)
                if (l.toNode == n.id && l.toPin == pin)
                    return true;
            return false;
        };
        // Output side of the same question: an output FANS OUT, so any edge
        // leaving it counts.
        auto pinFanout = [&](std::uint32_t pin)
        {
            for (const Arcane::GraphLink& l : graph.links)
                if (l.fromNode == n.id && l.fromPin == pin)
                    return true;
            return false;
        };

        // ================= LowestDetail: the node as a block =================
        // THE CONSTRAINT THAT SHAPES THIS: every pin must still be SUBMITTED,
        // or the wires touching it disappear. ed::Link is refused outright
        // unless BOTH endpoints are live this frame -- DoLink returns false at
        // imgui_node_editor.cpp:1639-1640 -- and m_IsLive is set only by
        // BeginPin (:5366). So dropping the pin rows cannot mean dropping the
        // pins. UE has the same rule for free (its wires are drawn by the panel
        // from the graph's own connectivity, with no LOD gate anywhere in
        // SGraphPanel's connection block or ConnectionDrawingPolicy) and its
        // nodes keep drawing pin ICONS even at LowestDetail; we get there by
        // submitting each pin as a zero-size anchor instead.
        //
        // Geometry: all inputs collapse to one point at the content's left
        // edge, all outputs to one point at its right edge, on a single
        // zero-height row under the title. The row is spaced out to the node's
        // LAST MEASURED width, so the block keeps the footprint the node had at
        // full detail instead of shrink-snapping to its title -- UE preserves
        // size across every one of its swaps, three different ways, and says so
        // (SGraphNode.cpp:947 "Saving enough space for a 'typical' title so the
        // transition isn't quite so abrupt"; SAnimationGraphNode's cached
        // GetLowDetailDesiredSize is the same idea done properly). That is also
        // what makes the width a fixed point: the row reproduces contentW, so
        // the block re-measures to the same number every frame.
        //
        // INTERACTION: a zero-size pin has a zero-size hot zone, so the pins
        // stop being grabbable -- which is the intent behind UE turning pins
        // HitTestInvisible at low LOD ("The pin becomes too small to use at low
        // LOD, so disable the hit test", SGraphPin.cpp:1481-1489). Selecting,
        // hovering, dragging and the node context menu all come off the NODE's
        // bounds and keep working. Nothing invisible is left behind: hidden
        // widgets are not submitted at all, so there are no dead hit zones.
        if (!showPinRows)
        {
            for (std::uint32_t pin = 0; pin < Arcane::GraphNodeInputCount(n); ++pin)
            {
                ed::BeginPin(InPin(n.id, pin), ed::PinKind::Input);
                // Zero-size anchor: the pivot the alignment path would have
                // produced from a zero-size rect IS the cursor, so naming it
                // outright changes no geometry and gives the gradient wires an
                // anchor at the one tier that has no dot to hang off.
                SetPinPivot(InPin(n.id, pin).Get(), ImGui::GetCursorScreenPos());
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ed::EndPin();
                ImGui::SameLine(0.0f, 0.0f);
            }
            RightAlignRow(contentW, 0.0f);
            for (std::uint32_t pin = 0; pin < Arcane::GraphNodeOutputCount(n); ++pin)
            {
                ed::BeginPin(OutPin(n.id, pin), ed::PinKind::Output);
                SetPinPivot(OutPin(n.id, pin).Get(), ImGui::GetCursorScreenPos());
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ed::EndPin();
                ImGui::SameLine(0.0f, 0.0f);
            }
            ImGui::Dummy(ImVec2(0.0f, 0.0f));   // terminate the SameLine chain
        }

        for (std::uint32_t pin = 0; showPinRows && pin < Arcane::GraphNodeInputCount(n); ++pin)
        {
            // Read the descriptor BEFORE any of the widgets below can mutate
            // the node: for a Custom pin, GraphPinDesc::name points into
            // n.customPins (MaterialGraph.hpp:376-377), so it must be consumed
            // ahead of the remove/rename controls.
            const Arcane::GraphPinDesc inDesc = Arcane::GraphNodeInputPin(n, pin);
            ed::BeginPin(InPin(n.id, pin), ed::PinKind::Input);
            // Wires land on the dot, not on the row's bounding corner: pivot at
            // the row's left edge, vertically centred, with a zero-size pivot
            // so the anchor is that single point.
            // LowDetail keeps the DOT and drops the label -- UE's split
            // exactly: the low-detail slot of a pin holds PinWidgetRef, the pin
            // icon, and drops PinContent, the label and value editor
            // (SGraphPin.cpp:354-364). The dot advances the cursor by a full
            // text line either way (DrawPinDot), so the row keeps its height
            // and the node keeps its shape across the transition.
            const ImVec2 inDot = DrawPinDot(GraphPinPaintOn(n, pin, /*input*/ true), pinWired(pin));
            // One radius OUTBOARD of the dot's centre -- the row's left edge,
            // which is exactly where the (0, 0.5) alignment used to put the
            // pivot: the dot is the row's first item, so pinRect.Min.x is its
            // left edge and the row's vertical centre is the dot's centre
            // (the dot's dummy is the full text line height). Same point as
            // before, now stated instead of inferred.
            SetPinPivot(InPin(n.id, pin).Get(),
                        ImVec2(inDot.x - PinDotRadius(), inDot.y));
            if (showPinText)
            {
                ImGui::SameLine();
                ImGui::TextUnformatted(inDesc.name);
            }
            ed::EndPin();
            // EndPin closes the pin's group: the item is the dot + its label.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                m_pinTip = { n.id, pin, true, true };
            // Custom pins are user-authored: width cycle + remove beside each.
            // Small per-pin controls, so they go with the labels -- UE collapses
            // its "+ Add pin" button at the same threshold
            // (SGraphNode.cpp:1681-1692).
            if (showPinText && n.type == Arcane::GraphNodeType::Custom)
            {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(pin));
                Arcane::GraphCustomPin& cp = n.customPins[pin];
                const char* wname = cp.width == 1 ? "f1" : cp.width == 2 ? "f2" : "f4";
                if (ImGui::SmallButton(wname))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    cp.width = cp.width == 1 ? 2 : cp.width == 2 ? 4 : 1;
                    valueEdited();
                    PushGraphUndo("Pin Width", std::move(before));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    // One step; drops the pin's links + literal and re-indexes
                    // the later pins' (the node page calls the same member).
                    (void)RemoveCustomPin(static_cast<std::size_t>((std::max)(0, m_activePass)), n.id, pin);
                    ImGui::PopID();
                    break;   // pin list changed under this loop -- redraw next frame
                }
                ImGui::PopID();
            }

            // Inline literal on an UNWIRED input pin (SG/UE parity: a pin
            // carries a value with no Const node feeding it). Codegen's argOr
            // checks `connected` FIRST (MaterialGraph.cpp:694-701), so a wire
            // hides the literal without destroying it -- which is why this
            // widget only has to disappear, never clear anything.
            // It is also the pin's default-VALUE widget, which is the other
            // half of what UE's low-detail pin slot drops (SGraphPin.cpp:304-341
            // builds LabelAndValue; :354-364 swaps the whole thing out).
            if (showPinText && !pinWired(pin) && Arcane::GraphPinAcceptsLiteral(n, pin))
            {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(pin));
                const int lanes =
                    Arcane::GraphPinLiteralLanes(Arcane::GraphNodeInputPin(n, pin).width);
                float buf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                const Arcane::GraphPinLiteral* lit = n.FindPinLiteral(pin);
                // A non-constant neutral (Panner's v.uv) prints as ITSELF: a
                // format string carrying no conversion is explicitly tolerated
                // by ImGui -- RoundScalarWithFormatT returns the value
                // untouched when "the value is not visible in the format
                // string" (ThirdParty/imgui/imgui_widgets.cpp:2496).
                const char* fmt = "%.3f";
                if (lit)
                    std::memcpy(buf, lit->v, sizeof(buf));
                else
                {
                    const Arcane::GraphPinNeutral nd = Arcane::GraphPinNeutralDefault(n, pin);
                    SeedPinNeutral(nd, lanes, buf);
                    if (nd.kind == Arcane::GraphPinNeutralKind::Expression)
                        fmt = nd.hlsl;
                }
                ImGui::SetNextItemWidth(GraphPinNeutralWidth(Arcane::Settings<GraphCanvasSettings>(), lanes));
                float pre[4];
                std::memcpy(pre, buf, sizeof(pre));
                const bool litExisted = lit != nullptr;   // `lit` may dangle once SetPinLiteral runs
                const bool changed =
                    lanes == 1 ? ImGui::DragFloat("##lit", buf, NodeDragSpeed(), 0.0f, 0.0f, fmt)
                    : lanes == 2 ? ImGui::DragFloat2("##lit", buf, NodeDragSpeed(), 0.0f, 0.0f, fmt)
                                 : ImGui::DragFloat4("##lit", buf, NodeDragSpeed(), 0.0f, 0.0f, fmt);
                bool existed = true;
                const bool escaped = CanvasDragEscape(pre, litExisted, buf, lanes, &existed);
                // Same bracketing as the Const payload drags below, and STRICTLY
                // safer: the drag wrote `buf`, not the graph, so the snapshot
                // this takes on the activation frame is always pre-edit.
                gestureBegin("Pin Value");
                if (escaped)
                {
                    // Back to the drag's start: an absent-until-touched literal
                    // is absent again, so the graph equals the gesture's before.
                    if (existed)
                        SetPinLiteral(n, pin, lanes, buf);
                    else
                        ErasePinLiteral(n, pin);
                    valueEdited();
                }
                else if (changed)
                {
                    // Absent-until-touched: a pin's first edit CREATES its entry
                    // from what the field was already showing (`buf` was seeded
                    // with the neutral), so the first nudge moves the material
                    // by one drag step instead of jumping to zero.
                    SetPinLiteral(n, pin, lanes, buf);
                    valueEdited();
                }
                EditGesture::EndAfterRow(UndoStack(), m_gesture, escaped);
                ImGui::PopID();
            }
        }

        // Per-type payload: value drags, cycle buttons, name fields, the Custom
        // node's HLSL preview. All of it is text and small controls, so it goes
        // at the same threshold as the pin labels -- UE drops its whole
        // equivalent band there (sequence-player scrub slider ->  16x16 spacer,
        // SGraphNodeSequencePlayer.cpp:159-180; anim function/tag chips,
        // SAnimationGraphNode.cpp:209-212; AI node description text,
        // SGraphNodeAI.cpp:103-108). A node being renamed has already been
        // pulled back to DefaultDetail above, so nothing is yanked mid-edit.
        //
        // Skipped by dispatching to a value with no enumerator rather than by
        // wrapping 250 lines in an `if` -- GraphNodeType has a fixed uint8_t
        // base (MaterialGraph.hpp:43), so 0xFF is a valid VALUE that no case
        // labels and every case list that grows will keep not labelling. It can
        // only reach `default:`.
        switch (showPinText ? n.type : static_cast<Arcane::GraphNodeType>(0xFF))
        {
            case Arcane::GraphNodeType::ConstFloat:
            {
                ImGui::SetNextItemWidth(Arcane::Settings<GraphCanvasSettings>().constFloatWidth);
                float pre[4];
                std::memcpy(pre, n.value, sizeof(pre));
                const bool changed = ImGui::DragFloat("##v", &n.value[0], NodeDragSpeed());
                const bool escaped = CanvasDragEscape(pre, true, n.value, 1);
                gestureBegin("Edit Value");
                if (changed || escaped) valueEdited();
                EditGesture::EndAfterRow(UndoStack(), m_gesture, escaped);
                break;
            }
            case Arcane::GraphNodeType::ConstFloat2:
            {
                ImGui::SetNextItemWidth(Arcane::Settings<GraphCanvasSettings>().constFloat2Width);
                float pre[4];
                std::memcpy(pre, n.value, sizeof(pre));
                const bool changed = ImGui::DragFloat2("##v", n.value, NodeDragSpeed());
                const bool escaped = CanvasDragEscape(pre, true, n.value, 2);
                gestureBegin("Edit Value");
                if (changed || escaped) valueEdited();
                EditGesture::EndAfterRow(UndoStack(), m_gesture, escaped);
                break;
            }
            case Arcane::GraphNodeType::ConstFloat4:
            case Arcane::GraphNodeType::ConstColor:
            {
                ImGui::SetNextItemWidth(Arcane::Settings<GraphCanvasSettings>().constFloat4Width);
                float pre[4];
                std::memcpy(pre, n.value, sizeof(pre));
                const bool changed = ImGui::DragFloat4("##v", n.value, NodeDragSpeed());
                const bool escaped = CanvasDragEscape(pre, true, n.value, 4);
                gestureBegin("Edit Value");
                if (changed || escaped) valueEdited();
                EditGesture::EndAfterRow(UndoStack(), m_gesture, escaped);
                if (n.type == Arcane::GraphNodeType::ConstColor)
                {
                    ImGui::SameLine();
                    // hdr = true: a ConstColor feeds raw shader maths and may exceed
                    // 1, where an sRGB encode is meaningless and a hex is a clamped
                    // lie -- so the popup shows linear floats and no hex rows. The
                    // swatch is still ENCODED, because a swatch is a display object
                    // whatever the value feeds. The SV response is poor for HDR
                    // values; inherent, and equally true in UE.
                    //
                    // The swatch is submitted in CANVAS space, outside the scope
                    // below. ed::Suspend leaves the canvas's transformed space, and
                    // Canvas::LeaveLocalSpace only transforms the vertices recorded
                    // inside it -- so a swatch drawn while suspended would render
                    // offset by the canvas position and pan, and unscaled by zoom.
                    // Only the popup has to leave, and the other two CanvasPopupScope
                    // uses in this file wrap exactly that much.
                    const bool swatchClicked =
                        ColorSwatchButton("##swatch", n.value, ImVec2(18, 18));
                    {
                        const CanvasPopupScope canvasPopup;   // popups live in screen space
                        // The id is computed INSIDE the scope and used for every
                        // id-consuming call here, so open/draw/gesture cannot disagree.
                        const ImGuiID popupId = ColorPopupId("##constcolorpopup");
                        if (swatchClicked)
                        {
                            std::memcpy(m_colorPopupOriginal, n.value,
                                        sizeof(m_colorPopupOriginal));
                            ImGui::OpenPopup(popupId);
                        }
                        popupGestureBegin("Edit Color", popupId);
                        if (ImGui::BeginPopup("##constcolorpopup"))
                        {
                            if (ColorPopupBody(n.value, m_colorPopupOriginal, /*hdr*/ true))
                                valueEdited();
                            ImGui::EndPopup();
                        }
                        popupGestureEnd(popupId);
                    }
                }
                break;
            }
            case Arcane::GraphNodeType::Param:
            case Arcane::GraphNodeType::TextureSample:
            {
                // Name: StableTextEdit holds the typed text while the InputText
                // is active; committed as ONE undoable edit on deactivate-
                // after-edit.
                StableTextEdit("##pname", m_textEdit,
                               TextKey(TextEditKind::NodeName, n.id),
                               n.paramName, Arcane::Settings<GraphCanvasSettings>().paramNameFieldWidth,
                               [&](const char* text)
                               {
                                   std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                                   const std::string oldName = n.paramName;
                                   const std::string newName = text;
                                   n.paramName = newName;
                                   valueEdited();
                                   PushGraphUndo("Rename Param", std::move(before));
                                   // Assisted rename: local override fix + the
                                   // dependent-instance walk (arms the
                                   // propagation modal on hits). Both names are
                                   // independent COPIES: BeginParamRename takes
                                   // const refs and re-scans every graph's nodes
                                   // for the old name -- THIS node included --
                                   // so neither argument should alias the node
                                   // it is reasoning about. (The pre-widget-
                                   // layer code passed m_nameBuf for the same
                                   // reason.)
                                   BeginParamRename(oldName, newName);
                               });

                if (n.type == Arcane::GraphNodeType::Param)
                {
                    // Type cycle button (popup-free combo stand-in).
                    static constexpr const char* kTypeNames[] = { "float", "float2",
                                                                  "float4", "color" };
                    static constexpr Arcane::MatParamType kTypes[] = {
                        Arcane::MatParamType::Float, Arcane::MatParamType::Float2,
                        Arcane::MatParamType::Float4, Arcane::MatParamType::Color,
                    };
                    int typeIdx = 0;
                    for (int t = 0; t < 4; ++t)
                        if (kTypes[t] == n.paramType)
                            typeIdx = t;
                    if (ImGui::SmallButton(kTypeNames[typeIdx]))
                    {
                        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                        n.paramType = kTypes[(typeIdx + 1) % 4];
                        n.paramDefault.type = n.paramType;
                        valueEdited();
                        PushGraphUndo("Param Type", std::move(before));
                    }

                    // Default value at the decl's width.
                    const int lanes =
                        static_cast<int>(Arcane::ComponentCount(n.paramType));
                    ImGui::SetNextItemWidth(GraphConstValueWidth(Arcane::Settings<GraphCanvasSettings>(), lanes));
                    bool changed = false;
                    float pre[4];
                    std::memcpy(pre, n.paramDefault.f, sizeof(pre));
                    if (lanes == 1)
                        changed = ImGui::DragFloat("##pdef", &n.paramDefault.f[0], NodeDragSpeed());
                    else if (lanes == 2)
                        changed = ImGui::DragFloat2("##pdef", n.paramDefault.f, NodeDragSpeed());
                    else
                        changed = ImGui::DragFloat4("##pdef", n.paramDefault.f, NodeDragSpeed());
                    const bool escaped = CanvasDragEscape(pre, true, n.paramDefault.f, lanes);
                    gestureBegin("Param Default");
                    if (changed || escaped) valueEdited();
                    EditGesture::EndAfterRow(UndoStack(), m_gesture, escaped);

                    bool ranged = n.hasRange;
                    if (ImGui::Checkbox("range", &ranged))
                    {
                        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                        n.hasRange = ranged;
                        valueEdited();
                        PushGraphUndo("Param Range", std::move(before));
                    }
                    if (n.hasRange)
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(Arcane::Settings<GraphCanvasSettings>().constParamRangeWidth);
                        float mm[2] = { n.rangeMin, n.rangeMax };
                        const float pre[4] = { mm[0], mm[1], 0.0f, 0.0f };
                        const bool rchanged = ImGui::DragFloat2(
                            "##prange", mm, Arcane::Settings<GraphCanvasSettings>().rangeDragSpeed);
                        const bool escaped = CanvasDragEscape(pre, true, mm, 2);
                        gestureBegin("Param Range");
                        if (rchanged || escaped)
                        {
                            n.rangeMin = mm[0];
                            n.rangeMax = mm[1];
                            valueEdited();
                        }
                        EditGesture::EndAfterRow(UndoStack(), m_gesture, escaped);
                    }
                }
                break;
            }
            case Arcane::GraphNodeType::PassInput:
            {
                // Which wired slot to sample: cycle button (validity against
                // the pass's actual wiring is codegen's job -- the badge says
                // when a slot is not wired).
                const char* slotName[4] = { "in0", "in1", "in2", "in3" };
                if (ImGui::SmallButton(slotName[n.passInputSlot %
                                                Arcane::kMaxPassInputs]))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    n.passInputSlot = (n.passInputSlot + 1) % Arcane::kMaxPassInputs;
                    valueEdited();
                    PushGraphUndo("Input Slot", std::move(before));
                }
                break;
            }
            case Arcane::GraphNodeType::Panner:
            {
                // UE's bFractionalPart: wrap the Time*speed offset in frac().
                // A checkbox is a DISCRETE edit -- one click IS the whole
                // change -- so it takes this file's discrete-edit shape
                // (snapshot inline, mutate, push immediately), copied from the
                // "range" checkbox in the Param case above, which is the same
                // widget doing the same job. The gestureBegin/EndAfterRow
                // bracket beside it exists to coalesce a MULTI-FRAME drag into
                // one undo step; a click has nothing to coalesce, and routing
                // it through the bracket would push the step a frame late for
                // no benefit. Reading the flip through a local `frac` keeps the
                // graph unmutated until after the snapshot is taken.
                bool frac = n.pannerFractional;
                if (ImGui::Checkbox("frac", &frac))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    n.pannerFractional = frac;
                    valueEdited();
                    PushGraphUndo("Panner Fraction", std::move(before));
                }
                break;
            }
            case Arcane::GraphNodeType::Swizzle:
            {
                // Mask edit: same StableTextEdit commit as param names (one
                // shared TextCommitState -- only one InputText is active at a
                // time; the keys are namespaced per site kind).
                StableTextEdit("##mask", m_textEdit, TextKey(TextEditKind::Swizzle, n.id),
                               n.swizzleMask, Arcane::Settings<GraphCanvasSettings>().swizzleFieldWidth,
                               [&](const char* text)
                               {
                                   std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                                   n.swizzleMask = text;
                                   valueEdited();
                                   PushGraphUndo("Edit Swizzle", std::move(before));
                               });
                break;
            }
            case Arcane::GraphNodeType::Custom:
            {
                // Add-pin + output width; the pin rows above carry the per-pin
                // width/remove controls.
                if (ImGui::SmallButton("+ pin"))
                    (void)AddCustomPin(static_cast<std::size_t>((std::max)(0, m_activePass)), n.id);
                ImGui::SameLine();
                const char* ow = n.customOutWidth == 1 ? "out: f1"
                                : n.customOutWidth == 2 ? "out: f2" : "out: f4";
                if (ImGui::SmallButton(ow))
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    n.customOutWidth = n.customOutWidth == 1 ? 2
                                       : n.customOutWidth == 2 ? 4 : 1;
                    valueEdited();
                    PushGraphUndo("Output Width", std::move(before));
                }

                // Body PREVIEW only, as plain draw-list text (an in-node
                // InputTextMultiline is a CHILD WINDOW -- it doesn't ride the
                // canvas transform, so its text drifts while the node drags and
                // ignores zoom). Editing happens in a Suspend'ed popup (normal
                // ImGui space), opened by the button below.
                {
                    const ShaderEditorSettings& es = Arcane::Settings<ShaderEditorSettings>();
                    const CustomBodyPreview preview = BuildCustomBodyPreview(
                        n.customBody, es.bodyPreviewLines, static_cast<std::size_t>(es.bodyPreviewChars));
                    for (const std::string& line : preview.lines)
                        ImGui::TextDisabled("%s", line.c_str());
                    if (preview.truncated)
                        ImGui::TextDisabled("...");
                }
                if (ImGui::SmallButton("Edit HLSL..."))
                    RequestBodyEdit(static_cast<std::size_t>(std::max(0, m_activePass)), n.id);
                break;
            }
            default:
                break;
        }

        for (std::uint32_t pin = 0; showPinRows && pin < Arcane::GraphNodeOutputCount(n); ++pin)
        {
            const Arcane::GraphPinDesc outDesc = Arcane::GraphNodeOutputPin(n, pin);
            // Label dropped with the input labels; the row still measures to the
            // dot, so the dot stays welded to the node's right edge either way.
            const float rowW = showPinText
                                   ? ImGui::CalcTextSize(outDesc.name).x +
                                         ImGui::GetStyle().ItemSpacing.x +
                                         PinDotRadius() * 2.0f
                                   : PinDotRadius() * 2.0f;
            RightAlignRow(contentW, rowW);
            ed::BeginPin(OutPin(n.id, pin), ed::PinKind::Output);
            if (showPinText)
            {
                ImGui::TextUnformatted(outDesc.name);
                ImGui::SameLine();
            }
            const ImVec2 outDot = DrawPinDot(GraphPinPaintOn(n, pin, /*input*/ false), pinFanout(pin));
            // Mirror of the input row: the dot is the row's LAST item, so the
            // (1, 0.5) alignment's pinRect.Max.x was the dot's right edge.
            SetPinPivot(OutPin(n.id, pin).Get(),
                        ImVec2(outDot.x + PinDotRadius(), outDot.y));
            ed::EndPin();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                m_pinTip = { n.id, pin, false, true };
        }

        // Unconditional: a node that got here is on screen, and on-screen
        // nodes show their thumbnail at every zoom (see the no-zoom-gate note).
        DrawNodePreviewImage(n, contentW);

        ImGui::PopID();
        ed::EndNode();

        // TITLE BAND + width measurement, both of which need the node's final
        // laid-out rect and so can only happen here. GetNodeBackgroundDrawList
        // paints into the node's own user-background channel -- above its body
        // fill, below its content and pin chrome
        // (imgui_node_editor.cpp:135-140) -- which is exactly where a header
        // band belongs. Coordinates are canvas space, the space both
        // GetNodePosition and plain ImGui use inside ed::Begin/End.
        //
        // Runs at EVERY tier, unguarded, and both halves want it to. The band
        // is what a LowestDetail node IS -- with the pin rows gone the node's
        // whole height is the header, so the band fills it and the block reads
        // as one colored bar with a title. And the measurement is what makes
        // the block keep its full-detail WIDTH: the anchor row above reproduces
        // contentW, so nodeSize.x comes back the same number it went in as, and
        // the cache neither drifts nor forgets across a zoom-out/zoom-in round
        // trip. (The one case it does move is a node born while zoomed out --
        // there is no earlier width to remember, so the block sizes to its
        // title and re-measures on the way back in.)
        //
        // The band/gap relationship, and why headerMaxY is what it is, lives on
        // DrawNodeTitleBand -- shared with the pass canvas.
        // The band takes the node's CATEGORY colour (s5.1.4); at LowestDetail it is
        // the whole node, so the category stays legible zoomed out. The pass canvas
        // (three calls above) and comment boxes (returned early) keep the neutral.
        const ImVec2 nodeSize = DrawNodeTitleBand(n.id, headerMaxY, GraphCategoryHeaderColor(info.category));
        if (nodeSize.x > 0.0f)
            m_nodeWidths[n.id] = nodeSize.x;
    }

    bool ShaderEditorDocument::NodeCulled(std::uint32_t nodeId) const
    {
        if (!m_cullRectValid)
            return false;
        // A node that has never been laid out has no size to test, and guessing
        // one would be worse than drawing it: draw it once, measure it, cull it
        // from the next frame on. UE has the same first-frame exemption by
        // construction -- GetDesiredSize is zero until Slate has arranged the
        // widget once.
        const ImVec2 size = ed::GetNodeSize(ed::NodeId(nodeId));
        if (size.x <= 0.0f || size.y <= 0.0f)
            return false;
        const ImVec2 pos = ed::GetNodePosition(ed::NodeId(nodeId));
        // Rectangle overlap, exactly SNodePanel::IsNodeCulled's four tests
        // (SNodePanel.cpp:1591-1597).
        return pos.x + size.x < m_cullMin.x ||
               pos.y + size.y < m_cullMin.y ||
               pos.x > m_cullMax.x ||
               pos.y > m_cullMax.y;
    }

    void ShaderEditorDocument::DrawPassWire(std::uint64_t linkId,
                                            std::uint64_t fromPinId,
                                            std::uint64_t toPinId)
    {
        const ed::LinkId id(linkId);
        const ed::PinId  fromPin(fromPinId);
        const ed::PinId  toPin(toPinId);
        // Same two-layer treatment the material graph uses: a TRANSPARENT
        // ed::Link purely for interaction (it still hit-tests at the real
        // thickness, but tessellates nothing), then our own curve on top.
        // Every pass wire carries the same thing, so both ends are the one
        // texture colour -- the gradient path is reused for the emphasis and
        // the exact curve match, not for a colour transition it has no types
        // to make.
        ed::Link(id, fromPin, toPin, ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
                 GraphWireThickness());
        const bool emphasize = ed::IsLinkSelected(id) || ed::GetHoveredLink() == id;
        DrawGradientWire(fromPin.Get(), toPin.Get(), PinTextureColor(),
                         PinTextureColor(), emphasize);
    }

    void ShaderEditorDocument::DrawCanvasBackdrop(GraphGridPhase& phase)
    {
        // Two things happen here, and only the second one is shared.
        //
        // CALL THIS BEFORE ed::Begin. The layering argument, the ScreenToCanvas
        // argument and the disclosed one-frame view lag are all written out once
        // at DrawGraphCanvasBackdrop (Widgets/GraphCanvasBackdrop.hpp) -- read
        // them there; every word of them applies unchanged to this canvas and to
        // the Assets panel's Graph lens.
        //
        // The phase is a PARAMETER because it is per-canvas STATE (a pan/zoom
        // history, GraphGridPhase::Update), and this document owns two canvases.
        const ImVec2 canvasMin  = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize = ImGui::GetContentRegionAvail();

        // ---- Off-screen cull rect, in CANVAS units ----
        // Recorded here because this is the one place per canvas that already
        // holds the screen rect BEFORE ed::Begin, which is where ScreenToCanvas
        // still means what it says (inside Begin/End the editor moves ImGui
        // itself into canvas space, imgui_canvas.cpp:476-487).
        //
        // UE's rule, ported: cull against the viewport expanded by a guard band
        // of a quarter of its size on every side (NodePanelDefs::GuardBandArea
        // = 0.25f, SNodePanel.cpp:224, applied as drawSize * -0.25 ..
        // drawSize * 1.25 at :1588-1589). The band is what keeps a node that is
        // one pixel off-screen from flickering between full and stand-in as the
        // view drifts, and it is why panning does not pop.
        if (canvasSize.x > 0.0f && canvasSize.y > 0.0f)
        {
            const ImVec2 tl = ed::ScreenToCanvas(canvasMin);
            const ImVec2 br = ed::ScreenToCanvas(ImVec2(canvasMin.x + canvasSize.x,
                                                        canvasMin.y + canvasSize.y));
            const float bandX = (br.x - tl.x) * CullGuardBand();
            const float bandY = (br.y - tl.y) * CullGuardBand();
            m_cullMin = ImVec2(tl.x - bandX, tl.y - bandY);
            m_cullMax = ImVec2(br.x + bandX, br.y + bandY);
            m_cullRectValid = true;
        }
        else
        {
            // No region to measure: cull nothing rather than everything.
            m_cullRectValid = false;
        }

        if (canvasSize.x <= 0.0f || canvasSize.y <= 0.0f)
            return;

        // The backdrop itself. The phase state rides on the DOCUMENT (one per
        // canvas) so a canvas keeps its history across view switches.
        DrawGraphCanvasBackdrop(canvasMin, canvasSize,
                                kCanvasColor, GraphThemeColor(&GraphThemeSettings::gridMinor),
                                GraphThemeColor(&GraphThemeSettings::gridMajor),
                                phase);
    }

    void ShaderEditorDocument::SetPinPivot(std::uint64_t pinId, ImVec2 p)
    {
        // PinPivotRect writes Pin::m_Pivot directly and clears m_ResolvePivot
        // (imgui_node_editor.cpp:5443-5448), so EndPin's alignment/size path
        // (:5412-5423) is skipped and the pivot IS this point. That is the
        // whole trick: the endpoint stops being something we infer from the
        // row's item rect and becomes something we hand over, so the curve we
        // draw and the curve the library hit-tests cannot drift apart.
        //
        // The pivot is a degenerate rect. With PinRadius and PinArrowSize both
        // 0 and SnapLinkToPinDir off (all defaults, imgui_node_editor.h:253-259
        // -- this document never overrides them), GetClosestLine's extents are
        // 0 and ImRect_ClosestLine of two points returns those two points
        // (imgui_node_editor.cpp:612-636), so Link::m_Start / m_End land
        // exactly here.
        ed::PinPivotRect(p, p);
        m_pinPivots[pinId] = p;
    }

    void ShaderEditorDocument::DrawGradientWire(std::uint64_t fromPinId,
                                                std::uint64_t toPinId,
                                                const ImVec4& fromColor,
                                                const ImVec4& toColor,
                                                bool emphasize) const
    {
        const auto itA = m_pinPivots.find(fromPinId);
        const auto itB = m_pinPivots.find(toPinId);
        // A pin that did not draw this frame has no anchor. ed::Link refuses
        // the same link for the same reason (DoLink bails on a non-live pin,
        // imgui_node_editor.cpp:1639-1640), so drawing nothing matches what the
        // interaction layer already decided.
        if (itA == m_pinPivots.end() || itB == m_pinPivots.end())
            return;

        const ImVec2 p0 = itA->second;
        const ImVec2 p3 = itB->second;

        // EMPHASIS STAYS HERE. What counts as emphasis is a canvas's own
        // business -- this one brightens both ends when the link is hovered or
        // selected, the Graph lens dims at rest and brightens on either
        // endpoint's selection -- so the shared stroke takes two FINAL colours.
        const ImVec4 a = emphasize ? GraphBrightenColor(fromColor) : fromColor;
        const ImVec4 b = emphasize ? GraphBrightenColor(toColor)   : toColor;

        // The curve, the channel retarget, the equal-colour fast path, the
        // segment budget and the midpoint-sampled walk are all
        // Widgets/GraphWire.hpp's now. The returned midpoint is for callers that
        // hang a label off it (the Graph lens does); this canvas has none.
        DrawGraphWire(p0, p3, a, b, GraphWireThickness(), GraphViewScale());
    }

    void ShaderEditorDocument::HandleGraphEdits()
    {
        Arcane::MaterialGraph& g = *ActiveGraphOpt();

        // Wire creation: one edge per input (silent replace), outputs fan out,
        // cycles refused silently at connect time (all SG rules). Every numeric
        // pin connects to every numeric pin -- the adaptation table absorbs
        // width differences, so validity is purely structural.
        {
        // ed::EndCreate() rides the scope object's destructor -- see
        // Widgets/CanvasEditScope.hpp for why it must be unconditional.
        const CanvasCreateScope create;
        if (create)
        {
            ed::PinId aId, bId;
            if (ed::QueryNewLink(&aId, &bId))
            {
                const DecodedPin a = DecodePin(aId);
                const DecodedPin b = DecodePin(bId);
                bool valid = a.valid && b.valid && a.isInput != b.isInput;
                const DecodedPin& out = a.isInput ? b : a;
                const DecodedPin& in = a.isInput ? a : b;
                if (valid)
                    valid = g.FindNode(out.node) && g.FindNode(in.node) &&
                            !WouldCycle(g, out.node, in.node);
                if (!valid)
                    ed::RejectNewItem();
                else if (ed::AcceptNewItem())
                {
                    std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                    std::erase_if(g.links, [&](const Arcane::GraphLink& l)
                                  { return l.toNode == in.node && l.toPin == in.pin; });
                    Arcane::GraphLink l;
                    l.fromNode = out.node;
                    l.fromPin = out.pin;
                    l.toNode = in.node;
                    l.toPin = in.pin;
                    g.links.push_back(l);
                    m_dirty = true;
                    if (m_live)
                        RegenerateFromGraph();
                    PushGraphUndo("Connect", std::move(before));
                }
            }
            else if (ed::QueryNewNode(&aId))
            {
                // Wire released over empty canvas -> the create searcher.
                // Popups cannot open here (canvas space); stash the dragged
                // pin and let DrawGraphPanel's Suspend block open it.
                const DecodedPin from = DecodePin(aId);
                if (!from.valid || !g.FindNode(from.node))
                    ed::RejectNewItem();
                else if (ed::AcceptNewItem())
                {
                    m_wireNode = from.node;
                    m_wirePin = from.pin;
                    m_wireIsInput = from.isInput;
                    m_wireCreateRequest = true;
                }
            }
        }
        }   // ~CanvasCreateScope -> ed::EndCreate()

        // Deletion (multi-select = one undo step). Link ids are this frame's
        // indices -- collect first, erase in descending order after the
        // queries. The Output node refuses deletion (SG: blocks are fixed).
        std::vector<std::size_t> linkIdxs;
        std::vector<std::uint32_t> nodeIds;
        {
        const CanvasDeleteScope del;   // ed::EndDelete() at the closing brace
        if (del)
        {
            ed::LinkId lid;
            while (ed::QueryDeletedLink(&lid))
            {
                const std::size_t idx = static_cast<std::size_t>(lid.Get()) - 1;
                if (idx < g.links.size() && ed::AcceptDeletedItem())
                    linkIdxs.push_back(idx);
                else if (idx >= g.links.size())
                    ed::RejectDeletedItem();
            }
            ed::NodeId nid;
            while (ed::QueryDeletedNode(&nid))
            {
                const std::uint32_t id = static_cast<std::uint32_t>(nid.Get());
                const Arcane::GraphNode* n = g.FindNode(id);
                if (!n || n->type == Arcane::GraphNodeType::Output)
                {
                    ed::RejectDeletedItem();
                    continue;
                }
                if (ed::AcceptDeletedItem())
                    nodeIds.push_back(id);
            }
        }
        }   // ~CanvasDeleteScope -> ed::EndDelete()

        if (!linkIdxs.empty() || !nodeIds.empty())
        {
            std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
            std::sort(linkIdxs.rbegin(), linkIdxs.rend());
            for (std::size_t idx : linkIdxs)
                g.links.erase(g.links.begin() + static_cast<std::ptrdiff_t>(idx));
            for (std::uint32_t id : nodeIds)
            {
                std::erase_if(g.nodes, [&](const Arcane::GraphNode& n)
                              { return n.id == id; });
                std::erase_if(g.links, [&](const Arcane::GraphLink& l)
                              { return l.fromNode == id || l.toNode == id; });
            }
            m_dirty = true;
            if (m_live)
                RegenerateFromGraph();
            PushGraphUndo("Delete", std::move(before));
        }

        // Canvas focus only. The node editor's built-in shortcuts are disabled.
        if (ed::IsActive())
        {
            EditorActions& keys = EditorActions::Get();
            keys.MarkContextActive(ActionContext::Graph);
            if (!ImGui::GetIO().WantTextInput)
            {
                if (keys.Pressed("graph.copy"))
                {
                    const std::string clip = BuildGraphClipJson();
                    if (!clip.empty()) ImGui::SetClipboardText(clip.c_str());
                }
                else if (keys.Pressed("graph.cut"))
                {
                    const std::string clip = BuildGraphClipJson();
                    if (!clip.empty())
                    {
                        ImGui::SetClipboardText(clip.c_str());
                        DeleteCanvasSelection();
                    }
                }
                else if (keys.Pressed("graph.paste"))
                    PasteGraphClipText(ImGui::GetClipboardText());
                else if (keys.Pressed("graph.duplicate"))
                {
                    const std::string clip = BuildGraphClipJson();
                    if (!clip.empty()) PasteGraphClipText(clip.c_str());
                }
                else if (keys.Pressed("graph.delete"))
                    DeleteCanvasSelection();
            }
        }
    }

    void ShaderEditorDocument::DeleteCanvasSelection()
    {
        const int count = std::max(0, ed::GetSelectedObjectCount());
        if (count == 0) return;
        std::vector<ed::NodeId> nodes(static_cast<std::size_t>(count));
        nodes.resize(static_cast<std::size_t>(ed::GetSelectedNodes(nodes.data(), count)));
        for (const ed::NodeId n : nodes) ed::DeleteNode(n);
        std::vector<ed::LinkId> links(static_cast<std::size_t>(count));
        links.resize(static_cast<std::size_t>(ed::GetSelectedLinks(links.data(), count)));
        for (const ed::LinkId l : links) ed::DeleteLink(l);
    }

    std::string ShaderEditorDocument::BuildGraphClipJson()
    {
        const Arcane::MaterialGraph& g = *ActiveGraphOpt();
        std::vector<ed::NodeId> sel(
            static_cast<std::size_t>(std::max(0, ed::GetSelectedObjectCount())));
        if (sel.empty())
            return {};
        const int count = ed::GetSelectedNodes(sel.data(), static_cast<int>(sel.size()));

        Arcane::MaterialGraph sub;
        std::unordered_set<std::uint32_t> picked;
        for (int i = 0; i < count; ++i)
        {
            const std::uint32_t id =
                static_cast<std::uint32_t>(sel[static_cast<std::size_t>(i)].Get());
            const Arcane::GraphNode* node = g.FindNode(id);
            if (!node || node->type == Arcane::GraphNodeType::Output)
                continue;   // the one fixed node never travels
            if (picked.insert(id).second)
                sub.nodes.push_back(*node);
        }
        if (sub.nodes.empty())
            return {};
        // Internal links only -- both endpoints in the selection.
        for (const Arcane::GraphLink& l : g.links)
            if (picked.count(l.fromNode) && picked.count(l.toNode))
                sub.links.push_back(l);

        nlohmann::json j = Arcane::GraphToJson(sub);
        j["kind"] = "arcane-graph-clip";
        return j.dump();
    }

    void ShaderEditorDocument::PasteGraphClipText(const char* text)
    {
        if (!text || !*text)
            return;
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object() ||
            j.value("kind", std::string()) != "arcane-graph-clip")
            return;   // foreign clipboard content -- not ours, ignore silently
        const std::optional<Arcane::MaterialGraph> sub = Arcane::GraphFromJson(j);
        if (!sub)
            return;

        // Recenter the subgraph on the mouse (canvas space).
        float cx = 0.0f, cy = 0.0f;
        int count = 0;
        for (const Arcane::GraphNode& n : sub->nodes)
            if (n.type != Arcane::GraphNodeType::Output)
            {
                cx += n.posX;
                cy += n.posY;
                ++count;
            }
        if (count == 0)
            return;
        cx /= static_cast<float>(count);
        cy /= static_cast<float>(count);
        const ImVec2 at = ed::ScreenToCanvas(ImGui::GetMousePos());

        Arcane::MaterialGraph& g = *ActiveGraphOpt();
        std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
        std::unordered_map<std::uint32_t, std::uint32_t> remap;
        ed::ClearSelection();
        for (const Arcane::GraphNode& src : sub->nodes)
        {
            if (src.type == Arcane::GraphNodeType::Output)
                continue;
            Arcane::GraphNode n = src;
            n.id = g.MintId();   // FRESH ids -- clip ids may collide or be stale
            remap[src.id] = n.id;
            n.posX += at.x - cx;
            n.posY += at.y - cy;
            ed::SetNodePosition(n.id, ImVec2(n.posX, n.posY));
            ed::SelectNode(n.id, true);   // the paste becomes the selection
            g.nodes.push_back(std::move(n));
        }
        for (const Arcane::GraphLink& l : sub->links)
        {
            const auto f = remap.find(l.fromNode);
            const auto t = remap.find(l.toNode);
            if (f == remap.end() || t == remap.end())
                continue;   // endpoint did not paste
            Arcane::GraphLink nl;
            nl.fromNode = f->second;
            nl.fromPin = l.fromPin;
            nl.toNode = t->second;
            nl.toPin = l.toPin;
            g.links.push_back(nl);
        }
        m_dirty = true;
        if (m_live)
            RegenerateFromGraph();
        PushGraphUndo("Paste", std::move(before));
    }

    bool ShaderEditorDocument::ApplyParamRefEdit(std::uint32_t nameHash, const AssetRefEdit& edit)
    {
        if (!m_boundTemplate || !m_instance || edit.op == AssetRefEdit::Op::None)
            return false;
        for (const Arcane::ParamDecl& d : m_boundTemplate->Params())
        {
            if (d.nameHash != nameHash || d.type != Arcane::MatParamType::Texture)
                continue;
            SetParamWithUndo(d, Arcane::MatParamValue::MakeTexture(
                edit.op == AssetRefEdit::Op::Set ? edit.guid : Arcane::Guid::Nil()));
            return true;
        }
        return false;
    }

    void ShaderEditorDocument::ResetParamWithUndo(const Arcane::ParamDecl& d)
    {
        if (!m_instance || !m_instance->HasOverride(d.nameHash))
            return;
        Arcane::MatParamValue before;
        m_instance->GetParam(d.nameHash, before);
        m_instance->ClearOverride(d.nameHash);
        if (Arcane::CommandStack* undo = m_services.undo ? m_services.undo() : nullptr)
            undo->Push(std::make_unique<ParamEditCommand>(
                m_anchor, d.nameHash, "Reset " + d.name,
                /*hadBefore=*/true, before, /*hasAfter=*/false, Arcane::MatParamValue{}));
    }

    void ShaderEditorDocument::SetParamWithUndo(const Arcane::ParamDecl& d,
                                                const Arcane::MatParamValue& value)
    {
        if (!m_instance)
            return;
        const bool hadBefore = m_instance->HasOverride(d.nameHash);
        Arcane::MatParamValue before;
        if (hadBefore)
            m_instance->GetParam(d.nameHash, before);
        if (!m_instance->Set(d.nameHash, value))
            return;
        if (UndoStack())
            UndoStack()->Push(std::make_unique<ParamEditCommand>(
                m_anchor, d.nameHash, "Edit " + d.name,
                hadBefore, before, /*hasAfter=*/true, value));
        // No binding refresh on a texture pick: texture params resolve by
        // Guid, fresh, every frame through NriTextureCache, so there is nothing
        // to invalidate.
    }

    void ShaderEditorDocument::DrawRenderingSection(PropertyGrid& grid, Arcane::CommandStack* undo)
    {
        std::optional<MeshMaterialMetadataState> metadata = CaptureMeshMaterialMetadata();
        if (!metadata)
            return;
        Arcane::MaterialBlendMode inheritedBlend = Arcane::MaterialBlendMode::Opaque;
        float inheritedCutoff = 0.5f;
        bool inheritedTwoSided = false;
        for (auto it = m_parentChain.rbegin(); it != m_parentChain.rend(); ++it)
        {
            if (it->blend) inheritedBlend = *it->blend;
            if (it->alphaCutoff) inheritedCutoff = *it->alphaCutoff;
            if (it->twoSided) inheritedTwoSided = *it->twoSided;
        }
        if (!grid.Section("Rendering"))
            return;
        PropertyGrid::Rows rows(grid, "##rendering");
        if (!rows)
            return;
        const bool inst = IsInstance();
        // EVERY ROW IS AN UNDO STEP (F3 plan 2, I2). On instances the T2
        // override cell replaces the unlabelled ##*_override boxes; inherited
        // rows draw dimmed and disabled.
        static constexpr const char* kBlendItems[] = { "Opaque", "Masked", "Transparent" };
        bool blendOverride = metadata->blend.has_value();
        if (inst) grid.SetNextRowDecor(RowDecor{ &blendOverride });
        const int picked = grid.ComboRow("Blend", kBlendItems, 3,
                                         static_cast<int>(metadata->blend.value_or(inheritedBlend)));
        if (inst && grid.LastRowEvents().overrideToggled)
        {
            metadata->blend = blendOverride ? std::optional<Arcane::MaterialBlendMode>(inheritedBlend) : std::nullopt;
            SetMeshMaterialMetadataWithUndo(*metadata);
        }
        else if (picked >= 0)
        {
            metadata->blend = static_cast<Arcane::MaterialBlendMode>(picked);
            SetMeshMaterialMetadataWithUndo(*metadata);
        }

        bool cutoffOverride = metadata->alphaCutoff.has_value();
        if (inst) grid.SetNextRowDecor(RowDecor{ &cutoffOverride });
        const float shownCutoff = metadata->alphaCutoff.value_or(inheritedCutoff);
        float cutoff = shownCutoff;
        (void)grid.FloatRow("Alpha cutoff", cutoff, 0.01f, Astra::Range(0.0, 1.0), "%.2f");
        const RowEvents cutoffEvents = grid.LastRowEvents();
        // ONE DRAG = ONE STEP: the before-state is latched at activation, the
        // step builds at close; live Apply while dragging (cpp :5747-5768).
        EditGesture::BeginOnActivate(undo, m_gesture,
            [] { return std::string("Edit Alpha Cutoff"); },
            [&]() -> std::function<void()>
            {
                m_cutoffGestureBefore = *metadata;
                return std::function<void()>([this, before = *metadata] { PushMeshMaterialMetadataUndo(before); });
            });
        if (inst && cutoffEvents.overrideToggled)
        {
            metadata->alphaCutoff = cutoffOverride ? std::optional<float>(inheritedCutoff) : std::nullopt;
            SetMeshMaterialMetadataWithUndo(*metadata);
        }
        else if (cutoffEvents.cancelled && m_cutoffGestureBefore)
        {
            *metadata = *m_cutoffGestureBefore;           // R2: back to the activation state, nullopt included
            ApplyMeshMaterialMetadata(*metadata);
        }
        else if (cutoff != shownCutoff)
        {
            metadata->alphaCutoff = cutoff;
            ApplyMeshMaterialMetadata(*metadata);         // LIVE; the step lands when the drag closes
        }
        EditGesture::EndAfterRow(undo, m_gesture, cutoffEvents.cancelled);
        if (!ImGui::IsItemActive())
            m_cutoffGestureBefore.reset();

        bool twoSidedOverride = metadata->twoSided.has_value();
        if (inst) grid.SetNextRowDecor(RowDecor{ &twoSidedOverride });
        bool twoSided = metadata->twoSided.value_or(inheritedTwoSided);
        const bool flipped = grid.CheckboxRow("Two sided", twoSided);
        if (inst && grid.LastRowEvents().overrideToggled)
        {
            metadata->twoSided = twoSidedOverride ? std::optional<bool>(inheritedTwoSided) : std::nullopt;
            SetMeshMaterialMetadataWithUndo(*metadata);
        }
        else if (flipped)
        {
            metadata->twoSided = twoSided;
            SetMeshMaterialMetadataWithUndo(*metadata);
        }
    }

    void ShaderEditorDocument::DrawParamsSection(PropertyGrid& grid, Arcane::CommandStack* undo)
    {
        const bool inst = IsInstance();
        const bool open = inst
            ? grid.Section("Parameters", true, [this] { ImGui::Checkbox("Only overridden", &m_showOnlyOverridden); })
            : grid.Section("Parameters");
        if (!open)
            return;
        PropertyGrid::Rows rows(grid, "##params");
        if (!rows)
            return;
        if (!m_instance || !m_boundTemplate)
        {
            grid.ReadOnlyRow("Status", "params appear after the first successful compile");
            return;
        }
        const AssetRefServices& refs = m_services.assetRefs ? *m_services.assetRefs : NoAssetRefServices();
        const auto& params = m_boundTemplate->Params();
        for (std::size_t i = 0; i < params.size(); ++i)
        {
            const Arcane::ParamDecl& d = params[i];
            if (inst && m_showOnlyOverridden && !m_instance->HasOverride(d.nameHash))
                continue;
            const Arcane::ParamMeta meta = i < m_boundMetas.size() ? m_boundMetas[i] : Arcane::ParamMeta{};
            Arcane::MatParamValue value;
            if (!m_instance->GetParam(d.nameHash, value))
                continue;
            const Arcane::MatParamValue shown = value;
            bool overridden = m_instance->HasOverride(d.nameHash);
            ImGui::PushID(d.name.c_str());
            // Instances: the override cell (the only override control, no reset).
            // Bases: the reset slot, live when an override exists (s4.1(d)).
            if (inst) grid.SetNextRowDecor(RowDecor{ &overridden });
            else      grid.SetNextRowDecor(RowDecor{ nullptr, true, overridden });
            const ParamWidget widget = WidgetFor(d.type);
            ImGuiID popupId = 0;
            AssetRefEdit texEdit;
            switch (widget)
            {
                case ParamWidget::SliderFloat:
                    (void)grid.SliderRow(d.name.c_str(), value.f[0], meta.sliderMin, meta.sliderMax);
                    break;
                case ParamWidget::DragFloat2:
                    (void)grid.VecRow(d.name.c_str(), value.f, 2, 0.01f, std::nullopt, "%.3f");
                    break;
                case ParamWidget::DragFloat4:
                    (void)grid.VecRow(d.name.c_str(), value.f, 4, 0.01f, std::nullopt, "%.3f");
                    break;
                case ParamWidget::ColorEdit:
                    (void)grid.ColorRow(d.name.c_str(), value.f, &popupId, /*hdr=*/false);   // a Color param IS a colour
                    break;
                case ParamWidget::TexturePicker:
                {
                    AssetRefArgs args;
                    args.guid = value.tex;
                    args.kindFilter = static_cast<int>(AssetKind::Texture);
                    args.readOnly = inst && !overridden;   // inherited: shown, not editable
                    texEdit = AssetRefRow(grid, d.name.c_str(), args, refs);
                    break;
                }
            }
            const RowEvents events = grid.LastRowEvents();
            if (!meta.tooltip.empty() && LabelCellHovered())
                ImGui::SetTooltip("%s\n%s", d.name.c_str(), meta.tooltip.c_str());

            // The step builder (unchanged contract, cpp :5915-5957): before-state
            // read on the activation frame, the step built at close, a no-op
            // guard for unchanged closes. It also latches m_liveParamSeed.
            auto buildParamEdit = [&]() -> std::function<void()>
            {
                const bool hadBefore = m_instance->HasOverride(d.nameHash);
                Arcane::MatParamValue before{};
                if (hadBefore)
                    m_instance->GetParam(d.nameHash, before);
                m_liveParamSeed = LiveParamSeed{ d.nameHash, hadBefore, before };
                return std::function<void()>(
                    [this, nameHash = d.nameHash, name = d.name, hadBefore, before]
                    {
                        Arcane::MatParamValue after;
                        if (!m_instance || !m_instance->GetParam(nameHash, after))
                            return;
                        const bool hasAfter = m_instance->HasOverride(nameHash);
                        if (hadBefore == hasAfter && (!hadBefore || before == after))
                            return;   // nothing changed -- no step, redo intact
                        if (Arcane::CommandStack* s = m_services.undo ? m_services.undo() : nullptr)
                            s->Push(std::make_unique<ParamEditCommand>(
                                m_anchor, nameHash, "Edit " + name, hadBefore, before, hasAfter, after));
                    });
            };
            if (widget == ParamWidget::TexturePicker)
            {
                (void)ApplyParamRefEdit(d.nameHash, texEdit);   // single-shot: no gesture (cpp :5594-5596)
            }
            else
            {
                EditGesture::BeginOnActivate(undo, m_gesture, [&] { return "Edit " + d.name; }, buildParamEdit);
                if (events.cancelled && m_liveParamSeed.nameHash == d.nameHash)
                {
                    if (m_liveParamSeed.hadBefore) m_instance->Set(d.nameHash, m_liveParamSeed.before);
                    else                           m_instance->ClearOverride(d.nameHash);
                }
                else if (!(value == shown))
                {
                    m_instance->Set(d.nameHash, value);   // LIVE: the next Tick packs the CB, no recompile
                }
                EditGesture::EndAfterRow(undo, m_gesture, events.cancelled);
                if (widget == ParamWidget::ColorEdit && popupId != 0)
                {
                    EditGesture::BeginOnPopupOpen(undo, m_gesture, popupId,
                                                  [&] { return "Edit " + d.name; }, buildParamEdit);
                    EditGesture::EndOnPopupClose(undo, m_gesture, popupId);
                }
            }
            if (inst && events.overrideToggled)
            {
                if (overridden)
                {
                    Arcane::MatParamValue resolved;
                    if (m_instance->GetParam(d.nameHash, resolved))
                        SetParamWithUndo(d, resolved);      // "Edit <name>" (cpp :5805-5812)
                }
                else
                    ResetParamWithUndo(d);                  // "Reset <name>"
            }
            else if (!inst && events.resetClicked)
                ResetParamWithUndo(d);
            ImGui::PopID();
        }
    }
}
