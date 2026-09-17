// ScriptTests — C# gameplay scripting (Phase 16: hostfxr hosting).
//
// Pure-logic marshaling always runs; hosting tests need the .NET runtime +
// the built managed sandbox and NF_SKIP otherwise (missing-RTL must never
// look like a passing suite).

#include <NF/Scripting/CSharpHost.hpp>
#include <NF/Scripting/CSharpMarshal.hpp>
#include <NF/Test/TestFramework.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace nf;
using namespace nf::scripting;

namespace {

// -- hosting test plumbing (used only when configured) -----------------------

#if NF_CSHARP_HOSTING

bool csharp_ready(std::string& asm_dir, std::string& why) {
    if (!CSharpHost::runtime_available()) {
        why = "no .NET runtime on this machine";
        return false;
    }
#ifdef NF_CSHARP_ASM_DIR
    asm_dir = NF_CSHARP_ASM_DIR;
#else
    why = "managed sandbox dir unknown";
    return false;
#endif
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(fs::path(asm_dir) / "NFGameScript.dll", ec) || ec) {
        why = "managed sandbox DLL missing in " + asm_dir;
        return false;
    }
    if (!fs::exists(fs::path(asm_dir) / "NFGameScript.runtimeconfig.json", ec) || ec) {
        why = "managed runtimeconfig missing in " + asm_dir;
        return false;
    }
    return true;
}

std::string sandbox_type() {
    return "NF.GameScript.NFSandbox, NFGameScript";
}

// Host log capture: plain function (no captures) for the C# callback.
std::vector<std::pair<int, std::string>> g_logs;

void test_log(int level, const u8* msg_bytes, int msg_len) {
    std::string text;
    if (msg_bytes != nullptr && msg_len > 0) {
        text.assign(reinterpret_cast<const char*>(msg_bytes),
                    static_cast<usize>(msg_len));
    }
    g_logs.emplace_back(level, text);
}

#endif // NF_CSHARP_HOSTING

} // namespace

// --- pure-logic marshaling (always runs, no runtime needed) --------------------

NF_TEST(csharp_marshal_string_buffer) {
    u8 buf[16];
    NF_CHECK(write_string_to_buffer("hi", buf, sizeof(buf)) == 2);
    NF_CHECK(buf[0] == 'h' && buf[1] == 'i');
    NF_CHECK(write_string_to_buffer("", buf, sizeof(buf)) == 0);
    NF_CHECK(write_string_to_buffer("", nullptr, 0) == 0);
    NF_CHECK(write_string_to_buffer("1234567890123456", buf, sizeof(buf)) == 16); // exact fit
    NF_CHECK(write_string_to_buffer("12345678901234567", buf, sizeof(buf)) == -1); // overflow
    NF_CHECK(write_string_to_buffer("x", nullptr, 0) == -1);
    // Multibyte UTF-8 crosses as raw bytes (Arabic: 2 bytes per letter).
    const std::string arabic = "سند";
    const long long written = write_string_to_buffer(arabic, buf, sizeof(buf));
    NF_CHECK(written == static_cast<long long>(arabic.size()));
    NF_CHECK(std::string(reinterpret_cast<char*>(buf), static_cast<usize>(written)) == arabic);
}

NF_TEST(csharp_marshal_layout_and_names) {
    NF_CHECK(sizeof(NetVec3) == 12);
    NF_CHECK(sizeof(HostApi) == sizeof(void*));
    NF_CHECK(sizeof(CSharpEntity) == 8); // C# long, fixed width both sides
    NF_CHECK(csharp_type_name("NFGameScript", "NF.GameScript.NFSandbox") ==
             "NF.GameScript.NFSandbox, NFGameScript");
    NetVec3 v{1.0f, 2.0f, 3.0f};
    NF_CHECK(v.x == 1.0f && v.y == 2.0f && v.z == 3.0f);
    HostApi api;
    NF_CHECK(api.log == nullptr);
}

#if NF_CSHARP_HOSTING

// --- hosting (skips without a .NET runtime + sandbox build) ---------------------

NF_TEST(csharp_host_initializes_and_shuts_down) {
    std::string asm_dir, why;
    if (!csharp_ready(asm_dir, why)) NF_SKIP(why);

    CSharpHost host;
    NF_CHECK(!host.valid());
    std::string err;
    NF_CHECK(host.initialize(asm_dir + "/NFGameScript.runtimeconfig.json", &err));
    NF_CHECK(host.valid());
    NF_CHECK(host.initialize(asm_dir + "/NFGameScript.runtimeconfig.json", &err)); // re-init
    NF_CHECK(host.valid());
    host.shutdown();
    NF_CHECK(!host.valid());

    CSharpHost dead;
    NF_CHECK(!dead.initialize(asm_dir + "/Nope.runtimeconfig.json", &err)); // missing config
    NF_CHECK(!err.empty());
    NF_CHECK(!dead.initialize("", &err));
    NF_CHECK(!dead.get_method("a.dll", "T, A", "M", &err)); // uninitialized host
    NF_CHECK(!err.empty());
}

NF_TEST(csharp_add_and_scale) {
    std::string asm_dir, why;
    if (!csharp_ready(asm_dir, why)) NF_SKIP(why);

    CSharpHost host;
    std::string err;
    NF_CHECK(host.initialize(asm_dir + "/NFGameScript.runtimeconfig.json", &err));
    const std::string asm_path = asm_dir + "/NFGameScript.dll";
    auto* add_raw = host.get_method(asm_path, sandbox_type(), "Add", &err);
    NF_CHECK(add_raw != nullptr);
    auto* scale_raw = host.get_method(asm_path, sandbox_type(), "Scale", &err);
    NF_CHECK(scale_raw != nullptr);
    auto add = reinterpret_cast<CSAddFn>(add_raw);
    auto scale = reinterpret_cast<CSScaleFn>(scale_raw);
    NF_CHECK(add(2, 3) == 5);
    NF_CHECK(add(-7, 7) == 0);
    NF_CHECK_NEAR(scale(2.0f, 2.5f), 5.0f, 1e-6f);
}

NF_TEST(csharp_echo_roundtrip) {
    std::string asm_dir, why;
    if (!csharp_ready(asm_dir, why)) NF_SKIP(why);

    CSharpHost host;
    std::string err;
    NF_CHECK(host.initialize(asm_dir + "/NFGameScript.runtimeconfig.json", &err));
    auto* echo_raw =
        host.get_method(asm_dir + "/NFGameScript.dll", sandbox_type(), "Echo", &err);
    NF_CHECK(echo_raw != nullptr);
    auto echo = reinterpret_cast<CSEchoFn>(echo_raw);

    const std::string text = "hello سند C#";
    u8 out[64];
    const int written =
        echo(reinterpret_cast<const u8*>(text.data()), static_cast<int>(text.size()), out,
             static_cast<int>(sizeof(out)));
    NF_CHECK(written == static_cast<int>(text.size()));
    NF_CHECK(std::string(reinterpret_cast<char*>(out), static_cast<usize>(written)) == text);

    NF_CHECK(echo(reinterpret_cast<const u8*>("toolong"), 7, out, 3) == -1); // overflow
    NF_CHECK(echo(nullptr, 4, out, static_cast<int>(sizeof(out))) == -1);     // null in
    NF_CHECK(echo(reinterpret_cast<const u8*>("x"), 1, nullptr, 0) == -1);    // null out
}

NF_TEST(csharp_step_vec3) {
    std::string asm_dir, why;
    if (!csharp_ready(asm_dir, why)) NF_SKIP(why);

    CSharpHost host;
    std::string err;
    NF_CHECK(host.initialize(asm_dir + "/NFGameScript.runtimeconfig.json", &err));
    auto* step_raw =
        host.get_method(asm_dir + "/NFGameScript.dll", sandbox_type(), "StepVec3", &err);
    NF_CHECK(step_raw != nullptr);
    auto step = reinterpret_cast<CSStepVec3Fn>(step_raw);

    NetVec3 pos{1.0f, 2.0f, 3.0f};
    NetVec3 vel{10.0f, 0.0f, -4.0f};
    step(&pos, &vel, 0.5f);
    NF_CHECK_NEAR(pos.x, 6.0f, 1e-6f);
    NF_CHECK_NEAR(pos.y, 2.0f, 1e-6f);
    NF_CHECK_NEAR(pos.z, 1.0f, 1e-6f);
    step(nullptr, &vel, 1.0f); // nulls are a no-op, never a crash
    step(&pos, nullptr, 1.0f);
    NF_CHECK_NEAR(pos.x, 6.0f, 1e-6f);
}

NF_TEST(csharp_calls_back_into_host) {
    std::string asm_dir, why;
    if (!csharp_ready(asm_dir, why)) NF_SKIP(why);

    CSharpHost host;
    std::string err;
    NF_CHECK(host.initialize(asm_dir + "/NFGameScript.runtimeconfig.json", &err));
    auto* log_raw =
        host.get_method(asm_dir + "/NFGameScript.dll", sandbox_type(), "CallLog", &err);
    NF_CHECK(log_raw != nullptr);
    auto call_log = reinterpret_cast<CSCallLogFn>(log_raw);

    g_logs.clear();
    HostApi api;
    api.log = &test_log;
    const std::string msg = "from C# سند";
    call_log(&api, 2, reinterpret_cast<const u8*>(msg.data()),
             static_cast<int>(msg.size()));
    NF_CHECK(g_logs.size() == 1);
    NF_CHECK(g_logs[0].first == 2);
    NF_CHECK(g_logs[0].second == msg);

    call_log(nullptr, 1, reinterpret_cast<const u8*>("x"), 1); // null api: no-op
    NF_CHECK(g_logs.size() == 1);
}

NF_TEST(csharp_script_lifecycle_and_isolation) {
    std::string asm_dir, why;
    if (!csharp_ready(asm_dir, why)) NF_SKIP(why);

    CSharpHost host;
    std::string err;
    NF_CHECK(host.initialize(asm_dir + "/NFGameScript.runtimeconfig.json", &err));
    CSharpScript script(host, asm_dir + "/NFGameScript.dll", sandbox_type());
    NF_CHECK(script.valid());
    NF_CHECK(script.error().empty());

    NF_CHECK(script.start(1));
    NF_CHECK(script.start(2));
    NF_CHECK(script.update(1, 1.0f / 60.0f));
    NF_CHECK(script.update(1, 1.0f / 60.0f));
    NF_CHECK(script.update(1, 1.0f / 60.0f));
    NF_CHECK(script.update(2, 1.0f / 60.0f));
    int c1 = -1, c2 = -1;
    NF_CHECK(script.counter(1, c1) && c1 == 3); // entity state is isolated
    NF_CHECK(script.counter(2, c2) && c2 == 1);

    NF_CHECK(script.update(1, 0.0f)); // non-positive dt never advances
    NF_CHECK(script.counter(1, c1) && c1 == 3);

    NF_CHECK(script.update(9, 1.0f / 60.0f)); // unknown entity starts at 1
    int c9 = -1;
    NF_CHECK(script.counter(9, c9) && c9 == 1);

    NF_CHECK(script.start(1)); // restart resets
    NF_CHECK(script.counter(1, c1) && c1 == 0);
}

NF_TEST(csharp_missing_method_fails_loudly) {
    std::string asm_dir, why;
    if (!csharp_ready(asm_dir, why)) NF_SKIP(why);

    CSharpHost host;
    std::string err;
    NF_CHECK(host.initialize(asm_dir + "/NFGameScript.runtimeconfig.json", &err));
    NF_CHECK(host.get_method(asm_dir + "/Missing.dll", sandbox_type(), "Add", &err) ==
             nullptr);
    NF_CHECK(!err.empty());
    NF_CHECK(host.get_method(asm_dir + "/NFGameScript.dll", "Nope.Type, NFGameScript", "Add",
                             &err) == nullptr);
    NF_CHECK(!err.empty());
    NF_CHECK(host.get_method(asm_dir + "/NFGameScript.dll", sandbox_type(), "Nope", &err) ==
             nullptr);
    NF_CHECK(!err.empty());
    NF_CHECK(host.get_method("", "", "", &err) == nullptr);

    CSharpScript bad(host, asm_dir + "/NFGameScript.dll", "Nope.Type, NFGameScript");
    NF_CHECK(!bad.valid());
    NF_CHECK(!bad.error().empty());
    NF_CHECK(!bad.start(1) && !bad.update(1, 0.016f));
    int c = 0;
    NF_CHECK(!bad.counter(1, c));
}

NF_TEST(csharp_full_tick_loop) {
    std::string asm_dir, why;
    if (!csharp_ready(asm_dir, why)) NF_SKIP(why);

    // A 60-tick mini game loop through the script wrapper: same entity, same
    // dt, same count every run (managed Update is pure counting).
    CSharpHost host;
    std::string err;
    NF_CHECK(host.initialize(asm_dir + "/NFGameScript.runtimeconfig.json", &err));
    CSharpScript script(host, asm_dir + "/NFGameScript.dll", sandbox_type());
    NF_CHECK(script.valid());
    NF_CHECK(script.start(42));
    for (int i = 0; i < 60; ++i) {
        NF_CHECK(script.update(42, 1.0f / 60.0f));
    }
    int c = -1;
    NF_CHECK(script.counter(42, c) && c == 60);
}

#else // !NF_CSHARP_HOSTING

NF_TEST(csharp_host_reports_unconfigured) {
    CSharpHost host;
    NF_CHECK(!CSharpHost::runtime_available());
    std::string err;
    NF_CHECK(!host.initialize("anything.json", &err));
    NF_CHECK(!err.empty());
    NF_CHECK(host.get_method("a.dll", "T, A", "M", &err) == nullptr);
}

#endif // NF_CSHARP_HOSTING
