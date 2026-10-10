"""
Автоскрытие полос прокрутки с плавным затуханием бегунка.

Общая логика для всех прокручиваемых областей Lumen: редактор кода,
дерево файлов, терминал, панели задач и символов. Логика одна, чтобы
геометрия «плавающего» бегунка и тайминги затухания не разъезжались
между панелями.

Бегунок прячется, а сама полоса остаётся на месте: замена на
ScrollBarAlwaysOff сузила бы вьюпорт и вызвала reflow текста.
"""

from PyQt6.QtCore import (QObject, QEvent, QTimer, QVariantAnimation,
                        QEasingCurve)
from PyQt6.QtGui import QColor, QPalette

from editor.constants import SCROLLBAR_HIDE_DELAY_MS, SCROLLBAR_FADE_MS
from editor.scrollbar_qss import scrollbar_qss
from editor.settings_store import app_settings

# Ступени градации альфы бегунка: 192 значений достаточно, а каждый
# лишний setStyleSheet — переключение стиля полосы на Windows, из-за
# чего нативная полоса мигает светлым. Поэтому шагов мало, и QSS
# ставится только при реальном изменении ступени.
ALPHA_STEPS = 12


class ScrollBarAutoHide(QObject):
    """Прячет бегунок полос при простое и возвращает его по активности.

    Устанавливается на QAbstractScrollArea (CodeEditor, QTreeWidget и
    т.п.): берёт у неё полосы и вьюпорт, ловит события мыши, колеса и
    клавиатуры, а по истечении SCROLLBAR_HIDE_DELAY_MS гасит бегунок
    анимацией длительностью SCROLLBAR_FADE_MS.
    """

    def __init__(self, scroll_area, parent=None):
        super().__init__(parent)
        self._area = scroll_area
        self._bars = (scroll_area.verticalScrollBar(),
                      scroll_area.horizontalScrollBar())
        self._viewport = scroll_area.viewport()
        self._shown = True
        self._on_scrollbar = False
        self._bg = ""
        self._fg = QColor(128, 128, 128)
        self._alpha = 1.0
        self._applied = 1.0

        self._anim = QVariantAnimation(self)
        self._anim.setStartValue(1.0)
        self._anim.setEndValue(1.0)
        self._anim.setDuration(SCROLLBAR_FADE_MS)
        self._anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._anim.valueChanged.connect(self._on_alpha)

        self._hide_timer = QTimer(self)
        self._hide_timer.setSingleShot(True)
        self._hide_timer.setInterval(self._delay())
        self._hide_timer.timeout.connect(self._hide)
        self._hide_timer.start()

        for bar in self._bars:
            if bar is None:
                continue
            bar.installEventFilter(self)
            bar.valueChanged.connect(self.activity)
        self._viewport.installEventFilter(self)
        self._area.installEventFilter(self)

    # --- настройки ---

    def _enabled(self) -> bool:
        return app_settings().get_bool("editor.scrollbar_auto_hide", True)

    def _delay(self) -> int:
        return app_settings().get_int("editor.scrollbar_hide_delay",
                                      SCROLLBAR_HIDE_DELAY_MS)

    # --- цвета темы ---

    def set_colors(self, bg: str, fg: QColor) -> None:
        """Задать фон полосы и цвет бегунка (обычно из apply_theme)."""
        self._bg = bg
        self._fg = QColor(fg)
        self._render(force=True)

    # --- отрисовка ---

    def _render(self, force: bool = False) -> None:
        """Перерисовать полосы с текущей альфой бегунка.

        Ширина полосы не меняется — меняется только прозрачность
        бегунка, поэтому содержимое не переверстывается.
        """
        if not self._bg:
            return
        step = round(self._alpha * ALPHA_STEPS) / ALPHA_STEPS
        if not force and step == self._applied:
            return
        self._applied = step
        qss = scrollbar_qss(self._bg, self._fg, alpha=step)
        for bar in self._bars:
            if bar is None:
                continue
            pal = bar.palette()
            pal.setColor(QPalette.ColorRole.Window, QColor(self._bg))
            pal.setColor(QPalette.ColorRole.Base, QColor(self._bg))
            bar.setPalette(pal)
            bar.setAutoFillBackground(True)
            bar.setStyleSheet(qss)

    def _on_alpha(self, value) -> None:
        self._alpha = float(value)
        self._render()

    def _fade_to(self, target: float) -> None:
        """Плавно довести бегунок до target (1.0 — виден, 0.0 — скрыт)."""
        if not app_settings().get_bool("editor.scrollbar_fade", True):
            self._anim.stop()
            self._alpha = target
            self._render(force=True)
            return
        if abs(self._alpha - target) < 0.001:
            return
        self._anim.stop()
        self._anim.setStartValue(self._alpha)
        self._anim.setEndValue(target)
        self._anim.start()

    # --- состояние ---

    def _show(self) -> None:
        if not self._shown:
            self._shown = True
            self._fade_to(1.0)

    def _hide(self) -> None:
        if not self._enabled():
            return
        if self._on_scrollbar:
            return
        if self._shown:
            self._shown = False
            self._fade_to(0.0)

    def activity(self) -> None:
        """Любое действие пользователя показывает полосы и откладывает hide."""
        if not self._enabled():
            # Настройку выключили, пока полосы были скрыты — вернуть их.
            self._show()
            return
        self._show()
        self._hide_timer.start()

    def reload(self) -> None:
        """Перечитать настройки (после смены в диалоге настроек).

        Без этого выключенное автоскрытие не вернёт бегунок, уже
        спрятанный до смены настройки, — он появился бы только при
        первом действии пользователя в этой области.
        """
        self._hide_timer.setInterval(self._delay())
        if self._enabled():
            self.activity()
            return
        self._hide_timer.stop()
        self._shown = True
        self._fade_to(1.0)

    # --- события ---

    def eventFilter(self, obj, event):
        if obj in self._bars:
            et = event.type()
            if et == QEvent.Type.Enter:
                self._on_scrollbar = True
                self._show()
            elif et == QEvent.Type.Leave:
                self._on_scrollbar = False
                self.activity()
            return False
        et = event.type()
        if (et == QEvent.Type.MouseMove
                or et == QEvent.Type.Wheel
                or et == QEvent.Type.KeyPress
                or et == QEvent.Type.MouseButtonPress):
            self.activity()
        return False