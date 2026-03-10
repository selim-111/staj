"""
Simple GUI application for monitoring PLC variables in real time.

This program creates a Tkinter window showing the latest values read from the
PLC. It uses a background thread to poll at high speed, but the GUI updates at
around 10 Hz to stay responsive.

Usage:
    python ads_gui.py

Requirements:
    pip install pyads

The window displays each variable name and its most recent value. A Start/Stop
button controls acquisition. Data may also be optionally saved to a CSV file.
"""

import threading
import time
import csv
import os
import sys
import pyads
import tkinter as tk
from tkinter import ttk
from datetime import datetime
import queue

# PLC connection settings (same as before)
AMS_NET_ID = "5.108.104.207.1.1"
PLC_IP = "10.141.5.13"
ADS_PORT = 851
USERNAME = "Administrator"
PASSWORD = "1"

VARIABLES = [
    
    ("MAIN.box3_in[1]",     pyads.PLCTYPE_INT,  "box3_in_1"),
    ("MAIN.box3_in[2]",     pyads.PLCTYPE_INT,  "box3_in_2"),
    ("MAIN.box3_in[3]",     pyads.PLCTYPE_INT,  "box3_in_3"),
    ("MAIN.box3_in[4]",     pyads.PLCTYPE_INT,  "box3_in_4"),
    ("MAIN.box3_in[5]",     pyads.PLCTYPE_INT,  "box3_in_5"),
    ("MAIN.box3_in[6]",     pyads.PLCTYPE_INT,  "box3_in_6"),
    ("MAIN.box4_acc_in[1]", pyads.PLCTYPE_DINT, "box4_acc_in_1"),
    ("MAIN.box4_acc_in[2]", pyads.PLCTYPE_DINT, "box4_acc_in_2"),
    ("MAIN.box4_acc_in[3]", pyads.PLCTYPE_DINT, "box4_acc_in_3"),
    ("MAIN.box4_temp_in",   pyads.PLCTYPE_DINT, "box4_temp_in"),
    ("MAIN.box5_in[1]",     pyads.PLCTYPE_INT,  "box5_in_1"),
    ("MAIN.box5_in[2]",     pyads.PLCTYPE_INT,  "box5_in_2"),
    ("MAIN.box5_in[3]",     pyads.PLCTYPE_INT,  "box5_in_3"),
    ("MAIN.box5_in[4]",     pyads.PLCTYPE_INT,  "box5_in_4"),
    ("MAIN.box5_in[5]",     pyads.PLCTYPE_INT,  "box5_in_5"),
    ("MAIN.box5_in[6]",     pyads.PLCTYPE_INT,  "box5_in_6"),
]

TARGET_HZ = 1000.0  # 1 ms sampling target.
SAMPLE_INTERVAL = 1.0 / TARGET_HZ
UI_REFRESH_MS = 100  # Low-FPS UI; acquisition/saving runs independently.
MAX_UI_QUEUE_DRAIN_PER_REFRESH = 300
TABLE_MAX_ROWS = 20
LOG_ALL_SYMBOLS = False
ALL_SYMBOLS_PREFIX = "MAIN."
ALL_SYMBOLS_READ_INTERVAL = 0.2  # seconds

PRIMITIVE_SYMBOL_TYPES = {
    "BOOL",
    "BYTE",
    "USINT",
    "SINT",
    "UINT",
    "INT",
    "UDINT",
    "DINT",
    "ULINT",
    "LINT",
    "REAL",
    "LREAL",
    "TIME",
    "DATE",
    "TIME_OF_DAY",
    "TOD",
    "DATE_AND_TIME",
    "DT",
}


def get_app_base_dir():
    # When packaged (e.g., PyInstaller), write next to the executable.
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    # When running as a script, write next to this script file.
    return os.path.dirname(os.path.abspath(__file__))

class PLCReader(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.running = False
        self.data_queue = queue.Queue(maxsize=200)
        self.plc = None
        self.sample_interval = SAMPLE_INTERVAL
        self.symbols = [symbol for symbol, _, _ in VARIABLES]
        self.symbol_to_name = {symbol: name for symbol, _, name in VARIABLES}
        self.active_symbols = list(self.symbols)
        self.bulk_read_enabled = True
        self.active_variables = list(VARIABLES)
        self.invalid_symbols = []
        self.extra_log_symbols = []
        self.extra_log_values = {}
        self._next_extra_read = 0.0
        self.log_queue = None
        self.log_columns = []

        # Runtime acquisition stats
        self.samples_total = 0
        self.samples_dropped = 0
        self.read_errors = 0
        self.last_rate_hz = 0.0
        self.last_cycle_ms = 0.0
        self._rate_window_count = 0
        self._rate_window_start = time.perf_counter()
        self.overrun_total = 0

    @staticmethod
    def _is_simple_symbol_type(symbol_type):
        stype = (symbol_type or "").strip().upper()
        if stype in PRIMITIVE_SYMBOL_TYPES:
            return True
        if stype.startswith("STRING") or stype.startswith("WSTRING"):
            return True
        return False

    def _discover_extra_symbols(self):
        self.extra_log_symbols = []
        self.extra_log_values = {}
        if not LOG_ALL_SYMBOLS:
            return

        known_symbols = set(self.symbols)
        try:
            all_symbols = self.plc.get_all_symbols()
        except Exception:
            return

        for sym in all_symbols:
            name = getattr(sym, "name", "")
            symbol_type = getattr(sym, "symbol_type", "")

            if not name:
                continue
            if ALL_SYMBOLS_PREFIX and not name.startswith(ALL_SYMBOLS_PREFIX):
                continue
            if name in known_symbols:
                continue
            if not self._is_simple_symbol_type(symbol_type):
                continue

            try:
                self.plc.read_by_name(name)
                self.extra_log_symbols.append(name)
                self.extra_log_values[name] = None
            except Exception:
                continue

    def set_logging_target(self, log_queue, log_columns):
        self.log_queue = log_queue
        self.log_columns = list(log_columns)

    def clear_logging_target(self):
        self.log_queue = None
        self.log_columns = []

    def start_plc(self):
        try:
            pyads.add_route_to_plc(
                sending_net_id=AMS_NET_ID,
                ip_address=PLC_IP,
                username=USERNAME,
                password=PASSWORD,
            )
        except Exception:
            pass
        self.plc = pyads.Connection(AMS_NET_ID, ADS_PORT, PLC_IP)
        self.plc.open()

        # Validate symbols once to avoid retrying invalid tags each cycle.
        self.invalid_symbols = []
        self.active_variables = []
        self.active_symbols = []
        for symbol, plc_type, name in VARIABLES:
            try:
                self.plc.read_by_name(symbol, plc_type)
                self.active_variables.append((symbol, plc_type, name))
                self.active_symbols.append(symbol)
            except Exception:
                self.invalid_symbols.append(symbol)

        # Bulk read stays enabled as long as there is at least one valid symbol.
        self.bulk_read_enabled = len(self.active_symbols) > 0
        self._discover_extra_symbols()
        self._next_extra_read = time.perf_counter()

        self.running = True
        if not self.is_alive():
            self.start()

    def stop_plc(self):
        self.running = False
        if self.plc:
            self.plc.close()
            self.plc = None

    def run(self):
        next_run = time.perf_counter()
        while True:
            if self.running and self.plc:
                now = time.perf_counter()
                remaining = next_run - now
                if remaining > 0:
                    # Coarse sleep first, then spin very briefly for tighter 1 ms timing.
                    if remaining > 0.0002:
                        time.sleep(remaining - 0.0002)
                    while True:
                        now = time.perf_counter()
                        if now >= next_run:
                            break

                read_start = time.perf_counter()
                values = {}
                if self.bulk_read_enabled:
                    try:
                        raw_values = self.plc.read_list_by_name(self.active_symbols, cache_symbol_info=True)
                        for symbol in self.active_symbols:
                            values[self.symbol_to_name[symbol]] = raw_values.get(symbol)
                    except Exception:
                        self.bulk_read_enabled = False

                if not values:
                    for symbol, plc_type, name in self.active_variables:
                        try:
                            values[name] = self.plc.read_by_name(symbol, plc_type)
                        except Exception:
                            values[name] = None
                            self.read_errors += 1

                for _, _, name in VARIABLES:
                    values.setdefault(name, None)

                now = time.perf_counter()
                if self.extra_log_symbols and now >= self._next_extra_read:
                    extra_values = {}
                    try:
                        extra_values = self.plc.read_list_by_name(
                            self.extra_log_symbols,
                            cache_symbol_info=True,
                        )
                    except Exception:
                        for symbol_name in self.extra_log_symbols:
                            try:
                                extra_values[symbol_name] = self.plc.read_by_name(symbol_name)
                            except Exception:
                                extra_values[symbol_name] = None
                                self.read_errors += 1

                    for symbol_name in self.extra_log_symbols:
                        self.extra_log_values[symbol_name] = extra_values.get(symbol_name)
                    self._next_extra_read = now + ALL_SYMBOLS_READ_INTERVAL

                if self.extra_log_values:
                    values.update(self.extra_log_values)

                if self.log_queue is not None and self.log_columns:
                    # Block on queue when needed to avoid dropping any acquired sample.
                    log_row = [datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]]
                    log_row += [values.get(col, "") for col in self.log_columns]
                    self.log_queue.put(log_row)

                self.last_cycle_ms = (time.perf_counter() - read_start) * 1000.0

                try:
                    self.data_queue.put(values, block=False)
                except queue.Full:
                    # Keep latest sample in queue by discarding oldest.
                    try:
                        self.data_queue.get_nowait()
                    except queue.Empty:
                        pass
                    try:
                        self.data_queue.put(values, block=False)
                    except queue.Full:
                        self.samples_dropped += 1

                now = time.perf_counter()
                self.samples_total += 1
                self._rate_window_count += 1
                elapsed = now - self._rate_window_start
                if elapsed >= 1.0:
                    self.last_rate_hz = self._rate_window_count / elapsed
                    self._rate_window_start = now
                    self._rate_window_count = 0

                next_run += self.sample_interval
                if next_run < now:
                    self.overrun_total += 1
                    # Rebase schedule when the loop fell behind target period.
                    next_run = now
                    time.sleep(0)
            else:
                next_run = time.perf_counter()
                time.sleep(0.1)


class CSVLogger(threading.Thread):
    def __init__(self, log_columns):
        super().__init__(daemon=True)
        self.running = True
        self.queue = queue.Queue(maxsize=300000)
        filename = f"plc_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        self.file_path = os.path.join(get_app_base_dir(), filename)
        self.file = open(self.file_path, "w", newline="", buffering=1024 * 1024)
        self.writer = csv.writer(self.file)
        self.writer.writerow(["timestamp"] + list(log_columns))
        self.file.flush()
        self.rows_written = 0
        self._flush_interval_rows = 500
        self._flush_interval_seconds = 0.5
        self._last_flush = time.perf_counter()
        self.start()

    def run(self):
        while self.running or not self.queue.empty():
            try:
                row = self.queue.get(timeout=0.2)
            except queue.Empty:
                continue
            batch = [row]
            for _ in range(999):
                try:
                    batch.append(self.queue.get_nowait())
                except queue.Empty:
                    break

            self.writer.writerows(batch)
            self.rows_written += len(batch)

            now = time.perf_counter()
            if (
                self.rows_written % self._flush_interval_rows == 0
                or now - self._last_flush >= self._flush_interval_seconds
            ):
                self.file.flush()
                self._last_flush = now

    def stop(self):
        self.running = False
        self.join(timeout=5.0)
        self.file.flush()
        self.file.close()

class App(tk.Tk):
    def __init__(self, reader):
        super().__init__()
        self.title("PLC Viewer")
        self.reader = reader
        self.labels = {}
        self.logging = tk.BooleanVar(value=False)
        self.logfile = None
        self.logger = None
        self.current_values = {v[2]: None for v in VARIABLES}  # local cache of latest values
        self.display_rate_hz = 0.0
        self._display_window_count = 0
        self._display_window_start = time.perf_counter()
        self.run_start_time = None
        self.elapsed_before_stop = 0.0
        # state variables for logging and runtime

        start_btn = tk.Button(
            self,
            text="Start",
            command=self.start,
            bg="#800020",
            fg="white",
            activebackground="#660018",
            activeforeground="white",
        )
        start_btn.grid(row=0, column=0)

        stop_btn = tk.Button(
            self,
            text="Stop",
            command=self.stop,
            bg="#0057B7",
            fg="white",
            activebackground="#004A9A",
            activeforeground="white",
        )
        stop_btn.grid(row=0, column=1)
        tk.Checkbutton(self, text="Save CSV", variable=self.logging).grid(row=0, column=2)
        tk.Button(self, text="Reset Tablo", command=self.reset_table).grid(row=0, column=3)
        self.runtime_label = tk.Label(self, text="Calisma suresi: 00:00:00", anchor="w")
        self.runtime_label.grid(row=0, column=4, padx=(8, 0), sticky="w")
        self.is_running = False

        # variable labels on left (use dedicated frame to avoid uneven gaps)
        leftf = tk.Frame(self)
        leftf.grid(row=1, column=0, rowspan=len(VARIABLES)+1, sticky="nw")
        for idx, (_, _, name) in enumerate(VARIABLES, start=1):
            tk.Label(
                leftf,
                text=name,
                wraplength=95,
                justify="left",
                width=14,
                anchor="w",
            ).grid(row=idx, column=0, sticky="w", padx=2, pady=1)
            # Fixed width prevents geometry jitter when value length changes.
            lbl = tk.Label(leftf, text="---", width=12, anchor="e", font=("Consolas", 9))
            lbl.grid(row=idx, column=1, sticky="e", padx=2, pady=1)
            self.labels[name] = lbl

        # Notebook sekmeleri: yalnizca tablo
        notebook = ttk.Notebook(self)
        notebook.grid(row=1, column=2, columnspan=2, rowspan=len(VARIABLES)+2, sticky="nsew")
        self.grid_columnconfigure(2, weight=1)
        self.grid_columnconfigure(3, weight=1)
        self.grid_rowconfigure(1, weight=1)

        # tablo sekmesi
        tab1 = tk.Frame(notebook)
        notebook.add(tab1, text="Tablo")
        tk.Label(tab1, text="Recent samples:").pack(anchor="w")
        tableframe = tk.Frame(tab1)
        tableframe.pack(fill="both", expand=True)
        cols = ["time"] + [v[2] for v in VARIABLES]
        
        # Veriler için scrollable frame
        scroll_frame = tk.Frame(tableframe)
        scroll_frame.pack(fill="both", expand=True)

        # Başlık ve veriyi aynı genişlikte tutacak sol alan
        table_left = tk.Frame(scroll_frame)
        table_left.pack(side="left", fill="both", expand=True)

        # Grid tabanlı tablo - başlık ve veriler aynı alignment
        header_frame = tk.Frame(table_left, bg="lightgray")
        header_frame.pack(fill="x")

        # BAŞLIK SATIRI
        for i, c in enumerate(cols):
            header_frame.grid_columnconfigure(i, weight=1, uniform="tablecol")
            lbl = tk.Label(
                header_frame,
                text=c,
                bg="lightgray",
                font=("Arial", 8, "bold"),
                anchor="center",
                justify="center",
                wraplength=58,
                height=2,
            )
            lbl.grid(row=0, column=i, sticky="nsew", padx=0, pady=0)

        self.canvas = tk.Canvas(table_left, highlightthickness=0)
        self.canvas.pack(side="left", fill="both", expand=True)
        
        vsb = ttk.Scrollbar(scroll_frame, orient="vertical", command=self.canvas.yview)
        vsb.pack(side="right", fill="y")
        self.canvas.configure(yscrollcommand=vsb.set)
        
        # Canvas içine frame (veriler)
        self.data_frame = tk.Frame(self.canvas, bg="white")
        self.data_window = self.canvas.create_window((0, 0), window=self.data_frame, anchor="nw")
        
        def on_configure(event):
            self.canvas.itemconfig(self.data_window, width=event.width)
            self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        self.canvas.bind("<Configure>", on_configure)
        
        # Grid layout configuration for data
        for i in range(len(cols)):
            self.data_frame.grid_columnconfigure(i, weight=1, uniform="tablecol")
        
        # Data rows tracker (ring buffer, single-row updates)
        self.table_write_index = 0
        self.table_filled_rows = 0
        self.table_cells = []
        for row_idx in range(TABLE_MAX_ROWS):
            row_cells = []
            for col_idx in range(len(cols)):
                cell = tk.Label(self.data_frame, text="", anchor="center", bg="white", font=("Arial", 8))
                cell.grid(row=row_idx, column=col_idx, sticky="nsew", padx=0, pady=0)
                row_cells.append(cell)
            self.table_cells.append(row_cells)
        self.cols = cols

        self.hz_label = tk.Label(
            tab1,
            text="Acq: 0.0 Hz | Display: 0.0 Hz | log_q: 0 | bulk: on",
            anchor="w",
        )
        self.hz_label.pack(fill="x", pady=(4, 0))

        self.after(UI_REFRESH_MS, self.refresh)

    @staticmethod
    def _format_duration(seconds):
        total = int(max(seconds, 0))
        hours = total // 3600
        minutes = (total % 3600) // 60
        secs = total % 60
        return f"{hours:02}:{minutes:02}:{secs:02}"

    def reset_table(self):
        self.table_write_index = 0
        self.table_filled_rows = 0
        for row_cells in self.table_cells:
            for cell in row_cells:
                cell.config(text="")

    def start(self):
        # Start with an empty queue so display rate follows fresh samples.
        while not self.reader.data_queue.empty():
            try:
                self.reader.data_queue.get_nowait()
            except queue.Empty:
                break

        self._display_window_count = 0
        self._display_window_start = time.perf_counter()
        self.display_rate_hz = 0.0
        self.run_start_time = time.perf_counter()
        self.elapsed_before_stop = 0.0

        self.reader.start_plc()
        self.is_running = True
        # open logfile if logging
        if self.logging.get() and self.logger is None:
            log_columns = [v[2] for v in VARIABLES] + list(self.reader.extra_log_symbols)
            self.logger = CSVLogger(log_columns)
            self.logfile = self.logger.file
            self.reader.set_logging_target(self.logger.queue, log_columns)

    def stop(self):
        self.reader.stop_plc()
        self.is_running = False
        if self.run_start_time is not None:
            self.elapsed_before_stop = time.perf_counter() - self.run_start_time
            self.run_start_time = None
        self.reader.clear_logging_target()
        if self.logger:
            self.logger.stop()
            self.logger = None
        self.logfile = None

    def refresh(self):
        values = None
        drained = 0
        if self.is_running:
            # Limit queue drain work per UI tick so Tkinter stays responsive.
            while drained < MAX_UI_QUEUE_DRAIN_PER_REFRESH:
                try:
                    values = self.reader.data_queue.get_nowait()
                    drained += 1
                except queue.Empty:
                    break

            # If producer is faster than consumer, keep only the newest sample.
            if drained == MAX_UI_QUEUE_DRAIN_PER_REFRESH:
                while True:
                    try:
                        values = self.reader.data_queue.get_nowait()
                    except queue.Empty:
                        break
            if values is not None:
                self.current_values.update(values)
                self._display_window_count += 1

        now = time.perf_counter()
        elapsed_run = self.elapsed_before_stop
        if self.run_start_time is not None:
            elapsed_run += now - self.run_start_time
        self.runtime_label.config(text=f"Calisma suresi: {self._format_duration(elapsed_run)}")

        elapsed = now - self._display_window_start
        if elapsed >= 1.0:
            self.display_rate_hz = self._display_window_count / elapsed
            self._display_window_start = now
            self._display_window_count = 0

        cycle_ms = max(self.reader.last_cycle_ms, 0.001)
        target_text = f"{TARGET_HZ:.0f}"
        log_q_size = self.logger.queue.qsize() if self.logger else 0

        self.hz_label.config(
            text=(
                f"Acq: {self.reader.last_rate_hz:5.1f} Hz | "
                f"Display: {self.display_rate_hz:5.1f} Hz | "
                f"target {target_text} | cycle: {self.reader.last_cycle_ms:5.1f} ms | "
                f"drained: {drained} | "
                f"overrun: {self.reader.overrun_total} | "
                f"log_q: {log_q_size} | "
                f"dropped: {self.reader.samples_dropped} | "
                f"errors: {self.reader.read_errors} | invalid: {len(self.reader.invalid_symbols)} | "
                f"bulk: {'on' if self.reader.bulk_read_enabled else 'off'}"
            )
        )
        
        if self.is_running and values is not None:
            # Update left panel labels
            for name, lbl in self.labels.items():
                val = self.current_values.get(name)
                lbl.config(text=str(val))
            
            # CSV logging
            # Add table row
            ts = datetime.now()
            row = [ts.strftime("%H:%M:%S.%f")[:-3]]
            vals = [self.current_values.get(v[2], "") for v in VARIABLES]
            row += vals

            target_cells = self.table_cells[self.table_write_index]
            for col_idx, cell in enumerate(target_cells):
                cell.config(text=str(row[col_idx]))

            self.table_write_index = (self.table_write_index + 1) % TABLE_MAX_ROWS
            if self.table_filled_rows < TABLE_MAX_ROWS:
                self.table_filled_rows += 1
            

        # Canvas scroll
        # Scroll region is already updated on canvas configure; avoid per-frame relayout work.
        if self.table_filled_rows:
            self.canvas.yview_moveto(1.0)
        
        self.after(UI_REFRESH_MS, self.refresh)



if __name__ == "__main__":

    reader = PLCReader()
    app = App(reader)
    app.mainloop()
