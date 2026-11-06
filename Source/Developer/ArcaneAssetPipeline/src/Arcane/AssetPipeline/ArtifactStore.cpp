#include "Arcane/AssetPipeline/ArtifactStore.hpp"

#include "Arcane/AssetPipeline/ArtifactFormat.hpp"

#include <Arcane/Core/Constant.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Only for CurrentProcessId() below -- part of the per-invocation tmp-path uniqueness that
// makes concurrent same-key Commit() calls safe (see ArtifactStore.hpp's concurrency contract).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace Arcane::AssetPipeline
{
    namespace
    {
        ARC_CONSTANT("file format: a cook key (u64) as 16 hex digits in artifact file names")
        constexpr std::size_t kHexDigits = 16;   // std::uint64_t as lowercase hex, zero-padded

        // Explicit digit-by-digit formatting -- no <sstream>/<iomanip> locale surprises, and
        // matches the byte-explicit discipline used elsewhere in this library.
        [[nodiscard]] std::string ToHex16(std::uint64_t v)
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string s(kHexDigits, '0');
            for (std::size_t i = 0; i < kHexDigits; ++i)
            {
                const int shift = 4 * static_cast<int>(kHexDigits - 1 - i);
                s[i] = kDigits[(v >> shift) & 0xFu];
            }
            return s;
        }

        [[nodiscard]] std::optional<std::uint64_t> FromHex16(std::string_view hex)
        {
            if (hex.size() != kHexDigits) return std::nullopt;

            std::uint64_t v = 0;
            for (char c : hex)
            {
                std::uint64_t digit;
                if (c >= '0' && c <= '9')      digit = static_cast<std::uint64_t>(c - '0');
                else if (c >= 'a' && c <= 'f')  digit = static_cast<std::uint64_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')  digit = static_cast<std::uint64_t>(c - 'A' + 10);
                else return std::nullopt;

                v = (v << 4) | digit;
            }
            return v;
        }

        [[nodiscard]] std::uint32_t CurrentProcessId() noexcept
        {
#if defined(_WIN32)
            return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
            return static_cast<std::uint32_t>(::getpid());
#endif
        }

        // Process-local, strictly increasing -- combined with the process id, gives every
        // Commit() invocation (even two invocations for the SAME cook key, same process,
        // interleaved via re-entrancy) its own private tmp path. No randomness needed:
        // uniqueness only has to hold within "this process, right now", and a process id plus a
        // monotonic counter does that exactly.
        [[nodiscard]] std::uint64_t NextInvocationId() noexcept
        {
            static std::atomic<std::uint64_t> counter{0};
            return counter.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ArtifactStore::ArtifactStore(std::filesystem::path intermediateDir)
        : m_intermediateDir(std::move(intermediateDir))
    {
    }

    std::filesystem::path ArtifactStore::PathFor(std::uint64_t cookKey) const
    {
        const std::string hex = ToHex16(cookKey);
        const std::string hh = hex.substr(0, 2);
        return m_intermediateDir / "Artifacts" / hh / (hex + ".arcart");
    }

    bool ArtifactStore::Commit(std::uint64_t cookKey,
                                const std::function<bool(const std::filesystem::path&)>& writer)
    {
        const std::filesystem::path finalPath = PathFor(cookKey);

        std::error_code ec;
        std::filesystem::create_directories(finalPath.parent_path(), ec);

        // Per-invocation tmp path: process id + a process-local monotonic counter, NEVER just
        // the cook key. This is what makes concurrent same-key Commit() calls safe -- each
        // invocation's writer lands its bytes at a location no other invocation (same process
        // or a different one) can ever touch, so a rename can never publish a half-interleaved
        // file. See the concurrency contract in ArtifactStore.hpp.
        const std::filesystem::path tmpPath = finalPath.parent_path()
            / (finalPath.stem().string() + "." + std::to_string(CurrentProcessId()) + "-"
               + std::to_string(NextInvocationId()) + ".tmp");

        // A throwing writer must still leave no file at the final name and no stray tmp file;
        // Commit's contract is a plain bool, so the exception is caught here rather than
        // propagated -- callers of Commit never need to guard against an arbitrary writer's
        // exception type.
        bool wrote = false;
        try
        {
            wrote = writer(tmpPath);
        }
        catch (...)
        {
            wrote = false;
        }

        if (!wrote)
        {
            std::filesystem::remove(tmpPath, ec);
            return false;
        }

        std::filesystem::rename(tmpPath, finalPath, ec);
        if (ec)
        {
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
        return true;
    }

    std::optional<std::uint64_t> ArtifactStore::Lookup(const Guid& guid) const
    {
        const auto it = m_index.find(guid);
        if (it == m_index.end()) return std::nullopt;
        return it->second;
    }

    void ArtifactStore::PutIndex(const Guid& guid, std::uint64_t cookKey)
    {
        m_index[guid] = cookKey;
    }

    void ArtifactStore::RebuildIndexFromScan()
    {
        m_index.clear();
        m_allByGuid.clear();
        m_scanComplete = true;

        const std::filesystem::path artifactsDir = m_intermediateDir / "Artifacts";

        std::error_code ec;
        const bool dirExists = std::filesystem::exists(artifactsDir, ec);
        if (ec) { m_scanComplete = false; return; }
        if (!dirExists) return;   // nothing cooked yet: a complete (empty) scan

        std::filesystem::recursive_directory_iterator it(artifactsDir, ec);
        if (ec) { m_scanComplete = false; return; }

        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec))
        {
            if (ec) { m_scanComplete = false; break; }

            const std::filesystem::directory_entry& entry = *it;

            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || fileEc) continue;

            const std::filesystem::path& path = entry.path();
            if (path.extension() != ".arcart") continue;

            const std::optional<std::uint64_t> cookKey = FromHex16(path.stem().string());
            if (!cookKey) continue;   // not a name this store minted -- skip, never abort the scan

            // F2c Task 8: kind-agnostic prefix read, NOT ReadTextureArtifact -- see this
            // function's own doc comment (ArtifactStore.hpp) for why the old texture-only
            // read left every mesh artifact invisible to this index.
            const std::optional<ArtifactPrefix> prefix = ReadArtifactPrefix(path);
            if (!prefix) continue;   // corrupt/unreadable -- skip, never abort the scan

            m_index[prefix->sourceGuid] = *cookKey;
            m_allByGuid[prefix->sourceGuid].push_back(path);
        }
    }

    std::size_t ArtifactStore::SweepOrphans(const std::unordered_set<Guid>& liveGuids)
    {
        std::size_t removed = 0;

        for (auto it = m_index.begin(); it != m_index.end(); )
        {
            if (liveGuids.contains(it->first))
            {
                ++it;
                continue;
            }

            std::error_code ec;
            if (std::filesystem::remove(PathFor(it->second), ec))
                ++removed;

            it = m_index.erase(it);
        }

        return removed;
    }

    std::size_t ArtifactStore::RemoveAllExcept(const Guid& guid, std::uint64_t keepKey,
                                               std::vector<std::filesystem::path>* remaining)
    {
        // The last scan could not finish, so a stray may be invisible: report the whole
        // Artifacts directory as "left" and let the caller fail the guid rather than call it clean.
        if (!m_scanComplete && remaining)
            remaining->push_back(m_intermediateDir / "Artifacts");

        const auto found = m_allByGuid.find(guid);
        if (found == m_allByGuid.end())
            return 0;

        std::size_t removed = 0;
        std::vector<std::filesystem::path> kept;   // what stays in the scan record for this guid
        for (const std::filesystem::path& file : found->second)
        {
            const std::optional<std::uint64_t> key = FromHex16(file.stem().string());
            if (key && *key == keepKey)
            {
                kept.push_back(file);
                continue;
            }

            std::error_code removeEc;
            std::filesystem::remove(file, removeEc);

            std::error_code existsEc;
            if (std::filesystem::exists(file, existsEc) || existsEc)
            {
                kept.push_back(file);
                if (remaining) remaining->push_back(file);
            }
            else
            {
                ++removed;
            }
        }
        found->second = std::move(kept);
        return removed;
    }
}
