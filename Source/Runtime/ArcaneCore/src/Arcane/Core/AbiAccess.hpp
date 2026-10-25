#pragma once

// Forward declarations of the ABI manifest's accessors. A class whose private
// members game code reaches through inline accessors befriends one:
//     friend struct ::Arcane::Detail::AbiManifest::Access;        // Engine group
//     friend struct ::Arcane::Detail::AbiManifest::ClientAccess;  // Client group
//     friend struct ::Arcane::Detail::AbiManifest::Physics2DAccess;  // Physics2D group
// so the group manifest (Plugin/AbiManifestEngine.hpp, Client/AbiManifestClient.hpp,
// Physics2D/AbiManifestPhysics2D.hpp)
// can hash those members' offsets and sizes. This is the only thing the friend
// grants; the accessors have no behaviour (they hold constexpr hashing
// functions evaluated at compile time). Each group defines its own, so the
// Engine manifest needs no Client or Physics2D header.

namespace Arcane::Detail::AbiManifest { struct Access; struct ClientAccess; struct Physics2DAccess; }
