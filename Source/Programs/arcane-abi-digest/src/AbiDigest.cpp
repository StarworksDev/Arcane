// arcane-abi-digest: asks the COMPILER for the game-facing layout digest.
//
//   --print              print AbiManifestMix() (16 lowercase hex digits)
//   --print-commented    print the mix with a comment-appended copy of the Engine
//                        group manifest (Plugin/AbiManifestEngine.hpp)
//   --write              write build/abi-digest.txt (hex + one newline) and the
//                        four constants in Core/AbiVersion.hpp
//                        (kGamePluginABIDigest and the Input / Client / Physics2D
//                        sub-digests)
//   --check              exit 1 if the file is missing or disagrees with the mix,
//                        kGamePluginABIDigest disagrees with the mix, or a
//                        committed sub-digest disagrees with its group's mix
//   --root <dir>         the checkout (default: found upward from cwd, then the exe)
//   --committed-input <hex>, --committed-client <hex>, --committed-physics2d <hex>
//                        --check only: stand in for the compiled-in
//                        kGamePluginABIDigestInput / kGamePluginABIDigestClient /
//                        kGamePluginABIDigestPhysics2D
//                        (the [abi] test's witness that a stale one is caught)
//
// The digest is FoldGroups(Engine, Input, Client, Physics2D)
// (Core/AbiManifestHash.hpp). A game TU that cannot include the Input, Client or
// Physics2D headers folds the COMMITTED
// sub-digest in that group's place (Plugin/AbiManifestGame.hpp), so a stale
// sub-digest would make every such game disagree with the host: --check fails it.
//
// Exit codes: 0 ok, 1 check failed / io error, 2 usage. Nothing here registers a
// component; the digest is a pure constexpr evaluation of the group manifests.

#include <Arcane/Core/AbiManifest.hpp>
#include <Arcane/Core/AbiVersion.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace AbiDigestTool { std::uint64_t CommentedMix() noexcept; }

namespace
{
    namespace fs = std::filesystem;

    constexpr std::uint64_t kPlainMix = Arcane::AbiManifestMix();
    constexpr std::uint64_t kInputMix = Arcane::AbiManifestInputMix();
    constexpr std::uint64_t kClientMix = Arcane::AbiManifestClientMix();
    constexpr std::uint64_t kPhysics2DMix = Arcane::AbiManifestPhysics2DMix();

    std::string Hex16(std::uint64_t v)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
        return buf;
    }

    bool ParseHex16(const std::string& text, std::uint64_t& out)
    {
        if (text.empty() || text.size() > 16) return false;
        std::uint64_t v = 0;
        for (const char c : text)
        {
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0) return false;
            v = (v << 4) | static_cast<std::uint64_t>(d);
        }
        out = v;
        return true;
    }

    // Replaces the hex literal of `inline constexpr std::uint64_t <name> = 0x...ull;`.
    bool ReplaceConstant(std::string& text, const char* name, std::uint64_t value)
    {
        const std::string head = std::string("inline constexpr std::uint64_t ") + name + " = 0x";
        const auto at = text.find(head);
        if (at == std::string::npos) return false;
        const auto from = at + head.size();
        const auto to = text.find("ull;", from);
        if (to == std::string::npos) return false;
        text.replace(from, to - from, Hex16(value));
        return true;
    }

    bool IsRoot(const fs::path& dir)
    {
        std::error_code ec;
        return fs::is_regular_file(dir / "premake5.lua", ec) && fs::is_regular_file(dir / "build" / "modules.lua", ec);
    }

    fs::path FindRoot(const char* argv0)
    {
        std::error_code ec;
        for (fs::path start : { fs::current_path(ec), fs::absolute(argv0, ec).parent_path() })
        {
            for (fs::path dir = start; !dir.empty(); dir = dir.parent_path())
            {
                if (IsRoot(dir)) return dir;
                if (dir == dir.parent_path()) break;
            }
        }
        return {};
    }
}

int main(int argc, char** argv)
{
    bool print = false, printCommented = false, write = false, check = false;
    fs::path root;
    std::uint64_t committedInput = Arcane::kGamePluginABIDigestInput;
    std::uint64_t committedClient = Arcane::kGamePluginABIDigestClient;
    std::uint64_t committedPhysics2D = Arcane::kGamePluginABIDigestPhysics2D;
    bool overridden = false;
    const char* usage = "usage: arcane-abi-digest --print|--print-commented|--write|--check [--root <dir>] "
                        "[--committed-input <hex>] [--committed-client <hex>] [--committed-physics2d <hex>]\n";
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--print") print = true;
        else if (a == "--print-commented") printCommented = true;
        else if (a == "--write") write = true;
        else if (a == "--check") check = true;
        else if (a == "--root" && i + 1 < argc) root = argv[++i];
        else if ((a == "--committed-input" || a == "--committed-client" || a == "--committed-physics2d") && i + 1 < argc)
        {
            std::uint64_t& slot = (a == "--committed-input") ? committedInput
                                : (a == "--committed-client") ? committedClient : committedPhysics2D;
            if (!ParseHex16(argv[++i], slot))
            {
                std::fprintf(stderr, "arcane-abi-digest: %s wants up to 16 hex digits, not '%s'\n%s", a.c_str(), argv[i], usage);
                return 2;
            }
            overridden = true;
        }
        else { std::fprintf(stderr, "arcane-abi-digest: unknown argument '%s'\n%s", a.c_str(), usage); return 2; }
    }
    if (static_cast<int>(print) + printCommented + write + check != 1 || (overridden && !check))
    {
        std::fprintf(stderr, "%s", usage);
        return 2;
    }

    if (print) { std::printf("%s\n", Hex16(kPlainMix).c_str()); return 0; }
    if (printCommented) { std::printf("%s\n", Hex16(AbiDigestTool::CommentedMix()).c_str()); return 0; }

    if (root.empty()) root = FindRoot(argv[0]);
    if (root.empty() || !IsRoot(root))
    {
        std::fprintf(stderr, "arcane-abi-digest: cannot find the checkout root (pass --root <dir>)\n");
        return 1;
    }
    const fs::path file = root / "build" / "abi-digest.txt";
    const std::string hex = Hex16(kPlainMix);

    if (write)
    {
        const fs::path header = root / "Source" / "Runtime" / "ArcaneCore" / "src" / "Arcane" / "Core" / "AbiVersion.hpp";
        std::string text;
        {
            std::ifstream in(header, std::ios::binary);
            if (!in) { std::fprintf(stderr, "arcane-abi-digest: cannot read %s\n", header.string().c_str()); return 1; }
            std::stringstream ss; ss << in.rdbuf();
            text = ss.str();
        }
        if (!ReplaceConstant(text, "kGamePluginABIDigest", kPlainMix)
            || !ReplaceConstant(text, "kGamePluginABIDigestInput", kInputMix)
            || !ReplaceConstant(text, "kGamePluginABIDigestClient", kClientMix)
            || !ReplaceConstant(text, "kGamePluginABIDigestPhysics2D", kPhysics2DMix))
        {
            std::fprintf(stderr, "arcane-abi-digest: %s does not declare the four digest constants as "
                                 "`inline constexpr std::uint64_t <name> = 0x...ull;`\n", header.string().c_str());
            return 1;
        }

        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) { std::fprintf(stderr, "arcane-abi-digest: cannot write %s\n", file.string().c_str()); return 1; }
        out << hex << '\n';
        out.close();
        if (!out) { std::fprintf(stderr, "arcane-abi-digest: writing %s failed\n", file.string().c_str()); return 1; }

        std::ofstream hout(header, std::ios::binary | std::ios::trunc);
        if (!hout) { std::fprintf(stderr, "arcane-abi-digest: cannot write %s\n", header.string().c_str()); return 1; }
        hout << text;
        hout.close();
        if (!hout) { std::fprintf(stderr, "arcane-abi-digest: writing %s failed\n", header.string().c_str()); return 1; }

        std::printf("wrote %s: %s\nwrote %s: kGamePluginABIDigest 0x%sull, kGamePluginABIDigestInput 0x%sull, "
                    "kGamePluginABIDigestClient 0x%sull, kGamePluginABIDigestPhysics2D 0x%sull\nrebuild, then run --check\n",
                    file.string().c_str(), hex.c_str(), header.string().c_str(), hex.c_str(),
                    Hex16(kInputMix).c_str(), Hex16(kClientMix).c_str(), Hex16(kPhysics2DMix).c_str());
        return 0;
    }

    // --check
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "arcane-abi-digest: %s is missing; run arcane-abi-digest --write\n", file.string().c_str());
        return 1;
    }
    std::stringstream ss; ss << in.rdbuf();
    std::string line = ss.str();
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    int rc = 0;
    if (line != hex)
    {
        std::fprintf(stderr, "arcane-abi-digest: %s says %s but the compiler says %s; rebuild this tool, run --write, update kGamePluginABIDigest\n",
                     file.string().c_str(), line.c_str(), hex.c_str());
        rc = 1;
    }
    if (Arcane::kGamePluginABIDigest != kPlainMix)
    {
        std::fprintf(stderr, "arcane-abi-digest: kGamePluginABIDigest is %s but the compiler says %s; run --write\n",
                     Hex16(Arcane::kGamePluginABIDigest).c_str(), hex.c_str());
        rc = 1;
    }
    // A game that cannot include a group's headers folds the committed value.
    if (committedInput != kInputMix)
    {
        std::fprintf(stderr, "arcane-abi-digest: kGamePluginABIDigestInput is %s but the Input group's mix is %s; run --write\n",
                     Hex16(committedInput).c_str(), Hex16(kInputMix).c_str());
        rc = 1;
    }
    if (committedClient != kClientMix)
    {
        std::fprintf(stderr, "arcane-abi-digest: kGamePluginABIDigestClient is %s but the Client group's mix is %s; run --write\n",
                     Hex16(committedClient).c_str(), Hex16(kClientMix).c_str());
        rc = 1;
    }
    if (committedPhysics2D != kPhysics2DMix)
    {
        std::fprintf(stderr, "arcane-abi-digest: kGamePluginABIDigestPhysics2D is %s but the Physics2D group's mix is %s; run --write\n",
                     Hex16(committedPhysics2D).c_str(), Hex16(kPhysics2DMix).c_str());
        rc = 1;
    }
    if (rc == 0) std::printf("abi digest ok: %s\n", hex.c_str());
    return rc;
}
