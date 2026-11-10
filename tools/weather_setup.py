"""Window for selecting and saving a JMA forecast area."""

from __future__ import annotations

import queue
import threading
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk
from typing import Any, Callable

if __package__:
    from . import weather_service as service
else:
    import weather_service as service


class AreaSetupWindow:
    def __init__(self, root: tk.Tk, path: Path) -> None:
        self.root = root
        self.path = path
        self.results: queue.Queue[tuple[str, Any, Exception | None]] = queue.Queue()
        self.office_codes: list[str] = []
        self.weather_codes: list[str] = []
        self.temperature_codes: list[str] = []
        self.request_id = 0
        self.pending_id = 0
        self.saved = False

        root.title("天気予報地域の設定")
        root.resizable(False, False)
        frame = ttk.Frame(root, padding=16)
        frame.grid(sticky="nsew")
        ttk.Label(frame, text="気象台、天気の地域、気温の地点を選択してください。").grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 10))
        ttk.Label(frame, text="予報を担当する気象台").grid(row=1, column=0, sticky="w", padx=(0, 12))
        self.office = ttk.Combobox(frame, width=35, state="disabled")
        self.office.grid(row=1, column=1, sticky="ew", pady=4)
        self.office.bind("<<ComboboxSelected>>", self.on_office_selected)
        ttk.Label(frame, text="天気・降水確率の地域").grid(row=2, column=0, sticky="w")
        self.weather = ttk.Combobox(frame, width=35, state="disabled")
        self.weather.grid(row=2, column=1, sticky="ew", pady=4)
        self.weather.bind("<<ComboboxSelected>>", self.on_selection_changed)
        ttk.Label(frame, text="気温の地点").grid(row=3, column=0, sticky="w")
        self.temperature = ttk.Combobox(frame, width=35, state="disabled")
        self.temperature.grid(row=3, column=1, sticky="ew", pady=4)
        self.temperature.bind("<<ComboboxSelected>>", self.on_selection_changed)
        ttk.Label(frame, text="気温の地点は選択した天気の地域に対応するものを選んでください。").grid(
            row=4, column=0, columnspan=2, sticky="w", pady=(6, 0))
        self.status = tk.StringVar(value="気象庁から地域一覧を取得しています...")
        ttk.Label(frame, textvariable=self.status).grid(row=5, column=0, columnspan=2, sticky="w", pady=10)
        self.retry = ttk.Button(frame, text="一覧を再取得", command=self.load_offices, state="disabled")
        self.retry.grid(row=6, column=0, sticky="w")
        self.save = ttk.Button(frame, text="保存", command=self.save_selection, state="disabled")
        self.save.grid(row=6, column=1, sticky="e")
        root.after(100, self.poll_results)
        self.load_offices()

    def start_fetch(self, kind: str, func: Callable[[], Any]) -> None:
        self.request_id += 1
        self.pending_id = self.request_id
        request_id = self.request_id

        def worker() -> None:
            try:
                self.results.put((f"{kind}:{request_id}", func(), None))
            except (OSError, ValueError, KeyError, TypeError) as error:
                self.results.put((f"{kind}:{request_id}", None, error))

        threading.Thread(target=worker, daemon=True).start()

    @staticmethod
    def set_choices(combo: ttk.Combobox, choices: dict[str, str]) -> list[str]:
        entries = sorted(choices.items())
        combo["values"] = [f"{name} ({code})" for code, name in entries]
        combo.set("")
        combo["state"] = "readonly" if entries else "disabled"
        return [code for code, _ in entries]

    def load_offices(self) -> None:
        self.office_codes = self.set_choices(self.office, {})
        self.weather_codes = self.set_choices(self.weather, {})
        self.temperature_codes = self.set_choices(self.temperature, {})
        self.save["state"] = "disabled"
        self.retry["state"] = "disabled"
        self.status.set("気象庁から地域一覧を取得しています...")
        self.start_fetch("offices", service.forecast_offices)

    def on_office_selected(self, _event: tk.Event) -> None:
        office = self.office_codes[self.office.current()]
        self.weather_codes = self.set_choices(self.weather, {})
        self.temperature_codes = self.set_choices(self.temperature, {})
        self.save["state"] = "disabled"
        self.retry["state"] = "disabled"
        self.status.set("予報地域と気温の地点を取得しています...")
        self.start_fetch("areas", lambda: service.forecast_area_choices(service.fetch_forecast(office)))

    def on_selection_changed(self, _event: tk.Event) -> None:
        self.save["state"] = "normal" if self.weather.current() >= 0 and self.temperature.current() >= 0 else "disabled"

    def poll_results(self) -> None:
        try:
            while True:
                key, value, error = self.results.get_nowait()
                kind, request_id = key.split(":")
                if int(request_id) != self.pending_id:
                    continue
                if error is not None:
                    self.status.set(f"取得に失敗しました: {error}")
                    self.retry["state"] = "normal"
                elif kind == "offices":
                    self.office_codes = self.set_choices(self.office, value)
                    self.status.set("予報を担当する気象台を選択してください。")
                else:
                    weather, temperature = value
                    self.weather_codes = self.set_choices(self.weather, weather)
                    self.temperature_codes = self.set_choices(self.temperature, temperature)
                    self.status.set("天気の地域と気温の地点を選択してください。")
        except queue.Empty:
            pass
        self.root.after(100, self.poll_results)

    def save_selection(self) -> None:
        try:
            area = service.ForecastArea(
                self.office_codes[self.office.current()],
                self.weather_codes[self.weather.current()],
                self.temperature_codes[self.temperature.current()],
            )
            service.save_area(self.path, area)
        except (OSError, ValueError) as error:
            messagebox.showerror("保存エラー", f"設定を保存できませんでした: {error}", parent=self.root)
            return
        self.saved = True
        messagebox.showinfo("設定完了", "予報地域を保存しました。サービスを起動または再起動してください。", parent=self.root)
        self.root.destroy()


def setup_area(path: Path) -> None:
    try:
        root = tk.Tk()
    except tk.TclError as error:
        raise RuntimeError(f"Cannot open the area selection window: {error}") from error
    window = AreaSetupWindow(root, path)
    root.mainloop()
    if not window.saved:
        print("Forecast area setup cancelled.")
