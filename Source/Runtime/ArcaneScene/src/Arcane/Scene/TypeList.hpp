#pragma once

namespace Arcane
{
    // A bare compile-time type list. Astra has none of its own, and this needs
    // to be nothing more than a pack the roster expanders can unpack. Header-only,
    // no macro: Scene names its physics roster with it without including Engine.
    template<typename... Ts>
    struct TypeList {};
}
