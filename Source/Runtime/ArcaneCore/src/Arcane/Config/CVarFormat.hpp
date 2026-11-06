#pragma once

// The text forms of cvar values (settings spec 2026-10-03 s4.1, s4.8): what
// the console prints and parses (--set goes through the console's parser),
// and the "#RRGGBBAA" colour hex the JSON files use.

#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Core/Api.hpp>
#include <Json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    // "#RRGGBBAA", upper case. RGB are linear floats encoded through the sRGB
    // transfer and rounded to 8 bits; alpha is linear. Channels outside
    // [0,1] clamp. Decode(Encode(byte)) is exact for every byte value.
    ARC_CORE_API std::string CVarColorToHex(const CVarColor& linear);
    // "#RRGGBB" (alpha 1) or "#RRGGBBAA", either case. nullopt otherwise.
    ARC_CORE_API std::optional<CVarColor> CVarColorFromHex(std::string_view text);
    // Equal within one 8-bit step per channel, in the hex's own encoding
    // (sRGB for RGB, linear for alpha; outside [0,1] clamps as the hex does).
    // What "is default" compares with -- the Modified filter and the reset
    // arrow -- because a linear colour that went through a file's hex is not
    // bit-identical to the default it was picked at (settings plan Review
    // Focus 3).
    ARC_CORE_API bool CVarColorNearlyEqual(const CVarColor& a, const CVarColor& b);

    // The console's spelling. Bool "true"/"false"; integers decimal;
    // Float32/Float64 std::to_string (v1's form, unchanged); Vec* components
    // space-separated in shortest round-trip form; Color as hex; Enum as its
    // name, or the ordinal's digits when it is outside `enumNames`.
    ARC_CORE_API std::string FormatCVarValue(const CVarValue& value, const std::vector<std::string>& enumNames = {});

    // The value as JSON: bool -> boolean, integers/floats -> number, string ->
    // string, Vec2/3/4 -> number arrays of length 2/3/4. Color, Enum and any
    // other type -> the FormatCVarValue text as a JSON string.
    ARC_CORE_API nlohmann::json CVarValueToJson(const CVarValue& value);

    // Parse console / --set text as `type`. On failure: nullopt, and `error`
    // holds the reason the console shows ("expected int32", ...).
    //   Vec2/3/4: exactly N finite floats, separated by spaces and/or commas,
    //             optionally in [ ].
    //   Color:    "#RRGGBB", "#RRGGBBAA", or 3 or 4 finite linear floats.
    //   Enum:     an exact name from `enumNames`, or a decimal ordinal inside it.
    //   Float32/Float64: the whole token as one finite number; "nan", "inf"
    //             and trailing text ("1.5abc") are refused (settings plan
    //             Review Focus 3). Bool, the integers and String parse
    //             exactly as v1's Execute did.
    ARC_CORE_API std::optional<CVarValue> ParseCVarText(std::string_view text, CVarType type,
                                                           const std::vector<std::string>& enumNames, std::string& error);

    // The name's ordinal in `enumNames`, or nullopt.
    ARC_CORE_API std::optional<std::int32_t> CVarEnumOrdinal(const std::vector<std::string>& enumNames, std::string_view name);

    // "bool", "int32", "uint32", "int64", "uint64", "float", "double",
    // "string", "color", "vec2", "vec3", "vec4", "enum".
    ARC_CORE_API const char* CVarTypeName(CVarType type);

    // The settings windows' label for a cvar, from its last dotted segment:
    // "fitMinZoom" -> "Fit Min Zoom", "byteBudgetMB" -> "Byte Budget MB",
    // "HTTPPort" -> "HTTP Port", "grid3D" -> "Grid3D", "max_players" -> "Max Players".
    ARC_CORE_API std::string DeriveCVarDisplayName(std::string_view cvarName);
    // Every segment but the last, through the same word split, joined by '/':
    // "physics.solver.substeps" -> "Physics/Solver". A dot-less name -> "General".
    ARC_CORE_API std::string DeriveCVarCategoryPath(std::string_view cvarName);
}
