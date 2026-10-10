"""Build a SANAD target with MSVC env set manually.

Why this exists: in this environment `cmd /c "call vcvars64.bat && cmake ..."`
fails (cmd mangles the redirect and returns 1; `&&` chaining with `call` and
a redirect is unreliable). Setting INCLUDE/LIB/PATH by hand and invoking
cmake.exe directly is deterministic and avoids the whole cmd quoting mess.

Usage:
    python scripts/build_manual.py [Target] [--run [TestFilter]]
Examples:
    python scripts/build_manual.py RuntimeTests --run agent_paths
    python scripts/build_manual.py ToolTests
"""
import subprocess
import os
import sys

# --- MSVC + Windows SDK paths (VS 18 / toolset 14.51 / SDK 10.0.26100) ------
# Override via environment if the toolchain moves.
msvc = os.environ.get(
    'NF_MSVC',
    r'C:\Program Files\Microsoft Visual Studio\18\Community'
    r'\VC\Tools\MSVC\14.51.36231')
sdk = os.environ.get(
    'NF_SDK', r'C:\Program Files (x86)\Windows Kits\10')
sdk_ver = '10.0.26100.0'
cmake = os.environ.get('NF_CMAKE', r'C:\Program Files\CMake\bin\cmake.exe')
# Repo root: the parent of this script's folder (scripts/ -> repo root), so it
# works regardless of what the checkout directory is named on this machine.
workdir = os.environ.get('NF_ROOT') or os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))
build_dir = os.path.join(workdir, 'build', 'DebugNinja')

sdk_inc = os.path.join(sdk, 'Include', sdk_ver)
sdk_lib = os.path.join(sdk, 'Lib', sdk_ver)
sdk_bin = os.path.join(sdk, 'bin', sdk_ver, 'x64')

env = dict(os.environ)
env['INCLUDE'] = os.pathsep.join([
    os.path.join(msvc, 'include'),
    os.path.join(sdk_inc, 'ucrt'),
    os.path.join(sdk_inc, 'um'),
    os.path.join(sdk_inc, 'shared'),
    os.path.join(sdk_inc, 'winrt'),
])
env['LIB'] = os.pathsep.join([
    os.path.join(msvc, 'lib', 'x64'),
    os.path.join(sdk_lib, 'ucrt', 'x64'),
    os.path.join(sdk_lib, 'um', 'x64'),
])
env['PATH'] = os.pathsep.join([
    os.path.join(msvc, 'bin', 'Hostx64', 'x64'),
    sdk_bin,
    os.path.dirname(cmake),
    env.get('PATH', ''),
])


def main():
    args = list(sys.argv[1:])
    run = '--run' in args
    args = [a for a in args if a != '--run']
    target = args[0] if args else 'RuntimeTests'
    test_filter = args[1] if len(args) > 1 else ''

    result = subprocess.run(
        [cmake, '--build', build_dir, '--target', target, '--parallel'],
        env=env, capture_output=True, text=True, timeout=900,
    )
    print(f"BUILD {target} RC:", result.returncode)
    for line in (result.stdout + result.stderr).splitlines()[-5:]:
        print(" ", line)
    if result.returncode != 0:
        sys.exit(result.returncode)

    if not run:
        return

    exe = os.path.join(build_dir, 'bin', f'{target}.exe')
    cmd = [exe] + ([test_filter] if test_filter else [])
    run_result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=600)
    print(f"TEST {target} RC:", run_result.returncode)
    for line in (run_result.stdout + run_result.stderr).splitlines():
        if ('OK' in line or 'FAILED' in line or 'Passed:' in line
                or 'SKIP' in line):
            print(" ", line)
    sys.exit(run_result.returncode)


if __name__ == '__main__':
    main()
