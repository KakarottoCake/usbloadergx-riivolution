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
	"$DIHOOK_ASM" -mbig-endian -march=armv5te -mthumb \
		"$SRC/riivo/ios/redirect_ondemand.S" -o "$OUT/redirect_ondemand.o"
	"$DIHOOK_COPY" -O binary "$OUT/redirect_ondemand.o" "$OUT/redirect_ondemand.bin"
	"$OUT/test_dihook" "$OUT/redirect.bin" "$OUT/redirect_ondemand.bin"
else
	printf 'round-trip SKIPPED (no arm-none-eabi-as)\n'
	"$OUT/test_dihook"
fi

# Probe self-exclusion: the MEM2 scan matched our own image. Pure address
# classification, no console needed; the header is all this suite compiles.
build_run test_probeself

build_run test_fstwalk "$SRC/riivo/RiivoFstWalk.cpp" "$SRC/riivo/RiivoFstBuild.cpp"
build_run test_readverify "$SRC/riivo/RiivoReadVerify.cpp"
build_run test_patchguard "$SRC/riivo/RiivoPatchGuard.cpp" "$SRC/riivo/RiivoConfig.cpp"

# Memory-patch preflight: same checks as the apply path, read-only, plus the
# hold-back policy and the apply summary. DOL sections come from stubs and
# cache ops are shim no-ops; direct-RAM reads are target-only by nature, so
# absolute-address paths are exercised by PPC compile, not here.
build_run test_memcheck "$SRC/riivo/RiivoMemory.cpp" "$SRC/riivo/RiivoPatchGuard.cpp" "$SRC/riivo/RiivoConfig.cpp"

# The same patcher, writing for real. A 64-bit host can reserve the console
# addresses the patches target, so CommitWrite and VerifyAppliedPatches run
# unmodified: bytes land, and a byte changed underneath is reported. Skips
# itself with a note if the address space is not available.
build_run test_memapply "$SRC/riivo/RiivoMemory.cpp" "$SRC/riivo/RiivoPatchGuard.cpp" "$SRC/riivo/RiivoConfig.cpp"

# The table the loader hands to IOS: the only thing the two halves of the
# on-demand design share, across a byte-order boundary neither may depend on.
# Decoded here by a second implementation written from the format description,
# so builder and reader cannot drift together.
build_run test_redirtable "$SRC/riivo/RiivoRedirectTable.cpp"

# Setting aside the MEM2 the redirect table lives in, so the booting game's
# allocator never claims it. Mostly a test that it REFUSES: lowering a
# low-memory word we have misread is how the running game gets overwritten.
build_run test_mem2reserve "$SRC/riivo/RiivoMem2Reserve.cpp"

# Finding the cIOS routines that read raw sectors, which the on-demand design
# calls instead of the fragment reader. Synthetic fixture: the real 128 KB
# dump is someone's console and is not in the repository. Mostly refusals - a
# wrong address here is a hard freeze with nothing on screen.
build_run test_storageprobe "$SRC/riivo/RiivoStorageProbe.cpp" "$SRC/riivo/RiivoDiHook.cpp"

# Placing the on-demand module in the MEM2 the loader reserved: relocation,
# the parameter block, and the refusals. The module is built for the Starlet by
# an ARM compiler the Wii build does not have, so its bytes are carried in
# RiivoModuleBlob.cpp - which means the carried copy is the one that runs. When
# devkitARM is here, re-link the sources and compare, so editing only the C in
# source/riivo/ios cannot leave the console running the old module.
printf '\n=== test_moduleinstall ===\n'
g++ -O1 -Wall -Wextra -Wno-unused-parameter \
	-I"$HERE/shim" -I"$SRC" \
	-o "$OUT/test_moduleinstall" "$HERE/test_moduleinstall.cpp" \
	"$SRC/riivo/RiivoModuleInstall.cpp" "$SRC/riivo/RiivoModuleBlob.cpp" \
	"$SRC/riivo/RiivoMem2Reserve.cpp"
MODCC=""
if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
	MODCC="arm-none-eabi-gcc"
	MODLD="arm-none-eabi-ld"
	MODCOPY="arm-none-eabi-objcopy"
elif [ -x "/c/devkitPro/devkitARM/bin/arm-none-eabi-gcc" ]; then
	MODCC="/c/devkitPro/devkitARM/bin/arm-none-eabi-gcc"
	MODLD="/c/devkitPro/devkitARM/bin/arm-none-eabi-ld"
	MODCOPY="/c/devkitPro/devkitARM/bin/arm-none-eabi-objcopy"
fi
if [ -n "$MODCC" ]; then
	MODF="-c -O2 -Wall -Wextra -mcpu=arm926ej-s -mthumb -mthumb-interwork"
	MODF="$MODF -mbig-endian -ffreestanding -fno-builtin -fno-common"
	MODOBJ=""
	for m in riivo_fat riivo_redirect riivo_glue riivo_ios; do
		$MODCC $MODF -o "$OUT/$m.o" "$SRC/riivo/ios/$m.c"
		MODOBJ="$MODOBJ $OUT/$m.o"
	done
	MODGCC=$($MODCC -mcpu=arm926ej-s -mthumb -mthumb-interwork -mbig-endian \
		-print-libgcc-file-name)
	$MODLD -EB -T "$SRC/riivo/ios/module.ld" --emit-relocs \
		-o "$OUT/module.elf" $MODOBJ "$MODGCC"
	$MODCOPY -O binary --only-section=.text --only-section=.data \
		"$OUT/module.elf" "$OUT/module.bin"
	"$OUT/test_moduleinstall" "$OUT/module.bin"
else
	"$OUT/test_moduleinstall"
fi

# The on-demand boot layout: the module and the redirect table both come out
# of one MEM2 reservation, because lowering the arena twice is a way to get the
# second one wrong - and getting it wrong is silent, the game just allocates
# over whichever was left outside.
build_run test_ondemand "$SRC/riivo/RiivoOnDemand.cpp" 	"$SRC/riivo/RiivoModuleInstall.cpp" "$SRC/riivo/RiivoModuleBlob.cpp" 	"$SRC/riivo/RiivoMem2Reserve.cpp"

# The collector address as typed into a file on the card. The socket half
# needs a console; this is the half that fails quietly, by sending a boot
# log somewhere unintended or opening nothing without saying why.
build_run test_netlog -DRIIVO_HOST_TEST "$SRC/riivo/RiivoNet.cpp"

# Gate B contract: the versioned v1 manifest plus the shared address-model
# range checks. Independent LE decode, refusals, crc integrity, and the
# partition-range verdicts. Needs only the manifest TU.
build_run test_manifest "$SRC/riivo/RiivoManifest.cpp"

# WP1 fixtures: revision/disc filters with unknown-axis skipping, multi-XML
# merge precedence, skipped patch-ref accounting, selection round-trip.
# Needs RiivoConfig only (no pugixml: discs are built programmatically).
build_run test_resolvemerge "$SRC/riivo/RiivoConfig.cpp"

# WP2 fixtures: redirect specs (sub-ranges, whole-file, resize clamp) to v1
# manifest extents, chained into BuildManifestV1 + ValidateManifestV1.
# Needs the file planner plus the manifest, config, FST and pugixml, mirroring
# test_pipeline's link set.
build_run test_manifest_extents "$SRC/riivo/RiivoFile.cpp" "$SRC/riivo/RiivoManifest.cpp" "$SRC/riivo/RiivoConfig.cpp" "$SRC/riivo/RiivoFst.cpp" "$SRC/xml/pugixml.cpp"

# Two-phase reconciliation (Newer SMBW fix): early registration records
# against late placement, skip reasons, recovered-offset matching, and the
# previous-boot outcome parser. Header-only reconcile plus the FST builder
# (last-wins), the file planner (size-cache reuse) and the resolver.
build_run test_reconcile "$SRC/riivo/RiivoFstBuild.cpp" "$SRC/riivo/RiivoFst.cpp" "$SRC/riivo/RiivoFile.cpp" "$SRC/riivo/RiivoConfig.cpp"

printf '\nall suites passed\n'
