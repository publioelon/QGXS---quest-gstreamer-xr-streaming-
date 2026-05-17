from pathlib import Path

p = Path("launcher_gui.py")
text = p.read_text(encoding="utf-8-sig")

backup = Path("launcher_gui.py.bak_before_scan_button_position_fix")
backup.write_text(text, encoding="utf-8")

old = '        ttk.Button(row, text="Scan for Quest", command=self.scan_for_quest).grid(row=0, column=6, padx=(8, 0), sticky="w")\n'
new = '        ttk.Button(row, text="Scan for Quest", command=self.scan_for_quest).grid(row=0, column=14, padx=(16, 0), sticky="w")\n'

if old not in text:
    raise RuntimeError("Could not find the overlapping Scan for Quest button line.")

text = text.replace(old, new, 1)

p.write_text(text, encoding="utf-8")

print("Fixed Scan for Quest button position.")
print("Backup saved as launcher_gui.py.bak_before_scan_button_position_fix")