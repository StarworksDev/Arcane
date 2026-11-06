#pragma once
// ArcaneRemote-internal helpers shared by the world-reading commands (entity.*
// and ecs.*, Task 23; resource.*, Task 24) and the capture commands
// (capture.*, Tasks 27-28). Not exported: nothing outside this module
// includes it. Moved out of EcsCommands.cpp so resource.get reuses the same
// world pick, argument checks and inline-cap artifact path.
#include <Arcane/Remote/Commands/CaptureCommands.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Core/Constant.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Astra
{
class Registry;
class TypeMeta;
}

namespace Arcane::Remote
{
struct Job;       // Jobs.hpp
struct HostGate;  // Dispatcher.hpp
}

namespace Arcane::Remote::Detail
{

ARC_CONSTANT("spec s4.8: ECS and resource dumps over 256 KiB go to an artifact, never inline in the envelope")
inline constexpr std::size_t kInlineCapBytes = 262144;

// The session's artifact directory (<remote dir>/out; SetEcsArtifactDir sets
// it). Empty: no session armed one.
std::filesystem::path ArtifactDir();
void SetArtifactDir(const std::filesystem::path& dir);
// Clears the directory only when it is still `dir`.
void ReleaseArtifactDir(const std::filesystem::path& dir);

// args must be an object (null reads as empty) whose keys are all in `known`:
// arg.invalid / arg.unknown otherwise.
std::optional<Envelope> CheckKeys(const Call& call, std::initializer_list<std::string_view> known,
                                  std::string_view usage);

// The registry --world names; null with the pick's refusal otherwise
// (arg.missing, arg.unknown, host.unavailable).
Astra::Registry* PickRegistry(const Call& call, Envelope& refusal);

Error MakeIssue(std::string code, std::string message, std::string remediation);

// Strict UTF-8 (no overlongs, no surrogates, nothing past U+10FFFF). A wire
// string that becomes a filesystem path is checked with this BEFORE the path
// conversion: on POSIX that conversion takes any bytes, so the throw that
// refuses bad UTF-8 on Windows is not a check there.
bool IsValidUtf8(std::string_view text) noexcept;

// An error envelope whose one error is `issue` (detail and candidates kept).
Envelope ErrorEnvelope(const Call& call, Error issue);

// Writes `json` to <dir>/<stem>-<unix ms>-<n>.json through <file>.tmp, a close
// whose failure is checked, and a rename; the artifact (`kind`, format json,
// bytes, sha256), or nullopt with *why when any step fails.
std::optional<Artifact> WriteArtifact(const std::filesystem::path& dir, std::string_view stem, std::string_view kind,
                                      const std::string& json, std::string& why);

// Encodes tight RGBA8 to PNG bytes in memory, writes them to <file>.tmp
// through a wide-path std::ofstream whose close is checked, and renames it
// into place; the artifact (`kind`, RGBA8_UNORM, width, height, bytes, sha256
// of the written bytes), or nullopt with *why (the .tmp removed). No path is
// narrowed with path::string(): a non-ASCII path is written as given.
std::optional<Artifact> WritePngArtifact(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height,
                                         const unsigned char* rgba, std::string_view kind, std::string& why);

// ---- The capture ticket step (Task 27; shared with capture.compare, Task 28).
// Defined in CaptureCommands.cpp, beside the stored hooks.

// The hooks SetCaptureHooks stored (main thread only).
const CaptureHooks& StoredCaptureHooks();

// One armed render-ticket copy (rulings P2 (a), P3 (a)): the source's hook
// pair, the ticket the first step armed, and the job's deadline.
struct TicketCopy {
    ArmCopyFn arm = nullptr;
    DrainSlotFn drain = nullptr;
    void* hookUser = nullptr;
    std::uint64_t ticket = 0;
    std::chrono::steady_clock::time_point deadline{};
    CancelCopyFn cancel = nullptr;   // the pair's cancel (final fix pass, C-M12); null: the ring keeps it
};

// The job gave up on the ticket it armed (C-M12, G-M5): the host's ring
// forgets it (copy.cancel), so it cannot hold a slot or land in the next
// capture's bytes. A no-op before the arm, or with no cancel hook.
void CancelTicket(Job& job, TicketCopy& copy);

// The deadline a capture job armed for `call` gets: its timeoutMs, or the
// default when the call carries none, never past kMaxWaitTimeoutMs (C-M12).
std::chrono::steady_clock::time_point CaptureDeadline(const Call& call);

enum class TicketStep : std::uint8_t { Pending, Retired, Failed };
// One job step of a ticket copy. The first step arms (job.progress.label
// becomes "copied"); later steps drain that ticket. Pending: not retired yet
// (the job stays Running). Retired: *raw holds the retired copy, mapped
// non-null. Failed: job.envelope is the MakeError to finish with
// (capture.no-frame: arm refused, map lost or deadline passed;
// capture.minimized: the host is minimized). Reads no gate.frame.
TicketStep StepTicket(Job& job, const HostGate& gate, TicketCopy& copy, CaptureMapped& raw);

// The retired copy as tight RGBA8 (NormalizeCaptureRows: row padding dropped,
// BGRA swapped); false with *why when its geometry does not fit its bytes.
bool NormalizeRetired(const CaptureMapped& raw, std::vector<unsigned char>& rgba, std::string& why);

// The capture ring's third user (host.wait settled, Task 30): a job that has
// armed a ticket on the host pair holds the ring until that ticket retires or
// the job ends, and CaptureInFlight counts a hold whose job is still Running,
// so a screenshot or compare meanwhile answers host.busy capture-pending
// (ruling P4 (a): one capture in flight per host). Main thread only. A
// RegisterCaptureCommands that changes the table drops every hold.
void HoldCaptureRing(std::uint64_t jobId);
void ReleaseCaptureRing(std::uint64_t jobId);

// ---- The Protected-field walk (spec 0.7). resource.get's since Task 24;
// entity.get, ecs.query and schema.type's defaults share it (final fix pass,
// C-I5 / C-M8), so no read path writes a Protected value in clear.

// RemoteCVarService's mask, written in place of a field that may hold a secret.
inline constexpr std::string_view kMasked = "<protected>";

// Writes `data` (an instance of `meta`) into `out` as scene-format JSON with
// ReflectionJsonWriter, one key per serializable field (skipHidden: hidden
// fields too are skipped, as a schema's defaults are). A field that is
// Protected, or whose type may hold a Protected value (any field of a
// Protected type, a container the walk cannot see into, an unresolved type),
// is written as kMasked instead. False with *why when the writer refused a value.
bool WriteMaskedFields(const Astra::TypeMeta& meta, void* data, nlohmann::json& out, std::string& why,
                       bool skipHidden = false);

} // namespace Arcane::Remote::Detail

#endif
