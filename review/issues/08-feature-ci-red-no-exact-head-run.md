# A08 — Feature CI is continuously red and the reviewed head has no hosted run

Severity: High

Audit base: `11e3bb2b3ebcf331985f5a1765b2abc44994c86b`

## Finding

The feature branch continued for 15 pushes while the required Linux and Windows build jobs were red. The latest remote run at `eb8b88f` passed only macOS; Ubuntu g++, Ubuntu clang++, and Windows MSVC failed before CTest. The current local head is two commits newer and has no hosted run.

Direct cause: `tests/core/test_machine_runtime.cpp:620-627` uses `std::string` and `std::to_string`, but its include block at `:37-46` omits `<string>`. Apple libc++'s transitive includes hide the defect locally; GCC and MSVC reject it.

`design/00-status.md:92` still claims `CI build-and-test 4/4` is green. `design/06-master-plan.md:81-83` makes Windows CI compilation an explicit P1 exit condition.

## Required correction

Add the direct standard-library include, then require the exact candidate head to complete all four hosted matrix jobs successfully before any phase or slice GO. A stale or different head's result cannot satisfy this gate. Update the status document from live machine evidence rather than retained prose.

## Evidence

- GitHub Actions run `33081082539`, head `eb8b88f`: macOS success; both Ubuntu jobs and Windows failed in Build; CTest skipped.
- Failing GCC diagnostics cite `tests/core/test_machine_runtime.cpp:620-627` and explicitly require `<string>`.
- Feature runs from `1c25f42` through `eb8b88f`: 15 consecutive workflow failures.
- Local `git status`: clean, `ahead 2`; exact head `11e3bb2` has no hosted run.
