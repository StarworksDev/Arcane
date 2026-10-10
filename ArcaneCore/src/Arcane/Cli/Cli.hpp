#pragma once
// Arcane::Cli -- a small typed, declarative command-line argument parser.
//
// Register options/flags up front (name + default + help, optional short alias,
// Choices validation, Required, a numeric Type), then Parse(argc, argv). On
// --help/-h it prints usage and returns {ok=false, helpRequested, exitCode=0};
// on a bad/unknown/missing arg it prints the reason + usage and returns
// {ok=false, exitCode=2}; the caller does `if (!r.ok) return r.exitCode;`.
//
// Reusable engine-wide (ArcaneRuntime today; future tools/Game host/harnesses). std-only,
// presentation-free. Out of scope (documented extension points, NOT built):
// subcommands, config files, env-var fallback, positional options. REPEATABLE
// options ARE built -- see Many()/GetMany().
// PRESENTATION-FREE + C++23-clean.
#include <charconv>
#include <Arcane/Util/CharConv.hpp>   // FromChars: std::from_chars incl. floating point on every standard library
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Arcane/Core/Api.hpp>

namespace Arcane
{
    enum class CliType : std::uint8_t { String, Int, Uint, Double };

    class ARC_CORE_API Cli
    {
    public:
        Cli(std::string prog, std::string desc) : m_prog(std::move(prog)), m_desc(std::move(desc)) {}

        // Fluent builder over the just-registered option (references the Cli + index,
        // so it stays valid across m_opts reallocation).
        struct Builder
        {
            Cli* cli; std::size_t idx;
            Builder& Short(char s)                              { cli->m_opts[idx].shortAlias = s; return *this; }
            Builder& Choices(std::initializer_list<const char*> ch) { for (const char* s : ch) cli->m_opts[idx].choices.emplace_back(s); return *this; }
            Builder& Required()                                 { cli->m_opts[idx].required = true; return *this; }
            Builder& Type(CliType t)                            { cli->m_opts[idx].type = t; return *this; }
            Builder& Many()                                     { cli->m_opts[idx].many = true; return *this; }
        };

        Builder Flag(std::string name, std::string help);                         // bool, default off
        Builder Option(std::string name, std::string defaultValue, std::string help);

        // A nested class does not inherit the outer class's dllexport -- mark it
        // explicitly (Core-DLL split, spec 2026-09-15 s8).
        struct ARC_CORE_API Result
        {
            bool ok = false;
            bool helpRequested = false;
            int  exitCode = 0;                                  // 0 (help) or 2 (error) when !ok
            std::unordered_map<std::string, std::string> values;// option name -> resolved value (or default)
            std::unordered_map<std::string, bool>        flags; // flag name -> present?
            std::unordered_map<std::string, std::vector<std::string>> multi; // Many() option -> occurrences
            std::unordered_map<std::string, bool>        supplied; // option name -> explicitly given on the command line?

            [[nodiscard]] bool        Flag(std::string_view name) const;
            [[nodiscard]] std::string Get (std::string_view name) const;
            [[nodiscard]] std::vector<std::string> GetMany(std::string_view name) const;
            // True iff this Option was explicitly given on the command line, as opposed
            // to answering from its registered default. `values` is pre-seeded with every
            // option's default before parsing and overwritten in place on supply, so a
            // supplied value equal to the default is indistinguishable from an unsupplied
            // one by comparing `Get(name)` against the default string -- this is the seam
            // that makes that comparison unnecessary. Flags are unaffected: `Flag()` is
            // already unambiguous (there is no explicit-false state to confuse it with).
            [[nodiscard]] bool        Supplied(std::string_view name) const;
            template <typename T> [[nodiscard]] T GetAs(std::string_view name) const;
        };

        [[nodiscard]] Result Parse(int argc, char** argv) const;
        void PrintUsage() const;
        // The text PrintUsage prints. An option whose default is empty prints
        // no "(default ...)" suffix (S6-GATE: ArcaneServer's --fixed-dt
        // printed "(default )").
        [[nodiscard]] std::string Usage() const;

    private:
        struct Opt
        {
            std::string name, help, def;
            char shortAlias = 0;
            bool isFlag = false, required = false, many = false;
            CliType type = CliType::String;
            std::vector<std::string> choices;
        };
        const Opt* FindLong(std::string_view name) const;
        const Opt* FindShort(char c) const;

        std::string m_prog, m_desc;
        std::vector<Opt> m_opts;
    };

    // Typed access: the value was already validated at Parse time, so from_chars
    // succeeds; defensively returns T{} on an unexpected failure.
    template <typename T>
    T Cli::Result::GetAs(std::string_view name) const
    {
        const std::string s = Get(name);
        T out{};
        (void)Arcane::FromChars(s.data(), s.data() + s.size(), out);
        return out;
    }
}
