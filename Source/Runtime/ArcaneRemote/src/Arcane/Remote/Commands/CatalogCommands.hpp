#pragma once
// catalog.describe and schema.type (spec s5.3; Servitor P0 Task 17, ruling
// F13): the host side of `servitor describe` and `servitor schema`. They read
// only the process CommandRegistry and Astra's meta registry, so they hold no
// pointer and need no session; HostSession::Start registers them on every host.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// Registers, once per process (a second call is a no-op), two Observe host
// commands that run on the main thread with needsFrame false:
//   catalog.describe {name}   -> data {name, summary, access, mutates, play,
//                                thread, needsFrame, module, schema: SchemaOf(name)};
//                                an unregistered name is command.unknown (exit 2)
//   schema.type {kind, name}  -> kind "component" or "resource"; data is
//                                SchemaFor(*Astra::GetMetaByName(name)); a component
//                                may be named short (Transform) or qualified
//                                (Arcane::Transform), resolved by PickComponent
//                                over the types registered as components
//                                (EcsCommands.hpp; two short hits are
//                                component.ambiguous, exit 2); an unknown name is
//                                component.unknown / resource.not-found (exit 6)
ARC_REMOTE_API void RegisterCatalogCommands();

} // namespace Arcane::Remote

#endif
