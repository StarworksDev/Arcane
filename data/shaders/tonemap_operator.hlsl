// THE TONEMAP OPERATOR, shared (F5 plan 1 Task 3): ACES filmic + the true
// sRGB encode, cut byte for byte from tonemap.hlsl so the scene tonemap
// (tonemap.hlsl) and the post-tonemap overlay pass (overlay.hlsl) run ONE
// operator. A Linear overlay colour goes through exactly these two functions
// and nothing else -- no exposure term, no post chain -- so a drift here moves
// world pixels AND overlay pixels together.
//
// Included, never compiled on its own (compile-shaders.bat lists entry points,
// and this file has none). The CPU mirrors are listed in tonemap.hlsl's header.

// Narkowicz ACES filmic approximation (linear in -> [0,1] out).
float3 ACESFilmic(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Branchless sRGB encode, UE's form: below the knee the linear segment is the
// smaller value, above it the power segment is, so min() selects correctly and
// is continuous at 0.0031308. Input here is already saturated to [0,1] by
// ACESFilmic, so no negative-base pow is reachable.
float3 LinearToSrgb(float3 lin)
{
    return min(lin * 12.92, pow(max(lin, 0.0031308), 1.0 / 2.4) * 1.055 - 0.055);
}
