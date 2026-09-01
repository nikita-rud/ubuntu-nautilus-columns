#!/bin/bash
# Собирает и устанавливает этот Nautilus с колоночным режимом.
#
#   ./install.sh devel    — отдельное приложение «Files (Devel)» рядом с системным
#   ./install.sh replace  — заменяет системный Files (ставится в /usr/local)
#
# Зависимости сборки ставятся отдельно, один раз:
#   sudo apt build-dep nautilus && sudo apt install meson ninja-build
set -e

MODE="${1:-devel}"
cd "$(dirname "$0")"

COMMON=(--prefix=/usr/local -Ddocs=false -Dunity-launcher=true
        -Dxdg-terminal-exec=true -Dtests=none -Dcloudproviders=false -Dselinux=true)

case "$MODE" in
  devel)   EXTRA=(-Dprofile=Devel) ;;
  replace) EXTRA=() ;;
  *) echo "Использование: $0 [devel|replace]" >&2; exit 1 ;;
esac

rm -rf builddir
meson setup builddir "${COMMON[@]}" "${EXTRA[@]}"
ninja -C builddir

echo
echo ">>> Установка в /usr/local (нужен root)"
if command -v pkexec >/dev/null && [ -n "$WAYLAND_DISPLAY$DISPLAY" ]; then
  pkexec ninja -C "$PWD/builddir" install
else
  sudo ninja -C builddir install
fi

echo
if [ "$MODE" = replace ]; then
  echo "Готово. Системный Files заменён сборкой из /usr/local."
  echo "Откатиться: sudo ninja -C builddir uninstall"
else
  echo "Готово. В меню приложений появится отдельный «Files (Devel)»."
fi
echo
echo "Включить колоночный режим по умолчанию:"
echo "  gsettings set org.gnome.nautilus.preferences default-folder-viewer columns-view"
