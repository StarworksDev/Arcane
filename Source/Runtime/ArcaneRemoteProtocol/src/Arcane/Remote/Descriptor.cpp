#include <Arcane/Remote/Descriptor.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Platform/Process.hpp>

#include <Json.hpp>

#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h> // SetSecurityInfo, SetNamedSecurityInfoW (advapi32)
#include <sddl.h>   // ConvertStringSecurityDescriptorToSecurityDescriptorW (advapi32)
#else
#include <cerrno>
#include <fcntl.h>    // open, O_CLOEXEC
#include <sys/stat.h> // fchmod, S_IRUSR / S_IWUSR
#include <unistd.h>   // write, close
#endif

namespace Arcane::Remote
{

namespace
{

using Json = nlohmann::json;

std::string ToText(const Descriptor& d)
{
    const Json j = {
        { "protocol", d.protocol },
        { "hostKind", ToString(d.hostKind) },
        { "pid", d.pid },
        { "processStart", d.processStart },
        { "port", d.port },
        { "token", d.token },
        { "project", d.project },
        { "heartbeatUnixMs", d.heartbeatUnixMs },
        { "state", d.state },
        { "frame", d.frame },
        { "abi", d.abi },
        { "client", d.client },
        { "ownerPid", d.ownerPid },
        { "explicitRemote", d.explicitRemote },
    };
    // replace, not throw: a project path is never worth an exception here.
    return j.dump(2, ' ', false, Json::error_handler_t::replace);
}

std::optional<HostKind> ParseHostKind(std::string_view s)
{
    if (s == ToString(HostKind::Editor)) return HostKind::Editor;
    if (s == ToString(HostKind::Runtime)) return HostKind::Runtime;
    if (s == ToString(HostKind::Server)) return HostKind::Server;
    return std::nullopt;
}

// A non-negative integer that fits T; false when absent, ill-typed or too big.
template <class T>
bool ReadUnsigned(const Json& j, const char* key, T& out)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_unsigned()) return false;
    const auto v = it->get<std::uint64_t>();
    if (v > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) return false;
    out = static_cast<T>(v);
    return true;
}

bool ReadString(const Json& j, const char* key, std::string& out)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

bool ReadBool(const Json& j, const char* key, bool& out)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return false;
    out = it->get<bool>();
    return true;
}

#if defined(_WIN32)
// Spec AM5: the owner plus SYSTEM, full access, protected (no inherited ACEs).
constexpr const wchar_t* kOwnerOnlySddl = L"D:P(A;;FA;;;OW)(A;;FA;;;SY)";

struct OwnerOnlySd {
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    OwnerOnlySd()
    {
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(kOwnerOnlySddl, SDDL_REVISION_1, &sd, nullptr)) {
            sd = nullptr;
            return;
        }
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        if (!::GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present) dacl = nullptr;
    }
    ~OwnerOnlySd()
    {
        if (sd) ::LocalFree(sd);
    }
    OwnerOnlySd(const OwnerOnlySd&) = delete;
    OwnerOnlySd& operator=(const OwnerOnlySd&) = delete;
    bool Valid() const { return sd != nullptr && dacl != nullptr; }
};

constexpr SECURITY_INFORMATION kProtectedDacl = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;

bool WriteOwnerOnlyTemp(const std::filesystem::path& tmp, const std::string& text, const OwnerOnlySd& owner)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = owner.sd;
    sa.bInheritHandle = FALSE;
    // A new temp is born owner-only through `sa`; CREATE_ALWAYS ignores `sa`
    // for a temp a crash left behind, so the DACL is set on the handle too,
    // before a byte of the token is written.
    const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE | WRITE_DAC, 0, &sa, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = ::SetSecurityInfo(h, SE_FILE_OBJECT, kProtectedDacl, nullptr, nullptr, owner.dacl, nullptr) == ERROR_SUCCESS;
    const char* p = text.data();
    std::size_t left = text.size();
    while (ok && left > 0) {
        const DWORD chunk = left > std::numeric_limits<DWORD>::max() ? std::numeric_limits<DWORD>::max()
                                                                      : static_cast<DWORD>(left);
        DWORD wrote = 0;
        if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) { ok = false; break; }
        p += wrote;
        left -= wrote;
    }
    if (!::CloseHandle(h)) ok = false;
    return ok;
}
#else
constexpr mode_t kOwnerReadWrite = S_IRUSR | S_IWUSR; // 0600 (spec AM5)

bool WriteOwnerOnlyTemp(const std::filesystem::path& tmp, const std::string& text)
{
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kOwnerReadWrite);
    if (fd < 0) return false;
    // open's mode applies only to a NEW file: a temp a crash left behind keeps
    // its old mode unless it is set here, before the token is written.
    bool ok = ::fchmod(fd, kOwnerReadWrite) == 0;
    const char* p = text.data();
    std::size_t left = text.size();
    while (ok && left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) ok = false;
    return ok;
}
#endif

} // namespace

std::filesystem::path DescriptorPath(const std::filesystem::path& projectRoot, HostKind kind, std::uint32_t pid)
{
    const std::string name = std::string(ToString(kind)) + "-" + std::to_string(pid) + ".json";
    return Paths::Join(Paths::Location::ProjectSaved, Paths::ForProject(projectRoot), std::filesystem::path("Remote") / name);
}

std::filesystem::path DescriptorDir(const std::filesystem::path& projectRoot, bool projectless)
{
    if (projectless) {
        const std::filesystem::path user = Paths::Get(Paths::Location::EditorUserDir);
        return user.empty() ? std::filesystem::path() : user / "Remote";
    }
    return Paths::Join(Paths::Location::ProjectSaved, Paths::ForProject(projectRoot), "Remote");
}

bool WriteDescriptorAtomic(const std::filesystem::path& path, const Descriptor& desc)
{
    if (path.empty() || !path.has_filename()) return false;
    std::error_code ec;
    const std::filesystem::path dir = path.parent_path();
    if (!dir.empty()) {
#if defined(_WIN32)
        std::filesystem::create_directories(dir, ec);
        if (ec) return false;
#else
        const bool existed = std::filesystem::exists(dir, ec);
        std::filesystem::create_directories(dir, ec);
        if (ec) return false;
        // A directory this call made is the owner's alone (0700): the POSIX
        // half of AM5. One that already existed (a --remote-descriptor
        // override's parent) is not ours to re-permission. Best-effort: the
        // file's own 0600 is the guarantee.
        if (!existed)
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
#endif
    }

    std::filesystem::path tmp = path;
    tmp += ".tmp";
    const std::string text = ToText(desc);
    // The token is in `text`; nothing below logs it.
#if defined(_WIN32)
    const OwnerOnlySd owner;
    bool ok = owner.Valid() && WriteOwnerOnlyTemp(tmp, text, owner);
    if (ok) ok = ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    if (!ok) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    // A rename keeps the temp's DACL; it is applied again to the final name (AM5).
    return ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, kProtectedDacl, nullptr, nullptr,
                                   owner.dacl, nullptr) == ERROR_SUCCESS;
#else
    if (!WriteOwnerOnlyTemp(tmp, text)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, path, ec); // rename(2): an atomic replace on POSIX
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        return false;
    }
    return true;
#endif
}

std::optional<Descriptor> ReadDescriptor(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const Json j = ParseJsonBounded(text, kMaxRequestDepth);   // flat; any local file may sit in Saved/Remote (Task 30)
    if (j.is_discarded() || !j.is_object()) return std::nullopt;

    Descriptor d;
    // Required: what a client needs to judge and reach the host. `protocol` is
    // returned as written (rev 8.3 V6): a newer major reaches Select, which
    // answers protocol.mismatch rather than target.none.
    std::string kind;
    if (!ReadUnsigned(j, "protocol", d.protocol)) return std::nullopt;
    if (!ReadString(j, "hostKind", kind)) return std::nullopt;
    const auto hostKind = ParseHostKind(kind);
    if (!hostKind) return std::nullopt;
    d.hostKind = *hostKind;
    if (!ReadUnsigned(j, "pid", d.pid) || d.pid == 0) return std::nullopt;
    if (!ReadUnsigned(j, "port", d.port)) return std::nullopt;
    if (!ReadString(j, "token", d.token)) return std::nullopt;

    // Optional: a missing or ill-typed key keeps the struct default (abi 0).
    // Unknown keys are ignored.
    ReadUnsigned(j, "processStart", d.processStart);
    ReadString(j, "project", d.project);
    if (const auto it = j.find("heartbeatUnixMs"); it != j.end() && it->is_number_integer()) {
        if (!it->is_number_unsigned() || it->get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            d.heartbeatUnixMs = it->get<std::int64_t>();
    }
    ReadString(j, "state", d.state);
    ReadUnsigned(j, "frame", d.frame);
    ReadUnsigned(j, "abi", d.abi);
    ReadString(j, "client", d.client);
    ReadUnsigned(j, "ownerPid", d.ownerPid);
    ReadBool(j, "explicitRemote", d.explicitRemote);
    return d;
}

void TouchHeartbeat(const std::filesystem::path& path, std::int64_t unixMs, std::string state, std::uint64_t frame)
{
    auto d = ReadDescriptor(path);
    if (!d) return;
    d->heartbeatUnixMs = unixMs;
    d->state = std::move(state);
    d->frame = frame;
    WriteDescriptorAtomic(path, *d);
}

bool IsHung(const Descriptor& desc, std::int64_t nowUnixMs)
{
    // Unsigned difference: a hostile or garbled heartbeat cannot overflow.
    if (nowUnixMs <= desc.heartbeatUnixMs) return false;
    const std::uint64_t age = static_cast<std::uint64_t>(nowUnixMs) - static_cast<std::uint64_t>(desc.heartbeatUnixMs);
    return age > static_cast<std::uint64_t>(kHungAfterMs);
}

bool IsStale(const Descriptor& desc)
{
    return !Platform::IsProcessLive(desc.pid, desc.processStart);
}

} // namespace Arcane::Remote

#endif
