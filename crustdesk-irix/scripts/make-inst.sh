#!/bin/sh
# Make the inst(1M) product -- what IRIX's own installer and Software Manager
# install -- from a linked agent, with the gendist of an IRIX machine.
#
#   scripts/make-inst.sh BUILD_ID [BINARY]       (make inst does this)
#
# gendist only exists on IRIX (inst_dev.sw, "Software Packager", on the IRIX
# 5.3 development CD), so the product description (inst/cdeskvint.spec and
# .idb) and the files go to the machine scripts/indigo.py reaches, gendist
# runs there, and the three files it writes come back as
# build/C-Desk-Vint-IRIX-5.3+-BUILD_ID.tardist: a plain tar of them, which
# Software Manager opens and `inst -f` reads once unpacked. A 5.3 gendist
# writes the format every inst from 5.3 to 6.5 reads.
#
# The inst version is the build's date and the commit's time of day
# (YYYYMMDD and hundredths of a day), so later builds are upgrades -- the
# scheme ../rustdesk-irix65-agent uses.
set -e
cd "$(dirname "$0")/.."
BUILD_ID=${1:?usage: make-inst.sh BUILD_ID [BINARY]}
BIN=${2:-build/C-Desk-Vint}
[ -f "$BIN" ] || { echo "make-inst.sh: no $BIN (make; scripts/indigo.py link)" >&2; exit 1; }

DATE=$(printf %s "$BUILD_ID" | cut -c1-8)
REV=$(printf %s "$BUILD_ID" | cut -d- -f2)
FRAC=00
if [ "$REV" = "$(git rev-parse --short=9 HEAD 2>/dev/null)" ]; then
	FRAC=$(TZ=UTC git log -1 --format=%cd --date=format-local:%H:%M HEAD |
	    awk -F: '{ printf "%02d", int(($1 * 60 + $2) * 100 / 1440) }')
fi
VERSION=$DATE$FRAC
NAME=C-Desk-Vint-IRIX-5.3+-$BUILD_ID

ST=build/inst-stage
rm -rf $ST
mkdir -p $ST
cp "$BIN" $ST/C-Desk-Vint
cp pkg/cdeskvint.init pkg/cdeskvint.config pkg/README.txt inst/cdeskvint.idb $ST/
sed "s/@VERSION@/$VERSION/" inst/cdeskvint.spec > $ST/cdeskvint.spec

R=/usr/tmp/cdv-inst
I="python3 scripts/indigo.py"
$I run "rm -rf $R; mkdir $R $R/bin $R/pkg $R/dist" > /dev/null
$I put $ST/C-Desk-Vint $R/bin/C-Desk-Vint \
    $ST/cdeskvint.init $R/pkg/cdeskvint.init \
    $ST/cdeskvint.config $R/pkg/cdeskvint.config \
    $ST/README.txt $R/pkg/README.txt \
    $ST/cdeskvint.spec $R/cdeskvint.spec \
    $ST/cdeskvint.idb $R/cdeskvint.idb
$I run "chmod 755 $R/bin/C-Desk-Vint $R/pkg/cdeskvint.init; chmod 644 $R/pkg/cdeskvint.config $R/pkg/README.txt" > /dev/null
$I run "cd $R && /usr/sbin/gendist -verbose -sbase $R -idb $R/cdeskvint.idb -spec $R/cdeskvint.spec -dist $R/dist -all 2>&1; ls -l $R/dist"
for f in cdeskvint cdeskvint.idb cdeskvint.sw; do
	$I get $R/dist/$f $ST/$f
done
$I run "rm -rf $R" > /dev/null

rm -f build/$NAME.tardist
(cd $ST && tar --format=v7 --owner=0 --group=0 -cf ../$NAME.tardist cdeskvint cdeskvint.idb cdeskvint.sw)
echo "inst version $VERSION"
ls -l build/$NAME.tardist
