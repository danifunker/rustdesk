#!/usr/bin/env bash
# Stub PowerPC C compiler: emits a valid (empty) host object so minicargo can
# proceed. Purpose is to exercise mrustc's Rust front-end only; the .o is junk.
args=("$@")
# expand response file (mrustc passes `cc @cmdfile`)
if [[ "${args[0]}" == @* ]]; then
  mapfile -t args < <(tr ' ' '\n' < "${args[0]#@}" | sed 's/^"//;s/"$//' | grep -v '^$')
fi
out=""
for ((i=0;i<${#args[@]};i++)); do
  [[ "${args[i]}" == "-o" ]] && out="${args[i+1]}"
done
[ -z "$out" ] && out="a.out"
mkdir -p "$(dirname "$out")"
echo 'static int _stub;' > /tmp/_stub_$$.c
if printf '%s\n' "${args[@]}" | grep -qx -- '-c'; then
  gcc -c /tmp/_stub_$$.c -o "$out" 2>/dev/null
else
  gcc /tmp/_stub_$$.c -shared -o "$out" 2>/dev/null || : > "$out"
fi
rm -f /tmp/_stub_$$.c
exit 0
