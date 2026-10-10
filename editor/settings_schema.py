"""
Схема панели настроек: категории слева (дерево) и страницы справа.

Дерево: категория без "children" — лист (одна страница); категория с
"children" — ветвь, дети — листовые страницы. Каждая страница — список
"items", описывающих контролы (тип и параметры).

Типы контролов (editor/settings_widgets.py):
  toggle    — тумблер (bool)
  text      — однострочное поле (str)
  multiline — многострочное поле (str) — доп. аргументы LSP и т.п.
  spin      — числовой ввод (int): min/max/unit
  combo     — выпадающий список (str): options
  radio     — радиокнопки (str): options [(value, label)]
  path      — поле пути с кнопкой обзора (str): mode file|directory, filter
  button    — кнопка-действие: action (обрабатывается диалогом)
  info      — статичный текст
"""

# Действия кнопок (обрабатываются SettingsDialog)
ACTION_RESET_ALL = "reset_all"
ACTION_RESTART_LSP = "restart_lsp"


def _shortcuts_items() -> list:
    """Пункты страницы Shortcuts из реестра editor/shortcuts.py."""
    from editor.shortcuts import SHORTCUT_DEFAULTS, SHORTCUT_ITEMS
    return [
        {
            "key": f"shortcuts.{s['id']}",
            "label": s["label"],
            "type": "shortcut",
            "default": SHORTCUT_DEFAULTS[s["id"]],
        }
        for s in SHORTCUT_ITEMS
    ]


SCHEMA: list = [
    {
        "key": "general",
        "title": "General",
        "items": [
            {
                "key": "general.ui_font_family",
                "label": "UI font",
                "type": "combo",
                "help": "Interface font. Applies to menus, tabs, the file "
                        "tree and all dialogs.",
            },
            {
                "key": "general.ui_font_size",
                "label": "UI font size",
                "type": "spin",
                "min": 8,
                "max": 20,
                "unit": " pt",
                "help": "Interface font size (applies to all widgets).",
            },
        ],
    },
    {
        "key": "editor",
        "title": "Editor",
        "items": [
            {
                "key": "editor.code_font_family",
                "label": "Code font",
                "type": "combo",
                "help": "Monospace font for editors, terminals and "
                        "annotation popups.",
            },
            {
                "key": "editor.code_font_size",
                "label": "Code font size",
                "type": "spin",
                "min": 10,
                "max": 40,
                "unit": " pt",
                "help": "Code font size for all open editors.",
            },
            {
                "key": "editor.tab_width",
                "label": "Tab width",
                "type": "spin",
                "min": 1,
                "max": 16,
                "help": "Width of a tab in spaces.",
            },
            {
                "key": "editor.eol_mode",
                "label": "Line endings",
                "type": "radio",
                "options": [
                    ("lf", "LF (Unix)"),
                    ("crlf", "CRLF (Windows)"),
                ],
            },
            {
                "key": "editor.show_line_numbers",
                "label": "Line numbers",
                "type": "toggle",
            },
            {
                "key": "editor.show_whitespace",
                "label": "Show whitespace",
                "type": "toggle",
            },
            {
                "key": "editor.highlight_caret_line",
                "label": "Highlight caret line",
                "type": "toggle",
            },
            {
                "key": "editor.auto_indent",
                "label": "Auto indent",
                "type": "toggle",
            },
            {
                "key": "editor.word_wrap",
                "label": "Word wrap",
                "type": "toggle",
            },
            {
                "key": "editor.show_fold_margin",
                "label": "Show fold margin",
                "type": "toggle",
                "help": "Show the fold gutter with fold/unfold arrows "
                        "next to the line numbers.",
            },
            {
                "key": "editor.scrollbar_auto_hide",
                "label": "Hide scrollbars when idle",
                "type": "toggle",
                "help": "Scrollbars fade out after a pause without mouse "
                        "movement, scrolling or typing. They keep their width, "
                        "so the text never reflows.",
            },
            {
                "key": "editor.scrollbar_fade",
                "label": "Fade scrollbars",
                "type": "toggle",
                "help": "Scrollbar handle fades out smoothly instead of "
                        "disappearing instantly.",
            },
            {
                "key": "editor.scrollbar_hide_delay",
                "label": "Scrollbar hide delay (ms)",
                "type": "int",
                "help": "Idle time in milliseconds before the scrollbars are "
                        "hidden.",
            },
            {
                "key": "editor.underline_preprocessor",
                "label": "Underline # directives",
                "type": "toggle",
                "help": "Underline preprocessor / macro directives "
                        "(#include, #define, …) in C/C++ files.",
            },
            {
                "key": "editor.fold_on_open",
                "label": "Fold on open",
                "type": "radio",
                "options": [
                    ("off", "Off"),
                    ("all", "Fold all"),
                    ("level1", "Fold level 1"),
                ],
                "help": "Automatically collapse code blocks when a file "
                        "is opened. Level 1 keeps the top-level structure "
                        "visible and folds nested blocks.",
            },
            {
                "key": "editor.trim_trailing_whitespace",
                "label": "Trim trailing whitespace on save",
                "type": "toggle",
                "help": "Remove spaces and tabs from the end of every "
                        "line when the file is saved.",
            },
        ],
    },
    {
        "key": "view",
        "title": "View",
        "items": [
            {
                "key": "view.spotlight",
                "label": "Spotlight (dim inactive panes)",
                "type": "toggle",
                "help": "Dim or wash out all panels except the active one, "
                        "so attention stays on the current split. When you "
                        "switch to another panel the dimming smoothly "
                        "moves over to it.",
            },
            {
                "key": "view.spotlight_dark_alpha",
                "label": "Spotlight opacity (dark themes)",
                "type": "spin",
                "min": 0,
                "max": 255,
                "unit": "/255",
                "help": "Opacity of the dimming layer for dark themes "
                        "(0 = fully transparent, 255 = opaque).",
            },
            {
                "key": "view.spotlight_light_alpha",
                "label": "Spotlight opacity (light themes)",
                "type": "spin",
                "min": 0,
                "max": 255,
                "unit": "/255",
                "help": "Opacity of the dimming layer for light themes "
                        "(0 = fully transparent, 255 = opaque).",
            },
            {
                "key": "view.spotlight_skip_terminal",
                "label": "Don't dim terminal panes",
                "type": "toggle",
                "help": "Leave a pane undimmed while its active tab is a "
                        "terminal. As soon as an editor tab is activated "
                        "in that pane, dimming returns.",
            },
            {
                "key": "view.spotlight_skip_diagnostics",
                "label": "Don't dim the diagnostics pane",
                "type": "toggle",
                "help": "Leave the pane with the diagnostics list "
                        "undimmed while the Diagnostics tab is active.",
            },
        ],
    },
    {
        "key": "autosave",
        "title": "Autosave",
        "items": [
            {
                "key": "autosave.enabled",
                "label": "Enable autosave",
                "type": "toggle",
                "help": "Automatically save modified files to disk "
                        "after the configured interval. Untitled tabs "
                        "are skipped.",
            },
            {
                "key": "autosave.interval",
                "label": "Autosave interval",
                "type": "spin",
                "min": 5,
                "max": 600,
                "unit": " s",
                "help": "How often modified files are saved "
                        "(in seconds).",
            },
            {
                "key": "autosave.on_focus_lost",
                "label": "Save when window loses focus",
                "type": "toggle",
                "help": "Also save all modified files when the editor "
                        "window loses focus (for example when you switch "
                        "to another application).",
            },
        ],
    },
    {
        "key": "terminal",
        "title": "Terminal",
        "items": [
            {
                "key": "terminal.font_size",
                "label": "Terminal font size",
                "type": "spin",
                "min": 8,
                "max": 32,
                "unit": " pt",
            },
            {
                "key": "terminal.info",
                "label": "",
                "type": "info",
                "text": "Changes apply to terminals that are open now "
                        "and to new ones created afterwards.",
            },
        ],
    },
    {
        "key": "lsp",
        "title": "LSP",
        "children": [
            {
                "key": "lsp.general",
                "title": "General",
                "items": [
                    {
                        "key": "lsp.enabled",
                        "label": "Enable language servers",
                        "type": "toggle",
                        "help": "Master switch for all LSP features.",
                    },
                    {
                        "key": "lsp.info",
                        "label": "",
                        "type": "info",
                        "text": "Server path and extra arguments are used "
                                "when an LSP session starts. Press "
                                "Restart LSP to apply to running editors.",
                    },
                    {
                        "key": "lsp.restart",
                        "label": "Restart LSP servers now",
                        "type": "button",
                        "action": ACTION_RESTART_LSP,
                    },
                ],
            },
            {
                "key": "lsp.clangd",
                "title": "clangd (C/C++)",
                "items": [
                    {
                        "key": "lsp.clangd_path",
                        "label": "Server path",
                        "type": "path",
                        "mode": "file",
                        "filter": "clangd (clangd.exe);;All Files (*)",
                        "help": "Path to clangd.exe. Empty = auto-detect "
                                "(PATH / common LLVM locations).",
                    },
                    {
                        "key": "lsp.clangd_args",
                        "label": "Extra arguments",
                        "type": "multiline",
                        "height": 80,
                        "placeholder": "one argument per line,\n"
                                       "e.g.\n--background-index\n"
                                       "--log=error",
                        "help": "Appended to the default clangd command line.",
                    },
                    {
                        "key": "lsp.clangd.standard",
                        "label": "C++ standard",
                        "type": "combo",
                        "help": "Standard passed to clangd via --std=. "
                                "Auto = clangd decides (compile_commands.json "
                                "or its default). The list is detected from "
                                "the installed clang.",
                    },
                    {
                        "key": "lsp.clangd.error_lens",
                        "label": "ErrorLens (inline diagnostics)",
                        "type": "toggle",
                        "help": "Show diagnostics right in the code: "
                                "underlines and messages at the end of "
                                "the line.",
                    },
                    {
                        "key": "lsp.clangd.hover",
                        "label": "Hover tooltips",
                        "type": "toggle",
                        "help": "Show type/signature popups while hovering "
                                "over code.",
                    },
                    {
                        "key": "lsp.clangd.inlay_hints",
                        "label": "Inlay hints",
                        "type": "toggle",
                        "help": "Show inline type annotations "
                                "(auto a: int = 10) from the server.",
                    },
                    {
                        "key": "editor.auto_import.cpp",
                        "label": "Auto-import (#include)",
                        "type": "toggle",
                        "help": "When completing a symbol from a header that "
                                "is not yet included, add the #include line "
                                "automatically at the top of the file.",
                    },
                ],
            },
            {
                "key": "lsp.python",
                "title": "basedpyright (Python)",
                "items": [
                    {
                        "key": "lsp.python_path",
                        "label": "Python interpreter",
                        "type": "combo",
                        "help": "Python used by basedpyright for analysis. "
                                "Detected from the py launcher and the "
                                "registry; the selected version is shown in "
                                "the status bar.",
                    },
                    {
                        "key": "lsp.basedpyright_path",
                        "label": "Server path",
                        "type": "path",
                        "mode": "file",
                        "filter": "basedpyright-langserver (*.exe);;All Files (*)",
                        "help": "Path to basedpyright-langserver(.exe). "
                                "Empty = auto-detect.",
                    },
                    {
                        "key": "lsp.basedpyright_args",
                        "label": "Extra arguments",
                        "type": "multiline",
                        "height": 80,
                        "placeholder": "one argument per line,\n"
                                       "e.g.\n--verbose",
                        "help": "Appended to the default basedpyright "
                                "command line (after --stdio).",
                    },
                    {
                        "key": "lsp.python.type_checking",
                        "label": "Type checking",
                        "type": "radio",
                        "options": [
                            ("default", "Default"),
                            ("off", "Off"),
                            ("basic", "Basic"),
                            ("standard", "Standard"),
                            ("strict", "Strict"),
                        ],
                        "help": "Type checking mode sent to basedpyright. "
                                "Off disables type diagnostics (syntax "
                                "errors stay); Strict enables the most "
                                "strict checks.",
                    },
                    {
                        "key": "lsp.python.error_lens",
                        "label": "ErrorLens (inline diagnostics)",
                        "type": "toggle",
                        "help": "Show diagnostics right in the code: "
                                "underlines and messages at the end of "
                                "the line.",
                    },
                    {
                        "key": "lsp.python.hover",
                        "label": "Hover tooltips",
                        "type": "toggle",
                        "help": "Show type/signature popups while hovering "
                                "over code.",
                    },
                    {
                        "key": "lsp.python.inlay_hints",
                        "label": "Inlay hints",
                        "type": "toggle",
                        "help": "Show inline type annotations "
                                "(auto a: int = 10) from the server.",
                    },
                    {
                        "key": "editor.auto_import.python",
                        "label": "Auto-import (import)",
                        "type": "toggle",
                        "help": "When completing a symbol that needs an "
                                "import, add the import line automatically "
                                "at the top of the file.",
                    },
                ],
            },
            {
                "key": "lsp.cl_lsp",
                "title": "cl-lsp (Common Lisp)",
                "items": [
                    {
                        "key": "lsp.cl_lsp_path",
                        "label": "Server path",
                        "type": "path",
                        "mode": "file",
                        "filter": "cl-lsp (*.exe *.bat *.cmd);;All Files (*)",
                        "help": "Path to cl-lsp (the Roswell script or the "
                                "server executable). On Windows a script "
                                "without .exe/.bat is launched through "
                                "'ros -Q --'. Empty = auto-detect "
                                "(PATH / ~/.roswell).",
                    },
                    {
                        "key": "lsp.cl_lsp_args",
                        "label": "Extra arguments",
                        "type": "multiline",
                        "height": 80,
                        "placeholder": "one argument per line,\n"
                                       "e.g.\n--log=debug",
                        "help": "Appended to the default cl-lsp command line.",
                    },
                    {
                        "key": "lsp.sbcl_path",
                        "label": "SBCL path (check)",
                        "type": "path",
                        "mode": "file",
                        "filter": "SBCL (sbcl.exe);;All Files (*)",
                        "help": "Path to sbcl.exe for the local check "
                                "(Ctrl+Shift+L). cl-lsp sends no "
                                "diagnostics, so the file is checked by "
                                "running SBCL directly. Empty = auto-detect "
                                "(PATH / ~/.roswell/impls).",
                    },
                    {
                        "key": "lsp.lisp.error_lens",
                        "label": "ErrorLens (inline diagnostics)",
                        "type": "toggle",
                        "help": "Show diagnostics right in the code: "
                                "underlines and messages at the end of "
                                "the line.",
                    },
                    {
                        "key": "lsp.lisp.hover",
                        "label": "Hover tooltips",
                        "type": "toggle",
                        "help": "Show type/signature popups while hovering "
                                "over code.",
                    },
                ],
            },
            {
                "key": "lsp.rust",
                "title": "rust-analyzer (Rust)",
                "items": [
                    {
                        "key": "lsp.rust_analyzer_path",
                        "label": "Server path",
                        "type": "path",
                        "mode": "file",
                        "filter": "rust-analyzer (rust-analyzer.exe);;All Files (*)",
                        "help": "Path to rust-analyzer. Empty = auto-detect "
                                "(PATH / ~/.cargo/bin). Usually installed via "
                                "'rustup component add rust-analyzer'.",
                    },
                    {
                        "key": "lsp.rust_analyzer_args",
                        "label": "Extra arguments",
                        "type": "multiline",
                        "height": 80,
                        "placeholder": "one argument per line",
                        "help": "Appended to the default rust-analyzer "
                                "command line.",
                    },
                    {
                        "key": "lsp.rust.check_on_save",
                        "label": "Check on save",
                        "type": "toggle",
                        "help": "Run Rust compiler diagnostics on save "
                                "(cargo check). Off only parses the open "
                                "file, which is faster.",
                    },
                    {
                        "key": "lsp.rust.error_lens",
                        "label": "ErrorLens (inline diagnostics)",
                        "type": "toggle",
                        "help": "Show diagnostics right in the code: "
                                "underlines and messages at the end of "
                                "the line.",
                    },
                    {
                        "key": "lsp.rust.hover",
                        "label": "Hover tooltips",
                        "type": "toggle",
                        "help": "Show type/signature popups while hovering "
                                "over code.",
                    },
                    {
                        "key": "lsp.rust.inlay_hints",
                        "label": "Inlay hints",
                        "type": "toggle",
                        "help": "Show inline type annotations from "
                                "rust-analyzer (let x: i32 = 10).",
                    },
                ],
            },
            {
                "key": "lsp.lumen",
                "title": "lumen-lsp (Lumen)",
                "items": [
                    {
                        "key": "lsp.lumen_path",
                        "label": "Server path",
                        "type": "path",
                        "mode": "file",
                        "filter": "lumen-lsp (lumen-lsp.exe);;All Files (*)",
                        "help": "Path to lumen-lsp.exe. Empty = auto-detect "
                                "(next to the editor / PATH).",
                    },
                    {
                        "key": "lsp.lumen_args",
                        "label": "Extra arguments",
                        "type": "multiline",
                        "height": 80,
                        "placeholder": "one argument per line",
                        "help": "Appended to the default lumen-lsp command "
                                "line (after --stdio).",
                    },
                    {
                        "key": "lsp.lumen.check_on_save",
                        "label": "Check on save",
                        "type": "toggle",
                        "help": "Run the full Lumen analysis pipeline "
                                "(semantic + deep analysis) when the file "
                                "is saved. Diagnostics and inlay hints stay "
                                "enabled either way.",
                    },
                    {
                        "key": "lsp.lumen.deep_analysis",
                        "label": "Deep analysis",
                        "type": "toggle",
                        "default": True,
                        "help": "Run the deep analysis stage in lumen-lsp "
                                "(type inference, undefined variables, type "
                                "errors, inlay hints, type-aware completion). "
                                "Turn it off to make analysis much faster on "
                                "large files: only syntax errors and basic "
                                "highlighting stay, and completion suggests "
                                "keywords only. Takes effect after Restart "
                                "LSP.",
                    },
                    {
                        "key": "lsp.lumen.error_lens",
                        "label": "ErrorLens (inline diagnostics)",
                        "type": "toggle",
                        "help": "Show diagnostics right in the code: "
                                "underlines and messages at the end of "
                                "the line.",
                    },
                    {
                        "key": "lsp.lumen.hover",
                        "label": "Hover tooltips",
                        "type": "toggle",
                        "help": "Show type/signature popups while hovering "
                                "over code.",
                    },
                    {
                        "key": "lsp.lumen.inlay_hints",
                        "label": "Inlay hints",
                        "type": "toggle",
                        "help": "Show inline type annotations "
                                "(let a: auto = 10; -> a: i64) from "
                                "lumen-lsp.",
                    },
                ],
            },
        ],
    },
    {
        "key": "themes",
        "title": "Themes",
        "items": [
            {
                "key": "themes.current",
                "label": "Color theme",
                "type": "combo",
                "help": "Theme is applied immediately; a non-default "
                        "selection is remembered and applied at startup.",
            },
            {
                "key": "themes.reset",
                "label": "Reset all settings",
                "type": "button",
                "action": ACTION_RESET_ALL,
            },
            {
                "key": "themes.info",
                "label": "",
                "type": "info",
                "text": "Themes are JSON files in editor/themes. You can "
                        "add new themes there and they will appear here.",
            },
        ],
    },
    {
        "key": "shortcuts",
        "title": "Shortcuts",
        "items": _shortcuts_items(),
    },
    {
        "key": "about",
        "title": "About",
        "items": [
            {
                "key": "about.text",
                "label": "",
                "type": "info",
                        "text": "LumenIDE\n\n"
                                "A tile-based code editor built with PyQt6, "
                                "QScintilla and qframelesswindow.\n\n"
                                "Built-in LSP support for C/C++ (clangd), "
                                "Python (basedpyright), Common Lisp (cl-lsp) "
                                "and Rust (rust-analyzer), "
                                "built-in Lumen language "
                                "(.lm), terminals, tasks and custom themes.\n\n"
                                "Settings are stored in editor/settings.json.",
            },
        ],
    },
]


def iter_leaf_pages():
    """Категории-листья и дочерние страницы ветвей:
    (page_key, title, items) в порядке дерева."""
    for cat in SCHEMA:
        if "children" in cat:
            for child in cat["children"]:
                yield child["key"], child["title"], child.get("items", [])
        else:
            yield cat["key"], cat["title"], cat.get("items", [])


def page_by_key(page_key: str):
    for key, title, items in iter_leaf_pages():
        if key == page_key:
            return key, title, items
    return None


def categories():
    """Дерево категорий: (key, title, is_branch, children)."""
    tree = []
    for cat in SCHEMA:
        if "children" in cat:
            tree.append({
                "key": cat["key"],
                "title": cat["title"],
                "children": [
                    {"key": c["key"], "title": c["title"]}
                    for c in cat["children"]
                ],
            })
        else:
            tree.append({
                "key": cat["key"],
                "title": cat["title"],
                "children": [],
            })
    return tree
