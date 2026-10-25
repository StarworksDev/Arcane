// Prints one `name hex` line per mutation variant (see MutBase.cpp et al).
#include <cstdint>
#include <cstdio>

namespace AbiMutants
{
    std::uint64_t MixBase() noexcept;
    std::uint64_t MixProcessPrivate() noexcept;
    std::uint64_t MixAabbSwap() noexcept;
    std::uint64_t MixViewSwap() noexcept;
    std::uint64_t MixClientPrivate() noexcept;
    std::uint64_t MixPresGameInput() noexcept;
}

int main()
{
    const auto line = [](const char* name, std::uint64_t v) { std::printf("%s %016llx\n", name, static_cast<unsigned long long>(v)); };
    line("base", AbiMutants::MixBase());
    line("process-private", AbiMutants::MixProcessPrivate());
    line("aabb-swap", AbiMutants::MixAabbSwap());
    line("view-swap", AbiMutants::MixViewSwap());
    line("client-private", AbiMutants::MixClientPrivate());
    line("pres-gameinput", AbiMutants::MixPresGameInput());
    return 0;
}
