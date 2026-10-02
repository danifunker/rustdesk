#! /bin/sh
#
# Install C-Desk-Vint, the RustDesk agent, from this directory. Run as root.
#
#   /usr/local/sbin/C-Desk-Vint   the agent
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

for d in /usr/local /usr/local/sbin /var/C-Desk-Vint; do
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
echo "Settings (ID server and key, password, port) are in /var/C-Desk-Vint/prefs;"
echo "after editing it:  /etc/init.d/cdeskvint stop; /etc/init.d/cdeskvint start"
echo "Its log is /var/C-Desk-Vint/log. Read-me: README.txt in this directory."
