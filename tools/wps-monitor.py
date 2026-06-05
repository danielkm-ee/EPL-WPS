#!/usr/bin/env python3
"""
wps-monitor.py — EPL EDM Wire Power Supply serial monitor.

Requires: pyserial  (pip install pyserial)
"""

import glob
import queue
import threading
import tkinter as tk
from tkinter import ttk, messagebox
import serial


# ---------------------------------------------------------------------------
# SerialWorker — owns the serial.Serial object; runs on its own thread
# ---------------------------------------------------------------------------

class SerialWorker(threading.Thread):
    def __init__(self, port: str, baud: int, rx_queue: "queue.Queue[str]"):
        super().__init__(daemon=True)
        self._port     = port
        self._baud     = baud
        self._queue    = rx_queue
        self._lock     = threading.Lock()
        self._stop_evt = threading.Event()
        self._serial: serial.Serial | None = None

    def run(self):
        try:
            self._serial = serial.Serial(self._port, self._baud, timeout=0.1)
        except serial.SerialException as exc:
            self._queue.put(f"__ERROR__ {exc}")
            return

        self._queue.put("__CONNECTED__")

        while not self._stop_evt.is_set():
            try:
                raw = self._serial.readline()
            except serial.SerialException as exc:
                self._queue.put(f"__ERROR__ {exc}")
                break
            if raw:
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                if line:
                    self._queue.put(line)

        self._serial.close()
        self._queue.put("__DISCONNECTED__")

    def send(self, text: str):
        """Send a newline-terminated command. Thread-safe."""
        with self._lock:
            if self._serial and self._serial.is_open:
                self._serial.write((text + "\n").encode("utf-8"))

    def stop(self):
        self._stop_evt.set()


# ---------------------------------------------------------------------------
# App — main window
# ---------------------------------------------------------------------------

LOG_MAX_LINES = 500
POLL_INTERVAL_MS = 100


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("EPL WPS Monitor")
        self.resizable(True, True)

        self._worker: SerialWorker | None = None
        self._rx_queue: queue.Queue[str] = queue.Queue()
        self._live_telemetry = False

        # StringVars for telemetry readouts
        self._sv: dict[str, tk.StringVar] = {k: tk.StringVar(value="—") for k in (
            "STATE", "FAULT",
            "INPUT CURRENT", "INPUT POWER",
            "DISCHARGE CURRENT", "DISCHARGE VOLTAGE", "OUTPUT POWER",
            "SUCCESS RATE", "DISCHARGES", "EDGE",
        )}

        self._build_ui()
        self._set_connected(False)
        self._poll_queue()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self):
        root = ttk.Frame(self, padding=6)
        root.grid(sticky="nsew")
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        root.columnconfigure(0, weight=1)

        row = 0

        # 1. Connection bar
        conn = ttk.LabelFrame(root, text="Connection", padding=4)
        conn.grid(row=row, column=0, sticky="ew", pady=(0, 4))
        conn.columnconfigure(1, weight=1)

        ttk.Label(conn, text="Port:").grid(row=0, column=0, sticky="w")
        self._port_var = tk.StringVar()
        self._port_cb  = ttk.Combobox(conn, textvariable=self._port_var, width=18)
        self._port_cb.grid(row=0, column=1, sticky="ew", padx=4)
        ttk.Button(conn, text="↺", width=2,
                   command=self._refresh_ports).grid(row=0, column=2)

        ttk.Label(conn, text="Baud:").grid(row=0, column=3, padx=(8, 2))
        self._baud_var = tk.StringVar(value="115200")
        ttk.Entry(conn, textvariable=self._baud_var, width=8).grid(row=0, column=4)

        self._conn_btn = ttk.Button(conn, text="Connect", command=self._on_connect_toggle)
        self._conn_btn.grid(row=0, column=5, padx=(8, 0))

        self._refresh_ports()
        row += 1

        # 2. Telemetry panel
        tel = ttk.LabelFrame(root, text="Telemetry", padding=4)
        tel.grid(row=row, column=0, sticky="ew", pady=(0, 4))
        for c in range(4):
            tel.columnconfigure(c, weight=1)

        fields_left  = [("STATE", "STATE"), ("FAULT", "FAULT"),
                        ("Input Current", "INPUT CURRENT"), ("Input Power", "INPUT POWER")]
        fields_right = [("Discharge Current", "DISCHARGE CURRENT"),
                        ("Discharge Voltage", "DISCHARGE VOLTAGE"),
                        ("Output Power", "OUTPUT POWER"),
                        ("Success Rate", "SUCCESS RATE"),
                        ("Discharges", "DISCHARGES"), ("Edge", "EDGE")]

        for i, (label, key) in enumerate(fields_left):
            ttk.Label(tel, text=label + ":").grid(row=i, column=0, sticky="w")
            lbl = ttk.Label(tel, textvariable=self._sv[key], width=18, anchor="w")
            lbl.grid(row=i, column=1, sticky="w", padx=(2, 12))
            if key == "FAULT":
                self._fault_label = lbl

        for i, (label, key) in enumerate(fields_right):
            ttk.Label(tel, text=label + ":").grid(row=i, column=2, sticky="w")
            ttk.Label(tel, textvariable=self._sv[key], width=18, anchor="w").grid(
                row=i, column=3, sticky="w", padx=2)

        btn_row = max(len(fields_left), len(fields_right))
        self._live_btn = ttk.Button(tel, text="● Live", command=self._on_live_toggle)
        self._live_btn.grid(row=btn_row, column=0, columnspan=2, pady=(6, 0), sticky="w")
        ttk.Button(tel, text="One-shot", command=self._on_oneshot).grid(
            row=btn_row, column=2, columnspan=2, pady=(6, 0), sticky="w")
        row += 1

        # 3. DPOT panel
        dpot = ttk.LabelFrame(root, text="DPOT Voltage", padding=4)
        dpot.grid(row=row, column=0, sticky="ew", pady=(0, 4))

        ttk.Label(dpot, text="Target V (64–200):").grid(row=0, column=0, sticky="w")
        self._vtable_var = tk.StringVar(value="80")
        ttk.Spinbox(dpot, from_=64, to=200, textvariable=self._vtable_var,
                    width=6).grid(row=0, column=1, padx=4)
        self._set_v_btn = ttk.Button(dpot, text="Set Voltage",
                                     command=self._on_set_voltage)
        self._set_v_btn.grid(row=0, column=2)

        ttk.Label(dpot, text="Raw wiper (0–110):").grid(row=1, column=0, sticky="w",
                                                         pady=(4, 0))
        self._wiper_var = tk.StringVar(value="0")
        ttk.Spinbox(dpot, from_=0, to=110, textvariable=self._wiper_var,
                    width=6).grid(row=1, column=1, padx=4, pady=(4, 0))
        self._set_raw_btn = ttk.Button(dpot, text="Set Raw",
                                       command=self._on_set_raw)
        self._set_raw_btn.grid(row=1, column=2, pady=(4, 0))
        row += 1

        # 4. Control bar
        ctrl = ttk.Frame(root)
        ctrl.grid(row=row, column=0, sticky="ew", pady=(0, 4))
        self._reset_btn = tk.Button(ctrl, text="Reset Device", bg="#c0392b",
                                    fg="white", activebackground="#922b21",
                                    activeforeground="white",
                                    command=self._on_reset)
        self._reset_btn.pack(side="left")
        row += 1

        # 5. Log pane
        log_frame = ttk.LabelFrame(root, text="Serial Log", padding=4)
        log_frame.grid(row=row, column=0, sticky="nsew", pady=(0, 0))
        root.rowconfigure(row, weight=1)
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)

        self._log = tk.Text(log_frame, height=12, state="disabled",
                            font=("Monospace", 9), wrap="none")
        self._log.grid(row=0, column=0, sticky="nsew")
        sb = ttk.Scrollbar(log_frame, command=self._log.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self._log["yscrollcommand"] = sb.set

        self._log.tag_configure("send",  foreground="#2980b9")
        self._log.tag_configure("ok",    foreground="#27ae60")
        self._log.tag_configure("error", foreground="#c0392b")

    # ------------------------------------------------------------------
    # Port helpers
    # ------------------------------------------------------------------

    def _refresh_ports(self):
        ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
        self._port_cb["values"] = ports
        if ports and not self._port_var.get():
            self._port_var.set(ports[0])

    # ------------------------------------------------------------------
    # Connect / disconnect
    # ------------------------------------------------------------------

    def _on_connect_toggle(self):
        if self._worker is None:
            self._on_connect()
        else:
            self._on_disconnect()

    def _on_connect(self):
        port = self._port_var.get().strip()
        if not port:
            messagebox.showerror("No port", "Select a serial port first.")
            return
        try:
            baud = int(self._baud_var.get())
        except ValueError:
            messagebox.showerror("Bad baud", "Baud rate must be an integer.")
            return

        self._worker = SerialWorker(port, baud, self._rx_queue)
        self._worker.start()
        self._conn_btn.configure(text="Disconnect")

    def _on_disconnect(self):
        if self._worker:
            self._worker.stop()
            self._worker = None
        if self._live_telemetry:
            self._live_telemetry = False
            self._live_btn.configure(text="● Live")

    def _set_connected(self, connected: bool):
        state = "normal" if connected else "disabled"
        for w in (self._live_btn, self._set_v_btn, self._set_raw_btn, self._reset_btn):
            w.configure(state=state)

    # ------------------------------------------------------------------
    # Queue polling — runs on the main thread every POLL_INTERVAL_MS
    # ------------------------------------------------------------------

    def _poll_queue(self):
        try:
            while True:
                line = self._rx_queue.get_nowait()
                self._handle_rx(line)
        except queue.Empty:
            pass
        self.after(POLL_INTERVAL_MS, self._poll_queue)

    def _handle_rx(self, line: str):
        if line == "__CONNECTED__":
            self._set_connected(True)
            self._log_append("-- connected --\n", "ok")
            return
        if line == "__DISCONNECTED__":
            self._worker = None
            self._set_connected(False)
            self._conn_btn.configure(text="Connect")
            self._log_append("-- disconnected --\n", "error")
            return
        if line.startswith("__ERROR__ "):
            self._worker = None
            self._set_connected(False)
            self._conn_btn.configure(text="Connect")
            self._log_append(f"ERROR: {line[10:]}\n", "error")
            return

        tag = "ok" if line.startswith("OK:") else ("error" if line.startswith("ERROR:") else None)
        self._log_append(f"<< {line}\n", tag)
        self._parse_telemetry_line(line)

    # ------------------------------------------------------------------
    # Telemetry parser
    # ------------------------------------------------------------------

    def _parse_telemetry_line(self, line: str):
        # Separator: try colon-split first (AVG_* lines), then space-split
        if ":" in line:
            key, _, rest = line.partition(":")
            key  = key.strip()
            rest = rest.strip()
        else:
            parts = line.split(None, 1)
            key  = parts[0] if parts else ""
            rest = parts[1].strip() if len(parts) > 1 else ""

        # Strip trailing unit tokens for display-ready values
        def strip_unit(s: str) -> str:
            for unit in (" A", " V", " W", "%"):
                if s.endswith(unit):
                    return s[: -len(unit)] + unit  # keep it readable
            return s

        mapping = {
            "FIRMWARE_VERSION":                    lambda v: self.title(f"EPL WPS Monitor — fw {v}"),
            "STATE":                               lambda v: self._sv["STATE"].set(v),
            "FAULT":                               lambda v: self._update_fault(v),
            "INPUT_CURRENT":                       lambda v: self._sv["INPUT CURRENT"].set(v),
            "INPUT_POWER":                         lambda v: self._sv["INPUT POWER"].set(v),
            "AVG_DISCHARGE_CURRENT":               lambda v: self._sv["DISCHARGE CURRENT"].set(v),
            "AVG_DISCHARGE_VOLTAGE":               lambda v: self._sv["DISCHARGE VOLTAGE"].set(v),
            "AVG_OUTPUT_POWER":                    lambda v: self._sv["OUTPUT POWER"].set(v),
            "AVG_DISCHARGE_SUCCESS_RATE":          lambda v: self._sv["SUCCESS RATE"].set(v),
            "DISCHARGES_SINCE_OPERATION_STARTED":  lambda v: self._sv["DISCHARGES"].set(v),
        }

        if key in mapping:
            mapping[key](rest)
            return

        # Edge-detection one-liners (no key=value structure)
        if line == "EDGE DETECTED":
            self._sv["EDGE"].set("DETECTED")
        elif line == "NO EDGE DETECTED YET":
            self._sv["EDGE"].set("not yet")

    def _update_fault(self, value: str):
        self._sv["FAULT"].set(value)
        self._fault_label.configure(
            foreground="#c0392b" if value != "NONE" else ""
        )

    # ------------------------------------------------------------------
    # Log pane helpers
    # ------------------------------------------------------------------

    def _log_append(self, text: str, tag: str | None = None):
        self._log.configure(state="normal")
        if tag:
            self._log.insert("end", text, tag)
        else:
            self._log.insert("end", text)
        # Trim to LOG_MAX_LINES
        lines = int(self._log.index("end-1c").split(".")[0])
        if lines > LOG_MAX_LINES:
            self._log.delete("1.0", f"{lines - LOG_MAX_LINES}.0")
        self._log.see("end")
        self._log.configure(state="disabled")

    # ------------------------------------------------------------------
    # Command handlers
    # ------------------------------------------------------------------

    def _send(self, cmd: str):
        if self._worker:
            self._log_append(f">> {cmd}\n", "send")
            self._worker.send(cmd)

    def _on_live_toggle(self):
        self._live_telemetry = not self._live_telemetry
        if self._live_telemetry:
            self._live_btn.configure(text="■ Stop")
            self._send("SET_TELEMETRY on")
        else:
            self._live_btn.configure(text="● Live")
            self._send("SET_TELEMETRY off")

    def _on_oneshot(self):
        self._send("SEND_TELEMETRY")

    def _on_set_voltage(self):
        try:
            v = int(self._vtable_var.get())
        except ValueError:
            messagebox.showerror("Bad value", "Target voltage must be an integer.")
            return
        if not 64 <= v <= 200:
            messagebox.showerror("Out of range", "Target voltage must be 64–200 V.")
            return
        self._send(f"SET_DPOT_FROM_VTABLE {v}")

    def _on_set_raw(self):
        try:
            pos = int(self._wiper_var.get())
        except ValueError:
            messagebox.showerror("Bad value", "Wiper position must be an integer.")
            return
        if not 0 <= pos <= 110:
            messagebox.showerror("Out of range", "Wiper position must be 0–110.")
            return
        self._send(f"SET_DPOT {pos}")

    def _on_reset(self):
        if messagebox.askyesno("Reset Device",
                               "Disable output stage and reboot the MCU?"):
            self._send("RESET_DEVICE")


# ---------------------------------------------------------------------------

def main():
    app = App()
    app.mainloop()


if __name__ == "__main__":
    main()
