# ---------------------------------------------------------------------------
# LumenIDE — общие константы
# ---------------------------------------------------------------------------

# --- Шрифты ---
FONT_FAMILY = "Cascadia Code"       # шрифт интерфейса
CODE_FONT_FAMILY = "Fira Code"  # моноширинный шрифт редактора кода
CODE_FONT_SIZE = 20    # минимальный размер шрифта кода
UI_FONT_SIZE = 10        # шрифт интерфейса (QSS pt)
MENU_FONT_SIZE = 12      # меню в тайтл-баре (QSS pt)
TITLE_FONT_SIZE = 11     # заголовок окна (QSS pt)

# --- Кнопки окна (свернуть / развернуть / закрыть) ---
# qframelesswindow жёстко пишет setFixedSize(46, 32) и рисует иконки
# АБСОЛЮТНЫМИ координатами, рассчитанными на 46 px. Сужение виджета без
# перерисовки уводит иконки вправо от центра, поэтому ширину задаём сами
# и переносим рисунок в центр (см. apply_narrow_caption_buttons).
CAPTION_BUTTON_W = 34     # ширина кнопки окна (было 46)
CAPTION_ICON_BOX = 46     # геометрия, на которую рассчитана библиотека

# --- Редактор ---
DEFAULT_TAB_WIDTH = 4
SCROLLBAR_WIDTH = 20      # полоса шире бегунка, чтобы он «плавал» внутри
SCROLLBAR_HANDLE_INSET = 7  # отступ бегунка от краёв полосы: 20 - 2*7 = 6
SCROLLBAR_HANDLE_START = 6  # отступ от начала трека: сверху и слева
SCROLLBAR_HANDLE_END = 6    # отступ от конца трека: снизу и справа
SCROLLBAR_HIDE_DELAY_MS = 800   # пауза простоя перед автоскрытием полос
SCROLLBAR_FADE_MS = 180        # длительность затухания бегунка
CARET_WIDTH = 1
MARGIN_PADDING = 2       # отступ при расчёте ширины margin

# --- Коэффициенты оттенения (%) ---
# Направление зависит от типа темы: тёмная тема → светлее (lighter),
# светлая тема → темнее (darker).
EDITOR_BG_DARKEN = 107       # фон редактора относительно темы
SELECTED_TAB_DARKEN = 107    # выбранный таб
TAB_BAR_DARKEN = 104         # фон таб-бара
UNSELECTED_TAB_DARKEN = 103  # невыбранные табы
HOVER_TAB_DARKEN = 105       # таб при наведении
WHITESPACE_DOT_MIX = 0.2   # точки-пробелы: доля текста темы в смеси с фоном

# --- Цвета по умолчанию (fallback'и) ---
FALLBACK_BG = "#1e1e1e"
FALLBACK_FG = "#d4d4d4"
FALLBACK_BORDER = "#6c7086"
BLACK_FALLBACK = "#0d0d0d"   # для чистого #000000 (darker не работает)

# --- Отступы табов ---
TAB_PADDING_PANE = "7px 25px 7px 17px"     # EditorPane
TAB_PADDING_GLOBAL = "4px 20px 4px 12px"   # глобальный QSS

# --- Fold margin ---
FOLD_ICON_SIZE = 14   # ширина fold-маржина и размер иконок сворачивания

# --- Файловое дерево (sidebar) ---
SIDEBAR_DARKEN = 104   # фон панели дерева файлов относительно темы

# --- Тема по умолчанию ---
DEFAULT_THEME = "Monokai"

# --- Scintilla style ids ---
MAX_LEXER_STYLES = 128
STYLE_RANGE = 32  # стили 0-31 для SendScintilla
STYLE_DEFAULT = 32
