import unittest
import time
import tkinter as tk
from datetime import date, datetime
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock
from urllib.error import URLError

from tools import weather_service
from tools import weather_setup


class ForecastParsingTests(unittest.TestCase):
    def setUp(self):
        self.area = weather_service.ForecastArea("123456", "123450", "12345")
        self.products = [
            {
                "timeSeries": [
                    {
                        "timeDefines": [
                            "2026-11-10T00:00:00+09:00",
                            "2026-11-11T00:00:00+09:00",
                        ],
                        "areas": [
                            {
                                "area": {"code": "123450"},
                                "weatherCodes": ["101", "300"],
                                "weathers": ["晴れ時々くもり", "雨"],
                            }
                        ],
                    },
                    {
                        "timeDefines": [
                            "2026-11-10T00:00:00+09:00",
                            "2026-11-11T00:00:00+09:00",
                        ],
                        "areas": [
                            {
                                "area": {"code": "123450"},
                                "pops": ["20", "60"],
                            }
                        ],
                    },
                    {
                        "timeDefines": [
                            "2026-11-10T00:00:00+09:00",
                            "2026-11-10T09:00:00+09:00",
                        ],
                        "areas": [
                            {
                                "area": {"code": "12345"},
                                "temps": ["10", "22"],
                            }
                        ],
                    },
                ]
            }
        ]

    def test_builds_cdc_line_from_daily_forecast(self):
        forecast = weather_service.parse_forecast(
            self.products, date(2026, 11, 10), self.area
        )
        self.assertEqual(
            forecast,
            weather_service.DailyForecast("晴れ", "時々", "曇り", 22, 10, 20),
        )
        self.assertEqual(
            forecast.to_cdc_line("TODAY"),
            "TODAY,晴れ,時々,曇り,22,10,20\r\n",
        )

    def test_uses_weather_code_when_description_is_empty(self):
        self.products[0]["timeSeries"][0]["areas"][0]["weathers"][0] = ""
        forecast = weather_service.parse_forecast(
            self.products, date(2026, 11, 10), self.area
        )
        self.assertEqual(forecast.weather1, "晴れ")
        self.assertEqual(forecast.weather2, "")
        self.assertEqual(forecast.connector, "")

    def test_sends_empty_temperatures_when_jma_has_none(self):
        forecast = weather_service.parse_forecast(
            self.products, date(2026, 11, 11), self.area
        )
        self.assertIsNotNone(forecast)
        self.assertTrue(forecast.to_cdc_line("TODAY").endswith(",,,60\r\n"))

    def test_selected_areas_control_weather_and_temperature(self):
        series = self.products[0]["timeSeries"]
        series[0]["areas"].append({"area": {"code": "654321"}, "weatherCodes": ["300"], "weathers": ["雨"]})
        series[1]["areas"].append({"area": {"code": "654321"}, "pops": ["70"]})
        series[2]["areas"].append({"area": {"code": "54321"}, "temps": ["5", "15"]})
        selected = weather_service.ForecastArea("123456", "654321", "54321")
        forecast = weather_service.parse_forecast(self.products, date(2026, 11, 10), selected)
        self.assertEqual(forecast, weather_service.DailyForecast("雨", "", "", 15, 5, 70))


class AreaSetupTests(unittest.TestCase):
    def test_area_label_format_and_limit(self):
        names = {
            "12345": {"enName": "Example Station"},
            "54321": {"enName": "A Very Long Station Name Here"},
            "99999": {"enName": "invalid,name"},
        }
        with mock.patch.object(weather_service, "fetch_json", return_value=names) as fetch:
            self.assertEqual(weather_service.area_label("12345"), "Example Station")
            fetch.assert_called_with(weather_service.JMA_STATION_URL)
            label = weather_service.area_label("54321")
            self.assertEqual(label, "A Very Long Station...")
            self.assertLessEqual(len(label), weather_service.AREA_LABEL_MAX_LENGTH)
            with self.assertRaises(ValueError):
                weather_service.area_label("99999")

    def test_setup_saves_selection_and_loads_it(self):
        offices = {"offices": {"123456": {"name": "Office"}}}
        products = [{"timeSeries": [
            {"areas": [{"area": {"code": "123450", "name": "Weather"}, "weatherCodes": ["100"]}]},
            {"areas": [{"area": {"code": "12345", "name": "Station"}, "temps": ["10"]}]},
        ]}]
        with TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            with mock.patch.object(weather_service, "fetch_json", return_value=offices):
                self.assertEqual(weather_service.forecast_offices(), {"123456": "Office"})
            self.assertEqual(weather_service.forecast_area_choices(products),
                             ({"123450": "Weather"}, {"12345": "Station"}))
            area = weather_service.ForecastArea("123456", "123450", "12345")
            weather_service.save_area(path, area)
            self.assertEqual(weather_service.load_area(path), area)

    def test_setup_rejects_empty_choices(self):
        with self.assertRaises(ValueError):
            weather_service.forecast_area_choices([])

    def test_missing_and_invalid_configuration_fails(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            with self.assertRaisesRegex(RuntimeError, "--setup"):
                weather_service.load_area(path)
            path.write_text('{"office":"../../", "weather":"123450", "temperature":"12345"}', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "--setup"):
                weather_service.load_area(path)


class AreaSetupWindowTests(unittest.TestCase):
    def test_selects_and_saves_area_in_window(self):
        try:
            root = tk.Tk()
        except tk.TclError as error:
            self.skipTest(f"Tk display unavailable: {error}")
        root.withdraw()
        def close_window():
            try:
                root.destroy()
            except tk.TclError:
                pass
        self.addCleanup(close_window)
        products = [{"timeSeries": [
            {"areas": [{"area": {"code": "123450", "name": "Weather"}, "weatherCodes": ["100"]}]},
            {"areas": [{"area": {"code": "12345", "name": "Station"}, "temps": ["10"]}]},
        ]}]
        with TemporaryDirectory() as directory, \
             mock.patch.object(weather_service, "forecast_offices", return_value={"123456": "Office"}), \
             mock.patch.object(weather_service, "fetch_forecast", return_value=products), \
             mock.patch.object(weather_setup.messagebox, "showinfo"):
            path = Path(directory) / "config.json"
            window = weather_setup.AreaSetupWindow(root, path)

            def wait_for(predicate):
                deadline = time.monotonic() + 3
                while not predicate() and time.monotonic() < deadline:
                    root.update()
                    time.sleep(0.01)
                self.assertTrue(predicate())

            wait_for(lambda: bool(window.office_codes))
            window.office.current(0)
            window.on_office_selected(mock.Mock())
            wait_for(lambda: bool(window.weather_codes) and bool(window.temperature_codes))
            window.weather.current(0)
            window.temperature.current(0)
            window.on_selection_changed(mock.Mock())
            self.assertEqual(str(window.save["state"]), "normal")
            window.save_selection()
            self.assertEqual(weather_service.load_area(path),
                             weather_service.ForecastArea("123456", "123450", "12345"))


class FakeClock:
    """Deterministic time source; sleep() advances time and fires scheduled events."""

    class Stop(BaseException):
        pass

    def __init__(self, until):
        self.t = 0.0
        self.until = until
        self.events = []

    def at(self, when, action):
        self.events.append((when, action))

    def monotonic(self):
        return self.t

    def sleep(self, seconds):
        self.t += seconds
        for event in [e for e in self.events if e[0] <= self.t]:
            self.events.remove(event)
            event[1]()
        if self.t >= self.until:
            raise self.Stop()


class FakeSerial:
    def __init__(self, link):
        self.link = link
        self.lines = []
        self.closed = False
        self.flush_calls = 0

    def write(self, data):
        if self.link.broken:
            raise OSError("device disconnected")
        self.lines.append(data.decode("utf-8"))
        return len(data)

    def flush(self):
        self.flush_calls += 1

    def close(self):
        self.closed = True

    def commands(self):
        return [line.split(",")[0] for line in self.lines]


class Harness:
    TODAY = "TODAY,晴れ,時々,曇り,22,14,10\r\n"
    TOMORROW = "TOMORROW,曇り,,,19,12,40\r\n"

    def __init__(self, until):
        self.clock = FakeClock(until)
        self.broken = False
        self.connections = []
        self.open_attempts = 0
        self.fetch_calls = 0
        self.fetch_error = None
        self.service = weather_service.WeatherService(
            None,
            weather_service.ForecastArea("123456", "123450", "12345"),
            open_port=self.open_port,
            fetch=self.fetch,
            fetch_area_label=lambda code: "Example Station" if code == "12345" else "Wrong Area",
            resolve=lambda _: "COM9",
            monotonic=self.clock.monotonic,
            sleep=self.clock.sleep,
            clock=lambda: datetime(2026, 11, 10, 18, 0, tzinfo=weather_service.JST),
        )

    def open_port(self, _port):
        self.open_attempts += 1
        if self.broken:
            raise OSError("port not found")
        connection = FakeSerial(self)
        self.connections.append(connection)
        return connection

    def fetch(self, office):
        self.office = office
        self.fetch_calls += 1
        if self.fetch_error:
            raise self.fetch_error
        return []

    def unplug(self):
        self.broken = True

    def plug(self):
        self.broken = False

    def run(self):
        lines = {"TODAY": self.TODAY, "TOMORROW": self.TOMORROW}
        with mock.patch.object(weather_service, "forecast_lines", return_value=lines):
            try:
                self.service.run_forever()
            except FakeClock.Stop:
                pass


class ServiceBehaviourTests(unittest.TestCase):
    def test_01_startup_sends_time_today_tomorrow_in_order(self):
        h = Harness(until=1)
        h.run()
        self.assertEqual(h.connections[0].commands(), ["TIME", "AREA", "TODAY", "TOMORROW"])
        self.assertEqual(h.connections[0].lines[0], "TIME,2026,11,10,18,0\r\n")
        self.assertEqual(h.connections[0].flush_calls, 0)

    def test_02_time_is_sent_every_minute(self):
        for until, expected in ((60, 1), (61, 2), (121, 3)):
            h = Harness(until=until)
            h.run()
            self.assertEqual(h.connections[0].commands().count("TIME"), expected, until)
            self.assertEqual(h.connections[0].commands().count("AREA"), expected, until)

    def test_03_forecast_refreshes_every_30_minutes(self):
        h = Harness(until=1801)
        h.run()
        self.assertEqual(h.fetch_calls, 2)
        commands = h.connections[0].commands()
        self.assertEqual(commands.count("TODAY"), 2)
        self.assertEqual(commands.count("TOMORROW"), 2)
        self.assertEqual(commands[-2:], ["TODAY", "TOMORROW"])

    def test_04_fetch_failure_keeps_running_and_retries_after_60_seconds(self):
        h = Harness(until=1861)
        h.clock.at(1799, lambda: setattr(h, "fetch_error", URLError("offline")))
        h.clock.at(1830, lambda: setattr(h, "fetch_error", None))
        h.run()
        # Fetches at 0, 1800 (fails), 1860 (retry succeeds).
        self.assertEqual(h.fetch_calls, 3)
        self.assertEqual(len(h.connections), 1)
        self.assertEqual(h.connections[0].commands().count("TODAY"), 2)

    def test_04b_initial_fetch_failure_does_not_crash(self):
        h = Harness(until=70)
        h.fetch_error = URLError("offline")
        h.run()
        self.assertEqual(h.connections[0].commands(), ["TIME", "AREA", "TIME", "AREA"])
        self.assertEqual(h.fetch_calls, 2)

    def test_05_write_failure_closes_port_and_enters_reconnect_mode(self):
        h = Harness(until=80)
        h.clock.at(10, h.unplug)
        h.run()
        self.assertTrue(h.connections[0].closed)
        # One failed attempt every RECONNECT_INTERVAL_S while the port is gone.
        self.assertGreaterEqual(h.open_attempts, 3)
        self.assertEqual(len(h.connections), 1)

    def test_06_reconnect_resends_time_today_tomorrow(self):
        h = Harness(until=100)
        h.clock.at(10, h.unplug)
        h.clock.at(70, h.plug)
        h.run()
        self.assertEqual(len(h.connections), 2)
        self.assertEqual(h.connections[1].commands()[:4], ["TIME", "AREA", "TODAY", "TOMORROW"])

    def test_07_reconnect_resends_cached_forecast_without_contacting_jma(self):
        h = Harness(until=100)
        h.clock.at(10, h.unplug)
        h.clock.at(70, h.plug)
        h.run()
        self.assertEqual(h.fetch_calls, 1)
        self.assertEqual(h.connections[1].lines[1:4], ["AREA,Example Station\r\n", h.TODAY, h.TOMORROW])

    def test_09_forecast_survives_reconnect_even_if_jma_is_down(self):
        h = Harness(until=1900)
        h.clock.at(1790, h.unplug)
        h.clock.at(1799, lambda: setattr(h, "fetch_error", URLError("offline")))
        h.clock.at(1850, h.plug)
        h.run()
        self.assertEqual(len(h.connections), 2)
        self.assertEqual(h.connections[1].lines[1:4], ["AREA,Example Station\r\n", h.TODAY, h.TOMORROW])

    def test_area_lookup_failure_does_not_block_forecasts_and_retries(self):
        h = Harness(until=62)
        fetch_label = mock.Mock(side_effect=[URLError("offline"), "Example Station"])
        h.service.fetch_area_label = fetch_label
        h.run()
        self.assertEqual(fetch_label.call_count, 2)
        fetch_label.assert_called_with("12345")
        self.assertEqual(h.connections[0].commands(), ["TIME", "TODAY", "TOMORROW", "TIME", "AREA"])


class CdcLineFormatTests(unittest.TestCase):
    def test_08_today_and_tomorrow_lines(self):
        self.assertEqual(
            weather_service.DailyForecast("晴れ", "時々", "曇り", 22, 14, 10).to_cdc_line("TODAY"),
            "TODAY,晴れ,時々,曇り,22,14,10\r\n",
        )
        self.assertEqual(
            weather_service.DailyForecast("曇り", "", "", 19, 12, 40).to_cdc_line("TOMORROW"),
            "TOMORROW,曇り,,,19,12,40\r\n",
        )


if __name__ == "__main__":
    unittest.main()
