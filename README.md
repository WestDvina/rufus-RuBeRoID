# Rufus-RuBeRoID

Форк [Rufus](https://github.com/pbatard/rufus) с заменой неработающего FIDO для РФ, РБ и других стран, где Microsoft блокирует генерацию ссылок на ISO.

[![Latest Release](https://img.shields.io/github/release-pre/pbatard/rufus.svg?style=flat-square&label=Rufus%20Release)](https://github.com/pbatard/rufus/releases)
[![Licence](https://img.shields.io/badge/license-GPLv3-blue.svg?style=flat-square&label=License)](https://www.gnu.org/licenses/gpl-3.0.en.html)

---

## Проблема

Microsoft блокирует IP из РФ, РБ и некоторых других стран на CDN-серверах. Встроенный в Rufus скрипт **FIDO** перестал работать, так как не может обойти защиту Sentinel при запросе ссылок на ISO Windows 10/11.

## Решение

В Rufus-RuBeRoID механизм FIDO заменён на **прямое получение ссылок из публичного JSON-файла** в этом репозитории (`iso_links.json`).

Файл автоматически обновляется ботом [MS ISO Downloader](https://github.com/WestDvina/ms-iso-downloader), который:
1. Работает 24/7 на сервере за пределами РФ
2. Через прокси обходит Sentinel и получает свежие ссылки с CDN Microsoft
3. Кэширует их (TTL 22 часа)
4. Публикует в `iso_links.json` в этом репозитории

## Как это работает

```
Пользователь нажимает "Download" в Rufus-RuBeRoID
    │
    ├── Диалог: выберите Windows 10 или 11
    │   └── Для Win 10: x64 или x86 (Win 11 → только x64)
    │
    ├── GET github.com/WestDvina/rufus-RuBeRoID/main/iso_links.json
    │
    ├── Парсинг JSON → получение прямой CDN-ссылки
    │
    ├── Диалог "Сохранить как" → выбор места для ISO
    │
    ├── Скачивание ISO напрямую с CDN Microsoft
    │
    └── Rufus записывает ISO на флешку
```

### Формат `iso_links.json`

```json
{
  "links": {
    "10_x64": "https://software.download.prss.microsoft.com/...",
    "10_x86": "https://software.download.prss.microsoft.com/...",
    "11_x64": "https://software.download.prss.microsoft.com/..."
  },
  "published_at": 1783433769,
  "ttl_hours": 22
}
```

## Что изменено в исходном коде

| Файл | Изменение |
|------|-----------|
| `src/net.c` | FIDO (PowerShell + Named Pipe + LZMA) заменён на HTTP-запрос к GitHub JSON + TaskDialog выбора версии/архитектуры |
| `src/stdlg.c` | Убран `CheckForFidoThread` (скачивание Fido.ver, проверка подписи, запуск PowerShell). Кнопка Download всегда активна |
| `src/rufus.h` | Добавлен `RUFUS_REPO_RAW` — URL к raw-файлам репо |

## Сборка

```bash
# Visual Studio 2026
open rufus.sln → Build Solution

# MinGW
./configure && make
```

## Для владельца бота

### `publish_cache.py`

Скрипт в `c:\script\ms-iso-downloader\publish_cache.py`:
1. Читает `iso_link_cache.json` (кэш бота)
2. Проверяет TTL — если до протухания < 6 часов, дёргает API бота для обновления
3. Публикует `iso_links.json` в репо через git push

**Запуск по расписанию:** каждые 6 часов через Планировщик Windows.

## Ссылки

- Оригинальный Rufus: [github.com/pbatard/rufus](https://github.com/pbatard/rufus)
- Бот для получения ссылок: [@ms_windows_iso_downloader_bot](https://t.me/ms_windows_iso_downloader_bot)
- Статья на Дзен: [dzen.ru/a/ajY7jfQQCSpPBgkm](https://dzen.ru/a/ajY7jfQQCSpPBgkm)
- По вопросам работы бота: [@walgo](https://t.me/walgo)

Лицензия GPL v3.
