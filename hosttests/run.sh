#!/bin/sh
# Build and run the Riivolution host tests with the system compiler.
#
# The Riivolution modules that do the interesting work - parsing the game's file
# table, rebuilding it, and matching a mod against it - are deliberately free of
# any console dependency, so they can be compiled and RUN here rather than only
# syntax-checked for the Wii. Two tiny shims (shim/) stand in for <gctypes.h> and
# gprintf; nothing else about the sources is changed.
#
# Usage: sh hosttests/run.sh     (from the repository root)

set -e

SRC="$(cd "$(dirname "$0")/../source" && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/riivo-hosttests"
mkdir -p "$OUT"

build_run() {
	name="$1"
	shift
	printf '\n=== %s ===\n' "$name"
	# Include paths stay quoted: this repository is quite happily checked out
	# somewhere with a space in the path.
	g++ -O1 -Wall -Wextra -Wno-unused-parameter \
		-I"$HERE/shim" -I"$SRC" \
		-o "$OUT/$name" "$HERE/$name.cpp" "$@"
	"$OUT/$name"
}

# The file-table rebuilder on its own: round-trips, replacements, additions,
# directory subtree ends after insertion, hostile input, 64-bit offsets.
build_run test_fstbuild "$SRC/riivo/RiivoFstBuild.cpp" "$SRC/riivo/RiivoFst.cpp"

# The same rebuilder at the size of a real game: a 3920-file table.
build_run test_scale "$SRC/riivo/RiivoFstBuild.cpp" "$SRC/riivo/RiivoFst.cpp"

# The seam RiivoBoot walks on the console: FST -> BuildRedirects -> FstBuilder.
# Needs RiivoConfig (for JoinPath) and hence pugixml.
build_run test_pipeline "$SRC/riivo/RiivoFstBuild.cpp" "$SRC/riivo/RiivoFst.cpp" \
	"$SRC/riivo/RiivoFile.cpp" "$SRC/riivo/RiivoConfig.cpp" "$SRC/xml/pugixml.cpp"

# Where the rebuilt table gets written into the running game's memory. Mostly
# a test that it REFUSES: a wrong address here overwrites the game.
build_run test_fstinstall "$SRC/riivo/RiivoFstInstall.cpp"

# Where the mod region sits on the virtual disc the cIOS reads, and whether it
# clears the read ceiling and the fragment table.
build_run test_fragplan "$SRC/riivo/RiivoFragPlan.cpp"

# Tail-cluster recovery: the FAT driver under-reports files whose data ends
# inside a cluster's first sector. Needs the frag-list and driver shims, which
# test_fragtail.cpp provides itself.
build_run test_fragtail "$SRC/riivo/RiivoFragBuild.cpp"

# The redirect routine is Thumb-1 assembled outside the PPC build, and its
# bytes are embedded in RiivoDiHook.cpp. Reassemble here and hand the bytes
# to test_dihook, which proves the two identical - .S and C++ can never skew
# silently. Without devkitARM the round-trip is skipped with a warning below;
# the embedded-hex checks still run either way.
printf '\n=== test_dihook ===\n'
g++ -O1 -Wall -Wextra -Wno-unused-parameter \
	-I"$HERE/shim" -I"$SRC" \
	-o "$OUT/test_dihook" "$HERE/test_dihook.cpp" "$SRC/riivo/RiivoDiHook.cpp"
DIHOOK_ASM=""
if command -v arm-none-eabi-as >/dev/null 2>&1; then
	DIHOOK_ASM="arm-none-eabi-as"
elif [ -x "/c/devkitPro/devkitARM/bin/arm-none-eabi-as" ]; then
	DIHOOK_ASM="/c/devkitPro/devkitARM/bin/arm-none-eabi-as"
	DIHOOK_COPY="/c/devkitPro/devkitARM/bin/arm-none-eabi-objcopy"
fi
if [ -n "$DIHOOK_ASM" ]; then
	if [ -z "$DIHOOK_COPY" ]; then
		DIHOOK_COPY="$(dirname "$DIHOOK_ASM")/arm-none-eabi-objcopy"
	fi
	"$DIHOOK_ASM" -mbig-endian -march=armv5te -mthumb \
		"$SRC/riivo/ios/redirect.S" -o "$OUT/redirect.o"
	"$DIHOOK_COPY" -O binary "$OUT/redirect.o" "$OUT/redirect.bin"
	"$OUT/test_dihook" "$OUT/redirect.bin"
else
	printf 'round-trip SKIPPED (no arm-none-eabi-as)\n'
	"$OUT/test_dihook"
fi

# Probe self-exclusion: the MEM2 scan matched our own image. Pure address
# classification, no console needed; the header is all this suite compiles.
build_run test_probeself

printf '\nall suites passed\n'
