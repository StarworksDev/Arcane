// servitor: the loopback CLI that attaches to running engine hosts (spec AM9).
// Everything lives in the Servitor client (src/Servitor/); this file is the
// entry point only: on Windows it hands Run a UTF-8 argv (Task 30).
#if defined(ARC_BUILD_DIST)
#include <cstdio>

int main()
{
    std::fprintf(stderr, "Servitor is compiled out of Dist builds\n");
    return 2;
}
#else
#include <Servitor/Client.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#pragma comment(lib, "shell32.lib")

#include <string>
#include <vector>
#endif

int main(int argc, char** argv)
{
#if defined(_WIN32)
    // The narrow argv is the ANSI code page: a path or value outside it
    // arrives as '?'. The wide command line is the whole truth; every Servitor
    // consumer reads argv as UTF-8 (PathFromUtf8), as on POSIX.
    int wideCount = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &wideCount)) {
        std::vector<std::string> utf8;
        utf8.reserve(static_cast<std::size_t>(wideCount));
        for (int i = 0; i < wideCount; ++i)
            utf8.push_back(Servitor::WideToUtf8(wide[i]));
        LocalFree(wide);
        std::vector<char*> args;
        args.reserve(utf8.size() + 1);
        for (std::string& arg : utf8)
            args.push_back(arg.data());
        args.push_back(nullptr);
        return Servitor::Run(wideCount, args.data());
    }
#endif
    return Servitor::Run(argc, argv);
}
#endif
