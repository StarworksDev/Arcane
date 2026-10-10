#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/ConsoleModel.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Render/Nri/nodes/MeshCullNode.hpp>
#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Base/Log.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace Arcane;

TEST_CASE("cvar values round-trip and clamp", "[cvar]") {
    const CVarValue wide = CVarValue::UInt32(0xffffffffu);
    REQUIRE(wide.type == CVarType::UInt32);
    REQUIRE(wide.AsUInt32() == 0xffffffffu);
    const CVarValue bigger = CVarValue::UInt64(std::numeric_limits<std::uint64_t>::max());
    REQUIRE(bigger.AsUInt64() == std::numeric_limits<std::uint64_t>::max());

    const CVarValue clamped = Clamp(CVarValue::Int32(50), CVarValue::Int32(0), CVarValue::Int32(10));
    REQUIRE(clamped.AsInt32() == 10);
    const CVarValue lifted = Clamp(CVarValue::Int32(-3), CVarValue::Int32(0), CVarValue::Int32(10));
    REQUIRE(lifted.AsInt32() == 0);
    REQUIRE(Clamp(CVarValue::String("nope"), CVarValue::Int32(0), CVarValue::Int32(1)).AsString() == "nope");
}

TEST_CASE("cvar registry owns the value and kills stale handles", "[cvar]") {
    CVarRegistry reg;
    const CVarHandle h = reg.Register(CVarDesc{
        "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false),
        std::nullopt, std::nullopt, CVarFlags::Dev, "markers", "engine" });
    REQUIRE_FALSE(h.IsStale());
    REQUIRE(reg.Get(h)->AsBool() == false);

    reg.UnregisterModule("engine");
    REQUIRE_FALSE(reg.Get(h).has_value());

    const CVarHandle again = reg.Register(CVarDesc{
        "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(true),
        std::nullopt, std::nullopt, CVarFlags::None, "test cvar", "engine" });
    REQUIRE(again.index == h.index);
    REQUIRE(again.generation != h.generation);
    REQUIRE_FALSE(reg.Get(h).has_value());
    REQUIRE(reg.Get(again)->AsBool() == true);
}

TEST_CASE("duplicate cvar registration names both modules", "[cvar]") {
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{ "a.b", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "test cvar", "one" }).IsStale());
    REQUIRE(reg.Register(CVarDesc{ "a.b", CVarType::Int32, CVarValue::Int32(2), {}, {}, {}, "test cvar", "two" }).IsStale());
    REQUIRE(reg.LastError().find("one") != std::string::npos);
    REQUIRE(reg.LastError().find("two") != std::string::npos);
    REQUIRE(reg.Get(reg.Find("a.b"))->AsInt32() == 1);
}

TEST_CASE("weaker SetBy cannot stomp, and module unload pops its sets", "[cvar]") {
    CVarRegistry reg;
    const CVarHandle h = reg.Register(CVarDesc{
        "render.meshCull", CVarType::Bool, CVarValue::Bool(true), {}, {}, {}, "test cvar", "engine" });
    REQUIRE(reg.Set(h, CVarValue::Bool(false), SetBy::Project, "project") == SetResult::Applied);
    REQUIRE(reg.Set(h, CVarValue::Bool(true), SetBy::EngineConfig, "engine-config") == SetResult::RefusedWeaker);
    reg.Publish();
    REQUIRE(reg.Get(h)->AsBool() == false);

    REQUIRE(reg.Set(h, CVarValue::Bool(true), SetBy::Code, "tool") == SetResult::Applied);
    REQUIRE(reg.Set(h, CVarValue::Bool(false), SetBy::Console, "user") == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Get(h)->AsBool() == false);
    REQUIRE(reg.Explain("render.meshCull")->setBy == SetBy::Console);

    reg.UnregisterModule("user");
    reg.Publish();
    REQUIRE(reg.Get(h)->AsBool() == true);
    REQUIRE(reg.Explain("render.meshCull")->setBy == SetBy::Code);
}

TEST_CASE("a set is invisible until publish, and a callback set waits", "[cvar]") {
    CVarRegistry reg;
    const CVarHandle h = reg.Register(CVarDesc{
        "debug.step", CVarType::Int32, CVarValue::Int32(0), {}, {}, CVarFlags::UserSettable, "test cvar", "engine" });
    REQUIRE(reg.Set(h, CVarValue::Int32(3), SetBy::Code) == SetResult::Applied);
    REQUIRE(reg.Get(h)->AsInt32() == 0);
    int fires = 0;
    struct State { CVarRegistry* reg; int* fires; CVarHandle handle; };
    State state{ &reg, &fires, h };
    reg.AddCallback(h, [](CVarHandle handle, void* user) {
        auto* s = static_cast<State*>(user);
        ++(*s->fires);
        s->reg->Set(handle, CVarValue::Int32(9), SetBy::Code);
    }, &state);
    reg.Publish();
    REQUIRE(fires == 1);
    REQUIRE(reg.Get(h)->AsInt32() == 3);
    reg.Publish();
    REQUIRE(fires == 2);
    REQUIRE(reg.Get(h)->AsInt32() == 9);
    reg.Publish();
    REQUIRE(fires == 2);
}

TEST_CASE("cvarlist hides Hidden and cvar_explain names the winning layer", "[cvar]") {
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{ "secret.token", CVarType::String, CVarValue::String("x"), {}, {}, CVarFlags::Hidden, "test cvar", "engine" }).IsStale());
    REQUIRE_FALSE(reg.Register(CVarDesc{ "visible.knob", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "a knob", "engine" }).IsStale());
    const auto list = reg.List();
    bool sawSecret = false;
    bool sawKnob = false;
    for (const auto& e : list)
    {
        if (e.name == "secret.token") sawSecret = true;
        if (e.name == "visible.knob") sawKnob = true;
    }
    REQUIRE_FALSE(sawSecret);
    REQUIRE(sawKnob);

    const ExecResult explained = reg.Execute("cvar_explain visible.knob", CVarContext::Editor);
    REQUIRE(explained.ok);
    REQUIRE(explained.text.find("Default") != std::string::npos);
    REQUIRE(explained.text.find("visible.knob") != std::string::npos);
}

TEST_CASE("default-deny and cheat revert", "[cvar]") {
    CVarRegistry reg;
    CVarDesc plainDesc;
    plainDesc.name = "game.speed"; plainDesc.type = CVarType::Float32; plainDesc.defaultValue = CVarValue::Float32(1.f);
    plainDesc.help = "Speed."; plainDesc.module = "engine";
    const CVarHandle plain = reg.Register(plainDesc);
    REQUIRE(reg.Set(plain, CVarValue::Float32(2.f), SetBy::Console, {}, CVarContext::Client) == SetResult::Denied);
    REQUIRE(reg.Set(plain, CVarValue::Float32(2.f), SetBy::Console, {}, CVarContext::Editor) == SetResult::Applied);

    CVarDesc cheatDesc;
    cheatDesc.name = "game.noclip"; cheatDesc.type = CVarType::Bool; cheatDesc.defaultValue = CVarValue::Bool(false);
    cheatDesc.flags = CVarFlags::Cheat; cheatDesc.help = "Noclip."; cheatDesc.module = "engine";
    const CVarHandle cheat = reg.Register(cheatDesc);
    REQUIRE(reg.Set(cheat, CVarValue::Bool(false), SetBy::Project, "project") == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Set(cheat, CVarValue::Bool(true), SetBy::Code, "tool", CVarContext::Client) == SetResult::Denied);

    const CVarHandle gate = reg.Find("server.cheats");   // registered by every registry (settings spec s3.2)
    REQUIRE_FALSE(gate.IsStale());
    REQUIRE(reg.Set(gate, CVarValue::Bool(true), SetBy::Console, {}, CVarContext::Client) == SetResult::Denied);
    REQUIRE(reg.Set(gate, CVarValue::Bool(true), SetBy::Code, "editor", CVarContext::Editor) == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Set(cheat, CVarValue::Bool(true), SetBy::Code, "tool", CVarContext::Client) == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Get(cheat)->AsBool() == true);

    REQUIRE(reg.Set(gate, CVarValue::Bool(false), SetBy::Code, "editor", CVarContext::Editor) == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Get(cheat)->AsBool() == false);
    REQUIRE(reg.Explain("game.noclip")->setBy == SetBy::Project);
}

TEST_CASE("config apply warns on a cvar category and ignores a document", "[cvar]") {
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{ "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false), {}, {}, CVarFlags::Archive, "test cvar", "engine" }).IsStale());
    nlohmann::json diagnostics = { {"drawMarkers", true}, {"notACvar", 1} };
    const CVarApplyReport cvars = ApplyCVarCategory(reg, "diagnostics", diagnostics, SetBy::Project, false, "project");
    REQUIRE(cvars.unknownKeys.size() == 1);
    REQUIRE(cvars.unknownKeys[0] == "diagnostics.notACvar");
    reg.Publish();
    REQUIRE(reg.Get(reg.Find("diagnostics.drawMarkers"))->AsBool() == true);

    nlohmann::json input = { {"actionMaps", nlohmann::json::array()} };
    const CVarApplyReport document = ApplyCVarCategory(reg, "input", input, SetBy::Project, true, "project");
    REQUIRE(document.unknownKeys.empty());

    const auto user = std::filesystem::temp_directory_path() / "arcane-cvar-archive-test";
    std::filesystem::remove_all(user);
    WriteCVarArchive(reg, user);
    REQUIRE_FALSE(std::filesystem::exists(user / "diagnostics.json"));

    // T3-D2: only the User rung is archived -- a console set is a session value.
    REQUIRE(reg.Set(reg.Find("diagnostics.drawMarkers"), CVarValue::Bool(false), SetBy::Console) == SetResult::Applied);
    reg.Publish();
    WriteCVarArchive(reg, user);
    REQUIRE_FALSE(std::filesystem::exists(user / "diagnostics.json"));

    CVarRegistry userReg;
    REQUIRE_FALSE(userReg.Register(CVarDesc{ "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false), {}, {}, CVarFlags::Archive, "test cvar", "engine" }).IsStale());
    REQUIRE(userReg.Set(userReg.Find("diagnostics.drawMarkers"), CVarValue::Bool(true), SetBy::User) == SetResult::Applied);
    userReg.Publish();
    WriteCVarArchive(userReg, user);
    REQUIRE(std::filesystem::exists(user / "diagnostics.json"));
    std::filesystem::remove_all(user);
}

namespace
{
    std::string ReadText(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    void WriteText(const std::filesystem::path& file, const std::string& text)
    {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
    // The T3-D2 roster: what the archive may and may not write.
    void RegisterArchiveRoster(CVarRegistry& reg)
    {
        const auto add = [&](const char* name, CVarType type, CVarValue def, CVarFlags flags)
        {
            REQUIRE_FALSE(reg.Register(CVarDesc{ name, type, def, {}, {}, flags, "test cvar", "test" }).IsStale());
        };
        add("editor.legend", CVarType::Bool, CVarValue::Bool(true), CVarFlags::Archive);
        add("editor.graph.zoom", CVarType::Float32, CVarValue::Float32(1.0f), CVarFlags::Archive);   // a dotted key
        add("editor.plain", CVarType::Bool, CVarValue::Bool(false), CVarFlags::UserSettable);        // not Archive
        add("editor.untouched", CVarType::Int32, CVarValue::Int32(3), CVarFlags::Archive);           // default only
        add("editor.cheat", CVarType::Bool, CVarValue::Bool(false), CVarFlags::Archive | CVarFlags::Cheat);
        add("editor.dev", CVarType::Bool, CVarValue::Bool(false), CVarFlags::Archive | CVarFlags::Dev);
        add("editor.cli", CVarType::Int32, CVarValue::Int32(1), CVarFlags::Archive);                 // command line only
        add("editor.project", CVarType::Int32, CVarValue::Int32(1), CVarFlags::Archive);             // project layer only
        add("editor.layered", CVarType::Int32, CVarValue::Int32(1), CVarFlags::Archive);             // user, then command line
        add("input.bindings", CVarType::Int32, CVarValue::Int32(0), CVarFlags::Archive);             // a document category
    }
}

TEST_CASE("cvar archive T3-D2: only Archive values the User rung holds round-trip through the user dir", "[cvar]") {
    const auto user = std::filesystem::temp_directory_path() / "arcane-cvar-archive-t3d2";
    std::filesystem::remove_all(user);

    CVarRegistry reg;
    RegisterArchiveRoster(reg);
    const auto set = [&](const char* name, CVarValue v, SetBy by) { REQUIRE(reg.Set(reg.Find(name), std::move(v), by, "test") == SetResult::Applied); };
    set("editor.legend", CVarValue::Bool(false), SetBy::User);
    set("editor.graph.zoom", CVarValue::Float32(1.5f), SetBy::User);
    set("editor.plain", CVarValue::Bool(true), SetBy::User);
    set("editor.cheat", CVarValue::Bool(true), SetBy::User);
    set("editor.dev", CVarValue::Bool(true), SetBy::User);
    set("editor.cli", CVarValue::Int32(9), SetBy::CommandLine);
    set("editor.project", CVarValue::Int32(7), SetBy::Project);
    set("editor.layered", CVarValue::Int32(5), SetBy::User);
    set("editor.layered", CVarValue::Int32(9), SetBy::CommandLine);   // wins the session, never the file
    set("input.bindings", CVarValue::Int32(4), SetBy::User);
    reg.Publish();
    WriteCVarArchive(reg, user);

    REQUIRE(std::filesystem::exists(user / "editor.json"));
    CHECK_FALSE(std::filesystem::exists(user / "input.json"));       // a document category is never touched
    CHECK_FALSE(std::filesystem::exists(user / "editor.json.tmp"));  // written, then renamed over
    const auto doc = nlohmann::json::parse(ReadText(user / "editor.json"));
    INFO(doc.dump());
    CHECK(doc.size() == 3);
    CHECK(doc.at("legend") == false);
    CHECK(doc.at("graph.zoom") == 1.5f);
    CHECK(doc.at("layered") == 5);

    // A fresh registry reads it back as the loader does (Runtime::OpenProject's user layer).
    CVarRegistry fresh;
    RegisterArchiveRoster(fresh);
    const CVarApplyReport report = ApplyCVarDirectory(fresh, user, SetBy::User, "user");
    CHECK(report.unknownKeys.empty());
    fresh.Publish();
    const auto get = [&](const char* name) { return *fresh.Get(fresh.Find(name)); };
    CHECK(get("editor.legend").AsBool() == false);
    CHECK(get("editor.graph.zoom").AsFloat32() == 1.5f);
    CHECK(get("editor.layered").AsInt32() == 5);
    CHECK(get("editor.plain").AsBool() == false);       // not Archive: the default
    CHECK(get("editor.untouched").AsInt32() == 3);
    CHECK(get("editor.cheat").AsBool() == false);
    CHECK(get("editor.dev").AsBool() == false);
    CHECK(get("editor.cli").AsInt32() == 1);
    CHECK(get("editor.project").AsInt32() == 1);
    CHECK(fresh.Explain("editor.untouched")->setBy == SetBy::Default);
    std::filesystem::remove_all(user);
}

TEST_CASE("cvar archive T3-D2: a write merges into the file -- foreign keys stay, nested keys update in place, an unchanged file is not rewritten", "[cvar]") {
    const auto user = std::filesystem::temp_directory_path() / "arcane-cvar-archive-merge";
    std::filesystem::remove_all(user);
    WriteText(user / "editor.json", R"({"foreign": 42, "graph": {"zoom": 0.5, "other": "keep"}, "legend": true})");

    CVarRegistry reg;
    RegisterArchiveRoster(reg);
    REQUIRE(reg.Set(reg.Find("editor.legend"), CVarValue::Bool(false), SetBy::User) == SetResult::Applied);
    REQUIRE(reg.Set(reg.Find("editor.graph.zoom"), CVarValue::Float32(2.0f), SetBy::User) == SetResult::Applied);
    reg.Publish();
    WriteCVarArchive(reg, user);
    const auto doc = nlohmann::json::parse(ReadText(user / "editor.json"));
    INFO(doc.dump());
    CHECK(doc.at("foreign") == 42);                       // a key this registry does not own
    CHECK(doc.at("graph").at("other") == "keep");
    CHECK(doc.at("graph").at("zoom") == 2.0f);            // the nested leaf, not a second flat key
    CHECK_FALSE(doc.contains("graph.zoom"));
    CHECK(doc.at("legend") == false);

    const auto older = std::filesystem::last_write_time(user / "editor.json") - std::chrono::hours(1);
    std::filesystem::last_write_time(user / "editor.json", older);
    WriteCVarArchive(reg, user);                          // nothing changed: the file is left alone
    // Parenthesised: libc++ file_time_type has an __int128 rep Catch2 cannot stringify.
    CHECK((std::filesystem::last_write_time(user / "editor.json") == older));
    std::filesystem::remove_all(user);
}

TEST_CASE("cvar archive T3-D2: a corrupt or partial user file is skipped at load, and a write keeps it aside as .bad", "[cvar]") {
    const auto user = std::filesystem::temp_directory_path() / "arcane-cvar-archive-corrupt";
    std::filesystem::remove_all(user);
    WriteText(user / "editor.json", R"({"legend": fal)");          // a write cut short
    WriteText(user / "diagnostics.json", R"({"drawMarkers": true})");

    CVarRegistry reg;
    RegisterArchiveRoster(reg);
    REQUIRE_FALSE(reg.Register(CVarDesc{ "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false), {}, {}, CVarFlags::Archive, "test cvar", "test" }).IsStale());
    const CVarApplyReport report = ApplyCVarDirectory(reg, user, SetBy::User, "user");   // no throw, no crash
    CHECK(report.unknownKeys.empty());
    reg.Publish();
    CHECK(reg.Get(reg.Find("editor.legend"))->AsBool() == true);              // the torn file: defaults
    CHECK(reg.Explain("editor.legend")->setBy == SetBy::Default);
    CHECK(reg.Get(reg.Find("diagnostics.drawMarkers"))->AsBool() == true);    // the good file beside it still applies

    REQUIRE(reg.Set(reg.Find("editor.legend"), CVarValue::Bool(false), SetBy::User) == SetResult::Applied);
    reg.Publish();
    WriteCVarArchive(reg, user);
    CHECK(ReadText(user / "editor.json.bad") == R"({"legend": fal)");
    const auto doc = nlohmann::json::parse(ReadText(user / "editor.json"));
    CHECK(doc.at("legend") == false);
    std::filesystem::remove_all(user);
}

TEST_CASE("cvar archive: a corrupt user file that cannot be kept aside as .bad is left untouched, never overwritten", "[cvar]") {
    const auto user = std::filesystem::temp_directory_path() / "arcane-cvar-archive-nobackup";
    std::filesystem::remove_all(user);
    WriteText(user / "editor.json", R"({"legend": fal)");          // possibly the user's only hand edit
    std::filesystem::create_directories(user / "editor.json.bad");  // a DIRECTORY: copy_file cannot write there

    CVarRegistry reg;
    RegisterArchiveRoster(reg);
    REQUIRE(reg.Set(reg.Find("editor.legend"), CVarValue::Bool(false), SetBy::User) == SetResult::Applied);
    reg.Publish();
    WriteCVarArchive(reg, user);
    CHECK(ReadText(user / "editor.json") == R"({"legend": fal)");   // no backup, so no overwrite
    std::filesystem::remove_all(user);
}

TEST_CASE("cvar RevertLayer drops one rung everywhere and leaves the others", "[cvar]") {
    CVarRegistry reg;
    RegisterArchiveRoster(reg);
    REQUIRE(reg.Set(reg.Find("editor.layered"), CVarValue::Int32(2), SetBy::Project) == SetResult::Applied);
    REQUIRE(reg.Set(reg.Find("editor.layered"), CVarValue::Int32(5), SetBy::User) == SetResult::Applied);
    REQUIRE(reg.Set(reg.Find("editor.legend"), CVarValue::Bool(false), SetBy::User) == SetResult::Applied);
    reg.Publish();
    reg.RevertLayer(SetBy::User);
    reg.Publish();
    CHECK(reg.Get(reg.Find("editor.layered"))->AsInt32() == 2);
    CHECK(reg.Explain("editor.layered")->setBy == SetBy::Project);
    CHECK(reg.Get(reg.Find("editor.legend"))->AsBool() == true);
    CHECK(reg.Explain("editor.legend")->setBy == SetBy::Default);
}

TEST_CASE("command line set beats user and loses to code", "[cvar]") {
    CVarRegistry reg;
    // PlayerSafe: the table lets the Client context below write it (UserSettable
    // is derived from the audience, settings spec s3.2).
    const CVarHandle h = reg.Register(CVarDesc{
        .name = "game.speed", .type = CVarType::Int32, .defaultValue = CVarValue::Int32(1),
        .help = "test cvar", .module = "engine", .audience = Audience::PlayerSafe });
    REQUIRE(reg.Set(h, CVarValue::Int32(2), SetBy::User) == SetResult::Applied);
    ApplyCVarCommandLine(reg, { "game.speed=4" }, CVarContext::Client);
    REQUIRE(reg.Set(h, CVarValue::Int32(9), SetBy::Code) == SetResult::Applied);
    ApplyCVarCommandLine(reg, { "game.speed=5" }, CVarContext::Client);
    reg.Publish();
    REQUIRE(reg.Get(h)->AsInt32() == 9);
    REQUIRE(reg.Explain("game.speed")->setBy == SetBy::Code);
}

TEST_CASE("a command and a cvar cannot share a name", "[cvar]") {
    CVarRegistry reg;
    REQUIRE(reg.RegisterCommand("game.speed", CVarFlags::None, "", "mod", [](std::string_view, std::string&, void*) {}, nullptr));
    REQUIRE(reg.Register(CVarDesc{ "game.speed", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "test cvar", "engine" }).IsStale());
    REQUIRE(reg.LastError().find("command") != std::string::npos);
}

TEST_CASE("console model submits, completes, and refuses a player", "[cvar]") {
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{
        "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false), {}, {}, {}, "markers", "engine" }).IsStale());
    REQUIRE_FALSE(reg.Register(CVarDesc{
        "game.speed", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "test cvar", "engine" }).IsStale());
    ConsoleModel model;
    model.SetInput("diag");
    const auto matches = model.Complete(reg, CVarContext::Editor);
    REQUIRE(matches.size() == 1);
    REQUIRE(matches[0] == "diagnostics.drawMarkers");
    model.SetInput("cvar_explain diagnostics.drawMarkers");
    model.Submit(reg, CVarContext::Editor);
    REQUIRE(model.Lines().size() == 2);
    REQUIRE(model.Lines().back().text.find("Default") != std::string::npos);
    model.SetInput("game.speed 3");
    model.Submit(reg, CVarContext::Client);
    REQUIRE_FALSE(model.Lines().back().ok);
    REQUIRE(reg.Get(reg.Find("game.speed"))->AsInt32() == 1);

    // An accepted set is PENDING after Submit: the console is a writer like
    // any other, and only the frame driver's Publish makes it visible
    // (spec 6.4 -- read-your-own-writes is deliberately not provided).
    model.SetInput("game.speed 3");
    model.Submit(reg, CVarContext::Editor);
    REQUIRE(model.Lines().back().ok);
    REQUIRE(reg.Get(reg.Find("game.speed"))->AsInt32() == 1);
    reg.Publish();
    REQUIRE(reg.Get(reg.Find("game.speed"))->AsInt32() == 3);
}

TEST_CASE("render.meshCull defaults on and publishes off", "[cvar]") {
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle handle = reg.Find("render.meshCull");
#if defined(ARC_BUILD_DIST)
    if (handle.IsStale()) {   // Dist compiles the Dev cvar out; missing means on
        REQUIRE(MeshCullFrustumEnabled());
        return;
    }
#endif
    // Debug/Release: a cvar some other case removed FAILS here; it never
    // passes vacuously (the S1 gate's Debug seed 1930880111 did, with 0
    // assertions, before Plugin::Load stopped wiping a live module's cvars).
    REQUIRE_FALSE(handle.IsStale());
    REQUIRE(MeshCullFrustumEnabled());
    REQUIRE(reg.Set(handle, CVarValue::Bool(false), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    REQUIRE_FALSE(MeshCullFrustumEnabled());
    REQUIRE(reg.Set(handle, CVarValue::Bool(true), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    REQUIRE(MeshCullFrustumEnabled());
}

TEST_CASE("Dev cvars are absent when the registry is built without them", "[cvar]") {
    CVarRegistry reg{ false };
    REQUIRE(reg.Register(CVarDesc{ "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false), {}, {}, CVarFlags::Dev, "test cvar", "engine" }).IsStale());
    REQUIRE(reg.Find("diagnostics.drawMarkers").IsStale());
    REQUIRE_FALSE(reg.Register(CVarDesc{ "game.speed", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "test cvar", "engine" }).IsStale());
}

namespace
{
    ARC_CVAR(cvar_rangedProbe, "tests.rangedProbe", std::int32_t, 5,
             .min = 1, .max = 10, .flags = CVarFlags::Archive,
             .help = "ARC_CVAR range probe (CVarRegistryTest).");
}

TEST_CASE("ARC_CVAR registers its range and its module", "[cvar]") {
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("tests.rangedProbe");
    REQUIRE_FALSE(h.IsStale());
    CHECK(h == cvar_rangedProbe.Handle());
    CHECK(cvar_rangedProbe.Get() == 5);

    reg.Set(h, CVarValue::Int32(50), SetBy::Console);
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 10);            // max applied
    reg.Set(h, CVarValue::Int32(-4), SetBy::Console);
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 1);             // min applied

    const CVarHandle dup = reg.Register(CVarDesc{
        "tests.rangedProbe", CVarType::Int32, CVarValue::Int32(5),
        std::nullopt, std::nullopt, CVarFlags::None, "test cvar", "engine" });
    CHECK(dup.IsStale());
    CHECK(reg.LastError().find("module 'ArcaneTests'") != std::string::npos);   // declared by the macro's module

    reg.Set(h, CVarValue::Int32(5), SetBy::Console);
    reg.Publish();
}

TEST_CASE("log.level exists with range 0..6 and its publish drives the engine logger", "[cvar]") {
    Arcane::Log::Init();
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("log.level");
#if defined(ARC_BUILD_DIST)
    if (h.IsStale()) return;   // Dist compiles the Dev cvar out
#endif
    REQUIRE_FALSE(h.IsStale());   // Debug/Release: the pre-implementation run FAILS here
    const std::int32_t before = reg.Get(h)->AsInt32();
    REQUIRE(reg.Explain("log.level")->help.find("0 trace") != std::string::npos);
    REQUIRE(reg.Set(h, CVarValue::Int32(9), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Get(h)->AsInt32() == 6);                                        // clamped to max
    REQUIRE(Arcane::Log::Engine()->level() == spdlog::level::off);
    REQUIRE(reg.Set(h, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    REQUIRE(Arcane::Log::Engine()->level() == spdlog::level::debug);
    REQUIRE(reg.Set(h, CVarValue::Int32(before), SetBy::Code) == SetResult::Applied);   // leave the suite's logging as found
    reg.Publish();
    REQUIRE(Arcane::Log::Engine()->level() == static_cast<spdlog::level::level_enum>(before));
}

TEST_CASE("ListCommands lists live commands with List's Hidden/Dev rule; Complete covers them", "[cvar]") {
    CVarRegistry reg;
    bool sawList = false;
    for (const CVarListEntry& e : reg.ListCommands()) sawList = sawList || e.name == "cvarlist";
    REQUIRE(sawList);
    REQUIRE_FALSE(reg.Register(CVarDesc{ "cvar.knob", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "test cvar", "engine" }).IsStale());
    ConsoleModel model;
    model.SetInput("cvar");
    REQUIRE(model.Complete(reg, CVarContext::Editor) == std::vector<std::string>{ "cvar.knob", "cvar_explain", "cvarlist" });   // sorted
}

TEST_CASE("CompleteInput: one match takes the name and a space; several take the common prefix and list them", "[cvar]") {
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{ "game.speed", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "test cvar", "engine" }).IsStale());
    REQUIRE_FALSE(reg.Register(CVarDesc{ "game.spawnRate", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "test cvar", "engine" }).IsStale());
    ConsoleModel model;
    model.SetInput("game.spe");
    REQUIRE(model.CompleteInput(reg, CVarContext::Editor));
    REQUIRE(model.Input() == "game.speed ");
    model.SetInput("game.s");
    const std::size_t lines = model.Lines().size();
    REQUIRE(model.CompleteInput(reg, CVarContext::Editor));
    REQUIRE(model.Input() == "game.sp");
    REQUIRE(model.Lines().size() == lines + 1);
    REQUIRE(model.Lines().back().text.find("game.spawnRate") != std::string::npos);
    REQUIRE_FALSE(model.CompleteInput(reg, CVarContext::Editor));     // already the common prefix: nothing changes (the list repeats)
    model.SetInput("zzz");
    REQUIRE_FALSE(model.CompleteInput(reg, CVarContext::Editor));
}

TEST_CASE("Console history: Up/Down with the draft restored, consecutive duplicates skipped, capped by console.historySize", "[cvar]") {
    CVarRegistry reg;
    const CVarHandle cap = reg.Find("console.historySize");
    REQUIRE_FALSE(cap.IsStale());
    REQUIRE(reg.Get(cap)->AsInt32() == 64);
    ConsoleModel model;
    REQUIRE_FALSE(model.HistoryPrev());          // empty history
    for (const char* line : { "cvarlist", "cvarlist", "cvar_explain cheats" })
    {
        model.SetInput(line);
        model.Submit(reg, CVarContext::Editor);
    }
    REQUIRE(model.History().size() == 2);        // the duplicate was skipped
    model.SetInput("dra");
    REQUIRE(model.HistoryPrev());
    REQUIRE(model.Input() == "cvar_explain cheats");
    REQUIRE(model.HistoryPrev());
    REQUIRE(model.Input() == "cvarlist");
    REQUIRE_FALSE(model.HistoryPrev());          // at the oldest
    REQUIRE(model.HistoryNext());
    REQUIRE(model.HistoryNext());
    REQUIRE(model.Input() == "dra");             // past the newest: the draft is back
    REQUIRE_FALSE(model.HistoryNext());

    REQUIRE(reg.Set(cap, CVarValue::Int32(2), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    for (const char* line : { "a", "b", "c" }) { model.SetInput(line); model.Submit(reg, CVarContext::Editor); }
    REQUIRE(model.History() == std::deque<std::string>{ "b", "c" });
    model.SetInput("");
    model.Submit(reg, CVarContext::Editor);       // empty: not recorded
    REQUIRE(model.History().size() == 2);
}
