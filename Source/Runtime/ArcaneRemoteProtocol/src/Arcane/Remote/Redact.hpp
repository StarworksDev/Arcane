#pragma once
#include <Arcane/Modules/ArcaneRemoteProtocolApi.hpp>
#include <Arcane/Core/Constant.hpp>
#include <cstddef>
#include <string>
#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

ARC_CONSTANT("wire protocol: a bearer token is 64 hex chars")
inline constexpr std::size_t kTokenHexChars = 64;

// Replaces every occurrence of tokenHex, and every "Bearer " followed by 64+ hex chars, with <redacted>.
ARC_REMOTE_PROTOCOL_API std::string RedactSecrets(std::string_view text, std::string_view tokenHex);

} // namespace Arcane::Remote

#endif
