#!/bin/bash
# Builds the 4 Linux variants of the Baker editor plugin inside WSL and copies
# the resulting .so files back into the Windows project's bin/plugins folder.
#
# Prerequisites (WSL Ubuntu 22.04):
#   - build-essential, cmake, ninja-build
#   - Qt 6.5.3 gcc_64 at /opt/qt/6.5.3/gcc_64
#   - Linux UNIGINE 2.21 libs in <project>/lib:
#       libUnigine_x64.so libUnigine_x64d.so
#       libUnigine_double_x64.so libUnigine_double_x64d.so
#       libEditorCore_x64.so libEditorCore_x64d.so
#       libEditorCore_double_x64.so libEditorCore_double_x64d.so
#
# The project is mirrored into the native Linux filesystem first: building
# directly on /mnt/f is an order of magnitude slower (9p filesystem).
set -e

WIN_PROJECT=/mnt/f/repository/Ungine_addons/Baker
WORK=~/baker-linux
QT=/opt/qt/6.5.3/gcc_64

echo "=== syncing sources ==="
mkdir -p $WORK/source/plugins/zloy_pingvin $WORK/lib
cp -ru $WIN_PROJECT/include $WORK/
rsync -a --delete --exclude 'build*' --exclude 'lbuild*' $WIN_PROJECT/source/plugins/zloy_pingvin/Baker $WORK/source/plugins/zloy_pingvin/
cp -u $WIN_PROJECT/lib/lib*.so $WORK/lib/ 2>/dev/null || true

MISSING=0
for f in libUnigine_x64.so libEditorCore_x64.so; do
	[ -f $WORK/lib/$f ] || { echo "MISSING: lib/$f"; MISSING=1; }
done
[ $MISSING -eq 0 ] || { echo "Linux UNIGINE libraries are missing — copy them into $WIN_PROJECT/lib first"; exit 1; }

SRC=$WORK/source/plugins/zloy_pingvin/Baker

build_variant() {
	local dir=$1 type=$2 double=$3
	echo "=== $dir (type=$type double=$double) ==="
	cmake -S $SRC -B $SRC/$dir -G Ninja \
		-DCMAKE_BUILD_TYPE=$type -DUNIGINE_DOUBLE=$double \
		-DCMAKE_PREFIX_PATH=$QT >/dev/null
	cmake --build $SRC/$dir
}

build_variant lbuild          Release        0
build_variant lbuild_rwdi     RelWithDebInfo 0
build_variant lbuild_double   Release        1
build_variant lbuild_double_rwdi RelWithDebInfo 1

echo "=== copying results back ==="
OUT=$WIN_PROJECT/bin/plugins/zloy_pingvin/Baker
cp -v $WORK/bin/plugins/zloy_pingvin/Baker/lib*.so $OUT/

echo "=== done ==="
ls -la $OUT/lib*.so
