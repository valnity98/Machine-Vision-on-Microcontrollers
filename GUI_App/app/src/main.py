"""
main.py — STM32 Edge Vision Reference Dashboard.

Application entry point for the PySide6 dashboard.

Usage (from project root):
  python GUI_App/app/src/main.py

Requirements:
  python -m pip install -r requirements.txt
"""

from __future__ import annotations

import sys

from pathlib import Path

from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QApplication

from reference_window import ReferenceWindowController


def main() -> int:
    """Create and run the Qt application."""
    # Qt 6 enables high-DPI scaling automatically. Setting the deprecated
    # AA_EnableHighDpiScaling attribute would only produce a warning.
    app = QApplication(sys.argv)
    app.setApplicationName("STM32 Edge Vision Reference")

    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        icon_path = Path(sys._MEIPASS) / "assets" / "app_icon.ico"
    else:
        # Look for assets/ next to the script, then one level up
        _src_dir = Path(__file__).resolve().parent
        icon_path = _src_dir / "assets" / "app_icon.ico"
        if not icon_path.exists():
            icon_path = _src_dir.parent / "assets" / "app_icon.ico"
    if icon_path.exists():
        app.setWindowIcon(QIcon(str(icon_path)))

    controller = ReferenceWindowController()
    controller.show()

    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
