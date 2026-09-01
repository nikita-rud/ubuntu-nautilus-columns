# Nautilus с колоночным режимом

Форк GNOME Files (Nautilus) 50.2.2 из Ubuntu 26.04, в который добавлен
колоночный режим отображения — тот самый, что в macOS Finder.

Каждая колонка показывает содержимое одного каталога. Выбор папки
раскрывает её в следующей колонке справа, выбор файла — показывает
колонку с его описанием.

## Что добавлено

* Третий режим отображения рядом со «Значками» и «Списком».
  Переключается кнопкой в шапке окна (цикл из трёх), пунктом
  «View As» в меню вида, либо `Ctrl+1` / `Ctrl+2` / `Ctrl+3`.
* Выбор запоминается — режим можно включить один раз и навсегда.
* Шевроны у папок, автопрокрутка к новой колонке, переходы
  между колонками стрелками влево-вправо.
* Ширина колонок тянется мышью за разделитель и запоминается.
* Колонка предпросмотра для выделенного файла: миниатюра, имя,
  тип, размер, даты. Отключается в меню вида.
* Перетаскивание файлов между колонками, включая раскрытие папки
  при наведении на неё с перетаскиваемым файлом.
* «Создать папку» и вставка действуют на каталог активной колонки,
  а не на корневой.

## Установка

### Через apt (рекомендуется)

Подключается один раз, дальше обновления приезжают обычным `apt upgrade`:

```
sudo install -d /etc/apt/keyrings
curl -fsSL https://nikita-rud.github.io/ubuntu-nautilus-columns/KEY.asc \
  | sudo tee /etc/apt/keyrings/nautilus-columns.asc > /dev/null

echo "deb [signed-by=/etc/apt/keyrings/nautilus-columns.asc] \
https://nikita-rud.github.io/ubuntu-nautilus-columns stable main" \
  | sudo tee /etc/apt/sources.list.d/nautilus-columns.list

sudo apt update
sudo apt install nautilus nautilus-data
gsettings set org.gnome.nautilus.preferences default-folder-viewer columns-view
```

Отключить и вернуть штатный Nautilus:

```
sudo rm /etc/apt/sources.list.d/nautilus-columns.list \
        /etc/apt/keyrings/nautilus-columns.asc
sudo apt update
sudo apt install --reinstall --allow-downgrades \
  nautilus nautilus-data libnautilus-extension4
```

Только для Ubuntu 26.04, amd64. На других выпусках — сборка из исходников.

### Готовые пакеты одним файлом

Лежат в [Releases](https://github.com/nikita-rud/ubuntu-nautilus-columns/releases),
если не хочется подключать репозиторий. Тогда обновления придётся ставить
руками, а `sudo apt-mark hold nautilus nautilus-data libnautilus-extension4`
не даст системе вернуть официальную версию.

## Сборка из исходников

Один раз — зависимости:

```
sudo apt build-dep nautilus
sudo apt install meson ninja-build
```

Затем:

```
./install.sh devel      # отдельное приложение рядом с системным
./install.sh replace    # заменить системный Files
```

Для разработки, без установки:

```
meson setup builddir --prefix=/usr/local -Dprofile=Devel \
  -Ddocs=false -Dtests=none -Dcloudproviders=false
ninja -C builddir
./run-devel.sh
```

## Устройство

Основной код — `src/nautilus-columns-view.c`. Колоночный режим — это
подкласс `NautilusListBase`, наравне с `NautilusGridView` и
`NautilusListView`. Он переиспользует ту же общую модель
(`NautilusViewModel`), которая уже умеет держать несколько каталогов
одновременно ради раскрывающихся папок в списке: колонка N+1 — это
`GtkFilterListModel` по строкам, чей родитель — выбранная строка
колонки N.

Ветка `ubuntu` держит нетронутые деревья исходников Ubuntu, по коммиту на
версию. Ветка `main` — те же деревья плюс наши изменения. Поэтому переход на
новую версию Nautilus сводится к `git rebase --onto`, а конфликты сразу видны
построчно.

Версия пакета в `debian/changelog` не хранится в репозитории, а дописывается
при сборке (`tools/set-fork-version.sh`): Ubuntu добавляет свою запись ровно
в то же место файла, так что коммит с ней конфликтовал бы при каждом переносе.
Номер нашей ревизии поверх базы Ubuntu лежит в `FORK_REVISION` — увеличьте его,
если меняете код форка, не меняя версию Ubuntu.

## Автоматика

`.github/workflows/watch-upstream.yml` раз в сутки проверяет, не вышла ли
новая версия nautilus в Ubuntu. Если вышла — переносит наши коммиты на неё
(`tools/rebase-onto-ubuntu.sh`), собирает и публикует. Если перенос
конфликтует — заводит issue, дальше нужен человек.

Ждать этого стоит: наши изменения не просто добавляют файлы, они встраиваются
в чужой код — `nautilus-list-base.c`, `nautilus-files-view.c`,
`nautilus-window-slot.c`. Точечные обновления переносятся чисто почти всегда,
смена крупной версии GNOME — почти наверняка потребует рук.

`.github/workflows/build.yml` собирает пакеты, обновляет apt-репозиторий на
GitHub Pages и выкладывает релиз.

## Лицензия

GPL-3.0-or-later, как и у исходного Nautilus.
