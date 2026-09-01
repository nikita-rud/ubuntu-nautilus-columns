#!/bin/bash
# Переносит наши коммиты на свежий исходник nautilus из Ubuntu.
#
# Ветка `ubuntu` держит нетронутые деревья Ubuntu, по коммиту на версию.
# Ветка `main` — те же деревья плюс наши изменения. Обновление сводится к
# импорту нового дерева в `ubuntu` и `git rebase --onto`.
#
# Коды возврата:
#   0 — перенесено (или уже актуально, см. вывод)
#   2 — конфликт, нужен человек
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# В свежем клоне (например, в CI) локальных веток может не быть.
git rev-parse --verify -q ubuntu >/dev/null || git branch ubuntu origin/ubuntu
git rev-parse --verify -q main   >/dev/null || git branch main   origin/main

CURRENT=$(git show ubuntu:debian/changelog | head -1 | sed -n 's/^[^(]*(\([^)]*\)).*/\1/p')
NEW=$(apt-cache showsrc nautilus 2>/dev/null | awk '/^Version:/ {print $2}' | sort -V | tail -1)

if [ -z "$NEW" ]; then
  echo "Не удалось узнать версию nautilus в Ubuntu. Включены ли deb-src?" >&2
  exit 1
fi

echo "у нас:    $CURRENT"
echo "в Ubuntu: $NEW"

if dpkg --compare-versions "$NEW" le "$CURRENT"; then
  echo "NOTHING_TO_DO"
  exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
( cd "$WORK" && apt-get source nautilus >/dev/null 2>&1 )
SRC=$(find "$WORK" -maxdepth 1 -type d -name 'nautilus-*' | head -1)
if [ -z "$SRC" ]; then
  echo "Исходник не скачался" >&2
  exit 1
fi

OLD_BASE=$(git rev-parse ubuntu)

# Импортируем новое дерево как один коммит в ветке ubuntu.
git checkout -q ubuntu
git rm -rq --ignore-unmatch .
rsync -a --exclude=.git "$SRC"/ .
git add -A
git commit -q -m "Ubuntu $NEW baseline"
git tag "ubuntu/${NEW#*:}"

# И переносим наши коммиты поверх него.
git checkout -q main
if git rebase --onto ubuntu "$OLD_BASE" main; then
  echo "REBASED $NEW"
  exit 0
fi

git rebase --abort || true
git checkout -q main
echo "CONFLICT $NEW" >&2
exit 2
