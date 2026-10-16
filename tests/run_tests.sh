#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# ROM-1 host tests: the maths, the platform pieces kept from Felucca / X0X / AMB-1 / IND-1 / LCD-4 / CRT-4 /
# SEQ-4, the flash (the settings' objects), the converter and the engine (every preset sounding, the pitch
# exact at every quality the converter picks), the package with another bank in it, and the whole firmware
# app in the simulator (tests/scenarios/*.r1: the keys, the pages, MIDI in, the presets, region B's upload,
# the settings, the voices), with screenshots in build/scenarios/.
#   tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
FAIL=0
OUT=build/host
T=build/test
mkdir -p "$OUT" "$T" build/scenarios
run() {
    name="$1"; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $OUT/$name.log)"
        tail -15 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
run fastmath sh -c "$CC -O2 -ffp-contract=off -Wall -Wextra -Werror -o $OUT/fastmath_test tests/host/fastmath_test.c -lm && $OUT/fastmath_test"
run encoder sh -c "$CC -O2 -w -Ifirmware/hal -o $OUT/encoder_test tests/host/encoder_test.c && $OUT/encoder_test"
run trs sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/trs_test tests/host/trs_test.c && $OUT/trs_test"
run storage sh -c "$CC -O2 -o $OUT/storage_test tests/storage_test.c && $OUT/storage_test"
# the simulator first: it builds the converter and the starter sounds the rest use
run host-build sh host/build_host.sh
STARTER="build/gen/ROM-1 Starter.sf2"
run test-fonts python3 tests/fonts/make_test_fonts.py "$T"
# the converter and the engine: the test tones at three sizes (16-bit; 4-bit at the rate it has; 4-bit at
# lower rates), each tone's pitch to within 0.1 %; the starter sounds and the two fonts together, every
# preset sounding, every voice free after
run engine-build sh -c "$CC -O2 -std=c99 -Wall -Wextra -Werror -ffp-contract=off -DEN_HOST -Ifirmware/src/synth \
    -o $OUT/rom1_test tests/host/rom1_test.c firmware/src/synth/rom1.c -lm"
conv() {
    d="$T/$1"; shift
    mkdir -p "$d" && build/gen/rom1conv -o "$d" "$@" > "$d/conv.log"
}
pitch() {
    d="$T/$1"
    $OUT/rom1_test "$d/bank_a.bin" "$d/bank_b.bin" --freq 0 69 440 &&
    $OUT/rom1_test "$d/bank_a.bin" "$d/bank_b.bin" --freq 0 81 880 &&
    $OUT/rom1_test "$d/bank_a.bin" "$d/bank_b.bin" --freq 1 67 408.135 &&
    $OUT/rom1_test "$d/bank_a.bin" "$d/bank_b.bin" --freq 2 57 220 &&
    $OUT/rom1_test "$d/bank_a.bin" "$d/bank_b.bin" --freq 2 64 329.628 &&
    $OUT/rom1_test "$d/bank_a.bin" "$d/bank_b.bin" --freq 5 69 880
}
run convert-16bit conv tones -a 400000 -b 0 "$T/tones.sf2"
run convert-4bit conv tones4 -a 20000 -b 0 "$T/tones.sf2"
run convert-lowrate conv tonesl -a 9000 -b 0 "$T/tones.sf2"
run pitch-16bit pitch tones
run pitch-4bit pitch tones4
run pitch-lowrate pitch tonesl
run levels sh -c "grep -q '\"levels\":\[[1-9][0-9]*,0,0,0,0,0,0\]' $T/tones/conv.log && \
    grep -q '\"levels\":\[[0-9]*,[1-9]' $T/tones4/conv.log && ! grep -q '\"levels\":\[[0-9]*,[0-9]*,0,0,0,0,0\]' $T/tonesl/conv.log"
run convert-two conv two -a 300000 -b 282624 -n "Two fonts" "$T/tones.sf2" "$STARTER"
run engine-two $OUT/rom1_test "$T/two/bank_a.bin" "$T/two/bank_b.bin" "$T/two/all.wav"
run convert-starter conv starter -a 479000 -b 0 -n "ROM-1 Starter" "$STARTER"
run convert-pick conv pick -a 200000 -b 0 -n Picked "$T/tones.sf2@0:2,128:0" "$STARTER@0:0,0:48"
run engine-starter $OUT/rom1_test "$T/starter/bank_a.bin" "$T/starter/bank_b.bin"
run convert-too-big sh -c "mkdir -p $T/none && build/gen/rom1conv -a 40000 -b 0 -o $T/none \"$STARTER\"; [ \$? -eq 1 ]"
# the package: another bank in it; the update path, against the package (Felucca's tests; needs ./build.sh)
if [ -f build/rom1.fwsc ]; then
    run package python3 tests/pkg_test.py build/rom1.fwsc "$STARTER" "$T/tones.sf2" "$T/pkg"
    run ota-entry sh -c "$CC -o $OUT/ota_test tests/ota_test.c && $OUT/ota_test build/rom1.fwsc"
    run ota-patched $OUT/ota_test "$T/pkg/patched.fwsc"
    if [ -z "${AC79_SDK:-}" ]; then
        echo "  skip update-loader (needs AC79_SDK, as the build)"
    else
    run update-loader sh -c "head -c 100000 build/rom1.bin > $OUT/old_app.bin && \
        python3 tools/fm1pkg_make.py $OUT/old_app.bin build/loader/ota.bin $OUT/old.fwsc >/dev/null && \
        $CC -o $OUT/ldr_test tests/ldr_test.c && $OUT/ldr_test $OUT/old.fwsc build/rom1.fwsc"
    fi
    run installer python3 tests/install_test.py
else
    echo "  skip package and update-path tests (no build/rom1.fwsc: run ./build.sh)"
fi
# the browser's side: the package patched in JavaScript (in tests/pkg_test.py, above), region B's SysEx,
# the converter in WebAssembly against the command line's, the emulator
command -v node >/dev/null 2>&1 && run web-sysex node tests/host/rom1dev_test.mjs
if command -v emcc >/dev/null 2>&1 && command -v node >/dev/null 2>&1; then
    run conv-wasm sh -c "sh web/conv/build.sh >/dev/null && node tests/host/conv_wasm_test.mjs build/web/rom1conv.wasm $T \"$STARTER\""
    [ -f web/emu/build.sh ] && run emu sh -c "sh web/emu/build.sh >/dev/null && node tests/host/emu_test.mjs build/emu $T"
else
    echo "  skip the converter in WebAssembly and the emulator (needs emcc and node)"
fi
for s in tests/scenarios/*.r1; do
    n=$(basename "$s" .r1)
    mkdir -p "build/scenarios/$n"
    run "scenario-$n" build/host/rom1_host "$s" "build/scenarios/$n"
done
# the same scenarios under the address and undefined-behaviour sanitizers: a word access that is not word
# aligned works on this machine but faults on the FM-1 (SEQ-4 0.1's crash)
if $CC -fsanitize=address,undefined -x c -o "$OUT/san_probe" - >/dev/null 2>&1 <<'C'
int main(void) { return 0; }
C
then
    run sanitized-build sh -c "$CC -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -ffp-contract=off -std=c99 -w -DEN_HOST -Ifirmware/src -Ifirmware/src/synth -Ibuild/gen \
        -o $OUT/rom1_host_san host/rom1_host.c firmware/src/synth/rom1.c -lm"
    for s in tests/scenarios/*.r1; do
        n=$(basename "$s" .r1)
        mkdir -p "build/scenarios/san-$n"
        run "sanitized-$n" "$OUT/rom1_host_san" "$s" "build/scenarios/san-$n"
    done
    run sanitized-converter sh -c "$CC -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -std=c99 -w \
        -o $OUT/rom1conv_san tools/conv/rom1conv.c tools/conv/conv.c -lm && mkdir -p $T/san && \
        $OUT/rom1conv_san -a 300000 -b 282624 -o $T/san $T/tones.sf2 \"$STARTER\" >/dev/null && \
        $OUT/rom1conv_san -a 9000 -b 0 -o $T/san $T/tones.sf2 >/dev/null"
else
    echo "  skip sanitized scenarios (this compiler has no -fsanitize=address,undefined)"
fi
[ $FAIL -eq 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
