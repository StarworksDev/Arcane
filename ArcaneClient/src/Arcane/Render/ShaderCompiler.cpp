#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Core/Constant.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/RenderShaderSettings.hpp>

#include <charconv>
#include <cstring>

namespace Arcane
{
    // ------------------------------------------------------------------------
    // Diagnostic parsing (portable, pure -- unit-tested directly).
    // ------------------------------------------------------------------------
    namespace
    {
        struct SevToken
        {
            const char* token;
            ShaderDiagSeverity sev;
        };
        // Longest first: ": fatal error: " must win over ": error: ".
        constexpr SevToken kSevTokens[] = {
            { ": fatal error: ", ShaderDiagSeverity::Error },
            { ": error: ",       ShaderDiagSeverity::Error },
            { ": warning: ",     ShaderDiagSeverity::Warning },
            { ": note: ",        ShaderDiagSeverity::Note },
        };
        constexpr SevToken kBareSevTokens[] = {
            { "fatal error: ", ShaderDiagSeverity::Error },
            { "error: ",       ShaderDiagSeverity::Error },
            { "warning: ",     ShaderDiagSeverity::Warning },
            { "note: ",        ShaderDiagSeverity::Note },
        };

        bool ParseInt(std::string_view s, int& out)
        {
            if (s.empty())
                return false;
            const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
            return r.ec == std::errc() && r.ptr == s.data() + s.size() && out > 0;
        }

        // `file:line:col: sev: message` or a location-less `sev: message`.
        bool ParseDiagHeader(std::string_view line, ShaderDiag& out)
        {
            for (const SevToken& t : kSevTokens)
            {
                const std::size_t pos = line.find(t.token);
                if (pos == std::string_view::npos)
                    continue;

                std::string_view prefix = line.substr(0, pos);
                out.severity = t.sev;
                out.message = std::string(line.substr(pos + std::strlen(t.token)));

                // From the right: :col, :line, rest = file (file may contain ':').
                const std::size_t colSep = prefix.rfind(':');
                const std::size_t lineSep = colSep != std::string_view::npos && colSep > 0
                                                ? prefix.rfind(':', colSep - 1)
                                                : std::string_view::npos;
                int lineNo = 0;
                int colNo = 0;
                if (colSep != std::string_view::npos && lineSep != std::string_view::npos &&
                    ParseInt(prefix.substr(colSep + 1), colNo) &&
                    ParseInt(prefix.substr(lineSep + 1, colSep - lineSep - 1), lineNo))
                {
                    out.file = std::string(prefix.substr(0, lineSep));
                    out.line = lineNo;
                    out.col = colNo;
                }
                else
                {
                    out.file = std::string(prefix);
                    out.line = 0;
                    out.col = 0;
                }
                return true;
            }

            for (const SevToken& t : kBareSevTokens)
            {
                if (line.substr(0, std::strlen(t.token)) == t.token)
                {
                    out.severity = t.sev;
                    out.message = std::string(line.substr(std::strlen(t.token)));
                    out.file.clear();
                    out.line = 0;
                    out.col = 0;
                    return true;
                }
            }
            return false;
        }

        bool IsCaretLine(std::string_view line)
        {
            bool sawCaret = false;
            for (char c : line)
            {
                if (c == '^')
                    sawCaret = true;
                else if (c != ' ' && c != '\t' && c != '~')
                    return false;
            }
            return sawCaret;
        }

        std::vector<std::string_view> SplitLines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            std::size_t start = 0;
            while (start <= text.size())
            {
                std::size_t end = text.find('\n', start);
                if (end == std::string_view::npos)
                    end = text.size();
                std::string_view line = text.substr(start, end - start);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                lines.push_back(line);
                if (end == text.size())
                    break;
                start = end + 1;
            }
            return lines;
        }
    }

    std::vector<ShaderDiag> ParseDxcDiagnostics(std::string_view text)
    {
        std::vector<ShaderDiag> diags;
        const auto lines = SplitLines(text);

        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            ShaderDiag d;
            if (!ParseDiagHeader(lines[i], d))
                continue;

            // DXC echoes the offending source line and a caret line right after a
            // located diagnostic; capture the pair only when the caret confirms it.
            if (d.line > 0 && i + 2 < lines.size() && IsCaretLine(lines[i + 2]))
            {
                ShaderDiag probe;
                if (!ParseDiagHeader(lines[i + 1], probe))
                {
                    d.sourceLine = std::string(lines[i + 1]);
                    d.caret = std::string(lines[i + 2]);
                    i += 2;
                }
            }
            diags.push_back(std::move(d));
        }
        return diags;
    }
}


#include <Arcane/Platform/Platform.hpp>

#if ARC_PLATFORM_WINDOWS
#include <windows.h>

#include <unknwn.h>   // WIN32_LEAN_AND_MEAN strips COM from windows.h; dxcapi.h needs IUnknown

#include <dxcapi.h>
#include <wrl/client.h>
#else
// Linux port (2026-10-05): the same in-process service over libdxcompiler.so.
// The DXC release ships dxcapi.h with WinAdapter.h, its own COM shim
// (IUnknown, HRESULT, __uuidof/IID_PPV_ARGS, LPCWSTR = const wchar_t*), so
// the compile path below is shared; only module loading, the smart pointer,
// the exe path and the crash guard differ.
#include <dlfcn.h>
#include <dxcapi.h>
#include <Arcane/Platform/Process.hpp>   // LoadedLibraryPath, ExecutablePath (Linux + macOS)
#endif

#include <Arcane/Base/ServiceThread.hpp>
#include <Arcane/Config/Bindings/JobsBinding.hpp>   // jobs.shaderCompileThreads (settings arc S6-8)

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace Arcane
{
    namespace
    {
#if ARC_PLATFORM_WINDOWS
        using Microsoft::WRL::ComPtr;
#else
        // The slice of WRL's ComPtr this file uses (Get/Reset/Attach, &p for
        // IID_PPV_ARGS, which -- like WRL's operator& -- releases first).
        template <class T>
        class ComPtr
        {
        public:
            ComPtr() = default;
            ~ComPtr() { Reset(); }
            ComPtr(const ComPtr&) = delete;
            ComPtr& operator=(const ComPtr&) = delete;
            T* Get() const noexcept { return m_ptr; }
            T* operator->() const noexcept { return m_ptr; }
            explicit operator bool() const noexcept { return m_ptr != nullptr; }
            T** operator&() noexcept { Reset(); return &m_ptr; }
            void Attach(T* p) noexcept { Reset(); m_ptr = p; }
            void Reset() noexcept
            {
                if (m_ptr)
                {
                    m_ptr->Release();
                    m_ptr = nullptr;
                }
            }
        private:
            T* m_ptr = nullptr;
        };

        // A loaded shared object's own path (the toolchain hash reads its bytes).
        std::filesystem::path LoadedLibraryPath(void* handle)
        {
            return Platform::LoadedLibraryPath(handle);
        }
#endif

        ARC_CONSTANT("math: FNV-1a hash parameters")
        constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
        ARC_CONSTANT("math: FNV-1a hash parameters")
        constexpr std::uint64_t kFnvPrime = 1099511628211ull;

        std::uint64_t Fnv64(std::uint64_t h, const void* data, std::size_t size) noexcept
        {
            const auto* p = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < size; ++i)
            {
                h ^= p[i];
                h *= kFnvPrime;
            }
            return h;
        }

        std::uint64_t Fnv64Str(std::uint64_t h, std::string_view s) noexcept
        {
            return Fnv64(h, s.data(), s.size());
        }

        std::wstring Widen(std::string_view s)
        {
            std::wstring w;
            w.reserve(s.size());
            for (char c : s)
                w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            return w;
        }

        std::filesystem::path ExeDirectory()
        {
#if ARC_PLATFORM_WINDOWS
            wchar_t buf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            return std::filesystem::path(buf).parent_path();
#else
            const std::filesystem::path exe = Platform::ExecutablePath();
            std::error_code ec;
            return exe.empty() ? std::filesystem::current_path(ec) : exe.parent_path();
#endif
        }

        std::uint64_t HashFileBytes(std::uint64_t h, const std::filesystem::path& p)
        {
            std::ifstream f(p, std::ios::binary);
            if (!f)
                return h;
            char buf[64 * 1024];
            while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
                h = Fnv64(h, buf, static_cast<std::size_t>(f.gcount()));
            return h;
        }

        // The one SEH frame: treat dxcompiler as untrusted input -- a compiler
        // crash fails the job, it never unwinds the engine. No C++ objects with
        // destructors may live in this function (C2712).
        int CompileGuarded(IDxcCompiler3* compiler, const DxcBuffer* source,
                           LPCWSTR* argv, UINT32 argc, IDxcResult** outResult,
                           HRESULT* outHr) noexcept
        {
#if ARC_PLATFORM_WINDOWS
            __try
            {
                *outHr = compiler->Compile(source, argv, argc, nullptr, IID_PPV_ARGS(outResult));
                return 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 1;
            }
#else
            // No SEH off-Windows: a crash inside libdxcompiler.so is a process
            // crash (the Diagnostics crash path owns it), never a job failure.
            *outHr = compiler->Compile(source, argv, argc, nullptr, IID_PPV_ARGS(outResult));
            return 0;
#endif
        }

        // Build the CLI-shaped argv for one target (argv[0] = the virtual source
        // name, exactly how UE feeds IDxcCompiler3 -- DXC uses it for diagnostics).
        std::vector<std::string> BuildArgs(const ShaderCompileRequest& req, bool spirv,
                                           const std::vector<std::wstring>& settingsArgs)
        {
            std::vector<std::string> args;
            args.push_back(req.debugName);
            args.push_back("-T");
            args.push_back(req.profile);
            args.push_back("-E");
            args.push_back(req.entry);
            for (const auto& [name, value] : req.defines)
            {
                args.push_back("-D");
                args.push_back(value.empty() ? name : name + "=" + value);
            }
            if (spirv)
            {
                for (std::size_t i = 0; i < kSpirvArgCount; ++i)
                    args.push_back(kSpirvArgs[i]);
            }
            for (const std::wstring& arg : settingsArgs)
            {
                std::string narrow;
                narrow.reserve(arg.size());
                for (wchar_t ch : arg)
                    narrow.push_back(static_cast<char>(ch)); // DXC switches are ASCII
                args.push_back(std::move(narrow));
            }
            return args;
        }

        std::string JoinRepro(const std::vector<std::string>& args)
        {
            std::string cmd = "dxc.exe";
            for (const std::string& a : args)
            {
                cmd += ' ';
                cmd += a;
            }
            return cmd;
        }

        ShaderTargetResult CompileTarget(IDxcCompiler3* compiler,
                                         const ShaderCompileRequest& req, bool spirv,
                                         bool& crashed, bool& environmental,
                                         const std::vector<std::wstring>& settingsArgs)
        {
            ShaderTargetResult out;
            const std::vector<std::string> args = BuildArgs(req, spirv, settingsArgs);
            out.reproCmdLine = JoinRepro(args);

            std::vector<std::wstring> wide;
            wide.reserve(args.size());
            for (const std::string& a : args)
                wide.push_back(Widen(a));
            std::vector<LPCWSTR> argv;
            argv.reserve(wide.size());
            for (const std::wstring& w : wide)
                argv.push_back(w.c_str());

            DxcBuffer source{};
            source.Ptr = req.sourceUtf8.data();
            source.Size = req.sourceUtf8.size();
            source.Encoding = DXC_CP_UTF8;

            IDxcResult* rawResult = nullptr;
            HRESULT hr = E_FAIL;
            if (CompileGuarded(compiler, &source, argv.data(),
                               static_cast<UINT32>(argv.size()), &rawResult, &hr) != 0)
            {
                crashed = true;
                ShaderDiag d;
                d.file = req.debugName;
                d.severity = ShaderDiagSeverity::Error;
                d.message = "shader compiler crashed (structured exception inside dxcompiler.dll)";
                out.diags.push_back(std::move(d));
                return out;
            }
            ComPtr<IDxcResult> result;
            result.Attach(rawResult);

            if (FAILED(hr) || !result)
            {
                environmental = true;   // toolchain hiccup, not a content verdict
                ShaderDiag d;
                d.file = req.debugName;
                d.severity = ShaderDiagSeverity::Error;
                d.message = "IDxcCompiler3::Compile failed (hr=" + std::to_string(hr) + ")";
                out.diags.push_back(std::move(d));
                return out;
            }

            // Read the error buffer EVEN ON SUCCESS -- warnings live there.
            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
                errors && errors->GetStringLength() > 0)
            {
                out.diags = ParseDxcDiagnostics(
                    std::string_view(errors->GetStringPointer(), errors->GetStringLength()));
            }

            HRESULT status = E_FAIL;
            result->GetStatus(&status);
            if (FAILED(status))
                return out;

            ComPtr<IDxcBlob> object;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) &&
                object && object->GetBufferSize() > 0)
            {
                const auto* p = static_cast<const std::uint8_t*>(object->GetBufferPointer());
                out.bytecode.assign(p, p + object->GetBufferSize());
                out.succeeded = true;
            }
            else
            {
                ShaderDiag d;
                d.file = req.debugName;
                d.severity = ShaderDiagSeverity::Error;
                d.message = "compile reported success but produced no object blob";
                out.diags.push_back(std::move(d));
            }
            return out;
        }
    }

    struct ShaderCompiler::Impl
    {
        struct Job
        {
            std::uint64_t jobId = 0;
            ShaderCompileRequest req;
            std::uint64_t contentHash = 0;
        };
        struct Pending
        {
            std::uint64_t jobId = 0;
            double readyAt = 0.0;
            ShaderCompileRequest req;
        };

        // Toolchain (set once in Initialize).
#if ARC_PLATFORM_WINDOWS
        HMODULE hDxil = nullptr;
        HMODULE hCompiler = nullptr;   // stays loaded for process lifetime (COM DLL unload is unsafe)
#else
        void* hDxil = nullptr;         // dlopen handles; same never-unload rule
        void* hCompiler = nullptr;
#endif
        DxcCreateInstanceProc createInstance = nullptr;
        std::uint64_t toolchainHash = 0;
        double debounce = 0.0;
        std::vector<std::wstring> settingsArgs;

        // Main-thread state (Submit/Poll/Drain/CompileNow are main-thread-only).
        std::uint64_t nextJobId = 1;
        std::unordered_map<std::uint64_t, Pending> pendingByKey;   // coalesceKey != 0
        std::deque<Pending> pendingKeyless;
        std::unordered_map<std::uint64_t, std::uint64_t> latestJobForKey;
        std::unordered_map<std::uint64_t, ShaderCompileResult> cache;      // contentHash ->
        std::unordered_map<std::uint64_t, ShaderCompileResult> lastGood;   // coalesceKey ->
        std::vector<ShaderCompileResult> readyMain;   // cache-hit completions
        ComPtr<IDxcCompiler3> mainCompiler;           // CompileNow's instance

        // Worker state. The queue/mutex/cv stay HERE -- they are the debounce
        // and coalescing machinery, not generic plumbing. ServiceThread owns
        // only each thread's lifetime. jobs.shaderCompileThreads (settings arc
        // S6-8, Restart) workers share the ONE queue; 1, the default, compiles
        // in submission order as before. `stopping` is the shared stop flag:
        // any worker's ServiceThread wake sets it (they are only ever
        // destroyed together, in Shutdown).
        std::vector<std::unique_ptr<ServiceThread>> workers;
        bool stopping = false;   // guarded by mx
        std::mutex mx;
        std::condition_variable cv;
        std::deque<Job> queue;
        std::vector<ShaderCompileResult> done;
        int inflight = 0;      // guarded by mx

        std::uint64_t ContentHash(const ShaderCompileRequest& req) const
        {
            std::uint64_t h = kFnvOffset;
            h = Fnv64Str(h, req.sourceUtf8);
            h = Fnv64Str(h, req.entry);
            h = Fnv64Str(h, req.profile);
            for (const auto& [name, value] : req.defines)
            {
                h = Fnv64Str(h, name);
                h = Fnv64Str(h, value);
            }
            for (std::size_t i = 0; i < kSpirvArgCount; ++i)
                h = Fnv64Str(h, kSpirvArgs[i]);
            h = Fnv64(h, &toolchainHash, sizeof(toolchainHash));
            // Preserve every legacy default key: only an explicit DXC switch
            // contributes to the hash.
            for (const std::wstring& arg : settingsArgs)
                h = Fnv64(h, arg.data(), arg.size() * sizeof(wchar_t));
            return h;
        }

        ShaderCompileResult RunJob(IDxcCompiler3* compiler, const Job& job)
        {
            ShaderCompileResult r;
            r.jobId = job.jobId;
            r.coalesceKey = job.req.coalesceKey;
            r.contentHash = job.contentHash;
            r.debugName = job.req.debugName;
            r.dxil = CompileTarget(compiler, job.req, /*spirv=*/false, r.crashed, r.environmental, settingsArgs);
            if (r.crashed)
            {
                // The instance may be corrupt after the SEH crash -- never run
                // the second target through it (the caller drops the instance).
                ShaderDiag d;
                d.file = job.req.debugName;
                d.severity = ShaderDiagSeverity::Error;
                d.message = "SPIR-V compile skipped (compiler crashed on the DXIL target)";
                r.spirv.diags.push_back(std::move(d));
                return r;
            }
            r.spirv = CompileTarget(compiler, job.req, /*spirv=*/true, r.crashed, r.environmental, settingsArgs);
            return r;
        }

        void WorkerMain()
        {
            ComPtr<IDxcCompiler3> compiler;   // one instance per thread
            for (;;)
            {
                Job job;
                {
                    std::unique_lock lk(mx);
                    cv.wait(lk, [this] { return stopping || !queue.empty(); });
                    if (stopping)
                        return;
                    job = std::move(queue.front());
                    queue.pop_front();
                    ++inflight;
                }

                if (!compiler)
                    createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));

                ShaderCompileResult r = compiler
                    ? RunJob(compiler.Get(), job)
                    : ShaderCompileResult{};   // instance creation failed
                if (!compiler)
                {
                    r.jobId = job.jobId;
                    r.coalesceKey = job.req.coalesceKey;
                    r.contentHash = job.contentHash;
                    r.debugName = job.req.debugName;
                    r.environmental = true;
                    ShaderDiag d;
                    d.file = job.req.debugName;
                    d.message = "DxcCreateInstance failed on the compile worker";
                    r.dxil.diags.push_back(d);
                    r.spirv.diags.push_back(std::move(d));
                }
                if (r.crashed)
                    compiler.Reset();   // recreate a fresh instance for the next job

                {
                    std::lock_guard lk(mx);
                    done.push_back(std::move(r));
                    --inflight;
                }
            }
        }

        void Dispatch(Pending&& p)
        {
            const std::uint64_t hash = ContentHash(p.req);
            if (auto it = cache.find(hash); it != cache.end())
            {
                ShaderCompileResult r = it->second;
                r.jobId = p.jobId;
                r.coalesceKey = p.req.coalesceKey;
                r.fromCache = true;
                readyMain.push_back(std::move(r));
                return;
            }
            {
                std::lock_guard lk(mx);
                queue.push_back(Job{ p.jobId, std::move(p.req), hash });
            }
            cv.notify_one();
        }

        // Cache + last-good bookkeeping. Crashed/environmental results are never
        // cached (no content verdict). updateLastGood=false for superseded
        // results: their bytecode is still cache-worthy (undo back to a
        // just-compiled state must hit), but lastGood stays latest-only.
        void Absorb(const ShaderCompileResult& r, bool updateLastGood)
        {
            if (!r.fromCache && !r.crashed && !r.environmental)
            {
                ShaderCompileResult stored = r;
                stored.fromCache = false;
                cache[r.contentHash] = std::move(stored);
            }
            if (updateLastGood && r.coalesceKey != 0 && r.AllSucceeded())
                lastGood[r.coalesceKey] = r;
        }
    };

    ShaderCompiler::ShaderCompiler() : m_impl(new Impl) {}

    ShaderCompiler::~ShaderCompiler()
    {
        Shutdown();
        delete m_impl;
    }

    bool ShaderCompiler::Initialize()
    {
        return InitializeWithDebounce(Settings<RenderShaderSettings>().compileDebounceSeconds);
    }

    bool ShaderCompiler::InitializeWithDebounce(double debounceSeconds)
    {
        Impl& im = *m_impl;
        if (m_available)
            return true;
        im.debounce = debounceSeconds;
        im.settingsArgs = DxcArgumentsFor(Settings<RenderShaderSettings>());

#if ARC_PLATFORM_WINDOWS
        // Vendored trio beside the exe first (the postbuild copies), then the
        // regular search path. dxil.dll loads FIRST so dxcompiler's validator
        // finds it (missing validator = unsigned DXIL, warn but continue).
        const std::filesystem::path exeDir = ExeDirectory();
        const std::filesystem::path dxilPath = exeDir / L"dxil.dll";
        const std::filesystem::path compilerPath = exeDir / L"dxcompiler.dll";

        im.hDxil = LoadLibraryW(dxilPath.c_str());
        if (!im.hDxil)
            im.hDxil = LoadLibraryW(L"dxil.dll");
        if (!im.hDxil)
            ARC_WARN("ShaderCompiler: dxil.dll not found -- DXIL output will be unsigned");

        im.hCompiler = LoadLibraryW(compilerPath.c_str());
        if (!im.hCompiler)
            im.hCompiler = LoadLibraryW(L"dxcompiler.dll");
        if (!im.hCompiler)
        {
            ARC_ERROR("ShaderCompiler: dxcompiler.dll not found -- runtime compiles unavailable");
            return false;
        }

        im.createInstance = reinterpret_cast<DxcCreateInstanceProc>(
            GetProcAddress(im.hCompiler, "DxcCreateInstance"));
        if (!im.createInstance)
        {
            ARC_ERROR("ShaderCompiler: DxcCreateInstance export missing from dxcompiler.dll");
            return false;
        }

        // Toolchain bytes fold into every content hash -- a dxc upgrade
        // auto-invalidates the whole cache (UE's DXCWrapper trick).
        wchar_t modulePath[MAX_PATH] = {};
        GetModuleFileNameW(im.hCompiler, modulePath, MAX_PATH);
        std::uint64_t th = kFnvOffset;
        th = HashFileBytes(th, modulePath);
        if (im.hDxil)
        {
            GetModuleFileNameW(im.hDxil, modulePath, MAX_PATH);
            th = HashFileBytes(th, modulePath);
        }
        im.toolchainHash = th;
#else
        // Linux: the fetched release's libdxil.so/libdxcompiler.so beside the
        // exe first (the postbuild copies them when scripts/fetch-dxc-linux.sh
        // has run), then the loader's search path. Same order and the same
        // degradations as the Windows trio above. macOS: the Vulkan SDK's
        // libdxcompiler.dylib (scripts/fetch-vulkan-sdk-macos.sh); the SDK
        // ships no libdxil, so DXIL is unsigned there -- and never loaded, a
        // Mac running the Vulkan backend only.
        const std::filesystem::path exeDir = ExeDirectory();
        const std::string dxilName = Platform::SharedLibraryFileName("dxil");
        const std::string compilerName = Platform::SharedLibraryFileName("dxcompiler");
        im.hDxil = ::dlopen((exeDir / dxilName).c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!im.hDxil)
            im.hDxil = ::dlopen(dxilName.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!im.hDxil)
        {
#if ARC_PLATFORM_MACOS
            ARC_INFO("ShaderCompiler: no {} on macOS -- DXIL output (unused here) will be unsigned", dxilName);
#else
            ARC_WARN("ShaderCompiler: {} not found -- DXIL output will be unsigned", dxilName);
#endif
        }

        im.hCompiler = ::dlopen((exeDir / compilerName).c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!im.hCompiler)
            im.hCompiler = ::dlopen(compilerName.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!im.hCompiler)
        {
            ARC_ERROR("ShaderCompiler: {} not found -- runtime compiles unavailable", compilerName);
            return false;
        }

        im.createInstance = reinterpret_cast<DxcCreateInstanceProc>(
            ::dlsym(im.hCompiler, "DxcCreateInstance"));
        if (!im.createInstance)
        {
            ARC_ERROR("ShaderCompiler: DxcCreateInstance export missing from {}", compilerName);
            return false;
        }

        std::uint64_t th = kFnvOffset;
        th = HashFileBytes(th, LoadedLibraryPath(im.hCompiler));
        if (im.hDxil)
            th = HashFileBytes(th, LoadedLibraryPath(im.hDxil));
        im.toolchainHash = th;
#endif

        im.workers.clear();   // a re-Initialize replaces the workers, as the old optional's emplace did
        {
            std::lock_guard lk(im.mx);
            im.stopping = false;
        }
        const std::uint32_t threads = std::max<std::uint32_t>(1u, Settings<JobsSettings>().shaderCompileThreads);
        for (std::uint32_t i = 0; i < threads; ++i)
            im.workers.push_back(std::make_unique<ServiceThread>(
                i == 0 ? std::string("shader.compile") : std::format("shader.compile.{}", i),
                [this] { m_impl->WorkerMain(); },
                [this] { { std::lock_guard lk(m_impl->mx); m_impl->stopping = true; } m_impl->cv.notify_all(); }));
        m_available = true;
        ARC_INFO("ShaderCompiler: in-process dxc ready (debounce {:.0f} ms)", debounceSeconds * 1000.0);
        return true;
    }

    void ShaderCompiler::Shutdown()
    {
        Impl& im = *m_impl;
        // ~ServiceThread requests stop, fires the wake callback (which takes mx,
        // sets the shared stopping flag and notifies cv), and joins. Clearing
        // the vector runs it for every worker here.
        im.workers.clear();
        im.mainCompiler.Reset();
        // hCompiler/hDxil stay loaded on purpose: unloading a COM-style DLL that
        // may still own module-static state is a classic shutdown crash; the OS
        // reclaims them at process exit.
        m_available = false;
    }

    std::uint64_t ShaderCompiler::Submit(ShaderCompileRequest req, double now)
    {
        Impl& im = *m_impl;
        if (!m_available)
            return 0;

        const std::uint64_t jobId = im.nextJobId++;
        Impl::Pending p{ jobId, now + im.debounce, std::move(req) };

        if (p.req.coalesceKey != 0)
        {
            im.latestJobForKey[p.req.coalesceKey] = jobId;
            im.pendingByKey[p.req.coalesceKey] = std::move(p);   // newer replaces pending older
        }
        else
        {
            im.pendingKeyless.push_back(std::move(p));
        }
        return jobId;
    }

    void ShaderCompiler::Poll(double now)
    {
        Impl& im = *m_impl;
        if (!m_available)
            return;

        for (auto it = im.pendingByKey.begin(); it != im.pendingByKey.end();)
        {
            if (now >= it->second.readyAt)
            {
                Impl::Pending p = std::move(it->second);
                it = im.pendingByKey.erase(it);
                im.Dispatch(std::move(p));
            }
            else
            {
                ++it;
            }
        }
        while (!im.pendingKeyless.empty() && now >= im.pendingKeyless.front().readyAt)
        {
            Impl::Pending p = std::move(im.pendingKeyless.front());
            im.pendingKeyless.pop_front();
            im.Dispatch(std::move(p));
        }
    }

    std::vector<ShaderCompileResult> ShaderCompiler::Drain()
    {
        Impl& im = *m_impl;
        std::vector<ShaderCompileResult> raw = std::move(im.readyMain);
        im.readyMain.clear();
        {
            std::lock_guard lk(im.mx);
            raw.insert(raw.end(), std::make_move_iterator(im.done.begin()),
                       std::make_move_iterator(im.done.end()));
            im.done.clear();
        }

        std::vector<ShaderCompileResult> out;
        out.reserve(raw.size());
        for (ShaderCompileResult& r : raw)
        {
            // Superseded: a newer Submit for the same key exists -- drop it (the
            // newer job's result is the one the consumer must see). Its bytecode
            // still enters the content cache so undoing back to this state is a
            // cache hit, but lastGood stays latest-only.
            if (r.coalesceKey != 0)
            {
                const auto it = im.latestJobForKey.find(r.coalesceKey);
                if (it != im.latestJobForKey.end() && it->second != r.jobId)
                {
                    im.Absorb(r, /*updateLastGood=*/false);
                    continue;
                }
            }
            im.Absorb(r, /*updateLastGood=*/true);
            out.push_back(std::move(r));
        }
        return out;
    }

    bool ShaderCompiler::IsIdle() const
    {
        Impl& im = *m_impl;
        if (!im.pendingByKey.empty() || !im.pendingKeyless.empty() || !im.readyMain.empty())
            return false;
        std::lock_guard lk(im.mx);
        return im.queue.empty() && im.inflight == 0 && im.done.empty();
    }

    std::size_t ShaderCompiler::UndrainedCount() const
    {
        Impl& im = *m_impl;
        std::lock_guard lk(im.mx);
        return im.readyMain.size() + im.done.size();
    }

    ShaderCompileResult ShaderCompiler::CompileNow(const ShaderCompileRequest& req)
    {
        Impl& im = *m_impl;
        ShaderCompileResult r;
        if (!m_available)
        {
            r.debugName = req.debugName;
            return r;
        }

        const std::uint64_t hash = im.ContentHash(req);
        const std::uint64_t jobId = im.nextJobId++;
        if (auto it = im.cache.find(hash); it != im.cache.end())
        {
            r = it->second;
            r.jobId = jobId;
            r.coalesceKey = req.coalesceKey;
            r.fromCache = true;
            im.Absorb(r, /*updateLastGood=*/true);   // sync cache hit still refreshes LastGood
            return r;
        }

        if (!im.mainCompiler)
            im.createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&im.mainCompiler));
        if (!im.mainCompiler)
        {
            r.debugName = req.debugName;
            r.environmental = true;
            return r;
        }

        r = im.RunJob(im.mainCompiler.Get(), Impl::Job{ jobId, req, hash });
        if (r.crashed)
            im.mainCompiler.Reset();   // possibly-corrupt instance; recreate next call
        im.Absorb(r, /*updateLastGood=*/true);
        return r;
    }

    const ShaderCompileResult* ShaderCompiler::LastGood(std::uint64_t coalesceKey) const
    {
        const auto it = m_impl->lastGood.find(coalesceKey);
        return it != m_impl->lastGood.end() ? &it->second : nullptr;
    }
}
