#pragma once

// The scene-camera sweep: resolves the ACTIVE Camera entity into the ONE
// camera type every consumer reads -- a ViewTransform (ViewTransform.hpp; F4
// plan 1, spec s3) -- which the host pushes through ClientRuntime::SetView and
// RenderContext2D carries to the render side.
//
// Deliberately a PURE function of (registry, viewport size) rather than a system
// or a Runtime method. Three callers have to agree about the view or the feature
// is pointless -- ArcaneRuntime, play-in-editor, and the editor's camera-rect
// overlay -- and a system would have to be registered by the GAME module (the
// plugin owns Schedulers), which would make a camera in a scene depend on the
// game rebuilding. Being pure also means it is unit-testable with no host at all.

#include <Arcane/Base/Log.hpp>              // ARC_WARN (degenerate-basis fallback, Task 7/F2a)
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/DepthConvention.hpp>
#include <Arcane/Scene/RenderViewSettings.hpp>   // Ortho2DView: render.ortho2D.depthRange (S6-9)
#include <Arcane/Scene/SceneCameraView.hpp>        // SceneCameraView (its own header, F5 plan 1 Task 4)
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>   // perspectiveRH_ZO, lookAtRH

#include <utility>

#include <optional>

namespace Arcane
{
    // The active camera's view for a `viewport` (pixels), or nullopt when the
    // scene has no usable active Camera.
    //
    // A nullopt means "leave the stored camera ALONE" -- NOT "use identity". Every
    // scene authored before cameras existed has no Camera entity, and substituting
    // an identity view would draw those scenes at 1 px per meter in the corner: a
    // black-looking window with no explanation, which is the exact failure this
    // seam exists to end. Unity is explicit for the same reason ("No cameras
    // rendering") instead of silently inventing a viewpoint.
    //
    // outCount, when given, receives the number of ACTIVE cameras found, so a host
    // can warn once that a scene is ambiguous -- the contract the PostProcess sweep
    // already set. `warn` is the same switch ResolveFrameViews threads into both
    // sweeps. This orthographic sweep emits no diagnostic of its own: the
    // no-camera and multi-camera lines are printed by ResolveFrameViews, and only
    // when its flag is true, so a host's early resolve cannot double them. A
    // diagnostic added here must honour `warn`, as the perspective sweep does.
    inline std::optional<SceneCameraView> ActiveSceneCamera(Astra::Registry& reg,
                                                            glm::uvec2 viewport,
                                                            int* outCount = nullptr,
                                                            bool warn = true)
    {
        (void)warn;
        int       found = 0;
        glm::vec2 center{0.0f, 0.0f};
        float     halfH = 0.0f;

        // Viewed on Camera ALONE, with the pose fetched per hit, rather than
        // CreateView<Camera, WorldTransform>: WorldTransform is DERIVED, and on the
        // frame an entity first gets a Camera it may not have one yet (the
        // propagation pass that materialises it runs at a different point in the
        // frame for each host). A view over both would silently skip the camera for
        // that frame and the scene would flicker to the previous view.
        reg.CreateView<const Camera>().ForEach(
            [&](Astra::Entity e, const Camera& cam)
        {
            // Task 5 (Phase 4): the projection guard is a no-op for every scene
            // that predates the field (Camera::projection defaults to
            // Orthographic), which is what keeps this function's output
            // byte-identical for all of those. It exists so a Perspective camera
            // -- whose orthographicSize is just whatever default it never
            // touched -- cannot be swept up here and turned into a bogus 2D
            // offset+zoom view; it is ActivePerspectiveSceneCamera's below.
            if (!cam.active || cam.projection != CameraProjection::Orthographic || cam.orthographicSize <= 0.0f)
                return;
            if (found++ != 0)
                return;   // first active wins; keep counting for the caller's warning

            halfH = cam.orthographicSize;
            // Task 3 (F1): the world matrix is a mat4, so the translation is
            // column 3 (it was column 2). The ORTHOGRAPHIC path is otherwise
            // untouched and stays glm::vec2-shaped end to end -- an ortho camera
            // frames the XY plane and has no use for the entity's Z.
            if (const WorldTransform* wt = std::as_const(reg).GetComponent<WorldTransform>(e))
                center = glm::vec2(wt->matrix[3].x, wt->matrix[3].y);   // translation column
            else if (const Transform* lt = std::as_const(reg).GetComponent<Transform>(e))
                center = glm::vec2(lt->position);   // not propagated yet: local IS world for a root
        });

        if (outCount)
            *outCount = found;
        if (found == 0 || viewport.y == 0u || halfH <= 0.0f)
            return std::nullopt;

        SceneCameraView v;
        v.worldCenter = center;
        v.halfHeight  = halfH;
        // The camera's world position lands at the viewport CENTRE and the
        // half-height spans half the viewport, which is what makes the framing
        // resolution-independent (F4 plan 1 T3: the ViewTransform replaces the
        // zoom/offset pair; its AsAffine2D() recovers the same px-per-metre in X
        // and the NEGATED one in Y -- +Y up on a y-down canvas). The depth slab
        // is render.ortho2D.depthRange.
        v.view = Ortho2DView(center, halfH, viewport);
        return v;
    }

    // Task 5 (Phase 4): the perspective sibling. It used to return its own
    // {view, projection} pair; F4 plan 1 T3 made that pair THE camera type --
    // PerspectiveCameraView is now just ViewTransform (view + projection +
    // the viewport it was fitted to), the same type the ortho sweep produces.
    //
    // DEPTH CONVENTION: [0,1], not [-1,1] and not reverse-Z. glm defaults to
    // OpenGL's [-1,1] convention, and GLM_FORCE_DEPTH_ZERO_TO_ONE is not
    // defined anywhere in this tree, so PerspectiveProjection states [0,1] at
    // the call site via glm's explicit *_ZO entry point instead -- a tree-wide
    // define would also silently change every OTHER glm projection call,
    // ortho included, the moment one is added. Reverse-Z is a distinct,
    // separately-decided-against choice (it would additionally flip which of
    // near/far maps to NDC z=0 and swap the depth compare op); PerspectiveCameraTest.cpp
    // pins today's [0,1], forward-Z mapping explicitly so a future reverse-Z
    // switch is a deliberate decision, not a silent drift.
    //
    // HANDEDNESS: right-handed (the camera looks down -Z; +X is right, +Y is
    // up), matching glm's own RH family (perspectiveRH_*/lookAtRH). When this
    // was written there was no PRE-EXISTING 3D handedness to inherit from the
    // ortho path (Transform/WorldTransform were 2D-only) and sprite.hlsl
    // hardcodes clip.z = 0.0, so nothing upstream fixed a convention. Task 3
    // (F1) made Transform 3D, and it adopts THIS convention rather than
    // competing with it: +Z is toward the viewer, so a planar scene sits at
    // z = 0 and a camera looking down -Z sees it. RH is
    // instead FORCED by this task's own pinned property (PerspectiveCameraTest.cpp,
    // the 90-degree-fov case): a view-space point (z, 0, -z) lands exactly on
    // the RIGHT clip plane at fovY=90 deg/aspect=1 only if points in front of the
    // camera have NEGATIVE view-space Z, i.e. the camera looks down -Z (RH).
    // Under a LH camera (looking down +Z), that same point -- Z = -z < 0 --
    // would be BEHIND the camera and could not land on any clip plane of the
    // visible frustum at all.
    using PerspectiveCameraView = ViewTransform;

    // The projection half in isolation: a pure numeric function of the lens
    // parameters alone (no Registry, no Camera, no WorldTransform), so its
    // properties can be pinned directly -- aspect ratio's effect on x only,
    // the near/far -> NDC-z mapping, and the 90-degree-fov clip-plane identity
    // above -- without any ECS scaffolding.
    [[nodiscard]] inline glm::mat4 PerspectiveProjection(float fovYDegrees, float aspectRatio,
                                                          float nearZ, float farZ) noexcept
    {
        return glm::perspectiveRH_ZO(glm::radians(fovYDegrees), aspectRatio, nearZ, farZ);
    }

    // The active PERSPECTIVE camera's view for a `viewport` (pixels; the aspect
    // ratio is derived from it, and a zero height is nullopt), or
    // nullopt under the same "leave the stored camera ALONE" contract
    // ActiveSceneCamera documents above. Perspective cameras are swept
    // SEPARATELY from ActiveSceneCamera (which now skips any camera whose
    // projection is not Orthographic -- see its guard above), so a scene never
    // has to answer "which mode is the ambiguous first camera" -- each mode
    // resolves its own first-active-wins independently.
    //
    // VIEW: eye is the entity's FULL world position (Z included), and
    // orientation is read from the world matrix's own basis columns --
    // forward is the negated Z column, up is the Y column. Both are resolved
    // by the exact same WorldTransform-then-Transform-fallback rule
    // ActiveSceneCamera uses above (so moving OR rotating a camera entity
    // moves this lens the same way regardless of which branch supplied the
    // matrix).
    //
    // Task 3 (F1) NOTE, superseded below -- kept for the history: Transform
    // gained a 3D position and a quaternion in F1, but this lens was
    // deliberately left pinned at forward=(0,0,-1)/up=(0,1,0) anyway, on the
    // reasoning that wiring the real pose in early "would silently re-frame
    // every scene" the moment one gained a non-zero Z or a tilt, and that the
    // task to give a user a way to AIM a camera (F4) should own that change.
    //
    // Task 7 (F2a) NOTE -- that reasoning does not survive contact with this
    // codebase: every authored .arcscene in the tree defaults
    // Camera::projection to Orthographic, and the projection guard on
    // ActiveSceneCamera above (Task 5, Phase 4) already means the ortho and
    // perspective sweeps never contend for the same camera entity -- a scene
    // with no active Perspective camera cannot be re-framed by a change
    // confined to THIS function's math. So the deferral is lifted HERE, for
    // the perspective lens only. ActiveSceneCamera (orthographic) is left
    // deliberately untouched: it frames the XY plane by definition
    // (orthographicSize is a half-HEIGHT, a 2D concept), so an ortho camera's
    // Z and tilt have no ortho-meaningful reading to give them, and nothing
    // about this task changes that. The basis is read by orthonormalizing
    // the world matrix's forward/up columns rather than glm::quat_cast(world)
    // -- the world matrix can carry scale (the camera entity's own, or an
    // ancestor's), and quat_cast on a scaled matrix returns a skewed,
    // non-orthonormal rotation instead of erroring, which would silently mis-
    // aim the lens rather than refusing outright. A basis that orthonormalizes
    // to zero-length (an authored zero scale) falls back to this same
    // pinned forward/up rather than feeding lookAtRH a zero vector -- see the
    // ARC_WARN below for why that fallback is not deduplicated.
    //
    // F4 plan 1 lifted the 2D path onto ViewTransform; the ortho camera still
    // frames XY by definition.
    // `warn` defaults true, so a direct call still reports a degenerate basis
    // every time (the fallback is not deduplicated -- see the ARC_WARN below).
    // ResolveFrameViews passes its own flag through: ExtractFrame warns, and a
    // host that resolves again before DrawUI passes false.
    inline std::optional<ViewTransform> ActivePerspectiveSceneCamera(Astra::Registry& reg,
                                                                     glm::uvec2 viewport,
                                                                     int* outCount = nullptr,
                                                                     bool warn = true)
    {
        int       found = 0;
        glm::mat4 world{1.0f};   // identity: eye at origin, forward -Z, up +Y -- the F1 default
        float     fovYDegrees = 60.0f;
        float     nearZ = 0.1f;
        float     farZ  = 1000.0f;

        reg.CreateView<const Camera>().ForEach(
            [&](Astra::Entity e, const Camera& cam)
        {
            if (!cam.active || cam.projection != CameraProjection::Perspective)
                return;
            if (found++ != 0)
                return;   // first active wins; keep counting for the caller's warning

            fovYDegrees = cam.fovYDegrees;
            nearZ       = cam.nearZ;
            farZ        = cam.farZ;
            // Task 7 (F2a): capture the FULL matrix now, not just its
            // translation column -- same WorldTransform-then-Transform
            // fallback ActiveSceneCamera uses above.
            if (const WorldTransform* wt = std::as_const(reg).GetComponent<WorldTransform>(e))
                world = wt->matrix;
            else if (const Transform* lt = std::as_const(reg).GetComponent<Transform>(e))
                world = lt->ToMatrix();   // not propagated yet: local IS world for a root
        });

        if (outCount)
            *outCount = found;
        if (found == 0 || viewport.y == 0u || viewport.x == 0u || nearZ <= 0.0f || farZ <= nearZ)
            return std::nullopt;
        const float aspectRatio = float(viewport.x) / float(viewport.y);

        ViewTransform v;
        v.viewport = viewport;

        // Full-basis read (Task 7, F2a). Orthonormalize the columns rather than
        // glm::quat_cast(world): see the VIEW comment above for why a scaled
        // world matrix makes quat_cast unsafe here.
        glm::vec3 eye     = glm::vec3(world[3]);
        glm::vec3 forward = -glm::vec3(world[2]);
        glm::vec3 up      =  glm::vec3(world[1]);
        const float fLen = glm::length(forward);
        const float uLen = glm::length(up);
        if (fLen < 1e-6f || uLen < 1e-6f)
        {
            // A singular basis (zero scale) has no direction to read. lookAtRH would
            // divide by zero and hand every subsequent pass a NaN clip position, which
            // is undefined behaviour on the GPU rather than a wrong picture -- so fall
            // back to F1's pinned orientation and say so.
            //
            // Deliberately NOT deduplicated the way MeshCache::Request's `failed` set
            // or OutlineNode's m_warnedIdOverflow bool dedupe theirs: those live on a
            // long-lived cache/node object that can hold an "already told you" flag
            // across calls, and this function is deliberately NOT that (see the
            // file-top comment -- pure function of (registry, viewport), no host, no
            // state, three independent callers per frame). A degenerate camera basis
            // is also an authored bug (a zero scale), not a transient miss like a
            // still-loading asset, so re-asserting it every frame it persists is an
            // accepted cost, not an oversight -- the alternative (process-global
            // mutable warned-state in a header-only "pure" function) would leak across
            // every Registry that calls this, including unrelated tests in the same
            // binary. `warn` is the caller's opt-out, not a latch: false skips the
            // line and still takes the fallback.
            if (warn)
            {
                ARC_WARN("camera: entity has a degenerate basis (zero scale?) -- "
                         "falling back to forward -Z / up +Y");
            }
            forward = glm::vec3(0.0f, 0.0f, -1.0f);
            up      = glm::vec3(0.0f, 1.0f,  0.0f);
        }
        else
        {
            forward /= fLen;
            up      /= uLen;
        }
        v.view       = glm::lookAtRH(eye, eye + forward, up);
        v.projection = PerspectiveProjection(fovYDegrees, aspectRatio, nearZ, farZ);
        return v;
    }
}
