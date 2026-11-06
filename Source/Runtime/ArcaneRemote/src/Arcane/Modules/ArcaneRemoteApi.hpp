#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_REMOTE_STATIC)
    #define ARC_REMOTE_API
#elif defined(ARC_REMOTE_EXPORTS)
    #define ARC_REMOTE_API ARC_DLL_EXPORT
#else
    #define ARC_REMOTE_API ARC_DLL_IMPORT
#endif
