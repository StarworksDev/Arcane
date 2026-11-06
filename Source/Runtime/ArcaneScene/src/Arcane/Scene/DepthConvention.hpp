#pragma once

#include <cstdint>

namespace Arcane
{
    enum class DepthConvention : std::uint8_t
    {
        ForwardZ,
        ReverseZ,
    };

    inline constexpr DepthConvention kSelectedDepthConvention = DepthConvention::ForwardZ;
}
