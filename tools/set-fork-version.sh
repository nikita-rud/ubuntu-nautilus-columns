#!/bin/bash
# Приписывает в debian/changelog нашу запись поверх версии Ubuntu.
#
# Делается на лету при сборке, а не коммитом: Ubuntu добавляет свою запись
# в то же место файла, так что коммит конфликтовал бы при каждом переносе
# на новую версию.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Первую строку читаем один раз в переменную: конвейеры вида
# `head -1 ... | grep -q` при `set -o pipefail` могут упасть по SIGPIPE.
FIRST=$(sed -n '1p' debian/changelog)
BASE=$(printf '%s\n' "$FIRST" | sed -n 's/^[^(]*(\([^)]*\)).*/\1/p')
SUITE=$(printf '%s\n' "$FIRST" | sed -n 's/^[^)]*) \([^;]*\);.*/\1/p')
REV=$(cat FORK_REVISION)
VERSION="${BASE}+columns${REV}"

case "$FIRST" in
  *+columns*)
    echo "Запись форка уже на месте: $FIRST"
    exit 0
    ;;
esac

TMP=$(mktemp)
{
  echo "nautilus (${VERSION}) ${SUITE}; urgency=medium"
  echo
  echo "  * Add a Miller columns view mode, as in the macOS Finder."
  echo
  echo " -- nrud <nickruddy707@gmail.com>  $(date -R)"
  echo
  cat debian/changelog
} > "$TMP"
mv "$TMP" debian/changelog

echo "$VERSION"
