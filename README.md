# Rufus-RuBeRoID

[![Latest Release](https://img.shields.io/github/v/release/WestDvina/rufus-RuBeRoID?style=flat-square&label=Release)](https://github.com/WestDvina/rufus-RuBeRoID/releases)
[![Licence](https://img.shields.io/badge/license-GPLv3-blue.svg?style=flat-square&label=License)](https://www.gnu.org/licenses/gpl-3.0.en.html)

Форк [Rufus](https://github.com/pbatard/rufus), который исправляет ошибку **«Sentinel marked this request as rejected»** при скачивании ISO Windows 10/11.

---

## Ошибка Sentinel в Rufus

При попытке скачать образ Windows 10 или 11 через штатный Rufus программа выдаёт ошибку:

> **Sentinel marked this request as rejected**

Microsoft использует платформу **Sentinel** для защиты от автоматических запросов. Встроенный в Rufus скрипт FIDO (PowerShell) эмулирует браузер, но Sentinel распознаёт это и блокирует генерацию ссылки. В результате скачать ISO напрямую через Rufus стало невозможно.

## Решение

В Rufus-RuBeRoID неработающий FIDO заменён на собственный интеллектуальный алгоритм получения ссылок с обходом Sentinel и GEO-блокировки Microsoft. Ссылки доставляются через публичный JSON-файл `iso_links.json` в этом репозитории, который автоматически обновляется Telegram-ботом.

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
    ├── Скачивание ISO напрямую с CDN Microsoft (через HTTP)
    │
    └── Rufus записывает ISO на флешку
```

## Формат `iso_links.json`

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

## Что изменено

| Компонент | Изменение |
|-----------|-----------|
| FIDO | Заменён на собственный интеллектуальный алгоритм с обходом Sentinel и GEO-блокировки Microsoft |
| Проверка обновлений | URL и репозиторий переведены на этот форк |

## Ссылки

- **Статья на Дзен** — подробный разбор ошибки: [dzen.ru/a/alAIXnzQxXyhQxMS](https://dzen.ru/a/alAIXnzQxXyhQxMS)
- **Оригинальный Rufus**: [github.com/pbatard/rufus](https://github.com/pbatard/rufus)
- **Telegram-бот** для получения свежих ссылок: [@ms_windows_iso_downloader_bot](https://t.me/ms_windows_iso_downloader_bot)

## Поддержать проект

- **YooMoney**: [https://yoomoney.ru/to/410016940425865](https://yoomoney.ru/to/410016940425865)

Лицензия GPL v3.
