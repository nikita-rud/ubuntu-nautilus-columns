#!/bin/bash
# Приписывает в debian/changelog нашу запись поверх версии Ubuntu.
#
# Делается на лету при сборке, а не коммитом: Ubuntu добавляет свою запись
# в то же место файла, так что коммит конфликтовал бы при каждом переносе
# на новую версию.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

BASE=$(head -1 debian/changelog | sed -n 's/^[^(]*(\([^)]*\)).*/\1/p')
SUITE=$(head -1 debian/changelog | sed -n 's/^[^)]*) \([^;]*\);.*/\1/p')
REV=$(cat FORK_REVISION)
VERSION="${BASE}+columns${REV}"

if head -1 debian/changelog | grep -q '+columns'; then
  echo "Запись форка уже на месте: $(head -1 debian/changelog)"
  exit 0
fi

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
