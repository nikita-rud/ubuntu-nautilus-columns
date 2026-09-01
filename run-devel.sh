#!/bin/bash
# Запускает собранный здесь Nautilus как отдельное приложение
# (app-id org.gnome.Nautilus.Devel), не мешая системному.
cd "$(dirname "$0")"
for p in $(pgrep -f 'builddir/src/nautilus --new-window'); do
  [ "$p" != "$$" ] && [ "$p" != "$PPID" ] && kill "$p" 2>/dev/null
done
sleep 1
export GSETTINGS_SCHEMA_DIR="$PWD/builddir/data"
exec ./builddir/src/nautilus --new-window "${1:-$HOME}"
