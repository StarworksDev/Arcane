#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Util/CharConv.hpp>   // FromChars: std::from_chars incl. floating point on every standard library

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Material/GlobalParams.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>

namespace Arcane
{
    namespace
    {
        constexpr std::string_view kParamPrefix = "//@param";

        // Names the engine templates already own at global scope -- a param
        // with one of these would shadow or collide in the stitched source.
        // SpriteTexture belongs to the sprite template only, but reserving it
        // everywhere keeps a snippet portable across surfaces.
        constexpr std::string_view kReservedNames[] = {
            "Time", "DeltaTime", "ViewportSize", "MaterialSampler", "SpriteTexture",
            // Pass chains: upstream pass outputs (slot k of the pass's inputs).
            "InputTexture", "InputTexture1", "InputTexture2", "InputTexture3",
            "displace",   // the vertex-stage hook function
        };

        // The %{VERTEX_BODY} default: identity. A material's vertexSnippet
        // replaces this wholesale (same slot both templates).
        constexpr std::string_view kVertexPassthrough =
            "Varyings displace(Varyings v) { return v; }";

        std::string_view TrimView(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        }

        std::string_view TakeToken(std::string_view& s)
        {
            s = TrimView(s);
            std::size_t end = 0;
            while (end < s.size() && s[end] != ' ' && s[end] != '\t')
                ++end;
            std::string_view tok = s.substr(0, end);
            s.remove_prefix(end);
            return tok;
        }

        bool ParseFloat(std::string_view s, float& out)
        {
            s = TrimView(s);
            if (s.empty())
                return false;
            const auto r = Arcane::FromChars(s.data(), s.data() + s.size(), out);
            return r.ec == std::errc() && r.ptr == s.data() + s.size();
        }

        // "(a, b, c)" -> components. False on malformed text or wrong count.
        bool ParseTuple(std::string_view s, float* out, int minCount, int maxCount, int& count)
        {
            s = TrimView(s);
            if (s.size() < 2 || s.front() != '(' || s.back() != ')')
                return false;
            s = s.substr(1, s.size() - 2);

            count = 0;
            while (true)
            {
                const std::size_t comma = s.find(',');
                const std::string_view part = comma == std::string_view::npos ? s : s.substr(0, comma);
                if (count >= maxCount || !ParseFloat(part, out[count]))
                    return false;
                ++count;
                if (comma == std::string_view::npos)
                    break;
                s.remove_prefix(comma + 1);
            }
            return count >= minCount;
        }

        bool ValidIdentifier(std::string_view name)
        {
            if (name.empty())
                return false;
            auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
            auto digit = [](char c) { return c >= '0' && c <= '9'; };
            if (!alpha(name.front()))
                return false;
            for (char c : name)
                if (!alpha(c) && !digit(c))
                    return false;
            return true;
        }

        bool TypeFromToken(std::string_view tok, MatParamType& out)
        {
            if (tok == "float")   { out = MatParamType::Float;   return true; }
            if (tok == "float2")  { out = MatParamType::Float2;  return true; }
            if (tok == "float4")  { out = MatParamType::Float4;  return true; }
            if (tok == "color")   { out = MatParamType::Color;   return true; }
            if (tok == "texture") { out = MatParamType::Texture; return true; }
            return false;
        }

        const char* HlslType(MatParamType t)
        {
            switch (t)
            {
                case MatParamType::Float:  return "float";
                case MatParamType::Float2: return "float2";
                case MatParamType::Float4: return "float4";
                case MatParamType::Color:  return "float4";
                case MatParamType::Texture: break;
            }
            return "float4";
        }

        std::uint64_t Fnv64Str(std::uint64_t h, std::string_view s) noexcept
        {
            for (char c : s)
            {
                h ^= static_cast<std::uint8_t>(c);
                h *= 1099511628211ull;
            }
            return h;
        }
    }

    MaterialSourceParse ParseMaterialSource(std::string_view snippet)
    {
        MaterialSourceParse out;

        int lineNo = 0;
        std::size_t pos = 0;
        while (pos <= snippet.size())
        {
            ++lineNo;
            std::size_t end = snippet.find('\n', pos);
            if (end == std::string_view::npos)
                end = snippet.size();
            std::string_view line = TrimView(snippet.substr(pos, end - pos));
            const bool lastLine = end == snippet.size();
            pos = end + 1;

            if (line.substr(0, kParamPrefix.size()) != kParamPrefix)
            {
                if (lastLine)
                    break;
                continue;
            }

            auto fail = [&](const std::string& msg)
            {
                out.errors.push_back("line " + std::to_string(lineNo) + ": " + msg);
            };

            std::string_view rest = line.substr(kParamPrefix.size());
            // Require whitespace after the prefix so `//@paramx` is not a decl.
            if (!rest.empty() && rest.front() != ' ' && rest.front() != '\t')
            {
                fail("expected whitespace after //@param");
                if (lastLine) break; else continue;
            }

            ParamDecl d;
            ParamMeta meta;

            const std::string_view typeTok = TakeToken(rest);
            if (!TypeFromToken(typeTok, d.type))
            {
                fail("unknown param type '" + std::string(typeTok) +
                     "' (float|float2|float4|color|texture)");
                if (lastLine) break; else continue;
            }

            const std::string_view nameTok = TakeToken(rest);
            if (!ValidIdentifier(nameTok))
            {
                fail("invalid param name '" + std::string(nameTok) + "'");
                if (lastLine) break; else continue;
            }
            d.name = std::string(nameTok);

            bool bad = false;
            for (std::string_view reserved : kReservedNames)
            {
                if (nameTok == reserved)
                {
                    fail("'" + d.name + "' is reserved by the material template");
                    bad = true;
                    break;
                }
            }
            for (const ParamDecl& prev : out.decls)
            {
                if (prev.name == d.name)
                {
                    fail("duplicate param '" + d.name + "'");
                    bad = true;
                    break;
                }
            }
            if (bad)
            {
                if (lastLine) break; else continue;
            }

            // Optional `= <default>`; the range `[min..max]` may follow it.
            rest = TrimView(rest);
            if (!rest.empty() && rest.front() == '=')
            {
                rest.remove_prefix(1);
                rest = TrimView(rest);
                // The default runs until the range bracket (if any).
                const std::size_t bracket = rest.find('[');
                std::string_view defText = TrimView(
                    bracket == std::string_view::npos ? rest : rest.substr(0, bracket));
                rest = bracket == std::string_view::npos ? std::string_view{}
                                                         : rest.substr(bracket);

                float f[4] = {0, 0, 0, 0};
                int n = 0;
                bool ok = false;
                switch (d.type)
                {
                    case MatParamType::Float:
                        ok = ParseFloat(defText, f[0]);
                        d.def = MatParamValue::MakeFloat(f[0]);
                        break;
                    case MatParamType::Float2:
                        ok = ParseTuple(defText, f, 2, 2, n);
                        d.def = MatParamValue::MakeFloat2(f[0], f[1]);
                        break;
                    case MatParamType::Float4:
                        ok = ParseTuple(defText, f, 4, 4, n);
                        d.def = MatParamValue::MakeFloat4(f[0], f[1], f[2], f[3]);
                        break;
                    case MatParamType::Color:
                        ok = ParseTuple(defText, f, 3, 4, n);
                        d.def = MatParamValue::MakeColor(f[0], f[1], f[2], n == 4 ? f[3] : 1.0f);
                        break;
                    case MatParamType::Texture:
                        ok = false;   // texture defaults bind through the instance
                        break;
                }
                if (!ok)
                {
                    fail("bad default '" + std::string(defText) + "' for " +
                         std::string(typeTok) + " '" + d.name + "'");
                    if (lastLine) break; else continue;
                }
            }
            else if (d.type == MatParamType::Texture)
            {
                d.def = MatParamValue::MakeTexture(Guid::Nil());
            }
            else
            {
                // No default: zeroes of the declared type.
                MatParamValue zero;
                zero.type = d.type;
                d.def = zero;
            }

            // Optional `[min..max]` slider hint.
            rest = TrimView(rest);
            if (!rest.empty() && rest.front() == '[')
            {
                if (rest.back() != ']')
                {
                    fail("unterminated range on '" + d.name + "'");
                    if (lastLine) break; else continue;
                }
                const std::string_view range = rest.substr(1, rest.size() - 2);
                const std::size_t dots = range.find("..");
                float lo = 0.0f, hi = 0.0f;
                if (d.type == MatParamType::Texture ||
                    dots == std::string_view::npos ||
                    !ParseFloat(range.substr(0, dots), lo) ||
                    !ParseFloat(range.substr(dots + 2), hi) || hi < lo)
                {
                    fail("bad range on '" + d.name + "' (expected [min..max])");
                    if (lastLine) break; else continue;
                }
                meta.sliderMin = lo;
                meta.sliderMax = hi;
                rest = {};   // the whole remainder WAS the range
            }

            // Anything left is a malformed decl the author meant to matter --
            // the classic `//@param float Speed 2.0` (forgotten '=') must not
            // silently declare Speed = 0.
            rest = TrimView(rest);
            if (!rest.empty())
            {
                fail("unexpected trailing text '" + std::string(rest) + "' after '" +
                     d.name + "' (missing '='?)");
                if (lastLine) break; else continue;
            }

            out.decls.push_back(std::move(d));
            out.metas.push_back(std::move(meta));
            if (lastLine)
                break;
        }

        return out;
    }

    const char* MaterialTemplateFile(MaterialSurface surface)
    {
        // Mesh has no template file yet (see MaterialSurface's own comment in
        // MaterialSource.hpp). F2b Task 13 gave mesh materials an authoring
        // path (CreateMaterialAt's surface argument) and an open path
        // (ShaderEditorDocument now loads them), which made this arm
        // REACHABLE for the first time via Rebuild() -- closed there instead
        // of here: Rebuild() checks `SurfaceOf(m_surface) == Mesh` before
        // ever calling this function, and publishes the mesh-authoring story
        // through the toolbar ("not compiled here") rather than through this
        // guard. Without a guard HERE too, a FUTURE caller that starts
        // calling this directly for Mesh would silently get the Fullscreen
        // template back: a wrong answer that compiles clean and renders
        // wrong, which is precisely the defect class this spec refuses
        // elsewhere. ARC_ENSURE reports it loudly (once per call site) while
        // still returning a file so an unattended build keeps running; this
        // closes when F2b/Task 8 gives Mesh a real template.
        ARC_ENSURE(surface != MaterialSurface::Mesh,
                   "MaterialTemplateFile: MaterialSurface::Mesh has no template file yet "
                   "(F2b/Task 8) -- falling back to the fullscreen template");
        return surface == MaterialSurface::Sprite
                   ? "materials/sprite_material.hlsl"
                   : "materials/fullscreen_material.hlsl";
    }

    MaterialSurface MaterialSurfaceForKind(std::string_view kind)
    {
        if (kind == "sprite") return MaterialSurface::Sprite;
        if (kind == "mesh")   return MaterialSurface::Mesh;
        return MaterialSurface::Fullscreen;
    }

    std::string GenerateMaterialBindings(const MaterialTemplate& templ,
                                         MaterialSurface surface,
                                         std::uint32_t chainInputs)
    {
        // Same guard as MaterialTemplateFile above, and for the same reason
        // (including the F2b Task 13 reachability note there): Mesh has no
        // register map (cbuffer slot / texture base) yet, and the
        // `sprite`-only branch below would otherwise silently emit Fullscreen
        // bindings for it. Closes alongside MaterialTemplateFile's guard.
        ARC_ENSURE(surface != MaterialSurface::Mesh,
                   "GenerateMaterialBindings: MaterialSurface::Mesh has no register map yet "
                   "(F2b/Task 8) -- falling back to the fullscreen bindings");

        // Members emit in declaration order: HLSL's cbuffer packing applies the
        // same rules MaterialTemplate::Build used, so the shader's offsets match
        // the CPU layout byte for byte. Keep the two in lockstep. Register
        // assignments follow the surface map in GlobalParams.hpp.
        const bool sprite = surface == MaterialSurface::Sprite;
        const std::uint32_t cbSlot = sprite ? kSpriteMaterialCbSlot : kMaterialCbSlot;
        const std::uint32_t texBase = sprite ? kSpriteMaterialTextureBase : 0;
        std::string outText;

        bool anyNumeric = false;
        for (const ParamDecl& d : templ.Params())
            anyNumeric = anyNumeric || d.type != MatParamType::Texture;

        if (anyNumeric)
        {
            outText += "cbuffer Material : register(b" + std::to_string(cbSlot) + ")\n{\n";
            for (const ParamDecl& d : templ.Params())
            {
                if (d.type == MatParamType::Texture)
                    continue;
                outText += "    ";
                outText += HlslType(d.type);
                outText += ' ';
                outText += d.name;
                outText += ";\n";
            }
            outText += "};\n";
        }

        bool anyTexture = false;
        std::uint32_t textureCount = 0;
        for (const ParamDecl& d : templ.Params())
        {
            if (d.type != MatParamType::Texture)
                continue;
            outText += "Texture2D " + d.name + " : register(t" +
                       std::to_string(d.textureIndex + texBase) + ");\n";
            anyTexture = true;
            ++textureCount;
        }
        // Pass chains (fullscreen only): upstream pass outputs, at the slots
        // after the material's own textures. Slot 0 keeps the bare
        // "InputTexture" spelling (the pre-DAG name).
        if (!sprite)
            for (std::uint32_t i = 0; i < chainInputs && i < kMaxPassInputs; ++i)
                outText += "Texture2D InputTexture" + (i ? std::to_string(i) : "") +
                           " : register(t" +
                           std::to_string(textureCount + texBase + i) + ");\n";
        // The sprite template declares MaterialSampler itself (SpriteTexture
        // always needs it); emitting it here too would redeclare s0. Chains
        // always need it (the input textures sample through it).
        if (!sprite && (anyTexture || chainInputs > 0))
            outText += "SamplerState MaterialSampler : register(s0);\n";

        return outText;
    }

    std::string StitchShaderTemplate(
        std::string_view templateText,
        std::span<const std::pair<std::string_view, std::string_view>> slots,
        std::vector<std::string>* unresolved)
    {
        std::string outText;
        outText.reserve(templateText.size());

        std::size_t pos = 0;
        while (pos < templateText.size())
        {
            const std::size_t open = templateText.find("%{", pos);
            if (open == std::string_view::npos)
            {
                outText.append(templateText.substr(pos));
                break;
            }
            const std::size_t close = templateText.find('}', open + 2);
            if (close == std::string_view::npos)
            {
                outText.append(templateText.substr(pos));
                break;
            }

            outText.append(templateText.substr(pos, open - pos));
            const std::string_view name = templateText.substr(open + 2, close - open - 2);

            bool found = false;
            for (const auto& [slotName, value] : slots)
            {
                if (slotName == name)
                {
                    outText.append(value);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                outText.append(templateText.substr(open, close - open + 1));
                if (unresolved)
                    unresolved->push_back(std::string(name));
            }
            pos = close + 1;
        }
        return outText;
    }

    MaterialBuildResult BuildMaterialShaderSource(std::string_view templateText,
                                                  std::string_view snippet,
                                                  std::string materialName,
                                                  MaterialSurface surface,
                                                  std::string_view vertexSnippet,
                                                  std::uint32_t chainInputs)
    {
        MaterialBuildResult r;

        MaterialSourceParse parsed = ParseMaterialSource(snippet);
        r.errors = std::move(parsed.errors);
        r.metas = std::move(parsed.metas);

        std::uint64_t hash = 14695981039346656037ull;
        hash = Fnv64Str(hash, templateText);
        hash = Fnv64Str(hash, snippet);
        hash = Fnv64Str(hash, vertexSnippet);
        // The input-slot count changes the emitted bindings -- it is source.
        hash = (hash ^ chainInputs) * 1099511628211ull;
        r.templ = MaterialTemplate::Build(std::move(materialName), hash, std::move(parsed.decls));

        const std::string bindings = GenerateMaterialBindings(r.templ, surface, chainInputs);
        const std::pair<std::string_view, std::string_view> slots[] = {
            { "MATERIAL_CBUFFER", bindings },
            { "MATERIAL_BODY", snippet },
            { "VERTEX_BODY", vertexSnippet.empty() ? kVertexPassthrough : vertexSnippet },
        };
        std::vector<std::string> unresolved;
        r.hlsl = StitchShaderTemplate(templateText, slots, &unresolved);
        for (const std::string& slot : unresolved)
            r.errors.push_back("template: unresolved slot %{" + slot + "}");
        return r;
    }

    MaterialChainBuildResult BuildMaterialChainSource(std::string_view templateText,
                                                      std::span<const MaterialChainPassDesc> passes,
                                                      std::string materialName,
                                                      std::string_view vertexSnippet,
                                                      bool externalInput)
    {
        MaterialChainBuildResult r;
        r.hlsl.resize(passes.size());
        r.passErrors.resize(passes.size());
        r.passInputs.resize(passes.size());

        // DAG rules (what keeps the chain executable in span order): pass 0 is
        // the base and reads nothing but the scene; every real input
        // references an EARLIER pass; at most kMaxPassInputs per pass.
        // kSceneInput entries are post-mode-only. Violations are chain errors.
        for (std::size_t p = 0; p < passes.size(); ++p)
        {
            r.passInputs[p].assign(passes[p].inputs.begin(), passes[p].inputs.end());
            if (r.passInputs[p].size() > kMaxPassInputs)
                r.errors.push_back("pass " + std::to_string(p) + ": at most " +
                                   std::to_string(kMaxPassInputs) + " inputs");
            for (std::uint32_t in : r.passInputs[p])
            {
                if (in == kSceneInput)
                {
                    if (!externalInput)
                        r.errors.push_back(
                            "pass " + std::to_string(p) + ": reads the scene -- "
                            "assign this material as the scene post chain");
                    continue;
                }
                if (p == 0)
                    r.errors.push_back("pass 0 (the base) can read only the scene");
                else if (in >= p)
                    r.errors.push_back("pass " + std::to_string(p) + ": input " +
                                       std::to_string(in) +
                                       " must reference an earlier pass");
            }
        }

        // Merge declarations across passes: same name + same type = ONE shared
        // param (first declaration wins default/range -- the template dup rule,
        // chain-wide); conflicting types are chain errors on the later pass.
        std::vector<ParamDecl> decls;
        std::vector<ParamMeta> metas;
        std::uint64_t hash = 14695981039346656037ull;
        hash = Fnv64Str(hash, templateText);
        hash = Fnv64Str(hash, vertexSnippet);
        for (std::size_t p = 0; p < passes.size(); ++p)
        {
            hash = Fnv64Str(hash, passes[p].snippet);
            MaterialSourceParse parsed = ParseMaterialSource(passes[p].snippet);
            r.passErrors[p] = std::move(parsed.errors);
            for (std::size_t i = 0; i < parsed.decls.size(); ++i)
            {
                const ParamDecl& d = parsed.decls[i];
                bool known = false;
                for (const ParamDecl& existing : decls)
                {
                    if (existing.name != d.name)
                        continue;
                    if (existing.type != d.type)
                        r.errors.push_back("pass " + std::to_string(p) + ": param '" +
                                           d.name + "' redeclared with a different type");
                    known = true;
                    break;
                }
                if (!known)
                {
                    decls.push_back(d);
                    metas.push_back(parsed.metas[i]);
                }
            }
        }
        r.metas = std::move(metas);
        r.templ = MaterialTemplate::Build(std::move(materialName), hash, std::move(decls));

        // One binding block (union cbuffer + textures + the input-texture set)
        // stitched into EVERY pass -- a shared layout means one binding-set
        // shape and one packed CB for the whole chain; params declared by one
        // pass are simply visible to all of them. The decl count is uniform:
        // the max input count anywhere, min 1 (the pre-DAG snippets that read
        // a bare InputTexture keep compiling).
        for (const auto& ins : r.passInputs)
            r.chainInputSlots = (std::max)(r.chainInputSlots,
                                           static_cast<std::uint32_t>(ins.size()));
        const std::string bindings = GenerateMaterialBindings(
            r.templ, MaterialSurface::Fullscreen, r.chainInputSlots);
        for (std::size_t p = 0; p < passes.size(); ++p)
        {
            const std::pair<std::string_view, std::string_view> slots[] = {
                { "MATERIAL_CBUFFER", bindings },
                { "MATERIAL_BODY", passes[p].snippet },
                { "VERTEX_BODY",
                  vertexSnippet.empty() ? kVertexPassthrough : vertexSnippet },
            };
            std::vector<std::string> unresolved;
            r.hlsl[p] = StitchShaderTemplate(templateText, slots, &unresolved);
            if (p == 0)   // identical template per pass -- report slots once
                for (const std::string& slot : unresolved)
                    r.errors.push_back("template: unresolved slot %{" + slot + "}");
        }
        return r;
    }
}
