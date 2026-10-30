#!/bin/sh
# A shareable build names RustDesk's public ID server and nothing personal:
# not the user's own server, relay, key or console. Those are read from this
# machine's RustDesk client settings (and CDV_PRIVATE_STRINGS, one a line)
# and looked for with ../crustdesk-macos-classic/tools/find-strings.py,
# which never prints them.
#
#   scripts/check-generic.sh FILE...
here=$(cd "$(dirname "$0")/.." && pwd)
find=$here/../crustdesk-macos-classic/tools/find-strings.py
toml=${RUSTDESK_TOML:-$HOME/.config/rustdesk/RustDesk2.toml}
set -- --must rs-ny.rustdesk.com --must-not dani.tech "$@"
list=$(mktemp)
trap 'rm -f "$list"' EXIT
printf '%s\n' "${CDV_PRIVATE_STRINGS:-}" > "$list"
if [ -f "$toml" ]; then
    sed -n "s/^\(custom-rendezvous-server\|relay-server\|api-server\|key\) *= *'\(.*\)'\$/\2/p" "$toml" >> "$list"
fi
n=0
while IFS= read -r s; do
    s=${s%$(printf '\r')}
    s=${s#https://}
    s=${s#http://}
    [ -n "$s" ] || continue
    set -- --must-not "$s" "$@"
    n=$((n + 1))
done < "$list"
echo "checking for the public server and against $n personal string(s)"
exec python3 "$find" "$@"
