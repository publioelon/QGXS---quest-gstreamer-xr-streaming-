from pathlib import Path

p = Path("launcher_gui.py")
text = p.read_text(encoding="utf-8-sig")

backup = Path("launcher_gui.py.bak_before_quest_discovery")
backup.write_text(text, encoding="utf-8")

import_block = """try:
    from quest_discovery import discover_quest
except Exception as exc:
    discover_quest = None
    QUEST_DISCOVERY_IMPORT_ERROR = exc
"""

if "from quest_discovery import discover_quest" not in text:
    marker = "from typing import Optional\n"
    if marker not in text:
        raise RuntimeError("Could not find import marker: from typing import Optional")
    text = text.replace(marker, marker + "\n" + import_block + "\n", 1)

method_block = r'''
    def _quest_discovery_log(self, message: str) -> None:
        try:
            if hasattr(self, "log"):
                self.log(message)
            elif hasattr(self, "_log"):
                self._log(message)
            elif hasattr(self, "log_queue"):
                self.log_queue.put(message)
            elif hasattr(self, "log_text"):
                self.log_text.insert(tk.END, message + "\n")
                self.log_text.see(tk.END)
            else:
                print(message)
        except Exception:
            print(message)

    def scan_for_quest(self) -> None:
        self._quest_discovery_log("[gui] Scanning for Quest receiver...")

        if discover_quest is None:
            messagebox.showerror(
                "Quest discovery unavailable",
                "Could not import quest_discovery.py.\n\n"
                "Make sure quest_discovery.py is in the same folder as launcher_gui.py."
            )
            self._quest_discovery_log("[gui] Quest discovery unavailable: quest_discovery.py import failed.")
            return

        try:
            result = discover_quest(timeout_seconds=5.0, interval_seconds=0.25)
        except Exception as exc:
            messagebox.showerror("Quest discovery failed", str(exc))
            self._quest_discovery_log(f"[gui] Quest discovery failed: {exc}")
            return

        if result is None:
            messagebox.showwarning(
                "Quest not found",
                "Quest receiver was not found.\n\n"
                "Make sure the Quest APK is open and both devices are on the same Wi-Fi."
            )
            self._quest_discovery_log("[gui] Quest receiver not found.")
            return

        quest_ip = result["ip"]
        signaling_port = str(result["signaling_port"])

        self.host_var.set(quest_ip)
        self.port_var.set(signaling_port)

        self._quest_discovery_log(f"[gui] Found Quest receiver at {quest_ip}:{signaling_port}")
'''

if "def scan_for_quest" not in text:
    marker = "\n    @staticmethod\n    def _list_supported_images"
    if marker in text:
        text = text.replace(marker, "\n" + method_block + marker, 1)
    else:
        marker = "\n    def _list_supported_images"
        if marker not in text:
            raise RuntimeError("Could not find method insertion point near _list_supported_images")
        text = text.replace(marker, "\n" + method_block + marker, 1)

port_line = '        self._small_entry(row, "Port", self.port_var, 4, width=7)\n'
button_line = '        ttk.Button(row, text="Scan for Quest", command=self.scan_for_quest).grid(row=0, column=6, padx=(8, 0), sticky="w")\n'

if "Scan for Quest" not in text:
    if port_line not in text:
        raise RuntimeError("Could not find Host/Port UI line to insert Scan for Quest button")
    text = text.replace(port_line, port_line + button_line, 1)

p.write_text(text, encoding="utf-8")

print("Patched launcher_gui.py successfully.")
print("Backup saved as launcher_gui.py.bak_before_quest_discovery")