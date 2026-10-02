#!/bin/sh
# The cross compiler: Ubuntu's mips-linux-gnu GCC 13 and binutils, unpacked
# into a directory without installing anything (no root needed).
#
#   scripts/fetch-toolchain.sh [DIR]      (default build/toolchain)
#
# It is a Linux cross compiler; tools/irix-gcc turns it into an IRIX 5.3 one
# (MIPS I, o32, PIC, SGI's headers) and tools/irixobj.py makes its objects
# acceptable to SGI's ld. Tested with 13.3.0-6ubuntu2~24.04cross1 and binutils
# 2.42-2ubuntu1cross5 (Ubuntu 24.04).
set -eu
DIR=${1:-$(cd "$(dirname "$0")/.." && pwd)/build/toolchain}
mkdir -p "$DIR/debs"
DIR=$(cd "$DIR" && pwd)
cd "$DIR/debs"
apt-get download gcc-13-mips-linux-gnu cpp-13-mips-linux-gnu binutils-mips-linux-gnu \
	gcc-13-mips-linux-gnu-base libgcc-13-dev-mips-cross
for d in *.deb; do
	dpkg -x "$d" "$DIR"
done
"$DIR/usr/bin/mips-linux-gnu-gcc-13" --version | head -1
echo "toolchain in $DIR"
