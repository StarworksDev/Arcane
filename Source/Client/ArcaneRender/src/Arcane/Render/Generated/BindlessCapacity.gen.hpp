#pragma once
#include <cstdint>
#include <Arcane/Core/Constant.hpp>
namespace Arcane {
    ARC_CONSTANT("shader/GPU contract: bindless material table capacity; source of truth is build/engine-constants.lua")
    inline constexpr std::uint32_t kBindlessCapacity = 256;
}
