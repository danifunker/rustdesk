#! /bin/sh
# Remove C-Desk-Vint. Its settings in /var/C-Desk-Vint stay unless you add -a.
case "`/usr/bin/id`" in
uid=0\(*) ;;
*) echo "uninstall.sh: run this as root"; exit 1 ;;
esac
/etc/init.d/cdeskvint stop > /dev/null 2>&1
/sbin/chkconfig -f cdeskvint off
rm -f /etc/rc2.d/S99cdeskvint /etc/rc0.d/K01cdeskvint /etc/init.d/cdeskvint
rm -f /etc/config/cdeskvint /usr/local/sbin/C-Desk-Vint /usr/local/sbin/cdeskvint-panel
rm -f /usr/lib/X11/app-chests/C-Desk-Vint.chest
rm -rf /usr/local/lib/C-Desk-Vint
if [ "$1" = "-a" ]; then
	rm -rf /var/C-Desk-Vint
fi
echo "C-Desk-Vint removed."
