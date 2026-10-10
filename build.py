"""Сборка редактора SolarIDE в один exe через Nuitka (onefile).

Запуск: py -3.13 build.py
Результат: build/SolarIDE.exe — один файл со всеми DLL и ресурсами.
"""





import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent

ENTRY = ROOT / "editor" / "main.py"

LOGO_SVG = ROOT / "editor" / "editor_icons" / "new_logo.svg"


def render_logo_png(png: Path, size: int = 256) -> Path:
    """Растеризует svg-логотип в png для --windows-icon-from-ico.

    Nuitka иконку из exe принимает только растром, поэтому svg приходится
    заранее превращать в картинку. Кладём результат в build_input/ — эту
    папку Nuitka при сборке не чистит, в отличие от build/.
    """
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PyQt6.QtCore import QByteArray
    from PyQt6.QtGui import QColor, QGuiApplication, QImage, QPainter
    from PyQt6.QtSvg import QSvgRenderer

    app = QGuiApplication.instance() or QGuiApplication([])
    image = QImage(size, size, QImage.Format.Format_ARGB32)
    image.fill(QColor(0, 0, 0, 0))
    painter = QPainter(image)
    QSvgRenderer(QByteArray(LOGO_SVG.read_bytes())).render(painter)
    painter.end()
    png.parent.mkdir(parents=True, exist_ok=True)
    if not image.save(str(png)):
        raise RuntimeError("не удалось сохранить иконку: %s" % png)
    return png

def run_build() -> int:
    """Собирает standalone exe: PyQt6, qframelesswindow и ресурсы рядом."""
    # Маркер с реальным python: в собранном exe sys.executable — сам exe,
    # а basedpyright-langserver лежит в Scripts рядом с интерпретатором.
    marker = ROOT / "build_input" / "interpreter.txt"
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(sys.executable, encoding="utf-8")
    # Иконка exe: svg из editor_icons рендерим в png заранее.
    icon_png = render_logo_png(ROOT / "build_input" / "app_icon.png")
    args = [
        sys.executable, "-m", "nuitka",
        "--onefile",
        "--enable-plugin=pyqt6",
        "--include-package=editor",
        "--include-package=qframelesswindow",
        # Ресурсы: пути в коде считаются от __file__ модулей, поэтому
        # кладём их в dist по тем же относительным путям
        "--include-data-dir=editor/themes=editor/themes",
        "--include-data-dir=editor/files_icons=editor/files_icons",
        "--include-data-dir=editor/editor_icons=editor/editor_icons",
        "--include-data-files=editor/tasks.json=editor/tasks.json",
        "--include-data-files=editor/lsp/sbcl_check.lisp=editor/lsp/sbcl_check.lisp",
        "--include-data-files=build_input/interpreter.txt=editor/interpreter.txt",
        "--windows-console-mode=disable",
        "--windows-icon-from-ico=%s" % icon_png.relative_to(ROOT),
        "--output-dir=build",
        "--output-filename=Solar.exe",
        "--remove-output",
        "--assume-yes-for-downloads",
        "--zig",  # MinGW на Python 3.13 не работает, Zig — автоматический
        str(ENTRY),
    ]
    print("Сборка LumenIDE (Nuitka standalone)...")
    return subprocess.call(args, cwd=str(ROOT))


if __name__ == "__main__":
    try:
        import nuitka  # noqa: F401
    except ImportError:
        print("Nuitka не установлена: py -3.13 -m pip install nuitka")
        sys.exit(1)
    sys.exit(run_build())
