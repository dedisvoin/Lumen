"""
Панель ввода пояснения к закладке.

Открывается по клику в bookmark-маргин редактора: многострочное поле ввода,
как панель поиска файлов (FileFindPopup), но без списка. Enter создаёт
закладку с введённым текстом, Shift+Enter — перевод строки внутри пояснения,
Esc — отмена.

Сам QPlainTextEdit и есть «карточка» (фон/бордер/радиус), поэтому поле
ввода занимает всю плашку и прижато к левому верхнему углу. Тень рисуем
QGraphicsDropShadowEffect на этом же виджете, а вокруг оставляем прозрачный
запас (_shadow_margin), чтобы тень не обрезалась frameless-окном.
"""

from PyQt6.QtCore import Qt, QEvent
from PyQt6.QtGui import QColor, QPalette
from PyQt6.QtWidgets import (QFrame, QPlainTextEdit, QVBoxLayout,
                             QGraphicsDropShadowEffect)

from editor.theme_utils import ThemeColors
from editor.scrollbar_qss import scrollbar_qss


class BookmarkInputPopup(QFrame):
    """Многострочный ввод пояснения к закладке (поле = карточка)."""

    def __init__(self, editor, line, theme_manager, theme_name,
                 on_confirm, parent=None):
        super().__init__(
            parent,
            Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.NoDropShadowWindowHint,
        )
        self._editor = editor
        self._line = line
        self._on_confirm = on_confirm
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        # Прозрачный запас вокруг поля, чтобы тень не обрезалась окном.
        # Запас должен быть больше blurRadius/2 + смещение, иначе размытая
        # тень срезается границей frameless-окна (см. _SHADOW_BLUR ниже).
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self._shadow_margin = 26
        self.setFixedWidth(380 + self._shadow_margin * 2)

        tc = ThemeColors.from_theme_manager(theme_manager, theme_name)
        self._bg = tc.editor_bg_color.name()
        self._fg = tc.fg
        self._border = tc.border

        outer = QVBoxLayout(self)
        outer.setContentsMargins(self._shadow_margin, self._shadow_margin,
                                 self._shadow_margin, self._shadow_margin)
        outer.setSpacing(0)

        self._edit = QPlainTextEdit()
        self._edit.setFrameShape(QFrame.Shape.NoFrame)
        self._edit.setLineWrapMode(QPlainTextEdit.LineWrapMode.WidgetWidth)
        self._edit.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._edit.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self._edit.setStyleSheet(f"""
            QPlainTextEdit {{
                background-color: {self._bg};
                color: {self._fg};
                border: 0.5px solid {self._border};
                border-radius: 6px;
                padding: 6px 8px;
            }}
        """)
        # У QPlainTextEdit цвет текста берётся из палитры (роль Text),
        # а не из stylesheet (там color задаёт WindowText). Стиль сбрасывает
        # палитру, поэтому проставляем роль Text/Base уже ПОСЛЕ stylesheet.
        pal = self._edit.palette()
        pal.setColor(QPalette.ColorRole.Text, QColor(self._fg))
        pal.setColor(QPalette.ColorRole.Base, QColor(self._bg))
        self._edit.setPalette(pal)

        shadow = QGraphicsDropShadowEffect(self._edit)
        shadow.setBlurRadius(32)
        shadow.setOffset(0, 4)
        shadow.setColor(QColor(0, 0, 0, 80))
        self._edit.setGraphicsEffect(shadow)

        self._edit.setPlaceholderText(
            "Note...")
        self._edit.textChanged.connect(self._adjust_height)
        self._edit.installEventFilter(self)
        # Тот же стиль скроллбара, что у виджета поиска файлов (FileFindPopup)
        self._edit.verticalScrollBar().setStyleSheet(
            scrollbar_qss(self._bg, QColor(self._fg)))
        outer.addWidget(self._edit)
        self._adjust_height()

    def _adjust_height(self):
        """Подгоняет высоту поля под содержимое (до 8 строк, дальше скролл)."""
        doc = self._edit.document()
        lh = self._edit.fontMetrics().lineSpacing()
        # doc.size().height() ненадёжен до первой отрисовки (для пустого
        # документа ~1px), поэтому считаем по числу строк * высоте строки.
        lines = max(doc.lineCount(), 1)
        h = lines * lh + 20
        max_h = lh * 8 + 20
        if h > max_h:
            h = max_h
        self._edit.setFixedHeight(h)
        # Полоса прокрутки — только когда строк больше 6 (при 7+ показываем,
        # иначе скрываем, чтобы поле не «прыгало» из-за её появления).
        if lines < 7:
            self._edit.setVerticalScrollBarPolicy(
                Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        else:
            self._edit.setVerticalScrollBarPolicy(
                Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.adjustSize()

    def _accept(self):
        text = self._edit.toPlainText().strip()
        self.close()
        self._on_confirm(text, self._line)

    def showEvent(self, event):
        super().showEvent(event)
        self._edit.setFocus()
        cursor = self._edit.textCursor()
        cursor.movePosition(cursor.MoveOperation.End)
        self._edit.setTextCursor(cursor)

    def eventFilter(self, obj, event):
        if obj is self._edit and event.type() == QEvent.Type.KeyPress:
            if event.key() == Qt.Key.Key_Escape:
                self.close()
                return True
            # Enter без Shift — подтверждение; Shift+Enter — перевод строки
            if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
                if event.modifiers() & Qt.KeyboardModifier.ShiftModifier:
                    return super().eventFilter(obj, event)
                self._accept()
                return True
        return super().eventFilter(obj, event)

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape:
            self.close()
            return
        super().keyPressEvent(event)
