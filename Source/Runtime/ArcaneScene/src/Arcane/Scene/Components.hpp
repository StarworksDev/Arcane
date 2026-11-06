#pragma once

// Scene components: plain reflected data. Every engine component is
// ARC_REFLECT-annotated from day one (design rule) so the editor/JSON/server
// path stays open at ~zero cost via the Astra 3.2 visitFields seam. Header-only;
// reflect blocks at namespace scope register once per module (MetaRegistry is
// idempotent), and the simulation Registry is owned by the host module.

#include <Arcane/Guid.hpp>
#include <Arcane/Math/Aabb.hpp>   // WorldBounds::box (F3 plan 1 T2)
#include <Arcane/Math/DeterministicTrig.hpp>   // the planar bridge below (PR #20)

#include <Arcane/Reflection.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>          // quat, mat4_cast

#include <cmath>
#include <cstdint>
#include <string>

namespace Arcane
{
    // Task 3 (F1): the transform spine is 3D. position/scale are vec3 and
    // rotation is a QUATERNION -- not Euler angles, which have no canonical
    // order and gimbal-lock, and not a matrix, which cannot be interpolated on
    // the shortest arc (see the render interpolation note in
    // RenderSystems.hpp). Storage is ALWAYS radians-equivalent (glm::quat has
    // no unit of its own; nothing here changed). The editor still AUTHORS it
    // as EULER ANGLES: the Inspector's
    // FieldKind::Quat draws a three-axis Euler view over the quaternion and
    // honours the AngleFormat attribute on the reflect row (see
    // InspectorFields.cpp). Directive (2026-08-22): radians internally,
    // DEGREES in the editor, and no live conversion -- degrees are purely an
    // editor display artifact, never round-tripped through on an axis the
    // user did not touch. The reflect block below carries
    // AngleFormat::Degrees EXPLICITLY rather than omitting the attribute and
    // relying on AngleUnitForField's own Degrees default (InspectorMeta.cpp):
    // explicit-and-correct beats implicit-and-correct here, since a future
    // reader of just this reflect block would otherwise have to go look up
    // that default to know what unit the row shows. (An earlier version of
    // this comment carried AngleFormat::Radians for the opposite reason --
    // preserving the retired float field's on-screen unit across the 2D->3D
    // widening. That reasoning is superseded by the directive above.)
    //
    // Every scene in the tree today is planar and stays planar: a 2D pose is
    // this type with position.z == 0, a rotation about +Z only, and scale.z ==
    // 1, which reproduces the retired mat3 path exactly (pinned by
    // TransformSpineTest.cpp's "Z-axis quaternion reproduces the retired 2D
    // rotation matrix" case).
    struct Transform
    {
        // Astra change tracking (spec 2026-09-11 s6.2): 8 B per entity of
        // {added, changed} ticks so Changed<Transform> is EXACT per entity --
        // TransformPropagationSystem's pre-pass and Arcane::PhysicsSystem2D's paused
        // reconcile both need entity precision, and a coarse chunk stamp would
        // recompose every row of a touched chunk. Three other components are
        // tracked (F3 plan 1 T2): WorldTransform, MeshRenderer and WorldBounds
        // -- BoundsSystem and GpuSceneSync re-walk only the rows that moved.
        // Hidden is a tag and has no column to track.
        ARC_CHANGE_TRACKED

        glm::vec3 position{0.0f, 0.0f, 0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};   // identity; glm's ctor is (w, x, y, z)
        glm::vec3 scale{1.0f, 1.0f, 1.0f};

        // TRS in column-major homogeneous mat4: translate * rotate * scale, so
        // the local axes are scaled, then turned, then shifted (translation in
        // COLUMN 3 -- it was column 2 in the mat3 this replaced, which is the
        // one index every reader of a world matrix had to move).
        //
        // Built column-wise rather than as three glm::translate/scale products:
        // this is called once per entity per propagation pass, and the product
        // form spends two full 4x4 multiplies re-deriving a result whose shape
        // is known. Equivalence is pinned against the product form in
        // TransformSpineTest.cpp.
        glm::mat4 ToMatrix() const
        {
            glm::mat4 m = glm::mat4_cast(rotation);
            m[0] *= scale.x;
            m[1] *= scale.y;
            m[2] *= scale.z;
            m[3] = glm::vec4(position, 1.0f);
            return m;
        }
    };

    struct WorldTransform
    {
        ARC_CHANGE_TRACKED   // F3: Changed<> is exact per entity -- BoundsSystem and GpuSceneSync re-walk only the rows that moved
        glm::mat4 matrix{1.0f};             // computed by TransformPropagationSystem; never authored
    };

    // The drawable's WORLD-space box (F3, spec s2.2): engine-written by
    // BoundsSystem after transform propagation, never authored, never
    // serialized. Mesh = the artifact AABB through the world matrix; sprite =
    // the SpriteWorldQuad corners, Z widened by kSpriteDepthEpsilon. An entity
    // with nothing drawable (or an unresolved mesh) carries none. Read by the
    // CPU visible set, the GPU scene rows, the editor's framing.
    struct WorldBounds
    {
        ARC_CHANGE_TRACKED   // exact per entity: the GPU scene re-uploads exactly the moved rows
        Aabb box;
    };

    // The planar bridge (Task 3, F1). Three subsystems are deliberately still
    // 2D -- the physics simulation (Manifold2D is a 2D solver), the sprite
    // submission path, and the transform gizmo -- and each of them has to turn a
    // quaternion into the one angle it understands, and back. These two exist so
    // that conversion has ONE definition instead of a hand-rolled copy at each
    // seam that could drift in sign or convention.
    //
    // RotationZ reads the image of local +X, which is exactly how every consumer
    // of a WORLD matrix reads its angle (atan2 of basis column 0). That makes
    // the two agree for a planar pose, and degrade identically for one that is
    // not: an out-of-plane rotation projects onto the XY plane rather than
    // producing a different answer per call site.
    //
    // Both are DETERMINISTIC (PR #20): the physics write-back stores
    // RotationAboutZ(solver angle) and the v1 step trace reads RotationZ back,
    // so libm's sin/cos/atan2 -- which round differently on MSVC, glibc and
    // Apple -- split the trace by 1 ULP across platforms. The math lives in
    // ArcaneCore (Math/DeterministicTrig.hpp), compiled once under the
    // engine's flags: the same bits on every platform. RotationZ computes the
    // image of +X with glm's own formula, 2(xy + wz) and 1 - 2(y^2 + z^2).
    [[nodiscard]] inline float RotationZ(const glm::quat& q) noexcept
    {
        return DeterministicTrig::AngleAboutZ(q);
    }

    [[nodiscard]] inline glm::quat RotationAboutZ(float radians) noexcept
    {
        return DeterministicTrig::QuatAboutZ(radians);
    }

    // The primitive a SpriteRenderer draws. Lets the ONE canonical 2D submission
    // path (RenderSubmissionSystem) render filled circles + capsules, not just
    // rectangles -- so a body's sprite can MATCH its collider shape (a circle
    // fixture -> a disc), and a multi-fixture body can be drawn as one child
    // sprite per fixture of the right shape. Rect is the default (unchanged).
    enum class SpriteShape : uint8_t
    {
        Rect    = 0,   // axis quad, rotated by the WorldTransform; the default
        Circle  = 1,   // filled disc; diameter == size.x, rotation-invariant
        Capsule = 2,   // horizontal capsule (central rect + two end discs)
    };

    struct SpriteRenderer
    {
        // The .arcsprite asset drawn: it supplies the texture, the UV sub-rect,
        // the world size in meters (sourceSize / ppu) and the pivot. Nil (the
        // default) or unresolved -> a 1x1 m untextured tint quad. There is NO
        // size field: an entity is sized by its Transform scale, matching
        // Unity/Unreal (neither puts a size on the sprite renderer), so one
        // asset can be shared by many entities at different scales.
        Guid        sprite{};
        glm::vec4   tint{1.0f, 1.0f, 1.0f, 1.0f};
        int32_t     sortingLayer = 0;
        int32_t     orderInLayer = 0;
        SpriteShape shape = SpriteShape::Rect; // primitive drawn (Rect/Circle/Capsule)
        // Optional sprite material (Slice 8): the Guid of a SAVED .arcmat asset.
        // Nil (the default) = the plain sprite pipeline, byte-identical to the
        // pre-material path. Resolved to a Batcher2D material id through the
        // SpriteMaterialTable resource at submission; unresolved Guids draw as
        // plain sprites until their compile lands. Rect shape only.
        Guid        material{};
    };

    // The 3D sibling of SpriteRenderer (F2a). An entity carrying one draws its
    // .arcmesh through the opaque mesh pass, posed by its Transform.
    //
    // NO SIZE, for the same reason SpriteRenderer has none -- the Transform's
    // scale is the size, so one asset serves many entities. NO TINT either,
    // and that one IS a departure from SpriteRenderer: neither Unreal nor
    // Source 2 puts a per-instance colour on a mesh component, and both express
    // "the same mesh in a different colour" through material INSTANCES, which
    // Arcane already ships (MaterialAssetData::parent + sparse overrides IS
    // UMaterialInstance, and it is kind-agnostic). A tint would also make a red
    // cube expressible twice -- as a red material, or a white one tinted red --
    // which is the two-spellings defect the unit rule exists to prevent.
    struct MeshRenderer
    {
        ARC_CHANGE_TRACKED   // F3: Changed<> is exact per entity -- BoundsSystem and GpuSceneSync re-walk only the rows that moved

        // The .arcmesh asset drawn. Nil (the default) or unresolved -> draws
        // NOTHING. That is not an error: a scene may legitimately carry a slot
        // with no geometry yet, exactly as MeshInstance::mesh documents.
        Guid mesh{};

        // Per-entity material override. Nil (the default) falls through to the
        // MESH ASSET's own `material`, and a nil there resolves to white.
        // This is UMeshComponent::OverrideMaterials (MeshComponent.h:29-31,
        // "Per-Component material overrides") in miniature; Source 2 answers
        // the same question with m_materialGroups.
        //
        // An override naming an UNRESOLVABLE material falls through to the mesh
        // default and WARNS ONCE -- it must not silently render white, which
        // would hide the broken reference.
        Guid materialOverride{};

        // Transparent meshes sort by this explicit author priority first,
        // then by biased projected depth. Higher order draws later; the bias
        // is in world meters and is independent of material blend mode.
        std::int32_t translucencyRenderOrder = 0;
        float translucencyDepthSortBias = 0.0f;
    };

    // Arcane::PhysicsSettings2D (2026-09-11, 2D physics wiring, spec s5): the PER-SCENE
    // override of the project's physics.gravity setting. Read by Runtime::EnsurePhysics
    // from the SCENE-ROOT entity only (SceneRoot resource) -- beside Camera
    // and PostProcess, where scene-level facts already live; on any other
    // entity it is ignored (pinned by RuntimeTest). Presence IS the override:
    // add it to change gravity for this scene, remove it to fall back to the
    // project. +Y is UP (one right-handed world; the 2D plane is XY viewed
    // down -Z) since F4 plan 1 (spec 2026-09-17-f4-editor-3d-authoring s2), so
    // gravity points at NEGATIVE Y -- the physics-wiring era's "+Y down,
    // Manifold2D's y-down default" is superseded. The vendored library's own
    // WorldDef default is still Box2D's; the engine supplies this one.
    struct PhysicsSettings2D
    {
        glm::vec2 gravity{0.0f, -9.81f};   // m/s^2; +Y UP (F4, spec s2)
    };

    // The scene's post-processing stack (post arc): the Guid of a SAVED
    // fullscreen .arcmat whose pass DAG runs between the linear canvas and the
    // tonemap (the material IS the stack; kSceneInput wires read the scene
    // color). The host sweeps for it each frame -- the FIRST entity with a
    // valid material wins, nil or unresolved means no chain (today's path,
    // byte-identical). One scene, one chain; per-camera stacks are a non-goal.
    struct PostProcess
    {
        Guid material{};
    };

    // The scene's viewpoint. The entity's WorldTransform supplies the POSITION --
    // there is deliberately none stored here, the same call SpriteRenderer makes
    // about size, and the same one Unity and Unreal make (a Camera component /
    // ACameraActor is placed BY its transform; it does not carry one).
    //
    // Before this existed the camera was host state driven through
    // ClientRuntime::SetCamera -- a Sandbox-era artifact from when the physics showcase
    // owned its own camera class. That made the editor viewport and a standalone
    // runtime get their view from different places, so a scene could look correct
    // in the editor and render nothing in the game. A camera IS scene data.
    //
    // orthographicSize is the half-HEIGHT of the visible world in METERS -- the
    // resolution-INDEPENDENT unit (Unity's Camera.orthographicSize). Each host
    // derives zoom = viewportHalfHeight / orthographicSize per frame, so one scene
    // frames identically in a 720p runtime window and in a docked editor viewport
    // of any size. Storing a pixels-per-meter zoom instead would make every scene
    // wrong at any other window size.
    //
    // One scene, one active camera: the FIRST active one wins and >1 warns once,
    // exactly as PostProcess above resolves. `active` lets a scene keep alternates
    // (a cutscene angle, a debug wide shot) without deleting them.
    //
    // Task 5 (Phase 4): `projection` selects which of the two lens models below
    // applies. Orthographic is the default so every scene authored before this
    // field existed (every serialized Camera predating it) loads unchanged --
    // the 2D path (ActiveSceneCamera, SceneCamera.hpp) is gated on this default
    // and stays byte-identical. `fovYDegrees`/`nearZ`/`farZ` are Perspective-only
    // and, per the engine's MKS decision, `nearZ`/`farZ` are METERS (not an
    // arbitrary clip-space unit).
    enum class CameraProjection : std::uint8_t
    {
        Orthographic = 0,
        Perspective  = 1,
    };

    struct Camera
    {
        float orthographicSize = 5.0f;   // half-height of the view, in meters
        bool  active = true;
        CameraProjection projection = CameraProjection::Orthographic;
        float fovYDegrees = 60.0f;       // vertical field of view, degrees (Perspective only)
        float nearZ = 0.1f;              // meters (MKS); Perspective only
        float farZ  = 1000.0f;           // meters (MKS); Perspective only
    };

    // The editor-facing identity (Outliner arc): a STABLE Guid + display name.
    // Policy: the EDITOR adds this AT ENTITY CREATION ONLY (Edit::CreateEntity,
    // and scene load); runtime spawns are never forced to carry strings. A
    // rename is an EDIT of an existing one and never mints it -- an entity
    // without one has no durable identity to rename, so Edit::RenameEntity
    // refuses instead of adding. An entity without one displays as
    // "Entity <id>". The Guid is generated when the component is added and is
    // the durable cross-save identity -- entity ids are not.
    // Renamed from EntityInfo 2026-07-27: it IS the entity's durable identity,
    // the ECS equivalent of Unreal's intrinsic ActorLabel/ActorGuid, and the
    // surrounding comments already called it that ("identity is never minted
    // by rename", "structure-locked identity").
    struct Identity
    {
        Guid        id{};
        std::string name;
        // Non-trivially-copyable: the binary path (Play snapshots, scene
        // SaveBinary) serializes through this member instead of the POD
        // memcpy overload (Astra's HasSerializeMethod seam).
        template<typename Archive>
        void Serialize(Archive& ar)
        {
            ar(id);
            ar(name);
        }
    };

    // Marker ("tag component"): render submission skips entities carrying it
    // (the Outliner eye). Serialized like any component, so hidden stays
    // hidden in game; the eye applies it to an entity AND its descendants.
    struct Hidden {};
}

// Reflection blocks at namespace scope (NOT anonymous). WorldTransform::matrix is
// derived state -> Serializable(false) so name-keyed JSON skips it (recomputed on
// load; binary trivially-copyable path still round-trips it harmlessly).
namespace Arcane
{
    // NO Category on these three. The Inspector draws uncategorised fields
    // directly under the component header and each named category under a
    // collapsible sub-header, so a component whose every field shares one
    // category renders as "Transform > Transform > fields" -- a click and an
    // indent that separate nothing from anything. Categories earn their keep
    // where a component has several groups; SpriteRenderer below is that case.
    ARC_REFLECT_TYPE(Transform)
        ARC_REFLECT_FIELD(Transform, position)
            ARC_REFLECT_ATTR(Tooltip, "World position in meters (MKS units). +Y is UP: the 2D plane is XY viewed down -Z, the same convention as the 3D camera.")
        // AngleFormat is EXPLICIT (not omitted) so this row's unit is
        // readable at the reflect block itself, without a trip to
        // AngleUnitForField's default (InspectorMeta.cpp) to confirm it.
        // FieldKind::Quat reads it (InspectorView.cpp's Quat row) to decide
        // whether the three Euler drags show degrees or radians; Degrees is
        // the authoring unit the directive above asks for. Storage stays
        // radians-equivalent regardless -- this attribute affects ONLY what
        // the widget prints and reads back, via InspectorFields.cpp's
        // SyncQuatEulerViewDegrees/ApplyQuatEulerEditDegrees, which cache the
        // displayed triple in degrees so an untouched axis is never carried
        // through a unit conversion at all.
        ARC_REFLECT_FIELD(Transform, rotation)
            ARC_REFLECT_ATTR(AngleFormat, Arcane::Attr::AngleFormat::Unit::Degrees)
            ARC_REFLECT_ATTR(Tooltip, "Orientation, stored as a quaternion and edited as Euler angles about X/Y/Z. A 2D scene turns about Z only.")
        ARC_REFLECT_FIELD(Transform, scale)
    ARC_END_REFLECT_TYPE()

    // Hidden as well as Serializable(false): these are runtime-derived caches
    // (recomputed every frame / every fixed step), and the Inspector used to
    // advertise them as "unsupported" rows -- noise about state nobody authors.
    // Unity and UE both hide derived caches outright.
    ARC_REFLECT_TYPE(WorldTransform)
        ARC_REFLECT_FIELD(WorldTransform, matrix)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
    ARC_END_REFLECT_TYPE()

    // Same shape as WorldTransform: engine-written by BoundsSystem, never
    // authored, never serialized (F3 plan 1 T2).
    ARC_REFLECT_TYPE(WorldBounds)
        ARC_REFLECT_FIELD(WorldBounds, box)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_ENUM(SpriteShape)
        ARC_REFLECT_ENUM_VALUE(SpriteShape, Rect)
        ARC_REFLECT_ENUM_VALUE(SpriteShape, Circle)
        ARC_REFLECT_ENUM_VALUE(SpriteShape, Capsule)
    ARC_END_REFLECT_ENUM()

    // Guid reflects as a nested struct (hi/lo scalars) so the reflection->JSON
    // path round-trips component asset references (SpriteRenderer::material).
    // Registered here, NOT in Core -- Core stays Astra-free.
    ARC_REFLECT_TYPE(Guid)
        ARC_REFLECT_FIELD(Guid, hi)
        ARC_REFLECT_FIELD(Guid, lo)
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(SpriteRenderer)
        ARC_REFLECT_FIELD(SpriteRenderer, sprite)
            ARC_REFLECT_ATTR(Category, "Appearance")
            ARC_REFLECT_ATTR(Tooltip, "The .arcsprite asset this renderer draws. Nil renders an untextured 1x1 m tint quad scaled by the Transform.")
        ARC_REFLECT_FIELD(SpriteRenderer, tint)
            ARC_REFLECT_ATTR(Category, "Appearance")
        // Both are int32_t here but reach the batcher through
        // static_cast<uint16_t> (RenderSystems.hpp:104-105), which WRAPS rather
        // than clamps -- an authored -1 would sort as 65535, in front of
        // everything. The Range is the cast's own domain, and the Inspector
        // clamps to it on every path that writes these fields (drag, Ctrl+click
        // entry, and the multi-selection box), so an out-of-range value has to
        // come from somewhere other than the property panel.
        ARC_REFLECT_FIELD(SpriteRenderer, sortingLayer)
            ARC_REFLECT_ATTR(Category, "Sorting")
            ARC_REFLECT_ATTR(Range, 0.0, 65535.0, 1.0)
        ARC_REFLECT_FIELD(SpriteRenderer, orderInLayer)
            ARC_REFLECT_ATTR(Category, "Sorting")
            ARC_REFLECT_ATTR(Range, 0.0, 65535.0, 1.0)
        ARC_REFLECT_FIELD(SpriteRenderer, shape)
            ARC_REFLECT_ATTR(Category, "Shape")
        ARC_REFLECT_FIELD(SpriteRenderer, material)
            ARC_REFLECT_ATTR(Category, "Appearance")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(MeshRenderer)
        ARC_REFLECT_FIELD(MeshRenderer, mesh)
            ARC_REFLECT_ATTR(Category, "Appearance")
            ARC_REFLECT_ATTR(Tooltip, "The .arcmesh asset this renderer draws. Nil draws nothing. Size and orientation come from the Transform.")
        ARC_REFLECT_FIELD(MeshRenderer, materialOverride)
            ARC_REFLECT_ATTR(Category, "Appearance")
            ARC_REFLECT_ATTR(Tooltip, "Overrides the mesh asset's own material for this entity only. Nil uses the mesh's default.")
        ARC_REFLECT_FIELD(MeshRenderer, translucencyRenderOrder)
            ARC_REFLECT_ATTR(Category, "Translucency")
            ARC_REFLECT_ATTR(Tooltip, "Transparent draw priority. Higher values draw later.")
        ARC_REFLECT_FIELD(MeshRenderer, translucencyDepthSortBias)
            ARC_REFLECT_ATTR(Category, "Translucency")
            ARC_REFLECT_ATTR(Tooltip, "Bias added to transparent projected depth, in meters.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(PhysicsSettings2D)
        ARC_REFLECT_FIELD(PhysicsSettings2D, gravity)
            ARC_REFLECT_ATTR(Tooltip, "Gravity for THIS scene (m/s^2, +Y is down). Meaningful on the scene root only; overrides the project's physics.gravity setting while present.")
    ARC_END_REFLECT_TYPE()

    // One field, so no Category -- see Transform above.
    ARC_REFLECT_TYPE(PostProcess)
        ARC_REFLECT_FIELD(PostProcess, material)
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_ENUM(CameraProjection)
        ARC_REFLECT_ENUM_VALUE(CameraProjection, Orthographic)
        ARC_REFLECT_ENUM_VALUE(CameraProjection, Perspective)
    ARC_END_REFLECT_ENUM()

    // The Range floor on orthographicSize is deliberately above zero: a zero or
    // negative half-height has no view to derive, and ActiveSceneCamera treats
    // it as "no camera" rather than dividing by it. Field order matches the
    // struct's own (every other reflect block in this file does the same).
    // Categorized now that the component has two lens models worth of fields
    // (Task 5, Phase 4) -- `active`/`projection` are uncategorized (apply to
    // either lens), and each model's own fields group together so the
    // Inspector doesn't interleave a Perspective-only field between two
    // Orthographic-only ones.
    ARC_REFLECT_TYPE(Camera)
        ARC_REFLECT_FIELD(Camera, orthographicSize)
            ARC_REFLECT_ATTR(Category, "Orthographic")
            ARC_REFLECT_ATTR(Tooltip, "Half-height of the visible world, in meters. Resolution-independent: each host derives its zoom from the viewport height, so the framing holds at any window size.")
            ARC_REFLECT_ATTR(Range, 0.01, 10000.0, 0.1)
        ARC_REFLECT_FIELD(Camera, active)
            ARC_REFLECT_ATTR(Tooltip, "Only active cameras are considered; the first one found renders the scene.")
        ARC_REFLECT_FIELD(Camera, projection)
            ARC_REFLECT_ATTR(Tooltip, "Orthographic (the default) preserves every 2D scene. Perspective is the 3D lens (Phase 4's mesh slice).")
        ARC_REFLECT_FIELD(Camera, fovYDegrees)
            ARC_REFLECT_ATTR(Category, "Perspective")
            ARC_REFLECT_ATTR(Tooltip, "Vertical field of view, in degrees.")
            ARC_REFLECT_ATTR(Range, 1.0, 179.0, 1.0)
        ARC_REFLECT_FIELD(Camera, nearZ)
            ARC_REFLECT_ATTR(Category, "Perspective")
            ARC_REFLECT_ATTR(Tooltip, "Near clip plane distance, in meters (MKS).")
            ARC_REFLECT_ATTR(Range, 0.001, 10000.0, 0.01)
        ARC_REFLECT_FIELD(Camera, farZ)
            ARC_REFLECT_ATTR(Category, "Perspective")
            ARC_REFLECT_ATTR(Tooltip, "Far clip plane distance, in meters (MKS).")
            ARC_REFLECT_ATTR(Range, 0.01, 1000000.0, 1.0)
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(Identity)
        ARC_REFLECT_FIELD(Identity, id)
            ARC_REFLECT_ATTR(ReadOnly)
        ARC_REFLECT_FIELD(Identity, name)
            ARC_REFLECT_ATTR(Tooltip, "The Outliner display name for this entity. If empty, the Outliner shows \"Entity <id>\" instead.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(Hidden)
    ARC_END_REFLECT_TYPE()
}
