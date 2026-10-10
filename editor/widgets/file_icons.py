"""
Общие SVG-иконки IDE: типы файлов (editor/files_icons) и интерфейса
(editor/editor_icons).

Один источник правды для иконок, которые рисуются в дереве файлов
(file_tree.py) и на вкладках редактора (tab_bar.py). Иконки
перекрашиваются в цвета текущей темы и кэшируются по (имя, цвет).
"""

import os
import re
from pathlib import Path
from typing import Optional

from PyQt6.QtCore import Qt, QRectF
from PyQt6.QtGui import QColor, QIcon, QPainter, QPixmap, QGuiApplication
from PyQt6.QtSvg import QSvgRenderer

from editor.resources import editor_path

# Каталоги иконок (рядом с модулями: исходники, standalone и onefile)
FILE_ICONS_DIR = editor_path("files_icons")
UI_ICONS_DIR = editor_path("editor_icons")
SYMBOL_ICONS_DIR = UI_ICONS_DIR / "symbols"

# Какая иконка соответствует расширению файла
EXT_ICON_MAP = {
    ".c": "c",
    ".cpp": "cpp", ".cxx": "cpp", ".cc": "cpp",
    ".h": "cpp", ".hpp": "cpp",
    ".py": "python", ".pyw": "python",
    ".rs": "rust",
    ".lisp": "lisp", ".lsp": "lisp", ".cl": "lisp",
    ".el": "lisp", ".scm": "lisp", ".ss": "lisp", ".rkt": "lisp",
    ".lm": "lumen",
    ".asm": "assembler", ".nasm": "assembler", ".s": "assembler",
    ".inc": "assembler",
    ".ll": "llvm",
    ".exe": "executable", ".bat": "executable", ".cmd": "executable",
    ".txt": "text",
    ".md": "text",
    ".log": "text",
    ".json": "json",
    ".ini": "text",
    ".cfg": "text",
    ".conf": "text",
    ".toml": "text",
    ".yaml": "text",
    ".yml": "text",
    ".xml": "text",
    ".csv": "text",
}
DEFAULT_FILE_ICON = "undefine_language"

# Все явные fill/stroke-цвета в SVG заменяем на цвет темы
_SVG_COLOR_RE = re.compile(r'(fill|stroke)="#[0-9a-fA-F]{6}"')

# Рендерим SVG в увеличенном разрешении и плавно уменьшаем — так мелкие
# иконки получаются чёткими, без размытия и «лесенок» (как иконки фолдинга)
_SVG_RENDER_SCALE = 2

# Набор размеров иконки: Qt подберёт ближайший под плотность экрана,
# чтобы на масштабированных дисплеях (125–150%) не растягивать 16px
# и не размывать её
_ICON_SIZES = (16, 20, 24, 32, 48)

_file_cache: dict = {}
_ui_cache: dict = {}
_symbol_cache: dict = {}


def load_svg_icon(name: str, color: QColor) -> QIcon:
    """SVG-иконка из files_icons, перекрашенная в цвет темы."""
    key = (name, color.name())
    icon = _file_cache.get(key)
    if icon is None:
        icon = _render_svg(name, color, FILE_ICONS_DIR)
        _file_cache[key] = icon
    return icon


def file_icon_for_path(path: Optional[str], color: QColor) -> QIcon:
    """Иконка файла по расширению (в цвете темы)."""
    if not path:
        return load_svg_icon(DEFAULT_FILE_ICON, color)
    ext = os.path.splitext(path)[1].lower()
    return load_svg_icon(EXT_ICON_MAP.get(ext, DEFAULT_FILE_ICON), color)


def load_ui_icon(name: str, color: QColor, alpha: int = 255) -> QIcon:
    """SVG-иконка из editor_icons, перекрашенная в цвет темы.

    alpha < 255 приглушает иконку целиком (QtSvg не понимает rgba()
    в атрибутах fill/stroke — прозрачность задаём через QPainter).
    """
    key = (name, color.name(), alpha)
    icon = _ui_cache.get(key)
    if icon is None:
        icon = _render_svg(name, color, UI_ICONS_DIR, alpha)
        _ui_cache[key] = icon
    return icon


def load_symbol_icon(name: str, color: QColor, alpha: int = 255) -> QIcon:
    """SVG-иконка категории символа (editor_icons/symbols) в цвете темы.

    alpha < 255 приглушает иконку целиком (как load_ui_icon).
    """
    key = (name, color.name(), alpha)
    icon = _symbol_cache.get(key)
    if icon is None:
        icon = _render_svg(name, color, SYMBOL_ICONS_DIR, alpha)
        _symbol_cache[key] = icon
    return icon


def render_bookmark(color: QColor, size: int, height: int | None = None) -> QPixmap:
    """Рендерит иконку закладки (editor_icons/bookmark.svg) в точный размер.

    size — ширина/базовый размер, height — высота строки для вертикального
    центрирования (None → квадрат). Чёткость на HiDPI за счёт рендера в
    device-пиксели с учётом DPR экрана + суперсэмплинг, плавный downscale.
    Квадратный viewBox SVG сам центрируется в прямоугольнике (KeepAspectRatio),
    поэтому в высоком pixmap глиф оказывается по центру строки.
    """
    svg_path = UI_ICONS_DIR / "bookmark.svg"
    try:
        svg = svg_path.read_text(encoding="utf-8")
    except OSError:
        return QPixmap()
    svg = _SVG_COLOR_RE.sub(lambda m: f'{m.group(1)}="{color.name()}"', svg)
    renderer = QSvgRenderer()
    if not renderer.load(svg.encode("utf-8")):
        return QPixmap()
    if size <= 0:
        return QPixmap()
    h = size if height is None else max(size, int(height))
    try:
        screen = QGuiApplication.primaryScreen()
        dpr = screen.devicePixelRatio() if screen else 1.0
    except Exception:
        dpr = 1.0
    if dpr <= 0:
        dpr = 1.0
    ss = 1
    src_w = int(size * dpr * ss)
    src_h = int(h * dpr * ss)
    big = QPixmap(src_w, src_h)
    big.fill(Qt.GlobalColor.transparent)
    p = QPainter(big)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
    # Квадратный глиф центрируем явно внутри pixmap (по обеим осям), чтобы
    # иконка гарантированно стояла по центру строки, а не прижималась к краю.
    inner = min(src_w, src_h)
    off_x = (src_w - inner) / 2.0
    off_y = (src_h - inner) / 2.0
    renderer.render(p, QRectF(off_x, off_y, inner, inner))
    p.end()
    final_w = int(size * dpr)
    final_h = int(h * dpr)
    pm = big.scaled(final_w, final_h,
                   Qt.AspectRatioMode.IgnoreAspectRatio,
                   Qt.TransformationMode.SmoothTransformation)
    pm.setDevicePixelRatio(dpr)
    return pm


def _render_svg(name: str, color: QColor, directory: Path,
                alpha: int = 255) -> QIcon:
    svg_path = directory / f"{name}.svg"
    try:
        svg = svg_path.read_text(encoding="utf-8")
    except OSError:
        return QIcon()
    svg = _SVG_COLOR_RE.sub(
        lambda m: f'{m.group(1)}="{color.name()}"', svg
    )
    renderer = QSvgRenderer()
    if not renderer.load(svg.encode("utf-8")):
        return QIcon()
    icon = QIcon()
    for size in _ICON_SIZES:
        big = QPixmap(size * _SVG_RENDER_SCALE, size * _SVG_RENDER_SCALE)
        big.fill(Qt.GlobalColor.transparent)
        p = QPainter(big)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        p.setOpacity(alpha / 255)
        renderer.render(p, QRectF(0, 0, big.width(), big.height()))
        p.end()
        pm = big.scaled(size, size,
                        Qt.AspectRatioMode.IgnoreAspectRatio,
                        Qt.TransformationMode.SmoothTransformation)
        # Нативный стиль Windows (windows11) при выделении подкрашивает
        # иконку в синий, если у QIcon нет pixmap для режима Selected.
        # Регистрируем один и тот же pixmap для всех режимов и состояний —
        # тогда иконка не меняет цвет при выделении/нажатии.
        for mode in (QIcon.Mode.Normal, QIcon.Mode.Active,
                     QIcon.Mode.Selected, QIcon.Mode.Disabled):
            icon.addPixmap(pm, mode, QIcon.State.Off)
            icon.addPixmap(pm, mode, QIcon.State.On)
    return icon
