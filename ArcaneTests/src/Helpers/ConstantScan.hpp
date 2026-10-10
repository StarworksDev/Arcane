#pragma once

// The text scanner behind ConstantGuardTest. A NUMERIC CONSTANT is one
// declaration statement (joined across lines up to its ';') that is
//   - `constexpr` (not `if constexpr`, not a constexpr function), or a
//     `static/inline/extern const` of a non-char type, and
//   - has an initializer ('=' or '{') holding a numeric literal outside string
//     and character literals (a lambda initializer is not a constant).
// Every numeric declarator of the statement is a constant of its own:
// `constexpr double k1 = 0.045, k2 = 0.015;` gives two sites, both at the
// statement's line. The statement is MARKED when the nearest non-blank line
// above it starts with ARC_CONSTANT(; that one marker covers all its declarators.

#include "ReferenceProjectDir.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace Arcane::Test
{
    struct ConstantSite
    {
        std::string file;      // repo-relative, '/' separators
        int         line = 0;  // 1-based, the declaration's first line
        std::string symbol;
        bool        marked = false;
    };

    inline std::filesystem::path RepoRoot() { return FindReferenceProjectDir().parent_path(); }

    inline const std::vector<std::string>& ConstantScanRoots()
    {
        static const std::vector<std::string> roots = {
            "ArcaneCore/src", "ArcaneClient/src", "ArcaneEditor/src", "ArcaneRuntime/src",
            "ArcaneServer/src", "ArcaneCrashReporter/src", "ArcaneAssetPipeline/src",
        };
        return roots;
    }

    // Blanks "..." and '...' literals (a ' between two digits is a digit
    // separator, kept) and drops a trailing // comment.
    inline std::string StripLiteralsAndComments(const std::string& s)
    {
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') break;
            const bool digitSep = c == '\'' && i > 0 && i + 1 < s.size()
                && std::isdigit(static_cast<unsigned char>(s[i - 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 1]));
            if ((c == '"' || c == '\'') && !digitSep)
            {
                for (++i; i < s.size() && s[i] != c; ++i)
                    if (s[i] == '\\') ++i;
                out.push_back(' ');
                continue;
            }
            if (!digitSep) out.push_back(c);
        }
        return out;
    }

    // The names of the statement's NUMERIC declarators, in declaration order.
    //   - The first declarator's name comes from the head (the text before the
    //     first '=' or '{').
    //   - From that initializer up to the statement's first top-level ';', the
    //     text is split at commas outside (), {} and []. A piece is a further
    //     declarator only when it reads `name [..]* =` or `name [..]* {`. Any
    //     other piece (the ` 3>{...}` of `std::array<int, 3>{...}`) continues
    //     the previous declarator's initializer.
    //   - A declarator counts when its OWN initializer holds a numeric literal
    //     and is not a lambda (`constexpr auto f = [](...) {...}`).
    inline std::vector<std::string> NumericConstantNames(const std::string& raw)
    {
        static const std::regex kConstexpr(R"(^\s*(?:(?:static|inline|extern)\s+)*constexpr\s)");
        static const std::regex kConst(R"(^\s*(?:extern\s+)?(?:__declspec\(\w+\)\s+)?(?:(?:static|inline|extern)\s+)+const\s+(?!char\b|wchar_t\b))");
        static const std::regex kName(R"((\w+)\s*(?:\[[^\]]*\]\s*)*$)");
        static const std::regex kNextDeclarator(R"(^\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*[={])");
        static const std::regex kNumber(R"((?:^|[^\w.])\.?\d)");
        const std::string stmt = StripLiteralsAndComments(raw);
        if (!std::regex_search(stmt, kConstexpr) && !std::regex_search(stmt, kConst)) return {};
        const std::size_t init = stmt.find_first_of("={");
        if (init == std::string::npos) return {};
        const std::string head = stmt.substr(0, init);
        if (head.find('(') != std::string::npos && head.find("__declspec(") == std::string::npos) return {};
        std::smatch m;
        if (!std::regex_search(head, m, kName)) return {};

        // The initializer region [init, first top-level ';'), split at top-level commas.
        std::vector<std::string> pieces(1);
        int depth = 0;
        for (std::size_t i = init; i < stmt.size(); ++i)
        {
            const char c = stmt[i];
            if (c == '(' || c == '{' || c == '[') ++depth;
            else if ((c == ')' || c == '}' || c == ']') && depth > 0) --depth;
            else if (depth == 0 && c == ';') break;
            else if (depth == 0 && c == ',') { pieces.emplace_back(); continue; }
            pieces.back().push_back(c);
        }

        // (name, its initializer starting at its '=' or '{')
        std::vector<std::pair<std::string, std::string>> decls{ { m[1].str(), pieces.front() } };
        for (std::size_t p = 1; p < pieces.size(); ++p)
        {
            std::smatch d;
            if (std::regex_search(pieces[p], d, kNextDeclarator))
                decls.emplace_back(d[1].str(), pieces[p].substr(static_cast<std::size_t>(d.position(0) + d.length(0) - 1)));
            else
                decls.back().second += ',' + pieces[p];
        }

        std::vector<std::string> names;
        for (const auto& [name, initializer] : decls)
        {
            if (initializer.front() == '=')   // a lambda initializer is not a constant
                if (const std::size_t v = initializer.find_first_not_of(" \t", 1); v != std::string::npos && initializer[v] == '[') continue;
            if (std::regex_search(initializer, kNumber)) names.push_back(name);
        }
        return names;
    }

    // The statement's first NUMERIC declarator (empty when it declares none).
    inline std::string NumericConstantName(const std::string& raw)
    {
        std::vector<std::string> names = NumericConstantNames(raw);
        return names.empty() ? std::string{} : std::move(names.front());
    }

    inline std::vector<ConstantSite> ScanNumericConstants(const std::filesystem::path& repoRoot)
    {
        namespace fs = std::filesystem;
        std::vector<ConstantSite> out;
        for (const std::string& root : ConstantScanRoots())
        {
            const fs::path dir = repoRoot / root;
            if (!fs::is_directory(dir)) continue;
            for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir))
            {
                const std::string ext = e.path().extension().string();
                if (!e.is_regular_file() || (ext != ".hpp" && ext != ".h" && ext != ".cpp" && ext != ".inl")) continue;
                std::ifstream in(e.path());
                std::vector<std::string> lines;
                for (std::string l; std::getline(in, l);) lines.push_back(l);
                for (std::size_t i = 0; i < lines.size(); ++i)
                {
                    const std::string& first = lines[i];
                    const std::size_t nb = first.find_first_not_of(" \t");
                    if (nb == std::string::npos || first.compare(nb, 2, "//") == 0 || first[nb] == '*' || first[nb] == '#') continue;
                    if (first.find("const") == std::string::npos) continue;
                    std::string stmt = first;
                    for (std::size_t j = i + 1; stmt.find(';') == std::string::npos && j < lines.size() && j < i + 64; ++j)
                        stmt += ' ' + lines[j];
                    const std::vector<std::string> names = NumericConstantNames(stmt);
                    if (names.empty()) continue;
                    bool marked = false;   // one ARC_CONSTANT above the statement covers every declarator in it
                    for (std::size_t k = i; k-- > 0;)
                    {
                        const std::size_t p = lines[k].find_first_not_of(" \t");
                        if (p == std::string::npos) continue;
                        marked = lines[k].compare(p, 13, "ARC_CONSTANT(") == 0;
                        break;
                    }
                    const std::string file = fs::relative(e.path(), repoRoot).generic_string();
                    for (const std::string& name : names)
                        out.push_back(ConstantSite{ file, static_cast<int>(i + 1), name, marked });
                }
            }
        }
        return out;
    }
}
