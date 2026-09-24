#!/bin/sh
# The host build, for logic that must be iterated without a USB cable to a PSP.
#
#   scripts/test.sh             all checks
#   scripts/test.sh memwatch    only checks in the "memwatch" group
#
# A skip is not a pass (see tests/test_main.c), so the test binary's exit
# code is propagated as ours -- a caller that only checks $? still catches it.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/host"
mkdir -p "$OUT"

# vswhere's own location is fixed by the installer, unlike the VS install it
# reports on, which a VS update can move.
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
if [ ! -f "$VSWHERE" ]; then
    echo "scripts/test.sh: vswhere.exe not found at $VSWHERE -- install Visual Studio (with the C++ workload) or fix this path" >&2
    exit 1
fi

# -requires the C++ toolset component specifically: a VS install with only,
# say, the web workload reports an installationPath but has no cl.exe under it.
VSINSTALL=$("$VSWHERE" -latest -products '*' \
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
    -property installationPath)
if [ -z "$VSINSTALL" ]; then
    echo "scripts/test.sh: no Visual Studio install with the C++ (VC.Tools.x86.x64) component found -- install the 'Desktop development with C++' workload" >&2
    exit 1
fi
VCVARS="$VSINSTALL\VC\Auxiliary\Build\vcvars64.bat"

# cygpath -w rather than a hand-rolled slash swap: it also resolves the drive
# letter form cl.exe needs, which a plain substitution would get wrong for a
# UNC-mounted checkout.
ROOT_WIN=$(cygpath -w "$ROOT")
OUT_WIN=$(cygpath -w "$OUT")

# Every check file and every pure-logic module, by glob. Named one by one it
# was a file somebody would forget to add, and a check that is never compiled
# is indistinguishable from one that passes.
#
# The -e guard rather than trusting the glob: an unmatched pattern in sh
# expands to itself, which would hand cl.exe a literal asterisk.
SRC_FILES=
for f in "$ROOT"/tests/*.c "$ROOT"/src/utils/*.c "$ROOT"/src/media/*.c "$ROOT"/src/ui/*.c "$ROOT"/src/net/*.c "$ROOT"/src/jellyfin/*.c; do
    [ -e "$f" ] && SRC_FILES="$SRC_FILES $f"
done

SRC_ARGS=
for f in $SRC_FILES; do
    SRC_ARGS="$SRC_ARGS \"$(cygpath -w "$f")\""
done

# A .bat file on disk rather than a `cmd /c "..."` one-liner: handing cmd.exe
# a single argument that itself contains quoted sub-arguments makes the MSYS
# runtime re-escape those inner quotes when it builds the real Win32 command
# line, doubling them up into garbage. A bare path has no quotes to mangle.
# It lands in build/host, which is already gitignored build output.
BATFILE="$OUT/_build.bat"
cat > "$BATFILE" <<EOF
@echo off
rem vcvars64.bat itself probes for a bare "vswhere.exe" on PATH as an optional
rem step and prints a harmless "not recognized" line when it isn't there;
rem 2>nul swallows that noise without hiding a genuine setup failure, which
rem the errorlevel check below still catches.
call "$VCVARS" >nul 2>nul
if errorlevel 1 exit /b 1
cd /d "$OUT_WIN"
rem /fsanitize=address: the parsers here eat bytes straight off a network, so a
rem one-byte over-read is the defect class that matters most, and without ASan
rem an over-read of a few bytes lands inside the same malloc'd block and the
rem check passes while the bug is still there. /Zi so its reports carry line
rem numbers rather than raw addresses.
cl.exe /nologo /W3 /fsanitize=address /Zi /I"$ROOT_WIN\src" /I"$ROOT_WIN\tests" $SRC_ARGS /Fe:test.exe
if errorlevel 1 exit /b 1
rem The ASan runtime is a DLL that lives in the MSVC bin directory, which is on
rem PATH here inside vcvars but not in the shell that runs test.exe afterwards.
rem Copied next to the binary rather than exported, so the run needs no
rem environment of its own.
for /f "delims=" %%I in ('where clang_rt.asan_dynamic-x86_64.dll 2^>nul') do copy /y "%%I" "$OUT_WIN" >nul
EOF

# NO_PATHCONV: MSYS still inspects this single path argument and, seeing a
# drive letter and backslashes it doesn't expect from bash, can rewrite it
# into something cmd.exe won't find.
set +e
MSYS_NO_PATHCONV=1 cmd /c "$(cygpath -w "$BATFILE")"
build_rc=$?
set -e

# Checked, because build/host/test.exe from the LAST run is still sitting
# there: without this, a failed compile falls straight through to running the
# stale binary and reports a green tally for code that did not build. That has
# already happened once here -- a link that failed against a locked test.exe
# re-ran an older binary, which then aborted inside a bug its own source no
# longer contained. A suite that can report success for code it did not
# compile is worse than no suite.
if [ "$build_rc" -ne 0 ]; then
    echo "build failed (exit $build_rc) -- not running the previous binary" >&2
    rm -f "$OUT/test.exe"
    exit "$build_rc"
fi

# Disabled around the run so a failing (or skip-reporting) check binary can't
# trip `set -e` before its exit code is captured -- that code is the point.
set +e
"$OUT/test.exe" "$@"
rc=$?
set -e

exit $rc
