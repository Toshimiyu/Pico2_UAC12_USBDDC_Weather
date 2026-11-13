#!/usr/bin/env python3
"""Send Japan Meteorological Agency forecasts to the Pico USB CDC interface."""

from __future__ import annotations

import argparse
import json
import logging
import os
import signal
import sys
import time
from dataclasses import dataclass
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Callable
from urllib.error import URLError
from urllib.request import Request, urlopen

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


JMA_AREA_URL = "https://www.jma.go.jp/bosai/common/const/area.json"
JMA_STATION_URL = "https://www.jma.go.jp/bosai/amedas/const/amedastable.json"
JMA_FORECAST_URL = "https://www.jma.go.jp/bosai/forecast/data/forecast/{}.json"
JST = timezone(timedelta(hours=9))
PICO_VID = 0x2E8A
PICO_PID = 0xFEDE
WEATHER_CDC_INTERFACE = "Weather Service"
WEATHER_CDC_INTERFACE_NUMBER = 2
DIAGNOSTICS_CDC_INTERFACE_NUMBER = 4
DIAGNOSTICS_CDC_INTERFACE = "USB Diagnostics"
LEGACY_CDC_INTERFACE = "Weather/Diagnostic CDC"
WEATHER_TYPES = ("晴れ", "曇り", "雨", "雪")
CONNECTORS = ("", "時々", "のち")

FORECAST_RETRY_S = 60
FORECAST_START_DELAY_S = 5
FORECAST_GUARD_S = 5
RECONNECT_INTERVAL_S = 2
AREA_LABEL_MAX_LENGTH = 22

logger = logging.getLogger("weather_service")


@dataclass(frozen=True)
class ForecastArea:
    office: str
    weather: str
    temperature: str

    def __post_init__(self) -> None:
        for name, value, length in (("office", self.office, 6), ("weather", self.weather, 6),
                                    ("temperature", self.temperature, 5)):
            if not isinstance(value, str) or len(value) != length or not value.isascii() or not value.isdigit():
                raise ValueError(f"{name} must be a {length}-digit area code")


def config_path() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if not local_app_data:
        raise RuntimeError("LOCALAPPDATA is not set; the weather service requires Windows.")
    return Path(local_app_data) / "Pico2WeatherService" / "forecast_area.json"


def load_area(path: Path) -> ForecastArea:
    try:
        with path.open(encoding="utf-8") as file:
            data = json.load(file)
        if not isinstance(data, dict):
            raise ValueError("configuration must be a JSON object")
        return ForecastArea(data["office"], data["weather"], data["temperature"])
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise RuntimeError(f"Cannot load forecast area configuration ({error}). Run --setup first.") from error


def fetch_json(url: str) -> Any:
    request = Request(url, headers={"User-Agent": "Pico2-Weather-Service/1.0"})
    with urlopen(request, timeout=15) as response:
        return json.load(response)


def area_label(station_code: str) -> str:
    stations = fetch_json(JMA_STATION_URL)
    name = stations[station_code]["enName"]
    if not isinstance(name, str) or not name or any(ord(char) < 32 or ord(char) > 126 or char == "," for char in name):
        raise ValueError("JMA area name is not a printable ASCII label.")
    if len(name) > AREA_LABEL_MAX_LENGTH:
        name = name[:AREA_LABEL_MAX_LENGTH - 3].rstrip() + "..."
    return name


def forecast_offices() -> dict[str, str]:
    areas = fetch_json(JMA_AREA_URL)
    offices = {code: item["name"] for code, item in areas["offices"].items()}
    if not offices:
        raise ValueError("No forecast offices were found in the JMA response.")
    return offices


def forecast_area_choices(products: list[dict[str, Any]]) -> tuple[dict[str, str], dict[str, str]]:
    weather_choices: dict[str, str] = {}
    temperature_choices: dict[str, str] = {}
    for product in products:
        for series in product.get("timeSeries", []):
            for entry in series.get("areas", []):
                area = entry["area"]
                if "weatherCodes" in entry and "pops" not in entry:
                    weather_choices[area["code"]] = area["name"]
                if any(key in entry for key in ("temps", "tempsMin", "tempsMax")):
                    temperature_choices[area["code"]] = area["name"]
    if not weather_choices or not temperature_choices:
        raise ValueError("Weather areas or temperature locations are missing from the JMA response.")
    return weather_choices, temperature_choices


def save_area(path: Path, selected: ForecastArea) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as file:
        json.dump({"office": selected.office, "weather": selected.weather,
                   "temperature": selected.temperature}, file, ensure_ascii=False, indent=2)
        file.write("\n")


@dataclass(frozen=True)
class DailyForecast:
    weather1: str
    connector: str
    weather2: str
    high: int | None
    low: int | None
    precipitation: int

    def to_cdc_line(self, command: str) -> str:
        high = "" if self.high is None else self.high
        low = "" if self.low is None else self.low
        return (
            f"{command},{self.weather1},{self.connector},{self.weather2},"
            f"{high},{low},{self.precipitation}\r\n"
        )


def _as_date(value: str) -> date:
    return datetime.fromisoformat(value.replace("Z", "+00:00")).date()


def _find_area(series: dict[str, Any], codes: tuple[str, ...]) -> dict[str, Any] | None:
    for code in codes:
        for area in series.get("areas", []):
            area_info = area.get("area", {})
            if area_info.get("code") == code:
                return area
    return None


def _jma_condition(text: str, code: str) -> str:
    normalized = text.replace("くもり", "曇り")
    if "晴れ" not in normalized and "晴" in normalized:
        normalized = normalized.replace("晴", "晴れ")
    for token in WEATHER_TYPES:
        if token in normalized:
            return token
    first_digit = code[:1]
    return {"1": "晴れ", "2": "曇り", "3": "雨", "4": "雪"}.get(first_digit, "曇り")


def _weather_parts(text: str, code: str) -> tuple[str, str, str]:
    connector = ""
    split_at = -1
    split_size = 0
    for token, output in (("時々", "時々"), ("一時", "時々"), ("後", "のち"), ("のち", "のち")):
        index = text.find(token)
        if index >= 0 and (split_at < 0 or index < split_at):
            connector, split_at, split_size = output, index, len(token)

    if split_at >= 0:
        first_text = text[:split_at]
        second_text = text[split_at + split_size :]
        first = _jma_condition(first_text, code)
        second = _jma_condition(second_text, code)
        return first, connector, second

    return _jma_condition(text, code), "", ""


def _daily_temperatures(products: list[dict[str, Any]], target: date, area_code: str) -> tuple[int, int] | None:
    values: list[int] = []
    for product in products:
        for series in product.get("timeSeries", []):
            dates = series.get("timeDefines", [])
            area = _find_area(series, (area_code,))
            if area is None:
                continue
            for key in ("temps", "tempsMin", "tempsMax"):
                samples = area.get(key, [])
                for index, timestamp in enumerate(dates):
                    if index >= len(samples) or _as_date(timestamp) != target:
                        continue
                    try:
                        values.append(int(samples[index]))
                    except (TypeError, ValueError):
                        continue

    if not values:
        return None
    return max(values), min(values)


def _daily_weather(products: list[dict[str, Any]], target: date, area_code: str) -> tuple[str, str, str] | None:
    for product in products:
        for series in product.get("timeSeries", []):
            dates = series.get("timeDefines", [])
            area = _find_area(series, (area_code,))
            if area is None:
                continue
            weather_values = area.get("weathers", [])
            codes = area.get("weatherCodes", [])
            for index, timestamp in enumerate(dates):
                if _as_date(timestamp) != target or index >= len(codes):
                    continue
                text = weather_values[index] if index < len(weather_values) else ""
                return _weather_parts(text, str(codes[index]))
    return None


def _daily_precipitation(products: list[dict[str, Any]], target: date, area_code: str) -> int | None:
    values: list[int] = []
    for product in products:
        for series in product.get("timeSeries", []):
            dates = series.get("timeDefines", [])
            area = _find_area(series, (area_code,))
            if area is None:
                continue
            probabilities = area.get("pops", [])
            for index, timestamp in enumerate(dates):
                if index >= len(probabilities) or _as_date(timestamp) != target:
                    continue
                try:
                    values.append(int(probabilities[index]))
                except (TypeError, ValueError):
                    continue
    return max(values) if values else None


def parse_forecast(products: list[dict[str, Any]], target: date, area: ForecastArea) -> DailyForecast | None:
    weather = _daily_weather(products, target, area.weather)
    temperatures = _daily_temperatures(products, target, area.temperature)
    precipitation = _daily_precipitation(products, target, area.weather)
    if weather is None or precipitation is None:
        return None

    weather1, connector, weather2 = weather
    high, low = temperatures if temperatures is not None else (None, None)
    if weather1 not in WEATHER_TYPES or weather2 not in WEATHER_TYPES + ("",):
        return None
    if connector not in CONNECTORS:
        return None
    if high is not None and low is not None and not -30 <= low <= high <= 50:
        return None
    if not 0 <= precipitation <= 100:
        return None
    return DailyForecast(weather1, connector, weather2, high, low, precipitation)


def fetch_forecast(office: str) -> list[dict[str, Any]]:
    payload = fetch_json(JMA_FORECAST_URL.format(office))
    if not isinstance(payload, list) or not payload or not all(isinstance(item, dict) for item in payload):
        raise ValueError("The JMA forecast response has an unexpected JSON shape.")
    return payload


def _cdc_interface_role(port: Any) -> str | None:
    interface = (getattr(port, "interface", None) or "").strip().casefold()
    if interface == WEATHER_CDC_INTERFACE.casefold():
        return "weather"
    if interface == DIAGNOSTICS_CDC_INTERFACE.casefold():
        return "diagnostics"
    if interface == LEGACY_CDC_INTERFACE.casefold():
        return "legacy"

    interface_number = _port_interface_number(port)
    if interface_number == WEATHER_CDC_INTERFACE_NUMBER:
        return "weather"
    if interface_number == DIAGNOSTICS_CDC_INTERFACE_NUMBER:
        return "diagnostics"
    return None


def _port_interface_number(port: Any) -> int | None:
    location = getattr(port, "location", None) or ""
    suffix = location.rsplit(":x.", 1)
    if len(suffix) == 2 and suffix[1].isdigit():
        return int(suffix[1])
    return None


def resolve_port(requested_port: str | None) -> str:
    if requested_port:
        return requested_port
    if list_ports is None:
        raise RuntimeError("Install the serial dependency with: py -m pip install -r requirements-weather.txt")
    pico_ports = [
        port for port in list_ports.comports()
        if port.vid == PICO_VID and port.pid == PICO_PID
    ]
    weather_ports = [
        port for port in pico_ports
        if _cdc_interface_role(port) == "weather"
    ]
    if len(weather_ports) == 1:
        return weather_ports[0].device
    if len(weather_ports) > 1:
        raise RuntimeError(
            "Multiple Pico Weather Service CDC ports were found. Specify the weather COM port with --port COMx."
        )
    if len(pico_ports) == 1:
        if _cdc_interface_role(pico_ports[0]) == "legacy":
            return pico_ports[0].device
    if pico_ports:
        port_descriptions = []
        for port in pico_ports:
            interface_number = _port_interface_number(port)
            interface_label = str(interface_number) if interface_number is not None else "unknown"
            port_descriptions.append(f"{port.device} (USB interface {interface_label})")
        available_ports = ", ".join(port_descriptions)
        raise RuntimeError(
            "Could not identify the Pico Weather Service CDC port. "
            f"Matching Pico COM ports: {available_ports}. Specify it with --port COMx."
        )
    raise RuntimeError(
        "Pico CDC port not found. Connect the device or specify --port COMx."
    )


def time_line(now: datetime) -> str:
    return f"TIME,{now.year},{now.month},{now.day},{now.hour},{now.minute}\r\n"


def forecast_lines(products: list[dict[str, Any]], today: date, area: ForecastArea) -> dict[str, str]:
    lines: dict[str, str] = {}
    for command, day in (("TODAY", today), ("TOMORROW", today + timedelta(days=1))):
        forecast = parse_forecast(products, day, area)
        if forecast is None:
            logger.warning("JMA has no complete %s forecast for %s; keeping the Pico's previous data.", command, day)
            continue
        lines[command] = forecast.to_cdc_line(command)
    return lines


def send(connection: Any, line: str) -> None:
    payload = line.encode("utf-8")
    written = connection.write(payload)
    if written != len(payload):
        raise OSError(f"Incomplete CDC write: sent {written} of {len(payload)} bytes.")
    logger.info("Sent %s", line.strip())


def probe_connection(connection: Any) -> None:
    """Raises OSError when the device is gone; reading the queue size touches the driver without blocking."""
    getattr(connection, "in_waiting", 0)


def close_connection(connection: Any) -> None:
    """Releases the COM port even if the device is already gone."""
    try:
        connection.close()
        logger.info("Released the Pico CDC port.")
    except Exception as error:  # noqa: BLE001 - shutdown must never fail
        logger.warning("Could not close the Pico CDC port cleanly: %s", error)


def _raise_stop(signum: int, frame: Any) -> None:
    raise KeyboardInterrupt


def install_stop_handlers() -> None:
    # Ctrl+Break, console close and service stop are turned into the same clean shutdown as Ctrl+C.
    for name in ("SIGTERM", "SIGBREAK"):
        number = getattr(signal, name, None)
        if number is not None:
            signal.signal(number, _raise_stop)


def next_forecast_time(now: datetime) -> datetime:
    target = now.replace(minute=50, second=45, microsecond=0)
    if target <= now:
        target += timedelta(hours=1)
    return target


class WeatherService:
    """Keeps the latest forecast and resends it whenever the Pico reconnects."""

    def __init__(
        self,
        requested_port: str | None,
        area: ForecastArea,
        *,
        open_port: Callable[[str], Any] | None = None,
        fetch: Callable[[str], list[dict[str, Any]]] = fetch_forecast,
        fetch_area_label: Callable[[str], str] = area_label,
        resolve: Callable[[str | None], str] = resolve_port,
        monotonic: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
        clock: Callable[[], datetime] = lambda: datetime.now(JST),
        wx_timestamps: bool = False,
    ) -> None:
        self.requested_port = requested_port
        self.area = area
        self.open_port = open_port or self._open_serial
        self.fetch = fetch
        self.fetch_area_label = fetch_area_label
        self.resolve = resolve
        self.monotonic = monotonic
        self.sleep = sleep
        self.clock = clock
        self.wx_timestamps = wx_timestamps
        self.cached_lines: dict[str, str] = {}
        self.next_fetch = self.monotonic() + FORECAST_START_DELAY_S
        self.next_forecast_at: datetime | None = None
        self.area_label: str | None = None
        self.next_area_fetch = 0.0

    @staticmethod
    def _open_serial(port_name: str) -> Any:
        return serial.Serial(port_name, baudrate=115200, timeout=1, write_timeout=2)

    def refresh_forecast(self, now: datetime) -> bool:
        try:
            lines = forecast_lines(self.fetch(self.area.office), now.date(), self.area)
        except (OSError, URLError, TimeoutError, ValueError, KeyError, TypeError) as error:
            logger.error("Could not update JMA forecasts: %s", error)
            self.next_fetch = self.monotonic() + FORECAST_RETRY_S
            return False
        self.cached_lines.update(lines)
        self.next_fetch = 0.0
        self.next_forecast_at = next_forecast_time(self.clock())
        return bool(lines)

    def session(self, port_name: str) -> None:
        connection = self.open_port(port_name)
        try:
            logger.info("Connected to Pico CDC on %s.", port_name)
            last_time: datetime | None = None
            last_time_sent = self.monotonic()
            pending_forecasts = [key for key in ("TODAY", "TOMORROW") if key in self.cached_lines]
            while True:
                now = self.clock()
                monotonic_now = self.monotonic()
                minute = now.replace(second=0, microsecond=0)
                time_due = minute != last_time
                if time_due:
                    send(connection, time_line(now))
                    if self.wx_timestamps:
                        send(connection, f"WXTIME,{int(self.clock().timestamp() * 1000)}\r\n")
                    last_time = minute
                    last_time_sent = self.monotonic()
                new_area_label = False
                if self.area_label is None and monotonic_now >= self.next_area_fetch:
                    try:
                        self.area_label = self.fetch_area_label(self.area.temperature)
                        new_area_label = True
                    except (OSError, ValueError, KeyError, TypeError) as error:
                        logger.warning("Could not retrieve forecast area label: %s", error)
                        self.next_area_fetch = self.monotonic() + FORECAST_RETRY_S
                if self.area_label is not None and (time_due or new_area_label):
                    send(connection, f"AREA,{self.area_label}\r\n")
                away_from_minute = FORECAST_GUARD_S <= now.second < 60 - FORECAST_GUARD_S
                fetch_due = (self.next_forecast_at is None or now >= self.next_forecast_at)
                if (away_from_minute and monotonic_now - last_time_sent >= FORECAST_GUARD_S
                        and monotonic_now >= self.next_fetch and fetch_due):
                    if self.refresh_forecast(now):
                        pending_forecasts = [key for key in ("TODAY", "TOMORROW") if key in self.cached_lines]
                now = self.clock()
                if (pending_forecasts and now.replace(second=0, microsecond=0) == last_time
                        and FORECAST_GUARD_S <= now.second < 60 - FORECAST_GUARD_S
                        and self.monotonic() - last_time_sent >= FORECAST_GUARD_S):
                    send(connection, self.cached_lines[pending_forecasts.pop(0)])
                # Probe the port each second to notice unplugging quickly.
                probe_connection(connection)
                self.sleep(1)
        finally:
            close_connection(connection)

    def run_forever(self) -> None:
        if serial is None and self.open_port == self._open_serial:
            raise RuntimeError("Install the serial dependency with: py -m pip install -r requirements-weather.txt")
        while True:
            try:
                self.session(self.resolve(self.requested_port))
            except (OSError, RuntimeError) as error:
                logger.warning("Pico connection unavailable: %s; retrying in %d s.", error, RECONNECT_INTERVAL_S)
                self.sleep(RECONNECT_INTERVAL_S)


def configure_logging(log_file: str | None) -> None:
    handlers: list[logging.Handler] = []
    if sys.stderr is not None:
        handlers.append(logging.StreamHandler())
    if log_file:
        from logging.handlers import RotatingFileHandler
        handlers.append(RotatingFileHandler(log_file, maxBytes=512_000, backupCount=2, encoding="utf-8"))
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
                        handlers=handlers, force=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial port name, for example COM5. Auto-detected by default.")
    parser.add_argument("--log-file", help="Write a rotating log file (useful when run with pythonw.exe).")
    parser.add_argument("--setup", action="store_true", help="Open the forecast area selection window.")
    parser.add_argument("--wx-timestamps", action="store_true",
                        help="Send clock synchronization for WX measurement logs.")
    args = parser.parse_args(argv)
    configure_logging(args.log_file)
    install_stop_handlers()
    try:
        if args.setup:
            if __package__:
                from .weather_setup import setup_area
            else:
                from weather_setup import setup_area
            setup_area(config_path())
        else:
            WeatherService(args.port, load_area(config_path()), wx_timestamps=args.wx_timestamps).run_forever()
    except KeyboardInterrupt:
        logger.info("Weather service stopped.")
    except RuntimeError as error:
        logger.error("%s", error)
        return 1
    except (ImportError, OSError, ValueError, KeyError, TypeError) as error:
        logger.error("Weather setup failed: %s", error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
