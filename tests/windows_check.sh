#!/bin/bash
# Windows build checks, from Linux: cross-build (tools/build_windows.sh), then static checks of the executable, then
# (with Wine and a display) the Vulkan window scenarios on the Windows executable. Wine is not Windows: passing here
# shows the executable, its imports and its Windows code paths work under Wine's implementation of them.
#   tests/windows_check.sh [--no-wine]
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
status=0
check() {  # name, command...
    local name=$1; shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name"; status=1; fi
}
tools/build_windows.sh > build/windows_build.log 2>&1 || { echo "FAIL build (build/windows_build.log)"; exit 1; }
echo "ok   build"
dist=build/windows/dist
exe=$dist/CounterStrike.exe

# a 64-bit PE, nothing beyond Windows' own DLLs, the packaged ones and the driver's Vulkan loader
check "PE32+ x86-64" sh -c "file '$exe' | grep -q 'PE32+ executable.*x86-64'"
imports=$(llvm-objdump -p "$exe" | sed -n 's/.*DLL Name: //p' | sort -u)
unexpected=$(echo "$imports" | grep -viE '^(api-ms-win-crt-.*|kernel32|user32|ws2_32)\.dll$|^(SDL3|dxcompiler|vulkan-1)\.dll$' || true)
check "imports only system, packaged and Vulkan loader DLLs${unexpected:+ (unexpected: $unexpected)}" test -z "$unexpected"
for dll in SDL3.dll dxcompiler.dll; do check "packaged $dll" test -f "$dist/$dll"; done
check "no MinGW runtime DLL needed" sh -c "! echo '$imports' | grep -qiE 'libstdc|libgcc|libwinpthread'"

# the host functions overriding recompiled ones (weak definitions): the same set as the Linux window build, plus the
# XDK select (0x2630C8), which the Linux harness's winsock shim renames (FINDINGS.md "Windows build")
overrides() {
    python3 -I - "$@" <<'PY'
import re, subprocess, sys
syms = subprocess.run(['llvm-nm', sys.argv[1]], capture_output=True, text=True).stdout.split('\n')
addr, weak = {}, {}
for l in syms:
    p = l.split()
    if len(p) < 3: continue
    m = re.match(r'\.weak\.(.+?)\.default\.', p[2])
    if m: weak[m.group(1)] = p[0]
    else: addr.setdefault(p[2], p[0])
print('\n'.join(sorted(n for n in weak if n in addr and addr[n] != weak[n])))
PY
}
if [ -x build/window/CounterStrike ]; then
    win=$(overrides "$exe")
    lin=$(comm -12 <(nm build/harness/gen/*.o | awk '$2=="W"{print $3}' | sort -u) \
                   <(nm build/window/host/*.o | awk '$2=="T"{print $3}' | sort -u))
    diff=$(comm -3 <(echo "$lin") <(echo "$win") | tr -d '\t' | grep -vx sub_002630C8 || true)
    check "overrides as on Linux ($(echo "$win" | wc -l) functions)${diff:+ (differ: $diff)}" test -z "$diff"
fi

if [ "$1" != "--no-wine" ] && command -v wine > /dev/null; then
    [ -d build/zcache ] || echo "note: no build/zcache, the direct match runs without its cached sound banks"
    drive() { python3 -I tools/drive.py "$@" --wine --run build/winrun --env CSX_RENDERER=vulkan > build/winrun_last.txt 2>&1; }
    mkdir -p build/winrun
    check "wine: intro movies (vk_intro)" drive tests/scenarios/vk_intro.txt --timeout 200
    check "wine: direct match, movement, mouse look (vk_match_direct)" \
        drive tests/scenarios/vk_match_direct.txt --timeout 300 --launch tests/launch/de_airstrip_beginner
    check "wine: front end to match through the relaunch (vk_match)" drive tests/scenarios/vk_match.txt --timeout 500
    check "wine: relaunch handover" grep -q "previous instance waits" build/winrun/logs/cs_002.log
fi
exit $status
