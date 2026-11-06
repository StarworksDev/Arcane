#pragma once
#include <Arcane/Core/ModuleApi.hpp>

// The export macro for PhysicsSystem2D::operator(), whose body lives in the 2D
// solver module. Scene cannot include that module's headers, so the macro is
// spelled here from the same build defines (the solver module builds with
// ARC_PHYSICS2D_EXPORTS; ARC_PHYSICS2D_STATIC / ARC_MONOLITHIC build no DLL).
#if defined(ARC_MONOLITHIC) || defined(ARC_PHYSICS2D_STATIC)
    #define ARC_PHYSICS_SYSTEM2D_API
#elif defined(ARC_PHYSICS2D_EXPORTS)
    #define ARC_PHYSICS_SYSTEM2D_API ARC_DLL_EXPORT
#else
    #define ARC_PHYSICS_SYSTEM2D_API ARC_DLL_IMPORT
#endif
