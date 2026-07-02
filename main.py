from __future__ import annotations

import logging
import math
import os
import threading
import time
from datetime import datetime
from logging.handlers import RotatingFileHandler
from pathlib import Path

import requests
from dotenv import load_dotenv
from flask import Flask, abort, jsonify, request
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

load_dotenv()

app = Flask(__name__)

DATA_DIR = Path("weather_data")
FALLBACK_DATA_FILE = Path("weather_data_fallback.txt")
LOG_FILE = Path("weather.log")

WINDY_API_KEY = os.environ["WINDY_API_KEY"]
EXPECTED_KEY = os.environ["EXPECTED_KEY"]

WINDY_SYNC_DELAY_SEC = 310
WINDY_REQUEST_TIMEOUT_SEC = 10
WINDY_MAX_RETRIES = 3
FILE_WRITE_RETRIES = 3
FILE_WRITE_RETRY_DELAY_SEC = 0.2

TIME_FORMATS = (
    "%d-%m-%Y-%H-%M-%S",  # формат ESP8266
    "%Y-%m-%dT%H:%M:%SZ",
    "%Y-%m-%dT%H:%M:%S",
)

NAN_TOKENS = frozenset({"", "nan", "none", "null", "-"})

_latest_data_lock = threading.Lock()
_file_write_lock = threading.Lock()
_windy_timer_lock = threading.Lock()
_windy_timer: threading.Timer | None = None

latest_data: dict[str, object] = {}


def setup_logging() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=[
            RotatingFileHandler(
                LOG_FILE,
                maxBytes=5 * 1024 * 1024,
                backupCount=5,
                encoding="utf-8",
            ),
            logging.StreamHandler(),
        ],
        force=True,
    )


setup_logging()


def create_http_session() -> requests.Session:
    session = requests.Session()
    retry = Retry(
        total=WINDY_MAX_RETRIES,
        connect=WINDY_MAX_RETRIES,
        read=WINDY_MAX_RETRIES,
        backoff_factor=0.5,
        status_forcelist=(429, 500, 502, 503, 504),
        allowed_methods=frozenset({"GET"}),
    )
    adapter = HTTPAdapter(max_retries=retry)
    session.mount("https://", adapter)
    session.mount("http://", adapter)
    return session


http_session = create_http_session()


def extract_param(name: str) -> str | None:
    """Универсальный метод для получения параметра из args, form и JSON."""
    try:
        json_body = request.get_json(silent=True) or {}
    except Exception:
        json_body = {}

    value = request.args.get(name) or request.form.get(name) or json_body.get(name)
    if value is None:
        return None
    return str(value).strip()


def safe_float(value: str | None) -> float | None:
    if value is None:
        return None

    text = value.strip()
    if text.lower() in NAN_TOKENS:
        return None

    try:
        number = float(text)
    except ValueError:
        return None

    if math.isnan(number) or math.isinf(number):
        return None

    return number


def calculate_dew_point(temperature_c: float, humidity_percent: float) -> float:
    """
    Вычисляет точку росы (°C) по температуре (°C) и относительной влажности (%)
    с использованием формулы Магнуса.
    """
    a = 17.62
    b = 243.12

    gamma = (a * temperature_c) / (b + temperature_c) + math.log(
        humidity_percent / 100.0
    )
    return (b * gamma) / (a - gamma)


def calculate_dew_point_safe(
    temperature_c: float | None,
    humidity_percent: float | None,
) -> float | None:
    if temperature_c is None or humidity_percent is None:
        return None
    if humidity_percent <= 0 or humidity_percent > 100:
        return None

    try:
        return round(calculate_dew_point(temperature_c, humidity_percent), 2)
    except (ValueError, ZeroDivisionError, OverflowError):
        return None


def parse_weather_time(time_str: str | None) -> datetime:
    if not time_str:
        return datetime.utcnow()

    for fmt in TIME_FORMATS:
        try:
            return datetime.strptime(time_str, fmt)
        except ValueError:
            continue

    logging.warning("Не удалось распознать время '%s', используется UTC now", time_str)
    return datetime.utcnow()


def daily_data_path(record_time: datetime) -> Path:
    return (
        DATA_DIR
        / str(record_time.year)
        / f"{record_time.month:02d}"
        / f"{record_time.day:02d}.txt"
    )


def build_record_line(
    time_str: str,
    temp: str,
    rh: str,
    mbar: str,
    wind: str,
    gust: str,
) -> str:
    return f"{time_str} {temp} {rh} {mbar} {wind} {gust}\n"


def append_line_with_retry(path: Path, line: str) -> bool:
    for attempt in range(1, FILE_WRITE_RETRIES + 1):
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("a", encoding="utf-8") as file:
                file.write(line)
                file.flush()
            return True
        except OSError as exc:
            logging.error(
                "Ошибка записи в %s (попытка %s/%s): %s",
                path,
                attempt,
                FILE_WRITE_RETRIES,
                exc,
            )
            if attempt < FILE_WRITE_RETRIES:
                time.sleep(FILE_WRITE_RETRY_DELAY_SEC)

    return False


def save_weather_record(
    time_str: str,
    temp: str,
    rh: str,
    mbar: str,
    wind: str,
    gust: str,
) -> tuple[bool, str]:
    line = build_record_line(time_str, temp, rh, mbar, wind, gust)
    record_time = parse_weather_time(time_str)
    primary_path = daily_data_path(record_time)

    with _file_write_lock:
        if append_line_with_retry(primary_path, line):
            return True, str(primary_path)

        logging.warning("Переход на резервный файл %s", FALLBACK_DATA_FILE)
        if append_line_with_retry(FALLBACK_DATA_FILE, line):
            return True, str(FALLBACK_DATA_FILE)

    return False, str(primary_path)


def parse_record_line(line: str) -> dict[str, object] | None:
    parts = line.strip().split()
    if len(parts) < 6:
        return None

    time_str, temp, rh, mbar, wind, gust = parts[:6]
    temp_value = safe_float(temp)
    rh_value = safe_float(rh)

    return {
        "time": time_str,
        "temperature": temp,
        "humidity": rh,
        "pressure": mbar,
        "dp": calculate_dew_point_safe(temp_value, rh_value),
        "wind": wind,
        "gust": gust,
    }


def recover_latest_data() -> None:
    candidates: list[tuple[float, dict[str, object]]] = []

    search_paths = sorted(DATA_DIR.rglob("*.txt")) if DATA_DIR.exists() else []
    if FALLBACK_DATA_FILE.exists():
        search_paths.append(FALLBACK_DATA_FILE)

    for path in search_paths:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            logging.warning("Не удалось прочитать %s: %s", path, exc)
            continue

        if not lines:
            continue

        record = parse_record_line(lines[-1])
        if record is None:
            continue

        try:
            sort_key = parse_weather_time(str(record["time"])).timestamp()
        except (TypeError, ValueError, OSError):
            sort_key = path.stat().st_mtime

        candidates.append((sort_key, record))

    if not candidates:
        logging.info("Данные для восстановления latest_data не найдены")
        return

    _, recovered = max(candidates, key=lambda item: item[0])
    with _latest_data_lock:
        global latest_data
        latest_data = recovered

    logging.info("Восстановлены последние данные из архива: time=%s", recovered["time"])


def update_latest_data(record: dict[str, object]) -> dict[str, object]:
    with _latest_data_lock:
        global latest_data
        latest_data = record.copy()

    return record


def build_received_payload(
    time_str: str,
    temp: str,
    rh: str,
    mbar: str,
    wind: str,
    gust: str,
) -> dict[str, object]:
    temp_value = safe_float(temp)
    rh_value = safe_float(rh)

    return {
        "time": time_str,
        "temperature": temp,
        "humidity": rh,
        "pressure": mbar,
        "dp": calculate_dew_point_safe(temp_value, rh_value),
        "wind": wind,
        "gust": gust,
    }


def build_windy_params(data: dict[str, object]) -> dict[str, object] | None:
    params: dict[str, object] = {}

    temp = safe_float(str(data.get("temperature")))
    rh = safe_float(str(data.get("humidity")))
    mbar = safe_float(str(data.get("pressure")))
    wind = safe_float(str(data.get("wind", 0)))
    gust = safe_float(str(data.get("gust", 0)))
    dp = data.get("dp")

    if temp is not None:
        params["temp"] = temp
    if rh is not None:
        params["rh"] = rh
    if mbar is not None:
        params["mbar"] = mbar
    if isinstance(dp, (int, float)) and not math.isnan(float(dp)):
        params["dewpoint"] = dp
    if wind is not None:
        params["wind"] = wind
    if gust is not None:
        params["gust"] = gust

    if not params:
        return None

    return params


@app.route("/health", methods=["GET"])
def health():
    with _latest_data_lock:
        has_data = bool(latest_data)

    return jsonify({"status": "ok", "has_data": has_data}), 200


@app.route("/weather", methods=["GET", "POST"])
def weather():
    key = extract_param("key")
    if key != EXPECTED_KEY:
        abort(403)

    temp = extract_param("temp")
    rh = extract_param("rh")
    mbar = extract_param("mbar")
    time_str = extract_param("time")
    wind = extract_param("wind") or "0"
    gust = extract_param("gust") or "0"

    if temp is None or rh is None or mbar is None:
        logging.warning("Неполные данные: temp=%s rh=%s mbar=%s", temp, rh, mbar)
        return jsonify({"status": "error", "message": "missing required fields"}), 400

    if not time_str:
        time_str = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")

    received = build_received_payload(time_str, temp, rh, mbar, wind, gust)
    saved, storage_path = save_weather_record(time_str, temp, rh, mbar, wind, gust)

    if not saved:
        logging.error("Не удалось сохранить запись, ESP должен повторить отправку")
        return jsonify(
            {
                "status": "error",
                "message": "storage unavailable",
                "received": received,
            }
        ), 503

    update_latest_data(received)

    logging.info(
        "Received weather: T=%s°C, H=%s%%, P=%s hPa, "
        "DP=%s°C, wind=%s m/s, gust=%s m/s at %s -> %s",
        temp,
        rh,
        mbar,
        received["dp"],
        wind,
        gust,
        time_str,
        storage_path,
    )

    return jsonify({"status": "ok", "received": received}), 200


@app.errorhandler(Exception)
def handle_unexpected_error(exc: Exception):
    logging.exception("Необработанная ошибка запроса: %s", exc)
    return jsonify({"status": "error", "message": "internal server error"}), 500


def send_to_windy() -> None:
    with _latest_data_lock:
        data = latest_data.copy()

    if not data:
        logging.warning("Нет данных для отправки на Windy.")
        return

    params = build_windy_params(data)
    if params is None:
        logging.warning("Нет валидных числовых данных для Windy.")
        return

    url = f"https://stations.windy.com/pws/update/{WINDY_API_KEY}"

    for attempt in range(1, WINDY_MAX_RETRIES + 1):
        try:
            response = http_session.get(
                url,
                params=params,
                timeout=WINDY_REQUEST_TIMEOUT_SEC,
            )
            logging.info(
                "Отправлено на Windy: код %s (попытка %s/%s)",
                response.status_code,
                attempt,
                WINDY_MAX_RETRIES,
            )
            if response.ok:
                return
        except requests.RequestException as exc:
            logging.error(
                "Ошибка отправки на Windy (попытка %s/%s): %s",
                attempt,
                WINDY_MAX_RETRIES,
                exc,
            )

        if attempt < WINDY_MAX_RETRIES:
            time.sleep(attempt)

    logging.error("Windy: все попытки отправлены без успеха")


def _windy_sync_tick() -> None:
    try:
        send_to_windy()
    except Exception:
        logging.exception("Критическая ошибка цикла синхронизации Windy")
    finally:
        schedule_windy_sync()


def schedule_windy_sync(delay_sec: int = WINDY_SYNC_DELAY_SEC) -> None:
    global _windy_timer

    with _windy_timer_lock:
        if _windy_timer is not None:
            _windy_timer.cancel()

        _windy_timer = threading.Timer(delay_sec, _windy_sync_tick)
        _windy_timer.daemon = True
        _windy_timer.start()


def bootstrap() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    recover_latest_data()
    schedule_windy_sync()


bootstrap()

if __name__ == "__main__":
    send_to_windy()
    app.run(host="127.0.0.1", port=5050, threaded=True)
