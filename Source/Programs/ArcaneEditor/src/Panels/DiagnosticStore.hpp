#pragma once

// The store lives in ArcaneCore (Arcane/Base/DiagnosticStore.hpp). This header
// keeps the Arcane::Editor names so the panels do not change includes.

#include <Arcane/Base/DiagnosticStore.hpp>

namespace Arcane::Editor
{
    using DiagnosticStore = Arcane::DiagnosticStore;
    using SeverityMask = Arcane::SeverityMask;
    using Arcane::operator|;
    using Arcane::MaskOf;
    using Arcane::MaskFrom;
    using Arcane::MatchesDiagnosticFilter;
}
