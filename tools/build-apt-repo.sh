#!/bin/bash
# Собирает подписанный apt-репозиторий в public/ из готовых .deb.
# Приватный ключ ожидается в переменной APT_GPG_PRIVATE_KEY.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
DEBS=${1:-debs}
OUT=public

rm -rf "$OUT"
mkdir -p "$OUT/pool/main" "$OUT/dists/stable/main/binary-amd64"
cp "$DEBS"/*.deb "$OUT/pool/main/"

cd "$OUT"

apt-ftparchive packages pool > dists/stable/main/binary-amd64/Packages
gzip -9kf dists/stable/main/binary-amd64/Packages

# Пишем во временный файл: если создать dists/stable/Release заранее
# перенаправлением, apt-ftparchive найдёт его при обходе и впишет сам в себя.
TMP_RELEASE=$(mktemp)
apt-ftparchive \
  -o APT::FTPArchive::Release::Origin="nautilus-columns" \
  -o APT::FTPArchive::Release::Label="Nautilus with a columns view" \
  -o APT::FTPArchive::Release::Suite="stable" \
  -o APT::FTPArchive::Release::Codename="stable" \
  -o APT::FTPArchive::Release::Architectures="amd64" \
  -o APT::FTPArchive::Release::Components="main" \
  release dists/stable > "$TMP_RELEASE"
mv "$TMP_RELEASE" dists/stable/Release

GNUPGHOME=$(mktemp -d)
export GNUPGHOME
chmod 700 "$GNUPGHOME"
printf '%s' "$APT_GPG_PRIVATE_KEY" | gpg --batch --quiet --import
KEY=$(gpg --list-secret-keys --with-colons | awk -F: '/^fpr:/ {print $10; exit}')

gpg --batch --yes --quiet --default-key "$KEY" -abs \
    -o dists/stable/Release.gpg dists/stable/Release
gpg --batch --yes --quiet --default-key "$KEY" --clearsign \
    -o dists/stable/InRelease dists/stable/Release
gpg --armor --export "$KEY" > KEY.asc

rm -rf "$GNUPGHOME"

VERSION=$(head -1 ../debian/changelog | sed -n 's/^[^(]*(\([^)]*\)).*/\1/p')
cat > index.html <<HTML
<!doctype html>
<meta charset="utf-8">
<title>Nautilus с колоночным режимом — apt-репозиторий</title>
<style>
 body { font: 16px/1.6 system-ui, sans-serif; max-width: 46rem; margin: 3rem auto; padding: 0 1.5rem; }
 pre { background: #f4f4f5; padding: 1rem; border-radius: .5rem; overflow-x: auto; }
 code { font-size: .9em; }
 h1 { font-size: 1.6rem; }
</style>
<h1>Nautilus с колоночным режимом</h1>
<p>GNOME Files с третьим режимом отображения — колонками, как в macOS Finder.
   Для Ubuntu 26.04, amd64. Текущая версия: <code>$VERSION</code>.</p>
<h2>Подключить</h2>
<pre>sudo install -d /etc/apt/keyrings
curl -fsSL https://nikita-rud.github.io/ubuntu-nautilus-columns/KEY.asc \\
  | sudo tee /etc/apt/keyrings/nautilus-columns.asc &gt; /dev/null

echo "deb [signed-by=/etc/apt/keyrings/nautilus-columns.asc] \\
https://nikita-rud.github.io/ubuntu-nautilus-columns stable main" \\
  | sudo tee /etc/apt/sources.list.d/nautilus-columns.list

sudo apt update
sudo apt install nautilus nautilus-data
gsettings set org.gnome.nautilus.preferences default-folder-viewer columns-view</pre>
<p>Дальше обновления приезжают обычным <code>apt upgrade</code>.</p>
<h2>Отключить</h2>
<pre>sudo rm /etc/apt/sources.list.d/nautilus-columns.list /etc/apt/keyrings/nautilus-columns.asc
sudo apt update
sudo apt install --reinstall --allow-downgrades nautilus nautilus-data libnautilus-extension4</pre>
<p>Исходный код и история изменений —
   <a href="https://github.com/nikita-rud/ubuntu-nautilus-columns">на GitHub</a>.
   GPL-3.0-or-later, как и сам Nautilus.</p>
HTML

echo "Репозиторий собран в $OUT, версия $VERSION"
ls -la dists/stable/ pool/main/
