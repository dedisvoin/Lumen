"""
CodeSnap renderer
"""
from typing import List
from PyQt6.QtCore import Qt, QRect, QPointF
from PyQt6.QtGui import QColor, QPainter, QFont, QFontMetrics, QPainterPath


class CodeSnapRenderer:
    def __init__(self):
        pass

    def _style_to_color(self, style: int, theme_colors):
        if theme_colors is None:
            # fallback
            return QColor('#d4d4d4')
        # try common mappings used by themes
        # many themes have: keyword, string, number, comment, function, type
        try:
            if hasattr(theme_colors, 'keyword') and style in (1, 5, 6):
                return QColor(theme_colors.keyword)
            if hasattr(theme_colors, 'string') and style in (3, 4):
                return QColor(theme_colors.string)
            if hasattr(theme_colors, 'number') and style == 2:
                return QColor(theme_colors.number)
            if hasattr(theme_colors, 'comment') and style in (1,):
                # sometimes comment different; try
                pass
            if hasattr(theme_colors, 'comment') and style == 7:
                return QColor(theme_colors.comment)
            if hasattr(theme_colors, 'function') and style == 8:
                return QColor(theme_colors.function)
            if hasattr(theme_colors, 'type') and style == 9:
                return QColor(theme_colors.type)
        except Exception:
            pass
        # default
        if hasattr(theme_colors, 'fg'):
            return QColor(theme_colors.fg)
        return QColor('#d4d4d4')

    def draw(self, painter: QPainter, rect: QRect, data, settings: dict, theme_colors):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.save()

        bg_color = QColor(theme_colors.bg) if theme_colors and settings.get('use_theme_bg', True) else QColor('#1e1e1e')
        card_bg = QColor(bg_color)

        padding = settings.get('padding', 16)
        radius = 12 if settings.get('rounded', True) else 0

        if settings.get('shadow', True):
            shadow_rect = rect.adjusted(4, 4, -4, -4)
            path_shadow = QPainterPath()
            path_shadow.addRoundedRect(float(shadow_rect.x()), float(shadow_rect.y()), float(shadow_rect.width()), float(shadow_rect.height()), float(radius + 2), float(radius + 2))
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(0, 0, 0, 60))
            painter.drawPath(path_shadow)

        card_rect = rect.adjusted(padding // 2, padding // 2, -padding // 2, -padding // 2)
        path = QPainterPath()
        path.addRoundedRect(float(card_rect.x()), float(card_rect.y()), float(card_rect.width()), float(card_rect.height()), float(radius), float(radius))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(card_bg)
        painter.drawPath(path)

        header_h = 0
        if settings.get('macos_style', False):
            header_h = 28
            header_rect = card_rect.adjusted(0, 0, 0, -(card_rect.height() - header_h))
            path_h = QPainterPath()
            bl = header_rect.bottomLeft()
            br = header_rect.bottomRight()
            tr = header_rect.topRight()
            tl = header_rect.topLeft()
            r = radius
            path_h.moveTo(float(bl.x()), float(bl.y()))
            path_h.lineTo(float(br.x()), float(br.y()))
            path_h.lineTo(float(tr.x()) - r, float(tr.y()))
            path_h.quadTo(float(tr.x()), float(tr.y()), float(tr.x()), float(tr.y()) + r)
            path_h.lineTo(float(tl.x()), float(tl.y()) + r)
            path_h.quadTo(float(tl.x()), float(tl.y()), float(tl.x()) + r, float(tl.y()))
            path_h.closeSubpath()
            painter.setBrush(QColor(card_bg.lighter(110)) if card_bg.lightness() < 128 else QColor(card_bg.darker(110)))
            painter.drawPath(path_h)
            dot_r = 6
            dot_y = header_rect.center().y()
            colors_dots = [QColor('#ff5f56'), QColor('#ffbd2e'), QColor('#27c93f')]
            for i, c in enumerate(colors_dots):
                painter.setBrush(c)
                painter.setPen(Qt.PenStyle.NoPen)
                painter.drawEllipse(QPointF(header_rect.left() + 12 + i * (dot_r * 2 + 4), float(dot_y)), float(dot_r), float(dot_r))
            if settings.get('show_filename', True) and data and getattr(data, 'file_path', None):
                title = data.file_path.split('\\')[-1].split('/')[-1]
            else:
                title = ''
            if title:
                font_t = painter.font()
                font_t.setPointSize(max(8, font_t.pointSize() - 1))
                painter.setFont(font_t)
                if theme_colors and hasattr(theme_colors, 'fg'):
                    painter.setPen(QColor(theme_colors.fg))
                else:
                    painter.setPen(QColor('#cccccc'))
                painter.drawText(header_rect, Qt.AlignmentFlag.AlignCenter, title)

        code_rect = card_rect.adjusted(12, 12 + header_h, -12, -12)
        font = QFont("Consolas", 9)
        painter.setFont(font)
        fm = QFontMetrics(font)
        line_h = fm.height() + 2

        text = data.text if data else ''
        lines = text.splitlines()
        show_ln = settings.get('show_line_numbers', True)
        ln_width = 0
        if show_ln:
            max_ln = (getattr(data, 'line_from', 0) + len(lines)) if data else len(lines)
            ln_width = fm.horizontalAdvance(str(max_ln)) + 18
        styles = getattr(data, 'styles', None) if data else None
        y_cur = code_rect.top() + 4
        char_offset = 0
        for i, l in enumerate(lines):
            ln_num = (getattr(data, 'line_from', 0) + i + 1)
            if show_ln:
                if theme_colors and hasattr(theme_colors, 'comment'):
                    painter.setPen(QColor(theme_colors.comment))
                else:
                    painter.setPen(QColor('#6a9955'))
                painter.drawText(QRect(code_rect.left(), y_cur, ln_width - 10, line_h), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, str(ln_num))
            # draw line char by char with styles if available
            x_cur = code_rect.left() + ln_width
            if styles and len(styles) > char_offset:
                # try to draw with style mapping
                j = 0
                while j < len(l):
                    st = styles[char_offset + j] if (char_offset + j) < len(styles) else 0
                    col = self._style_to_color(st, theme_colors)
                    painter.setPen(col)
                    ch = l[j]
                    painter.drawText(QRect(x_cur, y_cur, fm.horizontalAdvance(ch), line_h), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, ch)
                    x_cur += fm.horizontalAdvance(ch)
                    j += 1
                char_offset += len(l) + 1  # +newline
            else:
                if theme_colors and hasattr(theme_colors, 'fg'):
                    painter.setPen(QColor(theme_colors.fg))
                else:
                    painter.setPen(QColor('#d4d4d4'))
                painter.drawText(QRect(x_cur, y_cur, code_rect.width() - ln_width, line_h), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, l)
                char_offset += len(l) + 1
            y_cur += line_h
            if y_cur > code_rect.bottom():
                break

        painter.restore()
