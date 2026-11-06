#pragma once

#include <Arcane/Scene/DepthConvention.hpp>

#include <NRIDescs.h>

namespace Arcane
{
    [[nodiscard]] inline float DepthClearValue(DepthConvention convention) noexcept
    {
        return convention == DepthConvention::ReverseZ ? 0.0f : 1.0f;
    }

    [[nodiscard]] inline nri::CompareOp NearerWinsOp(DepthConvention convention) noexcept
    {
        return convention == DepthConvention::ReverseZ ? nri::CompareOp::GREATER
                                                       : nri::CompareOp::LESS;
    }

    [[nodiscard]] inline nri::CompareOp NearerOrEqualOp(DepthConvention convention) noexcept
    {
        return convention == DepthConvention::ReverseZ ? nri::CompareOp::GREATER_EQUAL
                                                       : nri::CompareOp::LESS_EQUAL;
    }
}
