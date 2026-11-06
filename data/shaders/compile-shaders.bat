@echo off
:: Compiles every engine shader entry point to DXIL + SPIR-V loose artifacts.
:: Invoked by the premake prebuild step on the Arcane project; also runnable
:: by hand for the dev loop, with ARCANE_SHADER_DIR pointing at generated\
:: (Arcane/Render/ShaderPaths.hpp is what reads it). The artifact hot-reload
:: this line used to name, ShaderLibrary::Poll, is GONE -- that class was
:: deleted at NRI Phase 5a Task 9.5b-ii.
::
:: SPIR-V register shifts MUST match nvrhi::VulkanBindingOffsets defaults
:: (t=0, s=128, b=256, u=384). ShaderMake (vendored) replaces this script
:: when the shader count outgrows explicit lines.
::
:: SHARED TRUTH: Source/Client/ArcaneRender/src/Arcane/Render/ShaderConventions.hpp holds the
:: canonical shift flags + entry/profile conventions for the runtime compile
:: service (ShaderCompiler). SPIRV_FLAGS below must mirror kSpirvArgs there.
::
:: THE SHIFT IS PER (TYPE, SPACE), NOT GLOBAL PER TYPE -- see
:: ShaderConventions.hpp's header comment for the full account. mesh.hlsl
:: (Task 8/10) is the first source with registers outside space0 (b1 moved
:: to space1, the bindless material array sits at t0/space2), which is why
:: SPIRV_FLAGS below carries extra shift pairs beyond space0's: b/space1 and
:: t/space2 (Task 8/10), and t/space1 (F3 plan 1 T7 -- mesh.hlsl's instance
:: and visible-index SRVs at t0/t1 in space1 land at set 1 bindings 0/1,
:: beside b1's 257).
::
:: INVARIANT: every output artifact's entry point is explicit in this file and
:: must match the NRI loader's entryPointName. Most use <type>_main; the fixed
:: mesh pixel variants deliberately use distinct source entry points while all
:: retain the conventional _ps output suffix.
setlocal
set DXC=%~dp0..\..\ThirdParty\tools\dxc\dxc.exe
set SRC=%~dp0
set OUT=%~dp0generated
if not exist "%OUT%\dxil"  mkdir "%OUT%\dxil"
if not exist "%OUT%\spirv" mkdir "%OUT%\spirv"

set SPIRV_FLAGS=-spirv -D SPIRV=1 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 -fvk-b-shift 256 1 -fvk-t-shift 0 2 -fvk-t-shift 0 1 -fvk-u-shift 384 1

call :compile sprite  vs_main vs_6_5 sprite_vs  || exit /b 1
call :compile sprite  ps_main ps_6_5 sprite_ps  || exit /b 1
call :compile circle  vs_main vs_6_5 circle_vs  || exit /b 1
call :compile circle  ps_main ps_6_5 circle_ps  || exit /b 1
call :compile msdf    vs_main vs_6_5 msdf_vs    || exit /b 1
call :compile msdf    ps_main ps_6_5 msdf_ps    || exit /b 1
call :compile imgui   vs_main vs_6_5 imgui_vs   || exit /b 1
call :compile imgui   ps_main ps_6_5 imgui_ps   || exit /b 1
call :compile tonemap vs_main vs_6_5 tonemap_vs || exit /b 1
call :compile tonemap ps_main ps_6_5 tonemap_ps || exit /b 1
call :compile entity_id vs_main vs_6_5 entity_id_vs || exit /b 1
call :compile entity_id ps_main ps_6_5 entity_id_ps || exit /b 1
:: The id pass's MESH half (F4 plan 2, spec s7.1) -- MeshVertex in, id out, depth-tested inside the pick pass.
call :compile entity_id_mesh vs_main vs_6_5 entity_id_mesh_vs || exit /b 1
call :compile entity_id_mesh ps_main ps_6_5 entity_id_mesh_ps || exit /b 1
call :compile outline_seed vs_main vs_6_5 outline_seed_vs || exit /b 1
call :compile outline_seed ps_main ps_6_5 outline_seed_ps || exit /b 1
call :compile outline_jfa vs_main vs_6_5 outline_jfa_vs || exit /b 1
call :compile outline_jfa ps_main ps_6_5 outline_jfa_ps || exit /b 1
call :compile outline_composite vs_main vs_6_5 outline_composite_vs || exit /b 1
call :compile outline_composite ps_main ps_6_5 outline_composite_ps || exit /b 1
:: The opaque 3D pass (NRI Phase 4, Task 7) -- Lambert + one albedo texture.
call :compile mesh vs_main vs_6_5 mesh_vs || exit /b 1
call :compile mesh ps_main ps_6_5 mesh_ps || exit /b 1
call :compile mesh ps_masked_main ps_6_5 mesh_masked_ps || exit /b 1
call :compile mesh ps_transparent_main ps_6_5 mesh_transparent_ps || exit /b 1
:: F3 fine cull: UAV visible indices + indirect argument counters.
call :compile mesh_cull cs_main cs_6_5 mesh_cull_cs || exit /b 1
:: The 3D reference grid (F4 plan 1 Task 10, spec s5.2) -- analytic, depth-tested after the mesh pass.
call :compile grid vs_main vs_6_5 grid_vs || exit /b 1
call :compile grid ps_main ps_6_5 grid_ps || exit /b 1
:: The post-tonemap overlay pass (F5 plan 1 Task 3) -- lines/fills and the folded 3D grid, after the tonemap, sampling scene depth.
call :compile overlay vs_main vs_6_5 overlay_vs || exit /b 1
call :compile overlay ps_main ps_6_5 overlay_ps || exit /b 1
call :compile overlay grid_vs_main vs_6_5 overlay_grid_vs || exit /b 1
call :compile overlay grid_ps_main ps_6_5 overlay_grid_ps || exit /b 1
:: graph_grid_vs/_ps were emitted here until the NRI Phase 5a final-review fix
:: wave. Their only loader was GraphGridPass, deleted at Task 9.5a; the shader
:: graph's backdrop is now drawn with ImGui primitives
:: (Source/Programs/ArcaneEditor/src/Widgets/GraphGridPhase.hpp, DrawGraphGridFallback), so
:: graph_grid.hlsl was deleted with these two lines.
:: The deliberate GPU-fault dispatch (GpuFaultInjector.hpp) -- the ONLY compute
:: entry point in the tree, and not part of any render path. Built in every
:: configuration because this script has no configuration awareness; the code
:: that loads it is compiled out of Dist, so the artifact is inert there.
call :compile gpu_fault cs_main cs_6_5 gpu_fault_cs || exit /b 1
echo Shaders compiled to %OUT%
exit /b 0

:compile
"%DXC%" -T %3 -E %2 -Fo "%OUT%\dxil\%4.bin" "%SRC%%1.hlsl" || exit /b 1
"%DXC%" -T %3 -E %2 %SPIRV_FLAGS% -Fo "%OUT%\spirv\%4.bin" "%SRC%%1.hlsl" || exit /b 1
exit /b 0
