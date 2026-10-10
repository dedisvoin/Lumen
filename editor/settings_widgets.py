"""
Кастомные виджеты панели настроек: тумблеры, текстовые поля, пути, списки,
радиокнопки, спинбоксы и кнопки — с единым интерфейсом SettingsControl.

Каждый контрол умеет:
  .set_value(value) — загрузить значение из настроек,
  .get_value()      — вернуть текущее значение для сохранения.

Стандартные виджеты (QLineEdit/QComboBox/QSpinBox/QRadioButton/QPushButton)
красятся общим QSS диалога; кастомно рисуется только ToggleSwitch (ему
цвета передаются через set_colors из settings_dialog).
"""

from typing import Any, Callable, List, Optional, Tuple

from PyQt6.QtCore import (
    QAbstractAnimation,
    QEasingCurve,
    QModelIndex,
    QPoint,
    QPointF,
    QPropertyAnimation,
    QRect,
    QRectF,
    Qt,
    pyqtProperty,
)
from PyQt6.QtGui import (
    QColor, QFontMetrics, QKeySequence, QPainter, QPen, QFont, QPolygon,
)
from PyQt6.QtWidgets import (
    QAbstractButton,
    QComboBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPlainTextEdit,
    QPushButton,
    QRadioButton,
    QSizePolicy,
    QSpinBox,
    QStyle,
    QStyleOptionComboBox,
    QStyleOptionViewItem,
    QStyledItemDelegate,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from editor import themed_dialogs
from editor.font_settings import code_font_family


# ---------------------------------------------------------------------------
# ToggleSwitch — анимированный тумблер вкл/выкл (рисуется вручную)
# ---------------------------------------------------------------------------
class ToggleSwitch(QAbstractButton):
    """Переключатель-тумблер с плавной анимацией дорожки/ручки."""

    _W = 36
    _H = 20
    _KNOB = 14
    _ANIM_MS = 140

    def __init__(self, parent=None):
        super().__init__(parent)
        self._progress = 0.0  # 0 = выкл, 1 = вкл (плавная анимация)
        self._colors = None   # (track_off, track_on, knob) — QColor
        self.setCheckable(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFixedSize(self._W, self._H)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setStyleSheet("outline: 0;")

        self._anim = QPropertyAnimation(self, b"progress", self)
        self._anim.setDuration(self._ANIM_MS)
        self._anim.setEasingCurve(QEasingCurve.Type.InOutCubic)
        self._anim.valueChanged.connect(self.update)

    def set_colors(self, track_off: QColor, track_on: QColor, knob: QColor):
        self._colors = (QColor(track_off), QColor(track_on), QColor(knob))
        self.update()

    @pyqtProperty(float)
    def progress(self) -> float:
        return self._progress

    @progress.setter
    def progress(self, value: float):
        self._progress = max(0.0, min(1.0, value))
        self.update()

    def setChecked(self, checked: bool):
        if self.isChecked() == bool(checked):
            return
        self._anim.stop()
        self._anim.setStartValue(self._progress)
        self._anim.setEndValue(1.0 if checked else 0.0)
        super().setChecked(checked)
        self._anim.start()

    def nextCheckState(self):
        self.setChecked(not self.isChecked())

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        track_off, track_on, knob = self._colors or (
            QColor(70, 70, 70), QColor(86, 156, 214), QColor(240, 240, 240))
        on = self._progress

        def mix(a: QColor, b: QColor, t: float) -> QColor:
            return QColor(
                int(a.red() + (b.red() - a.red()) * t),
                int(a.green() + (b.green() - a.green()) * t),
                int(a.blue() + (b.blue() - a.blue()) * t),
            )

        track = mix(track_off, track_on, on)
        r = QRectF(1, 1, self.width() - 2, self.height() - 2)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(track)
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2)

        # Обводка дорожки с небольшим отступом: контрастная к заливке,
        # поэтому видна и на светлых, и на тёмных темах
        if track.lightness() > 120:
            border = track.darker(150)
        else:
            border = track.lighter(150)
        pen = QPen(border)
        pen.setWidthF(1.0)
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)
        br = QRectF(r.left() + 1, r.top() + 1,
                    r.width() - 2, r.height() - 2)
        p.drawRoundedRect(br, br.height() / 2, br.height() / 2)

        pad = 3
        travel = self.width() - self._KNOB - pad * 2
        kx = pad + travel * self._progress
        ky = (self.height() - self._KNOB) / 2.0
        p.setBrush(knob)
        p.drawEllipse(QRectF(kx, ky, self._KNOB, self._KNOB))
        p.end()


# ---------------------------------------------------------------------------
# Обёртки: единый интерфейс SettingsControl
# ---------------------------------------------------------------------------
class SettingsControl(QWidget):
    """Базовый класс контрола настроек.

    Подклассы обязаны реализовать set_value/get_value. Дочерний виджет
    кладётся в лайаут; get_value возвращает «сохраняемое» значение.
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(0, 0, 0, 0)
        self._layout.setSpacing(0)

    def set_value(self, value):
        raise NotImplementedError

    def get_value(self):
        raise NotImplementedError


class ToggleControl(SettingsControl):
    """Флажок-тумблер."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.switch = ToggleSwitch(self)
        self._layout.addWidget(self.switch)

    def set_colors(self, track_off, track_on, knob):
        self.switch.set_colors(track_off, track_on, knob)

    def set_value(self, value):
        self.switch.setChecked(bool(value))

    def get_value(self):
        return self.switch.isChecked()


class TextControl(SettingsControl):
    """Текстовое поле (QLineEdit)."""

    def __init__(self, placeholder: str = "", parent=None):
        super().__init__(parent)
        self.edit = QLineEdit(self)
        self.edit.setPlaceholderText(placeholder)
        self.edit.setSizePolicy(QSizePolicy.Policy.Expanding,
                                QSizePolicy.Policy.Fixed)
        self._layout.addWidget(self.edit)

    def set_value(self, value):
        self.edit.setText("" if value is None else str(value))

    def get_value(self):
        return self.edit.text()


class MultiLineControl(SettingsControl):
    """Многострочное поле (QPlainTextEdit) — для доп. аргументов и т.п."""

    def __init__(self, height: int = 90, placeholder: str = "",
                 parent=None):
        super().__init__(parent)
        self.edit = QPlainTextEdit(self)
        self.edit.setFixedHeight(height)
        self.edit.setPlaceholderText(placeholder)
        self.edit.setFont(QFont(code_font_family(), 9))
        self._layout.addWidget(self.edit)

    def set_value(self, value):
        self.edit.setPlainText("" if value is None else str(value))

    def get_value(self):
        return self.edit.toPlainText()


class PlusMinusSpinBox(QSpinBox):
    """QSpinBox с нарисованными кнопками «+» и «−».

    Нативные кнопки отключены (NoButtons): символы рисуются сами в правой
    части кистью из темы, поэтому одинаково видны на любой теме и при любом
    масштабировании (в отличие от нативных стрелок/±, которые рисуются
    цветом из палитры и на тёмных темах сливаются с фоном).
    """

    _BTN_W = 30

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setButtonSymbols(QSpinBox.ButtonSymbols.NoButtons)
        self._glyph_color = QColor("#d4d4d4")
        self._sep_color = QColor(0, 0, 0, 60)

    def set_plus_minus_colors(self, glyph: QColor, separator: QColor):
        self._glyph_color = QColor(glyph)
        self._sep_color = QColor(separator)
        self.update()

    def _button_rect(self) -> QRect:
        return QRect(self.width() - self._BTN_W, 0, self._BTN_W,
                     self.height())

    def paintEvent(self, event):
        super().paintEvent(event)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        br = self._button_rect()
        # Вертикальный разделитель между полем ввода и кнопками
        painter.setPen(QPen(self._sep_color, 1))
        painter.drawLine(br.left(), br.top() + 3, br.left(),
                         br.bottom() - 3)
        # Кнопки «+» (верх) и «−» (низ)
        pen = QPen(self._glyph_color, 1.6)
        painter.setPen(pen)
        cx = br.center().x() + 1
        plus_y = br.top() + br.height() // 4 + 2
        minus_y = br.top() + br.height() * 3 // 4
        arm = 3
        painter.drawLine(cx - arm, plus_y, cx + arm, plus_y)
        painter.drawLine(cx, plus_y - arm, cx, plus_y + arm)
        painter.drawLine(cx - arm, minus_y, cx + arm, minus_y)
        painter.end()

    def mousePressEvent(self, event):
        br = self._button_rect()
        if br.contains(event.position().toPoint()):
            if event.position().y() < br.center().y():
                self.stepBy(1)
            else:
                self.stepBy(-1)
            return
        super().mousePressEvent(event)


class SpinControl(SettingsControl):
    """Числовой ввод (QSpinBox)."""

    def __init__(self, minimum: int = 0, maximum: int = 100,
                 suffix: str = "", parent=None):
        super().__init__(parent)
        self.spin = PlusMinusSpinBox(self)
        self.spin.setRange(minimum, maximum)
        if suffix:
            self.spin.setSuffix(suffix)
        self.spin.setSizePolicy(QSizePolicy.Policy.Fixed,
                                QSizePolicy.Policy.Fixed)
        self._layout.addWidget(self.spin)

    def set_value(self, value):
        try:
            self.spin.setValue(int(value))
        except (TypeError, ValueError):
            pass

    def get_value(self):
        return self.spin.value()


class ArrowComboBox(QComboBox):
    """QComboBox с нарисованной вручную стрелкой: QSS не умеет рисовать
    треугольник через border, поэтому рисуем его в paintEvent цветом темы
    (задаётся через set_arrow_color)."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._arrow_color = QColor("#9aa0a6")

    def set_arrow_color(self, color: QColor):
        self._arrow_color = QColor(color)
        self.update()

    def paintEvent(self, event):
        super().paintEvent(event)
        opt = QStyleOptionComboBox()
        self.initStyleOption(opt)
        arrow = self.style().subControlRect(
            QStyle.ComplexControl.CC_ComboBox, opt,
            QStyle.SubControl.SC_ComboBoxArrow, self)
        if not (arrow.isValid() and arrow.width() > 0 and arrow.height() > 0):
            arrow = QRect(self.width() - 20, 0, 20, self.height())
        p = QPainter(self)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(self._arrow_color)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w = 8
        h = 5
        cx = arrow.center().x()
        top = arrow.center().y() - h // 2
        tri = QPolygon([
            QPoint(cx - w // 2, top),
            QPoint(cx + w // 2, top),
            QPoint(cx, top + h),
        ])
        p.drawPolygon(tri)
        p.end()


class ComboDelegate(QStyledItemDelegate):
    """Делегат для выпадающих списков: ограничивает ширину поп-апа и
    обрезает длинные строки многоточием, чтобы список не расползался
    на всю ширину самого длинного пункта."""

    MAX_W = 430
    ITEM_H = 22

    def __init__(self, parent=None):
        super().__init__(parent)
        self._max_w = self.MAX_W

    def set_max_width(self, width: int):
        self._max_w = max(width, 60)

    def sizeHint(self, option, index):
        hint = super().sizeHint(option, index)
        hint.setHeight(self.ITEM_H)
        try:
            fm = QFontMetrics(option.font)
            text = index.data(Qt.ItemDataRole.DisplayRole)
            w = fm.horizontalAdvance(str(text or "")) + 32
            hint.setWidth(min(int(w), self._max_w))
        except Exception:
            pass
        return hint

    def paint(self, painter, option, index):
        opt = QStyleOptionViewItem(option)
        self.initStyleOption(opt, index)
        try:
            fm = QFontMetrics(opt.font)
            text = index.data(Qt.ItemDataRole.DisplayRole)
            opt.text = fm.elidedText(str(text or ""), Qt.TextElideMode.ElideRight,
                                     opt.rect.width() - 16)
        except Exception:
            pass
        super().paint(painter, opt, index)


class ComboControl(SettingsControl):
    """Выпадающий список (QComboBox) — выбор из вариантов."""

    def __init__(self, options: Optional[List[str]] = None, parent=None):
        super().__init__(parent)
        self.combo = ArrowComboBox(self)
        self._delegate = ComboDelegate(self.combo)
        self.combo.setItemDelegate(self._delegate)
        if options:
            self.combo.addItems(options)
        self.combo.setSizePolicy(QSizePolicy.Policy.Expanding,
                                 QSizePolicy.Policy.Fixed)
        self._layout.addWidget(self.combo)

    def set_max_width(self, width: int):
        self.combo.setMaximumWidth(width)
        self._delegate.set_max_width(width)

    def set_options(self, options: List[str], current: Optional[str] = None):
        self.combo.clear()
        self.combo.addItems(options)
        if current is not None and current in options:
            self.combo.setCurrentText(current)

    def set_data(self, options: List[str], values: List[Any],
                 current: Optional[Any] = None):
        """Заполняет combo текстами (options) со скрытыми значениями (values)."""
        self.combo.clear()
        for text, value in zip(options, values):
            self.combo.addItem(str(text), value)
        if current is not None:
            idx = self.combo.findData(current)
            if idx >= 0:
                self.combo.setCurrentIndex(idx)

    def set_value(self, value):
        if value is None:
            return
        idx = self.combo.findData(value)
        if idx >= 0:
            self.combo.setCurrentIndex(idx)
            return
        if self.combo.findText(str(value)) >= 0:
            self.combo.setCurrentText(str(value))

    def get_value(self):
        idx = self.combo.currentIndex()
        if idx >= 0 and self.combo.itemData(idx) is not None:
            return self.combo.itemData(idx)
        return self.combo.currentText()


class RadioControl(SettingsControl):
    """Группа радиокнопок: options — список (value, label)."""

    def __init__(self, options: Optional[List[Tuple[str, str]]] = None,
                 parent=None):
        super().__init__(parent)
        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(16)
        self._buttons: List[Tuple[QRadioButton, str]] = []
        for value, label in options or []:
            btn = QRadioButton(label, self)
            row.addWidget(btn)
            self._buttons.append((btn, str(value)))
        row.addStretch(1)
        self._layout.addLayout(row)

    def set_value(self, value):
        for btn, val in self._buttons:
            btn.setChecked(val == str(value))

    def get_value(self):
        for btn, val in self._buttons:
            if btn.isChecked():
                return val
        return self._buttons[0][1] if self._buttons else ""


class PathControl(SettingsControl):
    """Поле пути с кнопкой обзора (файл или каталог)."""

    def __init__(self, mode: str = "file", name_filter: str = "",
                 parent=None):
        super().__init__(parent)
        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(6)
        self.edit = QLineEdit(self)
        self.edit.setPlaceholderText(
            "auto" if mode == "file" else "leave empty to auto-detect")
        row.addWidget(self.edit, 1)
        self._browse = QToolButton(self)
        self._browse.setText("Browse...")
        self._browse.setToolTip("Choose path")
        self._browse.setAutoRaise(True)
        self._browse.setCursor(Qt.CursorShape.PointingHandCursor)
        row.addWidget(self._browse)
        self._layout.addLayout(row)

        self._mode = mode
        self._filter = name_filter
        self._theme_manager = None
        self._theme_name = ""
        self._browse.clicked.connect(self._pick)

    def set_theme(self, theme_manager, theme_name: str):
        """Тема диалогов обзора путей (приходит из панели настроек)."""
        self._theme_manager = theme_manager
        self._theme_name = theme_name

    def set_value(self, value):
        self.edit.setText("" if value is None else str(value))

    def get_value(self):
        return self.edit.text()

    def _pick(self):
        tm, tn = self._theme_manager, self._theme_name
        text = self.edit.text()
        if self._mode == "directory":
            path = themed_dialogs.existing_directory(
                self, "Choose Directory", text, tm, tn)
        else:
            paths = themed_dialogs.open_file_names(
                self, "Choose File", text, self._filter or "All Files (*)",
                tm, tn)
            path = paths[0] if paths else ""
        if path:
            self.edit.setText(path)


class ButtonControl(SettingsControl):
    """Обычная кнопка с callback (например, «Сбросить настройки»)."""

    def __init__(self, text: str = "Action", on_click: Optional[Callable] = None,
                 parent=None):
        super().__init__(parent)
        self._button = QPushButton(text, self)
        self._button.setCursor(Qt.CursorShape.PointingHandCursor)
        if on_click is not None:
            self._button.clicked.connect(lambda _=False: on_click())
        self._layout.addWidget(self._button)

    def set_click_handler(self, on_click: Callable):
        self._button.clicked.connect(lambda _=False: on_click())

    def set_value(self, value):
        pass

    def get_value(self):
        return None


class InfoControl(SettingsControl):
    """Статичный текст (о программе, пояснения)."""

    def __init__(self, text: str = "", parent=None):
        super().__init__(parent)
        self.label = QLabel(text, self)
        self.label.setWordWrap(True)
        self.label.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse)
        self._layout.addWidget(self.label)

    def set_value(self, value):
        self.label.setText("" if value is None else str(value))

    def get_value(self):
        return None


# ---------------------------------------------------------------------------
# ShortcutControl — настройка горячей клавиши
# ---------------------------------------------------------------------------
class _ShortcutLineEdit(QLineEdit):
    """Поле захвата шортката: клик -> фокус, следующая клавиша -> значение.

    Esc — отмена (значение не меняется), Backspace/Delete — очистить
    (шорткат отключается), отдельно зажатый модификатор игнорируется.
    """

    _MODIFIER_KEYS = {
        Qt.Key.Key_Control,
        Qt.Key.Key_Shift,
        Qt.Key.Key_Alt,
        Qt.Key.Key_Meta,
        Qt.Key.Key_AltGr,
    }

    def __init__(self, parent=None):
        super().__init__(parent)
        self._on_captured = None
        self.setPlaceholderText("Click, then press new keys")
        self.setSizePolicy(QSizePolicy.Policy.Expanding,
                           QSizePolicy.Policy.Fixed)

    def set_capture_callback(self, callback):
        self._on_captured = callback

    def mousePressEvent(self, event):
        super().mousePressEvent(event)
        self.setFocus()

    def keyPressEvent(self, event):
        key = event.key()
        if key == Qt.Key.Key_Escape:
            event.accept()
            self.clearFocus()
            return
        if key in (Qt.Key.Key_Backspace, Qt.Key.Key_Delete):
            event.accept()
            if self._on_captured:
                self._on_captured("")
            return
        if key in self._MODIFIER_KEYS:
            event.accept()
            return
        text = QKeySequence(event.keyCombination()).toString()
        if text:
            event.accept()
            if self._on_captured:
                self._on_captured(text)


class ShortcutControl(SettingsControl):
    """Поле горячей клавиши + подсказка с дефолтом под ним."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._value = ""
        self._edit = _ShortcutLineEdit(self)
        self._edit.set_capture_callback(self._on_captured)
        self._layout.addWidget(self._edit)
        self._hint = QLabel(self)
        self._hint.setObjectName("shortcutDefault")
        self._hint.setAlignment(Qt.AlignmentFlag.AlignRight)
        self._hint.hide()
        self._layout.addWidget(self._hint)

    def _on_captured(self, text):
        self._value = text
        self._edit.setText(text)
        self._edit.clearFocus()

    def set_default(self, default: str):
        self._hint.setText(f"Default: {default}" if default else "")
        self._hint.setVisible(bool(default))

    def set_value(self, value):
        self._value = "" if value is None else str(value)
        self._edit.setText(self._value)

    def get_value(self):
        return self._value


# ---------------------------------------------------------------------------
# Построитель контролов по типу из схемы
# ---------------------------------------------------------------------------
def build_control(item: dict) -> SettingsControl:
    """Создаёт контрол по описанию из схемы (item)."""
    ctype = item.get("type", "text")
    if ctype == "toggle":
        return ToggleControl()
    if ctype == "text":
        return TextControl(placeholder=item.get("placeholder", ""))
    if ctype == "multiline":
        return MultiLineControl(
            height=int(item.get("height", 90)),
            placeholder=item.get("placeholder", ""))
    if ctype == "spin":
        return SpinControl(
            minimum=int(item.get("min", 0)),
            maximum=int(item.get("max", 100)),
            suffix=item.get("unit", ""))
    if ctype == "combo":
        return ComboControl(item.get("options"))
    if ctype == "radio":
        return RadioControl(item.get("options"))
    if ctype == "path":
        return PathControl(mode=item.get("mode", "file"),
                           name_filter=item.get("filter", ""))
    if ctype == "button":
        return ButtonControl(text=item.get("label", "Action"))
    if ctype == "shortcut":
        return ShortcutControl()
    return InfoControl(item.get("text", ""))
