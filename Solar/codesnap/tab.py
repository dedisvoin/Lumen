"""
CodeSnap tab widget (styled like SymbolsWidget)
"""
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QGuiApplication, QFont
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QSplitter, QGroupBox,
    QCheckBox, QPushButton, QLabel, QSlider, QFileDialog
)

from Solar.font_settings import code_font_family
from Solar.theme_utils import ThemeColors
from Solar.scrollbar_qss import scrollbar_qss
from Solar.codesnap.view import CodeSnapView


class CodeSnapTab(QWidget):
    def __init__(self, parent, data, theme_manager, theme_name=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.data = data
        self.theme_manager = theme_manager
        self.theme_name = theme_name or ""
        self.setObjectName('codesnapTab')

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        self.splitter = QSplitter(Qt.Orientation.Horizontal)
        layout.addWidget(self.splitter)

        self.settings_panel = QWidget()
        sp_layout = QVBoxLayout(self.settings_panel)
        sp_layout.setContentsMargins(10, 10, 10, 10)
        sp_layout.setSpacing(10)

        self.grp_disp = QGroupBox('Отображение')
        g1 = QVBoxLayout(self.grp_disp)
        self.cb_ln = QCheckBox('Показывать номера строк')
        self.cb_ln.setChecked(True)
        self.cb_macos = QCheckBox('Стиль как в macOS')
        self.cb_macos.setChecked(True)
        self.cb_fn = QCheckBox('Показывать заголовок файла')
        self.cb_fn.setChecked(True)
        g1.addWidget(self.cb_ln)
        g1.addWidget(self.cb_macos)
        g1.addWidget(self.cb_fn)
        sp_layout.addWidget(self.grp_disp)

        self.grp_style = QGroupBox('Оформление')
        g2 = QVBoxLayout(self.grp_style)
        self.cb_rounded = QCheckBox('Скруглённые углы')
        self.cb_rounded.setChecked(True)
        self.cb_shadow = QCheckBox('Тень')
        self.cb_shadow.setChecked(True)
        self.cb_theme = QCheckBox('Использовать фон темы')
        self.cb_theme.setChecked(True)
        g2.addWidget(self.cb_rounded)
        g2.addWidget(self.cb_shadow)
        g2.addWidget(self.cb_theme)
        sp_layout.addWidget(self.grp_style)

        self.grp_pad = QGroupBox('Отступы')
        g3 = QVBoxLayout(self.grp_pad)
        self.lbl_pad = QLabel('Отступы: 16px')
        self.slider_pad = QSlider(Qt.Orientation.Horizontal)
        self.slider_pad.setRange(8, 32)
        self.slider_pad.setValue(16)
        g3.addWidget(self.lbl_pad)
        g3.addWidget(self.slider_pad)
        sp_layout.addWidget(self.grp_pad)

        sp_layout.addStretch(1)

        btns = QHBoxLayout()
        btns.setSpacing(6)
        self.btn_copy = QPushButton('Скопировать в буфер')
        self.btn_save = QPushButton('Сохранить как PNG...')
        btns.addWidget(self.btn_copy)
        btns.addWidget(self.btn_save)
        sp_layout.addLayout(btns)

        self.settings_panel.setMinimumWidth(240)
        self.settings_panel.setMaximumWidth(340)

        self.preview = CodeSnapView(data=data, theme_manager=theme_manager, theme_name=self.theme_name)
        self.splitter.addWidget(self.settings_panel)
        self.splitter.addWidget(self.preview)
        self.splitter.setHandleWidth(1)
        self.splitter.setSizes([260, 900])

        self.cb_ln.toggled.connect(self._on_change)
        self.cb_macos.toggled.connect(self._on_change)
        self.cb_fn.toggled.connect(self._on_change)
        self.cb_rounded.toggled.connect(self._on_change)
        self.cb_shadow.toggled.connect(self._on_change)
        self.cb_theme.toggled.connect(self._on_change)
        self.slider_pad.valueChanged.connect(self._on_pad)
        self.btn_copy.clicked.connect(self._copy)
        self.btn_save.clicked.connect(self._save)

        if self.theme_name:
            self.apply_theme(self.theme_name)
        else:
            self._on_change()

    def apply_theme(self, theme_name: str):
        self.theme_name = theme_name or self.theme_name
        try:
            tc = ThemeColors.from_theme_manager(self.theme_manager, self.theme_name)
        except Exception:
            tc = None
        try:
            bg = tc.editor_bg_color.name() if tc and hasattr(tc, 'editor_bg_color') else "#1e1e1e"
        except Exception:
            bg = "#1e1e1e"
        try:
            fg = tc.fg if tc and hasattr(tc, 'fg') else "#d4d4d4"
        except Exception:
            fg = "#d4d4d4"
        try:
            muted = tc.muted if tc and hasattr(tc, 'muted') else "#8e8e8e"
        except Exception:
            muted = "#8e8e8e"
        try:
            border_c = getattr(tc, 'border', None)
            border = border_c.name() if hasattr(border_c, 'name') else "#2d2d2d"
        except Exception:
            border = "#2d2d2d"
        try:
            sel = getattr(tc, 'selection', None)
            hover = sel if isinstance(sel, str) else (sel.name() if hasattr(sel, 'name') else "#3a3d41")
        except Exception:
            hover = "#3a3d41"
        try:
            bgc = getattr(tc, 'bg', None)
            btn_bg = bgc.name() if hasattr(bgc, 'name') else bg
        except Exception:
            btn_bg = bg

        from PyQt6.QtGui import QColor
        try:
            fg_q = QColor(fg)
        except Exception:
            fg_q = QColor("#d4d4d4")
        try:
            bg_q = QColor(bg)
        except Exception:
            bg_q = QColor("#1e1e1e")

        self.setStyleSheet(f"""
            CodeSnapTab {{
                background: {bg};
            }}
            QGroupBox {{
                color: {fg};
                border: 1px solid {border};
                border-radius: 6px;
                margin-top: 10px;
                padding: 8px 6px 6px 6px;
                font-size: 9pt;
            }}
            QGroupBox::title {{
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 4px 0 4px;
                color: {muted};
            }}
            QCheckBox {{
                color: {fg};
                spacing: 6px;
            }}
            QCheckBox::indicator {{
                width: 14px;
                height: 14px;
            }}
            QLabel {{
                color: {fg};
            }}
            QPushButton {{
                background: {btn_bg};
                color: {fg};
                border: 1px solid {border};
                border-radius: 4px;
                padding: 6px 10px;
            }}
            QPushButton:hover {{
                background: {hover};
            }}
            QSlider::groove:horizontal {{
                height: 4px;
                background: {border};
                border-radius: 2px;
            }}
            QSlider::handle:horizontal {{
                background: {fg};
                width: 12px;
                height: 12px;
                margin: -4px 0;
                border-radius: 6px;
            }}
            QSplitter::handle {{
                background: {border};
            }}
        """ + scrollbar_qss(bg_q, fg_q))

        try:
            if hasattr(self.preview, 'apply_theme'):
                self.preview.apply_theme(self.theme_name)
            else:
                self.preview.set_theme(self.theme_name)
            self.preview.update()
        except Exception:
            pass

        self._on_change()

    def _on_pad(self, v):
        self.lbl_pad.setText(f'Отступы: {v}px')
        self._on_change()

    def _on_change(self):
        s = {
            'show_line_numbers': self.cb_ln.isChecked(),
            'macos_style': self.cb_macos.isChecked(),
            'show_filename': self.cb_fn.isChecked(),
            'rounded': self.cb_rounded.isChecked(),
            'shadow': self.cb_shadow.isChecked(),
            'use_theme_bg': self.cb_theme.isChecked(),
            'padding': self.slider_pad.value(),
        }
        try:
            self.preview.update_settings(s)
        except Exception:
            pass

    def _get_pixmap(self):
        return self.preview.grab()

    def _copy(self):
        pix = self._get_pixmap()
        QGuiApplication.clipboard().setPixmap(pix)

    def _save(self):
        path, _ = QFileDialog.getSaveFileName(self, 'Сохранить как PNG', 'codesnap.png', 'PNG Image (*.png)')
        if not path:
            return
        pix = self._get_pixmap()
        pix.save(path, 'PNG')
