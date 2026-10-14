#!/usr/bin/env bash
# Interactive marker: type a short label before each action in Bloody.
cd "$(dirname "$0")"
while read -r -p "метка (латиницей, без пробелов): " a; do
    [ -n "$a" ] && ./bloody-wine.sh mark "${a// /_}" && echo "  ok: $a"
done
