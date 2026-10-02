#! /bin/sh
#
# Install C-Desk-Vint, the RustDesk agent, from this directory. Run as root.
#
#   /usr/local/sbin/C-Desk-Vint   the agent
#   /usr/local/sbin/cdeskvint-panel  its settings panel (Toolchest > C-Desk-Vint)
#   /usr/local/lib/C-Desk-Vint    the panel's helper and README.txt
#   /etc/init.d/cdeskvint         started at boot (chkconfig cdeskvint)
#   /var/C-Desk-Vint/prefs        its settings, ID and key (made on first run)
#   /var/C-Desk-Vint/log          what it did
#
# Installing over an earlier copy keeps the settings, ID and password.

case "`/usr/bin/id`" in
uid=0\(*) ;;
*) echo "install.sh: run this as root"; exit 1 ;;
esac
cd `dirname $0` || exit 1

for d in /usr/local /usr/local/sbin /usr/local/lib /usr/local/lib/C-Desk-Vint /var/C-Desk-Vint; do
	[ -d $d ] || mkdir $d || exit 1
done
chmod 700 /var/C-Desk-Vint

if [ -f /etc/init.d/cdeskvint ]; then
	/etc/init.d/cdeskvint stop > /dev/null 2>&1
	sleep 2
fi
cp C-Desk-Vint /usr/local/sbin/C-Desk-Vint || exit 1
chmod 755 /usr/local/sbin/C-Desk-Vint
cp cdeskvint.init /etc/init.d/cdeskvint || exit 1
chmod 755 /etc/init.d/cdeskvint
cp cdeskvint-panel /usr/local/sbin/cdeskvint-panel || exit 1
chmod 755 /usr/local/sbin/cdeskvint-panel
cp cdv-helper.sh /usr/local/lib/C-Desk-Vint/cdv-helper.sh || exit 1
chmod 755 /usr/local/lib/C-Desk-Vint/cdv-helper.sh
cp README.txt /usr/local/lib/C-Desk-Vint/README.txt
chmod 644 /usr/local/lib/C-Desk-Vint/README.txt
if [ -d /usr/lib/X11/app-chests ]; then
	cp C-Desk-Vint.chest /usr/lib/X11/app-chests/C-Desk-Vint.chest
	chmod 644 /usr/lib/X11/app-chests/C-Desk-Vint.chest
fi
rm -f /etc/rc2.d/S99cdeskvint /etc/rc0.d/K01cdeskvint
ln -s ../init.d/cdeskvint /etc/rc2.d/S99cdeskvint
ln -s ../init.d/cdeskvint /etc/rc0.d/K01cdeskvint
/sbin/chkconfig -f cdeskvint on

/etc/init.d/cdeskvint start
sleep 3
echo
echo "C-Desk-Vint is installed and running. In a RustDesk client, connect to:"
echo
HOME=/var/C-Desk-Vint /usr/local/sbin/C-Desk-Vint -i -c /var/C-Desk-Vint/prefs
echo
echo "Settings: Toolchest > C-Desk-Vint (log out and in for the entry to show),"
echo "or edit /var/C-Desk-Vint/prefs (ID server and key, password, port);"
echo "after editing it:  /etc/init.d/cdeskvint stop; /etc/init.d/cdeskvint start"
echo "Its log is /var/C-Desk-Vint/log. Read-me: README.txt in this directory."
