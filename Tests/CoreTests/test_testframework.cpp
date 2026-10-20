// CoreTests/test_testframework.cpp — the test framework's own contract.
//
// The framework is the thing every other suite trusts, so the three defects it
// used to hide are pinned here rather than left to be discovered the next time
// a suite goes quiet:
//   * NF_CHECK_NEAR silently PASSED on NaN (IEEE-754: every comparison with
//     NaN is false, so "abs(a-b) > eps" never fires).
//   * A test throwing anything that is not a std::exception escaped run_all,
//     unwound through main and terminated the whole suite, so every test after
//     it never ran and no summary was printed.
//   * A filter that matched no case reported "0 passed | 0 failed" and exited
//     0 — a green run that executed nothing.
//
// The framework's own registry is used directly (a local TestRegistry, not
// the singleton the suite's main() drives), so the cases below cannot
// interfere with the suite's own cases.

#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

/// Runs `expr` and reports whether it threw. NF_CHECK_* throws
/// std::runtime_error, so that is what is caught.
template <typename F>
bool throws(F&& expr) {
    try {
        expr();
    } catch (const std::exception&) {
        return true;
    } catch (...) {
        return true;
    }
    return false;
}

} // namespace

NF_TEST(test_check_near_rejects_nan) {
    // The regression: a NaN difference must FAIL the check. Before the fix
    // this assertion itself was the victim — the comparison the macro made
    // was false for NaN, so the macro did not throw and the test passed.
    NF_CHECK(throws([] { NF_CHECK_NEAR(kNaN, 0.0f, 1e-5f); }));
    NF_CHECK(throws([] { NF_CHECK_NEAR(0.0f, kNaN, 1e-5f); }));
    // Both operands NaN, and NaN against a NaN-produced difference.
    NF_CHECK(throws([] { NF_CHECK_NEAR(kNaN, kNaN, 1e-5f); }));
}

NF_TEST(test_check_near_still_accepts_a_normal_match) {
    // The fix must not turn a passing comparison into a failure.
    NF_CHECK(!throws([] { NF_CHECK_NEAR(1.0f, 1.0f, 1e-5f); }));
    NF_CHECK(!throws([] { NF_CHECK_NEAR(1.0f, 1.00001f, 1e-3f); }));
    NF_CHECK(!throws([] { NF_CHECK_NEAR(0.0f, 0.0f, 0.0f); }));
    // ...and it must still fail a genuinely wrong value.
    NF_CHECK(throws([] { NF_CHECK_NEAR(1.0f, 2.0f, 1e-5f); }));
    NF_CHECK(throws([] { NF_CHECK_NEAR(0.0f, 1.0f, 0.0f); }));
}

NF_TEST(test_framework_counts_a_non_exception_throwable_as_a_failure) {
    // A test that throws something that is not a std::exception used to
    // escape run_all and terminate the whole process. It must now be a
    // failure of that case — and the OTHER cases must still run.
    nf::test::TestRegistry reg;
    reg.add("throws_int", [] { throw 42; });
    reg.add("throws_string", [] { throw std::string("boom"); });
    reg.add("passes_afterwards", [] {});

    const int rc = reg.run_all();
    NF_CHECK_EQ(rc, 1);
}

NF_TEST(test_framework_still_reports_skips_and_std_exceptions) {
    // The catch-all must not swallow the two cases that have their own
    // reporting: a skip is neither a pass nor a failure, and a
    // std::exception is a failure.
    nf::test::TestRegistry reg;
    reg.add("skips", [] { NF_SKIP("not applicable here"); });
    reg.add("throws_std_exception", [] { throw std::runtime_error("nope"); });

    const int rc = reg.run_all();
    NF_CHECK_EQ(rc, 1);
}

NF_TEST(test_framework_fails_a_filter_that_matches_nothing) {
    // "RHITests typo_in_test_name" used to report Passed: 0 | Failed: 0 and
    // exit 0 — indistinguishable from a green run.
    nf::test::TestRegistry reg;
    reg.add("alpha", [] {});

    NF_CHECK_EQ(reg.run_all("no_such_test_name"), 1);
    // A filter that DOES match still runs that case and passes.
    NF_CHECK_EQ(reg.run_all("alpha"), 0);
}
