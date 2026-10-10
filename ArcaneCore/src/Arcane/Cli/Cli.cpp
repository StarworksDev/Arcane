#include "Arcane/Cli/Cli.hpp"
#include <Arcane/Util/CharConv.hpp>   // FromChars: std::from_chars incl. floating point on every standard library

#include <charconv>
#include <cstdio>

namespace Arcane
{
    Cli::Builder Cli::Flag(std::string name, std::string help)
    {
        m_opts.push_back(Opt{ std::move(name), std::move(help), "false", 0, true, false, false, CliType::String, {} });
        return Builder{ this, m_opts.size() - 1 };
    }

    Cli::Builder Cli::Option(std::string name, std::string defaultValue, std::string help)
    {
        m_opts.push_back(Opt{ std::move(name), std::move(help), std::move(defaultValue), 0, false, false, false, CliType::String, {} });
        return Builder{ this, m_opts.size() - 1 };
    }

    const Cli::Opt* Cli::FindLong(std::string_view name) const
    {
        for (const Opt& o : m_opts) if (o.name == name) return &o;
        return nullptr;
    }

    const Cli::Opt* Cli::FindShort(char c) const
    {
        for (const Opt& o : m_opts) if (o.shortAlias != 0 && o.shortAlias == c) return &o;
        return nullptr;
    }

    bool Cli::Result::Flag(std::string_view name) const
    {
        const auto it = flags.find(std::string(name));
        return it != flags.end() && it->second;
    }
    
    std::string Cli::Result::Get(std::string_view name) const
    {
        const auto it = values.find(std::string(name));
        return it != values.end() ? it->second : std::string();
    }

    std::vector<std::string> Cli::Result::GetMany(std::string_view name) const
    {
        const auto it = multi.find(std::string(name));
        return it == multi.end() ? std::vector<std::string>{} : it->second;
    }

    bool Cli::Result::Supplied(std::string_view name) const
    {
        const auto it = supplied.find(std::string(name));
        return it != supplied.end() && it->second;
    }

    void Cli::PrintUsage() const
    {
        const std::string text = Usage();
        std::fputs(text.c_str(), stdout);
    }

    std::string Cli::Usage() const
    {
        std::string out = m_prog + " -- " + m_desc + "\n";
        const auto line = [&out](const std::string& name, const std::string& help)
        {
            // printf's "  --%-16s %s": the name left-aligned in 16 columns, never truncated.
            out += "  --" + name;
            if (name.size() < 16) out.append(16 - name.size(), ' ');
            out += ' ';
            out += help;
        };
        for (const Opt& o : m_opts)
        {
            line(o.name, o.help);
            if (!o.isFlag && !o.def.empty()) out += " (default " + o.def + ")";
            out += "\n";
        }
        line("help", "show this help");
        out += "\n";
        return out;
    }

    namespace
    {
        bool NumericOk(CliType t, const std::string& s)
        {
            if (t == CliType::String) return true;   // string is always ok, even when empty
            if (s.empty())            return false;  // every numeric type rejects empty

            const char* begin = s.data();
            const char* end   = s.data() + s.size();

            auto fullyParsed = [&](auto& v)
            {
                auto r = Arcane::FromChars(begin, end, v);
                return r.ec == std::errc{} && r.ptr == end;
            };

            switch (t)
            {
                case CliType::Uint: { std::uint64_t v; return fullyParsed(v); }
                case CliType::Int:  { std::int64_t  v; return fullyParsed(v); }
                default:            { double        d; return fullyParsed(d); }
            }
        }

        bool ChoiceOk(const std::vector<std::string>& choices, const std::string& v)
        {
            if (choices.empty())
                return true;

            for (const std::string& c : choices)
            {
                if (c == v)
                {
                    return true;
                }
            }
            return false;
        }
    }

    Cli::Result Cli::Parse(int argc, char** argv) const
    {
        Result r;
        for (const Opt& o : m_opts)
        {
            if (o.isFlag)
            {
                r.flags[o.name] = false;
                continue;
            }
            r.values[o.name] = o.def;
        }

        auto fail = [&](const std::string& why)
        {
            std::fprintf(stderr, "error: %s\n", why.c_str());
            PrintUsage();
            r.ok = false;
            r.exitCode = 2;
            return r;
        };

        for (int i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            if (a == "--help" || a == "-h")
            {
                PrintUsage();
                r.ok = false;
                r.helpRequested = true;
                r.exitCode = 0;
                return r;
            }

            const Opt* opt = nullptr;
            std::string inlineVal; bool hasInline = false;
            if (a.rfind("--", 0) == 0)
            {
                std::string body = a.substr(2);
                const auto eq = body.find('=');
                if (eq != std::string::npos)
                {
                    hasInline = true;
                    inlineVal = body.substr(eq + 1);
                    body = body.substr(0, eq);
                }
                opt = FindLong(body);
            }
            else if (a.size() >= 2 && a[0] == '-')
            {
                char sc = a[1];
                if (a.size() > 2 && a[2] == '=')
                {
                    hasInline = true;
                    inlineVal = a.substr(3);
                }
                opt = FindShort(sc);
            }
            if (!opt)
                return fail("unknown argument '" + a + "'");

            if (opt->isFlag)
            {
                if (hasInline)
                    return fail("flag '--" + opt->name + "' takes no value");
                r.flags[opt->name] = true;
                continue;
            }

            std::string val;
            if (hasInline)
            {
                val = inlineVal;
            }
            else if (i + 1 < argc)
            {
                val = argv[++i];
            }
            else return fail("option '--" + opt->name + "' requires a value");

            if (!ChoiceOk(opt->choices, val))
                return fail("'--" + opt->name + "' must be one of the allowed values, got '" + val + "'");
            if (!NumericOk(opt->type, val))
                return fail("'--" + opt->name + "' expects a number, got '" + val + "'");

            r.values[opt->name] = val;
            r.supplied[opt->name] = true;
            if (opt->many)
                r.multi[opt->name].push_back(val);
        }

        // Required semantics: a required option is declared with an EMPTY default
        // (as in the test); if the resolved value is still that empty default, the
        // user never supplied it, so treat it as missing.
        for (const Opt& o : m_opts)
        {
            if (o.required && !o.isFlag && r.values[o.name] == o.def && o.def.empty())
            {
                return fail("missing required option '--" + o.name + "'");
            }
        }
        r.ok = true; r.exitCode = 0;
        return r;
    }
}
