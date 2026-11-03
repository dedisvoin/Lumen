"""
Плавные hover-подсветки.

QSS `:hover` в Qt переключается мгновенно — анимации CSS в QSS нет.
Здесь живёт общий механизм плавного появления/исчезания подсветки:

- HoverFade — QObject-драйвер анимации прозрачности (0..1). Виджет
  вызывает fade_in()/fade_out() из enterEvent/leaveEvent и рисует
  подложку в paintEvent, умножая альфу на progress().
- install_titlebar_button_hover_fade() — патчит класс кнопок тайтл-бара
  библиотеки qframelesswindow (min/max/close), поэтому эффект действует
  во всех окнах с StandardTitleBar: главное окно, detached-окна, диалоги
  настроек/задач, стандартные диалоги (themed_dialogs).
"""

from PyQt6.QtCore import QEasingCurve, QObject, QVariantAnimation
from PyQt6.QtGui import QColor

HOVER_FADE_MS = 140  # длительность появления/исчезания подсветки


def mix_colors(a: QColor, b: QColor, t: float) -> QColor:
    """Интерполяция двух цветов (включая альфу), t в [0, 1]."""
    t = max(0.0, min(1.0, t))
    return QColor(
        round(a.red() + (b.red() - a.red()) * t),
        round(a.green() + (b.green() - a.green()) * t),
        round(a.blue() + (b.blue() - a.blue()) * t),
        round(a.alpha() + (b.alpha() - a.alpha()) * t),
    )


class HoverFade(QObject):
    """Плавное появление/исчезание hover-подсветки виджета.

    Владелец:
      - в enterEvent вызывает fade_in(), в leaveEvent — fade_out();
      - в paintEvent использует progress() (0..1), чтобы рисовать подложку
        с интерполированной прозрачностью.
    """

    def __init__(self, widget, duration_ms: int = HOVER_FADE_MS):
        super().__init__(widget)
        self._widget = widget
        self._value = 0.0
        self._anim = QVariantAnimation(self)
        self._anim.setDuration(duration_ms)
        self._anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._anim.valueChanged.connect(self._on_value)

    def _on_value(self, value):
        self._value = float(value)
        self._widget.update()

    def progress(self) -> float:
        """Текущая интенсивность подсветки в [0, 1]."""
        return self._value

    def fade_in(self):
        self._start(1.0)

    def fade_out(self):
        self._start(0.0)

    def _start(self, target: float):
        self._anim.stop()
        self._anim.setStartValue(self._value)
        self._anim.setEndValue(target)
        self._anim.start()


def install_titlebar_button_hover_fade():
    """Плавный hover кнопок тайтл-бара (min/max/close) библиотеки
    qframelesswindow.

    Патчим класс TitleBarButton: события входа/выхода начинают анимацию,
    а _getColors возвращает интерполированный между normal и hover цвет.
    Задевает все уже созданные и будущие кнопки (эффект применяется во
    всех окнах с StandardTitleBar). Идемпотентно.
    """
    try:
        from qframelesswindow.titlebar.title_bar_buttons import (
            TitleBarButton, TitleBarButtonState)
    except Exception:
        return

    if getattr(TitleBarButton, '_hover_fade_installed', False):
        return
    TitleBarButton._hover_fade_installed = True

    _orig_enter = TitleBarButton.enterEvent
    _orig_leave = TitleBarButton.leaveEvent

    def _fade(self):
        fade = getattr(self, '_hoverFade', None)
        if fade is None:
            fade = HoverFade(self)
            self._hoverFade = fade
        return fade

    def enterEvent(self, e):
        _fade(self).fade_in()
        self.setState(TitleBarButtonState.HOVER)
        _orig_enter(self, e)

    def leaveEvent(self, e):
        _fade(self).fade_out()
        self.setState(TitleBarButtonState.NORMAL)
        _orig_leave(self, e)

    def _getColors(self):
        if self._state == TitleBarButtonState.PRESSED:
            return self._pressedColor, self._pressedBgColor
        fade = getattr(self, '_hoverFade', None)
        a = fade.progress() if fade is not None else 0.0
        col = mix_colors(self._normalColor, self._hoverColor, a)
        bg = mix_colors(self._normalBgColor, self._hoverBgColor, a)
        return col, bg

    TitleBarButton.enterEvent = enterEvent
    TitleBarButton.leaveEvent = leaveEvent
    TitleBarButton._getColors = _getColors