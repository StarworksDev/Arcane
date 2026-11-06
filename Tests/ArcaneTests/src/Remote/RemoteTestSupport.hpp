#pragma once
#if !defined(ARC_BUILD_DIST)
#include <Arcane/Remote/Registry.hpp>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace RemoteTest {

// Sets the process registry's host kind; restores the previous kind on exit.
class ScopedHostKind {
public:
    explicit ScopedHostKind(Arcane::Remote::HostKind kind)
        : m_before(Arcane::Remote::CommandRegistry::Get().GetHostKind()) {
        Arcane::Remote::CommandRegistry::Get().SetHostKind(kind);
    }
    ~ScopedHostKind() { Arcane::Remote::CommandRegistry::Get().SetHostKind(m_before); }
    ScopedHostKind(const ScopedHostKind&) = delete;
    ScopedHostKind& operator=(const ScopedHostKind&) = delete;
private:
    Arcane::Remote::HostKind m_before;
};

// Calls setter(value) now and setter(nullptr) on scope exit, so a process-global
// command never keeps a pointer to a test's dead stack object. The setter is
// any callable taking T* (a Register*/Set* function, or a lambda adapting one;
// its return value is ignored).
template <class T>
class ScopedRemotePointer {
public:
    template <class Fn>
    ScopedRemotePointer(Fn setter, T* value)
        : m_set([setter](T* p) mutable { (void)setter(p); }) {
        m_set(value);
    }
    ~ScopedRemotePointer() { m_set(nullptr); }
    ScopedRemotePointer(const ScopedRemotePointer&) = delete;
    ScopedRemotePointer& operator=(const ScopedRemotePointer&) = delete;
private:
    std::function<void(T*)> m_set;
};

// Sets (a value) or unsets (nullopt) one environment variable; restores the
// previous value, or its absence, on scope exit. Modelled on
// ScopedEnvironmentValue (MeshNodeTest.cpp). _putenv_s(name, "") removes the
// variable on Windows; POSIX spells that unsetenv.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::optional<std::string>& value) : m_name(name) {
        if (const char* old = std::getenv(name)) {
            m_hadOld = true;
            m_old = old;
        }
        Apply(value);
    }
    ~ScopedEnv() { Apply(m_hadOld ? std::optional<std::string>(m_old) : std::nullopt); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
private:
    void Apply(const std::optional<std::string>& value) {
#if defined(_WIN32)
        _putenv_s(m_name, value ? value->c_str() : "");
#else
        if (value) ::setenv(m_name, value->c_str(), 1);
        else       ::unsetenv(m_name);
#endif
    }
    const char* m_name = nullptr;
    bool m_hadOld = false;
    std::string m_old;
};

}  // namespace RemoteTest
#endif
