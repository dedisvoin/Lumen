"""
themed_dialogs — стандартные диалоги с тайтл-баром как у TaskDialog.

QMessageBox, QInputDialog и QFileDialog (не нативные) получают frameless-окно
с StandardTitleBar (qframelesswindow): заголовок слева, кнопки свернуть/закрыть
справа, без кнопки разворота. Цвета берутся из текущей темы.

Тайтл-бар прижимается к верху диалога событийным фильтром (позиция при
ресайзе, отступ контента при первом показе), поэтому работает с любым
внутренним layout'ом стандартных диалогов.
"""

from typing import List, Optional, Tuple

from PyQt6.QtCore import QEvent, QObject, Qt
from PyQt6.QtGui import QColor
from PyQt6.QtWidgets import QDialog, QFileDialog, QInputDialog, QMessageBox

from qframelesswindow import StandardTitleBar

from editor.dialog_theme import dialog_theme_qss
from editor.theme_utils import ThemeColors

# Алиас для кнопок QMessageBox.StandardButton у вызывающих обёрток
StandardButton = QMessageBox.StandardButton


def style_titlebar(tb, tc: ThemeColors):
    """Оформление тайтл-бара в цветах темы (общий для TaskDialog и диалогов)."""
    bg = tc.title_bg or tc.editor_bg_color.name()
    fg = tc.fg
    tb.setStyleSheet(f"""
        StandardTitleBar {{
            background-color: {bg};
        }}
    """)
    tb.titleLabel.setStyleSheet(f"""
        QLabel {{
            color: {fg};
            font-size: 10pt;
            background: transparent;
            border: none;
            padding-left: 7px;
        }}
    """)
    for btn in (tb.minBtn, tb.maxBtn, tb.closeBtn):
        btn.setNormalColor(QColor(fg))
        btn.setHoverColor(QColor(fg))
        btn.setPressedColor(QColor(fg))
    hover_bg = QColor(fg)
    hover_bg.setAlpha(30)
    tb.minBtn.setHoverBackgroundColor(hover_bg)
    tb.maxBtn.setHoverBackgroundColor(hover_bg)
    tb.closeBtn.setHoverBackgroundColor(QColor(232, 17, 35))
    tb.closeBtn.setHoverColor(QColor(255, 255, 255))
    tb.closeBtn.setPressedColor(QColor(255, 255, 255))


class _TitleBarPoser(QObject):
    """Прижимает тайтл-бар к верху диалога и отодвигает контент под него."""

    def __init__(self, dialog, title_bar):
        super().__init__(dialog)
        self._tb = title_bar
        self._margins_done = False
        dialog.installEventFilter(self)

    def eventFilter(self, obj, ev):
        if ev.type() == QEvent.Type.Resize:
            self._tb.setGeometry(0, 0, obj.width(), self._tb.height())
        elif ev.type() == QEvent.Type.Show:
            if not self._margins_done:
                self._margins_done = True
                lay = obj.layout()
                if lay is not None:
                    m = lay.contentsMargins()
                    lay.setContentsMargins(
                        m.left(), m.top() + self._tb.height(),
                        m.right(), m.bottom())
            self._tb.raise_()
        return False


def _add_dwm_shadow(dialog):
    """DWM-тень/скругления как у TaskDialog (qframelesswindow).

    TaskDialog делает это в updateFrameless: addWindowAnimation вручную
    добавляет WS_CAPTION|WS_THICKFRAME (без caption-стиля DWM не рисует
    тень для frameless-окна), затем addShadowEffect расширяет фрейм
    DwmExtendFrameIntoClientArea — появляются тень и скруглённые углы.
    """
    try:
        from qframelesswindow.windows.window_effect import WindowsWindowEffect
        we = WindowsWindowEffect(dialog)
        hwnd = dialog.winId()
        we.addWindowAnimation(hwnd)
        we.addShadowEffect(hwnd)
        we.disableMaximizeButton(hwnd)
    except Exception:
        pass  # не-Windows или DWM недоступен — рамка из QSS остаётся


def install_titlebar(dialog, title, theme_manager, theme_name):
    """Frameless-окно + StandardTitleBar в стиле TaskDialog."""
    tc = ThemeColors.from_theme_manager(theme_manager, theme_name)
    dialog.setWindowFlags(
        dialog.windowFlags() | Qt.WindowType.FramelessWindowHint)
    tb = StandardTitleBar(dialog)
    tb.titleLabel.setText(title)
    tb.maxBtn.hide()
    # Заголовок — вплотную к левому краю (убираем отступ-спейсер)
    if tb.hBoxLayout.count() > 0:
        item = tb.hBoxLayout.itemAt(0)
        if item and item.spacerItem():
            tb.hBoxLayout.removeItem(item)
    if hasattr(tb, "iconLabel"):
        tb.iconLabel.hide()
    style_titlebar(tb, tc)
    # Обводка как у TaskDialog: там её рисует DWM-тень frameless-окна,
    # у обычных QDialog тени нет — добавляем рамку цветом темы.
    # QSS контента диалога (кнопки/поля/списки) — здесь же, на самом
    # диалоге: смена app-QSS на каждое переключение темы заставляет Qt
    # переполировать ВСЕ виджеты приложения (~900 мс), а этот QSS нужен
    # только диалогу в момент открытия.
    dialog.setStyleSheet(f"""
        QDialog {{
            border: 0.5px solid {tc.border};
        }}
        """ + dialog_theme_qss(tc))
    _add_dwm_shadow(dialog)
    _TitleBarPoser(dialog, tb)
    tb.setGeometry(0, 0, dialog.width(), tb.height())
    tb.raise_()
    return tb


# ---- QMessageBox ----

def _message_box(parent, title, text, icon, buttons, theme_manager,
                 theme_name) -> QMessageBox:
    box = QMessageBox(parent)
    box.setWindowTitle(title)
    box.setIcon(icon)
    box.setText(text)
    box.setStandardButtons(buttons)
    box.setDefaultButton(QMessageBox.StandardButton.Ok)
    install_titlebar(box, title, theme_manager, theme_name)
    box.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
    return box


def warning(parent, title, text, theme_manager, theme_name):
    box = _message_box(parent, title, text,
                       QMessageBox.Icon.Warning,
                       QMessageBox.StandardButton.Ok,
                       theme_manager, theme_name)
    box.exec()


def question(parent, title, text, buttons, default_button, theme_manager,
             theme_name) -> QMessageBox.StandardButton:
    box = QMessageBox(parent)
    box.setWindowTitle(title)
    box.setIcon(QMessageBox.Icon.Question)
    box.setText(text)
    box.setStandardButtons(buttons)
    box.setDefaultButton(default_button)
    install_titlebar(box, title, theme_manager, theme_name)
    box.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
    return QMessageBox.StandardButton(box.exec())


def about(parent, title, text, theme_manager, theme_name):
    box = QMessageBox(parent)
    box.setWindowTitle(title)
    box.setIcon(QMessageBox.Icon.Information)
    box.setText(text)
    box.setStandardButtons(QMessageBox.StandardButton.Ok)
    box.setDefaultButton(QMessageBox.StandardButton.Ok)
    install_titlebar(box, title, theme_manager, theme_name)
    box.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
    box.exec()


# ---- QInputDialog ----

def input_text(parent, title, label, theme_manager, theme_name,
               text: str = "") -> Tuple[str, bool]:
    dlg = QInputDialog(parent)
    dlg.setWindowTitle(title)
    dlg.setLabelText(label)
    dlg.setTextValue(text)
    install_titlebar(dlg, title, theme_manager, theme_name)
    ok = dlg.exec() == QDialog.DialogCode.Accepted
    # Значение читаем ДО удаления: WA_DeleteOnClose удалил бы диалог
    # к моменту обращения (RuntimeError: wrapped C/C++ object deleted)
    value = dlg.textValue() if ok else ""
    dlg.deleteLater()
    return value, ok


# ---- QFileDialog ----

def _file_dialog(parent, title, directory, accept_mode, file_mode,
                 theme_manager, theme_name) -> QFileDialog:
    dlg = QFileDialog(parent)
    dlg.setWindowTitle(title)
    if directory:
        dlg.setDirectory(directory)
    dlg.setAcceptMode(accept_mode)
    dlg.setFileMode(file_mode)
    dlg.setOption(QFileDialog.Option.DontUseNativeDialog, True)
    dlg.resize(760, 500)
    install_titlebar(dlg, title, theme_manager, theme_name)
    # WA_DeleteOnClose здесь ставить нельзя: при закрытии диалога Qt
    # выполняет deleteLater ДО того, как вызывающий код прочитает
    # selectedFiles() после exec() — объект уже уничтожен. Удаляем
    # диалог вручную (deleteLater) после чтения результата.
    return dlg


def open_file_names(parent, title, directory, name_filter, theme_manager,
                    theme_name) -> List[str]:
    dlg = _file_dialog(parent, title, directory,
                       QFileDialog.AcceptMode.AcceptOpen,
                       QFileDialog.FileMode.ExistingFiles,
                       theme_manager, theme_name)
    dlg.setNameFilter(name_filter)
    try:
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return []
        return dlg.selectedFiles()
    finally:
        dlg.deleteLater()


def save_file_name(parent, title, directory, theme_manager,
                   theme_name) -> Optional[str]:
    dlg = _file_dialog(parent, title, directory,
                       QFileDialog.AcceptMode.AcceptSave,
                       QFileDialog.FileMode.AnyFile,
                       theme_manager, theme_name)
    try:
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return None
        files = dlg.selectedFiles()
        return files[0] if files else None
    finally:
        dlg.deleteLater()


def existing_directory(parent, title, directory, theme_manager,
                       theme_name) -> Optional[str]:
    dlg = _file_dialog(parent, title, directory,
                       QFileDialog.AcceptMode.AcceptOpen,
                       QFileDialog.FileMode.Directory,
                       theme_manager, theme_name)
    dlg.setOption(QFileDialog.Option.ShowDirsOnly, True)
    try:
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return None
        files = dlg.selectedFiles()
        return files[0] if files else None
    finally:
        dlg.deleteLater()
