"""
SpotlightController — «зенитный спотлайт» тайловых панелей.

Когда включён (настройка view.spotlight), все панели, кроме активной,
приглушаются: поверх панели рисуется полупрозрачный прямоугольник
цвета фона редактора темы (см. _overlay_color) — неактивное содержимое
«уходит» в фон редактора.

При переключении фокуса на другую панель затемнение плавно «переезжает»:
прежняя панель затухает (оверлей проявляется), новая активизируется
(оверлей растворяется). Анимация — QPropertyAnimation на прозрачности
оверлея, ~180 мс.

Оверлеи не перехватывают мышь (WA_TransparentForMouseEvents): клик по
приглушённой панели сразу переводит фокус и запускает переезд.
"""

from typing import Optional

from PyQt6.QtCore import (
    Qt, QEvent, QObject, QTimer, QAbstractAnimation, QPropertyAnimation,
    QEasingCurve, pyqtProperty,
)
from PyQt6.QtGui import QColor, QPainter
from PyQt6.QtWidgets import QWidget, QApplication

from editor.widgets.editor_pane import EditorPane
from editor.theme_utils import ThemeColors

# Прозрачность затенения (alpha * fade): цвет RGB — фон редактора темы,
# прозрачность одинакова для тёмной и светлой темы.
_DARK_ALPHA = 230
_LIGHT_ALPHA = 190
_FADE_MS = 180
_SYNC_MS = 300


class _DimOverlay(QWidget):
    """Полупрозрачный прямоугольник поверх панели.

    fade (0..1) — текущая «затушенность»: 0 — панель активна (прозрачно),
    1 — полностью приглушена. Свойство анимируется QPropertyAnimation.
    """

    def __init__(self, color: QColor, parent=None):
        super().__init__(parent)
        self._color = QColor(color)
        self._fade = 0.0
        self._anim: Optional[QPropertyAnimation] = None
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground)

    def set_color(self, color: QColor):
        self._color = QColor(color)
        self.update()

    def _get_fade(self) -> float:
        return self._fade

    def _set_fade(self, value: float):
        self._fade = float(value)
        self.update()

    fade = pyqtProperty(float, _get_fade, _set_fade)

    def is_animating(self) -> bool:
        return (self._anim is not None
                and self._anim.state() == QAbstractAnimation.State.Running)

    def set_fade_animated(self, target: float):
        if self.is_animating():
            return
        if abs(self._fade - target) < 0.01:
            return
        self._anim = QPropertyAnimation(self, b"fade", self)
        self._anim.setDuration(_FADE_MS)
        self._anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._anim.setStartValue(self._fade)
        self._anim.setEndValue(float(target))
        self._anim.start()

    def paintEvent(self, event):
        if self._fade <= 0.003:
            return
        painter = QPainter(self)
        color = QColor(self._color)
        color.setAlpha(int(color.alpha() * self._fade))
        painter.fillRect(self.rect(), color)
        painter.end()


class SpotlightController(QObject):
    """Следит за активной панелью и анимирует оверлеи приглушения."""

    def __init__(self, tile_manager, theme_manager, parent=None):
        super().__init__(parent)
        self._tm = tile_manager
        self._themes = theme_manager
        self._enabled = False
        self._overlays = {}          # id(pane) -> _DimOverlay
        self._installed_focus = False
        self._dark_alpha = _DARK_ALPHA
        self._light_alpha = _LIGHT_ALPHA
        self._skip_terminal = False
        self._skip_diagnostics = False
        self._connected_panes = set()   # id(pane) с подключённым currentChanged
        self._sync_timer = QTimer(parent)
        self._sync_timer.setInterval(_SYNC_MS)
        self._sync_timer.timeout.connect(self.sync)

    # --- включение / выключение ---

    def set_enabled(self, enabled: bool):
        enabled = bool(enabled)
        if enabled == self._enabled:
            if enabled:
                self.sync()
            return
        self._enabled = enabled
        if enabled:
            app = QApplication.instance()
            if app is not None and not self._installed_focus:
                app.focusChanged.connect(self._on_focus_changed)
                self._installed_focus = True
            self._sync_timer.start()
            self.sync()
        else:
            self._sync_timer.stop()
            self._fade_all(0.0)

    @property
    def enabled(self) -> bool:
        return self._enabled

    def set_theme(self, theme_name: str):
        """Пересчитывает цвет оверлея: заливка цветом фона редактора темы."""
        try:
            tc = ThemeColors.from_theme_manager(self._themes, theme_name)
        except Exception:
            return
        color = self._overlay_color(tc)
        for overlay in self._overlays.values():
            try:
                overlay.set_color(color)
            except RuntimeError:
                continue

    def set_alphas(self, dark: int, light: int):
        """Прозрачность приглушения для тёмной и светлой темы (0..255)."""
        self._dark_alpha = max(0, min(255, int(dark)))
        self._light_alpha = max(0, min(255, int(light)))
        for overlay in self._overlays.values():
            try:
                overlay.set_color(self._current_color())
            except RuntimeError:
                continue

    def set_skip(self, terminal: bool, diagnostics: bool):
        """Не приглушать панели, чья активная вкладка — терминал (terminal)
        или список диагностик (diagnostics)."""
        self._skip_terminal = bool(terminal)
        self._skip_diagnostics = bool(diagnostics)
        self.sync()

    # --- отслеживание активной панели ---

    def _on_focus_changed(self, *_):
        self.sync()

    def _active_pane(self) -> Optional[EditorPane]:
        try:
            return self._tm.get_active_pane()
        except RuntimeError:
            return None

    # --- синхронизация оверлеев ---

    def sync(self):
        if not self._enabled:
            return
        try:
            panes = self._tm.panes()
        except RuntimeError:
            return
        active = self._active_pane()
        active_id = id(active) if active is not None else None
        color = self._current_color()

        alive = set()
        for pane in panes:
            pid = id(pane)
            alive.add(pid)
            self._connect_pane(pane)
            overlay = self._overlays.get(pid)
            if overlay is None:
                overlay = self._make_overlay(pane, color)
                self._overlays[pid] = overlay
            else:
                overlay.set_color(color)
            try:
                overlay.setGeometry(self._overlay_rect(pane))
                overlay.raise_()
                overlay.show()
            except RuntimeError:
                continue
            dimmed = (pid != active_id and not self._skip_pane(pane))
            overlay.set_fade_animated(1.0 if dimmed else 0.0)

        # Оверлеи закрытых/пересозданных панелей
        for pid in list(self._overlays):
            if pid not in alive:
                overlay = self._overlays.pop(pid)
                try:
                    overlay.deleteLater()
                except RuntimeError:
                    pass

    def _make_overlay(self, pane: EditorPane, color: QColor) -> _DimOverlay:
        overlay = _DimOverlay(color, pane)
        pane.installEventFilter(self)
        overlay.setGeometry(self._overlay_rect(pane))
        overlay.raise_()
        overlay.show()
        return overlay

    def _connect_pane(self, pane: EditorPane):
        """Подключает currentChanged вкладок панели: смена активной вкладки
        (редактор -> терминал/диагностики и обратно) сразу пересчитывает
        затенение."""
        pid = id(pane)
        if pid in self._connected_panes:
            return
        try:
            pane.tabs.currentChanged.connect(self.sync)
            self._connected_panes.add(pid)
        except (RuntimeError, AttributeError):
            pass

    def _skip_pane(self, pane: EditorPane) -> bool:
        """True — панель не приглушаем: активная вкладка — терминал или
        список диагностик (по настройкам)."""
        try:
            widget = pane.tabs.currentWidget()
        except RuntimeError:
            return False
        if widget is None:
            return False
        if self._skip_terminal:
            from editor.widgets.terminal import TerminalWidget
            if isinstance(widget, TerminalWidget):
                return True
        if self._skip_diagnostics:
            from editor.widgets.diagnostics_widget import DiagnosticsWidget
            if isinstance(widget, DiagnosticsWidget):
                return True
        return False

    @staticmethod
    def _overlay_rect(pane: EditorPane):
        """Прямоугольник оверлея: вся панель с отступом 1 px слева и снизу."""
        return pane.rect().adjusted(1, 0, 0, -1)

    def _current_color(self) -> QColor:
        try:
            tc = ThemeColors.from_theme_manager(
                self._themes, self._tm.current_theme)
        except Exception:
            tc = None
        return self._overlay_color(tc)

    def _overlay_color(self, tc) -> QColor:
        """Цвет заливки оверлея: фон редактора темы с прозрачностью
        по типу темы. Без темы — чёрный (тёмный фолбек)."""
        if tc is None:
            return QColor(0, 0, 0, self._dark_alpha)
        base = tc.bg_color
        alpha = self._dark_alpha if tc.dark else self._light_alpha
        return QColor(base.red(), base.green(), base.blue(), alpha)

    def _fade_all(self, target: float):
        for overlay in self._overlays.values():
            try:
                overlay.set_fade_animated(target)
            except RuntimeError:
                continue

    # --- геометрия при ресайзе панелей ---

    def eventFilter(self, obj, event):
        if (event.type() == QEvent.Type.Resize
                and isinstance(obj, EditorPane)):
            overlay = self._overlays.get(id(obj))
            if overlay is not None:
                try:
                    overlay.setGeometry(self._overlay_rect(obj))
                    overlay.raise_()
                except RuntimeError:
                    pass
            return False
        return super().eventFilter(obj, event)
