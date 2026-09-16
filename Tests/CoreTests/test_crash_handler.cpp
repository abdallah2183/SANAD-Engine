// CoreTests — crash handler arming (design doc 88).
//
// A real crash cannot (and must not) be triggered in-process, so these tests
// pin the contract around it: install/uninstall/re-arm state, config
// handling, and idempotence.

#include <NF/Core/CrashHandler.hpp>
#include <NF/Test/TestFramework.hpp>

using namespace nf;

NF_TEST(crash_handler_arms_and_disarms) {
    uninstall_crash_handler();
    NF_CHECK(!crash_handler_installed());
    CrashHandlerConfig cfg;
    cfg.dump_directory = "";
    NF_CHECK(install_crash_handler(cfg));
    NF_CHECK(crash_handler_installed());
    uninstall_crash_handler();
    NF_CHECK(!crash_handler_installed());
}

NF_TEST(crash_handler_rearm_replaces_config) {
    CrashHandlerConfig a;
    a.dump_directory = "dumps_a";
    a.dump_prefix = "GameA";
    NF_CHECK(install_crash_handler(a));
    NF_CHECK(crash_handler_installed());
    CrashHandlerConfig b;
    b.dump_directory = "dumps_b";
    NF_CHECK(install_crash_handler(b)); // re-arm: safe, replaces
    NF_CHECK(crash_handler_installed());
    uninstall_crash_handler();
    NF_CHECK(!crash_handler_installed());
    uninstall_crash_handler(); // double disarm: safe no-op
    NF_CHECK(!crash_handler_installed());
}
