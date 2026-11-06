#pragma once
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Astra/Reflection/TypeMeta.hpp>
#include <Json.hpp>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// The JSON Schema (draft 2020-12) of a reflected args struct (ruling P2 (b)).
// Astra's JsonSchemaGenerator produces the shape; a default-constructible type
// gets a "default" on each property, written by Arcane::ReflectionJsonWriter
// from one default-constructed instance; glm vectors, quaternions and matrices
// are number arrays, as the writer emits them.
ARC_REMOTE_API nlohmann::json SchemaFor(const Astra::TypeMeta& meta);

} // namespace Arcane::Remote

#endif
