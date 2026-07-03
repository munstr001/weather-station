# WeatherMS

Домашняя метеостанция на ESP8266 с сервером-приёмником на Flask.

Станция снимает температуру, влажность, давление и ветер, отправляет данные на сервер каждые 5 минут и буферизует их локально при потере связи. Сервер сохраняет архив по дням и периодически передаёт последние показания в Windy.

## Структура репозитория

```
WeatherMS/
├── main.py                          # Сервер Flask
├── requirements.txt                 # Зависимости Python
├── .env.example                     # Шаблон переменных окружения
├── templates/
│   └── dashboard.html               # Веб-дашборд
├── firmware/
│   └── weather_station/
│       └── weather_station.ino      # Прошивка ESP8266 (Wemos D1 mini)
├── weather_data/                    # Архив показаний (создаётся автоматически)
│   └── YYYY/MM/DD.txt
├── weather_data_fallback.txt        # Резервный журнал при сбое основного хранилища
└── weather.log                      # Лог сервера
```

## Железо

| Компонент | Назначение |
|-----------|------------|
| Wemos D1 mini (ESP8266) | Контроллер |
| BME280 (I2C, адрес 0x76 или 0x77) | Температура, влажность, давление |
| Анемометр на A0 | Скорость и порывы ветра |

Высота станции для пересчёта давления к уровню моря задаётся константой `STATION_ALTITUDE_M` в прошивке.

## Прошивка ESP8266

### Зависимости Arduino

Установите через Library Manager:

- `ESP8266WiFi` (ядро ESP8266)
- `Adafruit BME280 Library`
- `Adafruit Unified Sensor`
- `NTPClient`

### Настройка

Откройте `firmware/weather_station/weather_station.ino` и при необходимости измените:

```cpp
const char* WIFI_SSID = "...";
const char* WIFI_PASSWORD = "...";
const char* WEBHOOK_URL = "https://your-host:port/weather";
const char* API_KEY = "...";
```

Параметр `API_KEY` должен совпадать с `EXPECTED_KEY` на сервере.

### Прошивка

1. Подключите Wemos D1 mini по USB.
2. В Arduino IDE выберите плату **LOLIN(WEMOS) D1 R2 & mini**.
3. Укажите COM-порт и загрузите скетч.

### Поведение прошивки

- Сэмплирование ветра — каждую секунду.
- Отправка снимка — каждые 5 минут.
- При недоступности сервера данные пишутся в очередь LittleFS (`/queue.txt`).
- Очередь автоматически сливается при восстановлении Wi-Fi.
- При сбое BME280 можно отправлять последние валидные значения (`SEND_STALE_BME_VALUES`).

Формат времени в payload: `DD-MM-YYYY-HH-MM-SS`.

## Сервер Flask

### Установка

```bash
python -m venv .venv
.venv\Scripts\activate        # Windows
# source .venv/bin/activate   # Linux/macOS

pip install -r requirements.txt
copy .env.example .env        # Windows
# cp .env.example .env        # Linux/macOS
```

Заполните `.env` своими ключами (файл в git не попадает):

```env
WINDY_API_KEY=your-windy-api-key
EXPECTED_KEY=your-api-key
```

### Запуск

```bash
python main.py
```

Сервер слушает `127.0.0.1:5050`. Для продакшена используйте reverse proxy (nginx, Caddy) и HTTPS.

Веб-дашборд доступен на `/` — одностраничный интерфейс с автообновлением через SSE.

Пример Caddy для основного домена:

```caddy
https://{$SELF_STEAL_DOMAIN} {
    handle /weather* {
        reverse_proxy 127.0.0.1:5050
    }

    handle /api/* {
        reverse_proxy 127.0.0.1:5050
    }

    handle /health {
        reverse_proxy 127.0.0.1:5050
    }

    handle {
        reverse_proxy 127.0.0.1:5050
    }
}
```

### Эндпоинты

#### `GET /`

Веб-дашборд с текущими показаниями (адаптивный, mobile-friendly).

#### `GET /api/current`

JSON с последними данными для фронтенда.

#### `GET /api/stream`

Server-Sent Events: обновления каждые ~2 секунды при изменении данных.

#### `GET /health`

Проверка состояния сервера.

```json
{"status": "ok", "has_data": true}
```

#### `GET|POST /weather`

Приём данных от метеостанции.

| Параметр | Описание |
|----------|----------|
| `key` | API-ключ (обязателен) |
| `temp` | Температура, °C |
| `rh` | Относительная влажность, % |
| `mbar` | Давление, гПа |
| `wind` | Средняя скорость ветра, м/с |
| `gust` | Порыв ветра, м/с |
| `time` | Метка времени (опционально) |

Пример:

```bash
curl "http://127.0.0.1:5050/weather?key=YOUR_KEY&temp=22.5&rh=55&mbar=1013&wind=1.2&gust=3.4"
```

Ответ при успехе:

```json
{
  "status": "ok",
  "received": {
    "time": "2026-07-02T12:00:00Z",
    "temperature": "22.5",
    "humidity": "55",
    "pressure": "1013",
    "dp": 12.84,
    "wind": "1.2",
    "gust": "3.4"
  }
}
```

### Хранение данных

Каждая запись сохраняется в файл дня:

```
weather_data/2026/07/02.txt
```

Строка в файле:

```
02-07-2026-12-00-00 22.50 55.00 1013.00 1.20 3.40
```

При сбое основного каталога сервер пишет в `weather_data_fallback.txt` и возвращает **503**, чтобы ESP8266 повторил отправку позже.

### Windy

Последние показания отправляются на Windy PWS каждые **310 секунд**. Ключ задаётся в `.env` (`WINDY_API_KEY`).

### Отказоустойчивость сервера

- Безопасный парсинг `NaN` и некорректных значений от ESP.
- Повторные попытки записи на диск и HTTP-запросов.
- Восстановление `latest_data` из архива после перезапуска.
- Ротация лога `weather.log` (5 × 5 МБ).
- Потокобезопасная работа при параллельных запросах.

## Настройка ключей

| Переменная | Где | Назначение |
|------------|-----|------------|
| `API_KEY` | прошивка | Ключ отправки с ESP |
| `EXPECTED_KEY` | `.env` | Ключ проверки на сервере |
| `WINDY_API_KEY` | `.env` | Ключ станции Windy |

Значения `API_KEY` и `EXPECTED_KEY` должны совпадать.

## Лицензия

Проект для личного использования.
