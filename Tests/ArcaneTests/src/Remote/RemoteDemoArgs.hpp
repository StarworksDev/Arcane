#pragma once
#include <Arcane/Reflection.hpp>
#include <glm/vec3.hpp>

#if !defined(ARC_BUILD_DIST)

struct RemoteDemoArgs {
    int n = 3;
    glm::vec3 position{0.f, 0.f, 0.f};
};
ARC_REFLECT_TYPE(RemoteDemoArgs)
    ARC_REFLECT_FIELD(RemoteDemoArgs, n)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(RemoteDemoArgs, position)
ARC_REFLECT_TYPE_END()

#endif
