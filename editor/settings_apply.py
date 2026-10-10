"""
Применение настроек к окну редактора: тема, опции редакторов, терминалы,
шрифт интерфейса и переопределения LSP.

Вызывается из панели настроек (Apply/OK) и при старте приложения
(MainWindow.apply_settings_from_store). Никакого живого состояния не
хранит — каждый вызов перечитывает SettingsStore заново.
"""

import os

from PyQt6.QtGui import QFont
from PyQt6.QtWidgets import QApplication

from editor.constants import CODE_FONT_SIZE
from editor.font_settings import ui_font_family
from editor.settings_store import app_settings


# ---------------------------------------------------------------------------
# Итераторы по живым виджетам окна
# ---------------------------------------------------------------------------
def iter_editors(window):
    """Все редакторы всех панелей."""
    for pane in list(window.tile_manager._panes):
        try:
            for editor in pane.get_all_editors():
                yield editor
        except RuntimeError:
            continue


def iter_terminals(window):
    """Все терминалы всех панелей."""
    from editor.widgets.terminal import TerminalWidget
    for pane in list(window.tile_manager._panes):
        try:
            for i in range(pane.tabs.count()):
                widget = pane.tabs.widget(i)
                if isinstance(widget, TerminalWidget):
                    yield widget
        except RuntimeError:
            continue


# ---------------------------------------------------------------------------
# Опции отдельного редактора
# ---------------------------------------------------------------------------
def apply_editor_defaults(editor):
    """Применяет сохранённые настройки к новому редактору.

    Используется EditorPane.add_editor (новые вкладки наследуют настройки)
    и apply_settings (все открытые редакторы).
    """
    from PyQt6.Qsci import QsciScintilla
    store = app_settings()
    size = store.get_int("editor.code_font_size", CODE_FONT_SIZE)
    editor.zoomTo(size - CODE_FONT_SIZE)

    editor.setTabWidth(store.get_int("editor.tab_width", 4))
    editor.setMarginLineNumbers(2, store.get_bool("editor.show_line_numbers", True))
    editor.setWhitespaceVisibility(
        QsciScintilla.WhitespaceVisibility.WsVisible
        if store.get_bool("editor.show_whitespace", True)
        else QsciScintilla.WhitespaceVisibility.WsInvisible
    )
    editor.setCaretLineVisible(
        store.get_bool("editor.highlight_caret_line", False))
    editor.setAutoIndent(store.get_bool("editor.auto_indent", True))
    editor.setWrapMode(
        QsciScintilla.WrapMode.WrapCharacter
        if store.get_bool("editor.word_wrap", False)
        else QsciScintilla.WrapMode.WrapNone
    )
    editor.set_show_fold_margin(
        store.get_bool("editor.show_fold_margin", True))

    eol = store.get_str("editor.eol_mode", "lf")
    editor.setEolMode(
        QsciScintilla.EolMode.EolWindows
        if eol == "crlf" else QsciScintilla.EolMode.EolUnix
    )


# ---------------------------------------------------------------------------
# LSP-переопределения
# ---------------------------------------------------------------------------
def parse_args(text: str) -> list:
    """Строка-«по одному аргументу на строку» -> список аргументов."""
    args = []
    for line in (text or "").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            args.append(line)
    return args


def _override_for(language_id: str, path_key: str, args_key: str):
    """Собирает override (command, initial_options) для сервера из настроек;
    (None, None) — нет override."""
    from editor.lsp import config as lsp_config_module
    store = app_settings()
    path = store.get_str(path_key, "").strip()
    extra = parse_args(store.get_str(args_key, ""))
    base = lsp_config_module.lsp_config(language_id)
    if base is None:
        return None, None
    command = list(base.command)
    if language_id == "python":
        # basedpyright-langserver ищем рядом с выбранным интерпретатором
        from editor.python_env import selected_python
        info = selected_python()
        if info and info.get("path"):
            py_dir = os.path.dirname(info["path"])
            cand = os.path.join(py_dir, "Scripts", "basedpyright-langserver.exe")
            if os.path.isfile(cand):
                command[0] = cand
    if path and os.path.isfile(path):
        command[0] = path
    if extra:
        command.extend(extra)
    options = None
    if language_id == "cpp":
        # clangd не принимает --std в командной строке: стандарт передаём
        # через initializationOptions.fallbackFlags (используется для файлов
        # без записи в compile_commands.json).
        standard = store.get_str("lsp.clangd.standard", "").strip()
        if standard:
            options = {"fallbackFlags": [f"-std={standard}"]}
    elif language_id == "rust":
        # checkOnSave (cargo check по сохранению) передаётся через
        # initializationOptions; базовые опции сервера сохраняем.
        base = lsp_config_module.lsp_config("rust")
        ra = {}
        if base is not None:
            ra = dict((base.initial_options.get("rust-analyzer") or {}))
        ra["checkOnSave"] = {"enable": bool(
            store.get_bool("lsp.rust.check_on_save", True))}
        options = {"rust-analyzer": ra}
    elif language_id == "lumenlang":
        # Тумблеры lumen-lsp (анализ при сохранении, диагностики, inlay)
        # передаются через initializationOptions; базовые опции сохраняем.
        base = lsp_config_module.lsp_config("lumenlang")
        lm = {}
        if base is not None:
            lm = dict((base.initial_options.get("lumen") or {}))
        lm["checkOnSave"] = {"enable": bool(
            store.get_bool("lsp.lumen.check_on_save", True))}
        lm["deepAnalysis"] = {"enable": bool(
            store.get_bool("lsp.lumen.deep_analysis", True))}
        lm["diagnostics"] = {"enable": True}
        lm["inlayHints"] = {"enable": bool(
            store.get_bool("lsp.lumen.inlay_hints", True))}
        options = {"lumen": lm}
    return command, options


def _python_override_options():
    """initial_options для basedpyright: pythonPath = выбранный Python и
    режим тайпчекинга (typeCheckingMode) из панели настроек.

    Базовый analysis-блок (inlayHints, reportUnused*) берётся из
    конфига сервера, чтобы shallow-merge в lsp_config его не потерял.
    """
    from editor.lsp import config as lsp_config_module
    from editor.python_env import selected_python
    store = app_settings()

    base = lsp_config_module.lsp_config("python", ignore_enabled=True)
    analysis = {}
    if base is not None:
        analysis = dict(
            (base.initial_options.get("basedpyright") or {}).get("analysis")
            or {})
    mode = store.get_str("lsp.python.type_checking", "default")
    if mode and mode != "default":
        analysis["typeCheckingMode"] = mode

    opts = {}
    if analysis:
        opts["basedpyright"] = {"analysis": analysis}
    info = selected_python()
    if info and info.get("path"):
        opts["python"] = {"pythonPath": info["path"]}
    return opts or None


def _lisp_override():
    """Команда cl-lsp из настроек панели (path + extra args).

    В Windows cl-lsp — Roswell-скрипт без расширения: если пользователь
    указал не .exe/.bat/.cmd, запускаем его через 'ros -Q --'. В PATH,
    наоборот, ros работает сам, поэтому пустой путь оставляет базовую
    команду из конфига сервера.
    """
    from editor.lsp import config as lsp_config_module
    store = app_settings()
    path = store.get_str("lsp.cl_lsp_path", "").strip()
    extra = parse_args(store.get_str("lsp.cl_lsp_args", ""))
    base = lsp_config_module.lsp_config("lisp")
    if base is None:
        return None
    command = list(base.command)
    if path:
        if (os.name == "nt"
                and not path.lower().endswith((".exe", ".bat", ".cmd"))):
            command = ["ros", "-Q", "--", path]
        else:
            command = [path]
    if extra:
        command.extend(extra)
    return command


def apply_lsp_overrides():
    """Переписывает переопределения LSP-серверов из панели настроек."""
    from editor.lsp import config as lsp_config_module
    store = app_settings()
    lsp_config_module.set_lsp_enabled(
        store.get_bool("lsp.enabled", True))

    cmd, opts = _override_for(
        "cpp", "lsp.clangd_path", "lsp.clangd_args")
    lsp_config_module.set_user_override("cpp", cmd, opts)

    cmd, _ = _override_for(
        "python", "lsp.basedpyright_path", "lsp.basedpyright_args")
    opts = _python_override_options()
    lsp_config_module.set_user_override("python", cmd, opts)

    cmd = _lisp_override()
    lsp_config_module.set_user_override("lisp", cmd, None)

    cmd, opts = _override_for(
        "rust", "lsp.rust_analyzer_path", "lsp.rust_analyzer_args")
    lsp_config_module.set_user_override("rust", cmd, opts)

    cmd, opts = _override_for(
        "lumenlang", "lsp.lumen_path", "lsp.lumen_args")
    lsp_config_module.set_user_override("lumenlang", cmd, opts)

    lsp_config_module.set_feature_flags("cpp", {
        "error_lens": store.get_bool("lsp.clangd.error_lens", True),
        "hover": store.get_bool("lsp.clangd.hover", True),
        "inlay_hints": store.get_bool("lsp.clangd.inlay_hints", True),
    })
    lsp_config_module.set_feature_flags("python", {
        "error_lens": store.get_bool("lsp.python.error_lens", True),
        "hover": store.get_bool("lsp.python.hover", True),
        "inlay_hints": store.get_bool("lsp.python.inlay_hints", True),
    })
    lsp_config_module.set_feature_flags("lisp", {
        "error_lens": store.get_bool("lsp.lisp.error_lens", True),
        "hover": store.get_bool("lsp.lisp.hover", True),
    })
    lsp_config_module.set_feature_flags("rust", {
        "error_lens": store.get_bool("lsp.rust.error_lens", True),
        "hover": store.get_bool("lsp.rust.hover", True),
        "inlay_hints": store.get_bool("lsp.rust.inlay_hints", True),
    })
    lsp_config_module.set_feature_flags("lumenlang", {
        "error_lens": store.get_bool("lsp.lumen.error_lens", True),
        "hover": store.get_bool("lsp.lumen.hover", True),
        "inlay_hints": store.get_bool("lsp.lumen.inlay_hints", True),
    })


def restart_lsp(window):
    """Пересоздаёт LSP-сессии под новыми настройками (Apply в панели)."""
    mgr = window.lsp_manager
    editors = [ed for ed in iter_editors(window)]
    try:
        mgr._available.clear()
    except AttributeError:
        pass
    mgr.shutdown_all()
    for editor in editors:
        try:
            editor._lsp_bridge.attach()
        except Exception:
            pass


def apply_shortcuts(window):
    """Переприменяет настраиваемые горячие клавиши (страница Shortcuts).

    Проходит по всем QAction, помеченным _shortcut_id (menu_builder,
    main.py, detached_window.py), и ставит им шорткат из настроек; пустое
    значение отключает шорткат.
    """
    from PyQt6.QtGui import QAction
    from editor.shortcuts import SHORTCUT_DEFAULTS, parse_shortcut, shortcut_key
    for action in window.findChildren(QAction):
        sid = getattr(action, "_shortcut_id", None)
        if sid and sid in SHORTCUT_DEFAULTS:
            action.setShortcut(parse_shortcut(shortcut_key(sid)))


def apply_spotlight(window):
    """Включает/выключает «зенитный спотлайт» (страница View) во всех
    окнах — главном и деточках — и применяет прозрачность приглушения
    для тёмной и светлой темы."""
    from editor.spotlight import SpotlightController
    from editor.detached_window import all_detached_windows
    store = app_settings()
    enabled = store.get_bool("view.spotlight", False)
    dark_alpha = store.get_int("view.spotlight_dark_alpha", 230)
    light_alpha = store.get_int("view.spotlight_light_alpha", 190)
    skip_terminal = store.get_bool("view.spotlight_skip_terminal", False)
    skip_diag = store.get_bool("view.spotlight_skip_diagnostics", False)
    windows = [window]
    for detached in all_detached_windows():
        if detached is not window:
            windows.append(detached)
    for w in windows:
        controller = getattr(w, "spotlight", None)
        if isinstance(controller, SpotlightController):
            try:
                controller.set_enabled(enabled)
                controller.set_alphas(dark_alpha, light_alpha)
                controller.set_skip(skip_terminal, skip_diag)
            except RuntimeError:
                continue


# ---------------------------------------------------------------------------
# Полное применение
# ---------------------------------------------------------------------------
def apply_settings(window):
    """Применяет все сохранённые настройки к окну и серверам."""
    store = app_settings()

    # Тема: применяем только если пользователь явно выбрал её в панели
    # настроек (themes.current != "") — иначе сессия/меню остаются истиной.
    theme = store.get_str("themes.current", "")
    theme_applied = False
    if theme and theme in window.theme_manager.themes:
        window.apply_theme_to_all(theme)
        theme_applied = True

    # Шрифт интерфейса
    ui_size = store.get_int("general.ui_font_size")
    if ui_size > 0:
        QApplication.setFont(QFont(ui_font_family(), ui_size))
    # Если пользователь явно выбрал семейство UI-шрифта — пересобираем QSS,
    # где font-family зашит в стили (заголовок, вкладки, диалоги и т.п.).
    if store.get_str("general.ui_font_family", "").strip():
        try:
            window.apply_theme_to_all(window.current_theme)
        except Exception:
            pass

    # Опции редакторов + терминалы
    for editor in iter_editors(window):
        try:
            apply_editor_defaults(editor)
            # Переприменяем тему к лексеру, чтобы смена настройки
            # editor.underline_preprocessor вступила в силу сразу — но
            # только если apply_theme_to_all уже не темировал редакторы
            # (иначе двойное перекрашивание).
            if not theme_applied:
                cur = getattr(editor, "_current_theme", None)
                if cur:
                    editor.apply_theme(cur)
        except RuntimeError:
            continue
    for term in iter_terminals(window):
        try:
            term.set_font_size(store.get_int("terminal.font_size", 11))
        except RuntimeError:
            continue

    # Автосохранение: таймер и сохранение при потере фокуса (MainWindow)
    autosave_cfg = getattr(window, "configure_autosave", None)
    if callable(autosave_cfg):
        try:
            autosave_cfg()
        except Exception:
            pass

    # Горячие клавиши (страница Shortcuts)
    apply_shortcuts(window)

    # Зенитный спотлайт (страница View)
    apply_spotlight(window)

    # LSP: перезапускаем серверы только если переопределения/флаги/
    # включённость реально изменились — иначе косметические правки
    # (размер UI-шрифта) рвут работающие сессии на каждый Apply/OK.
    from editor.lsp import config as _lsp_cfg
    _langs = ("cpp", "python", "lisp", "rust", "lumenlang")
    _before = {_l: _lsp_cfg.lsp_config(_l) for _l in _langs}
    apply_lsp_overrides()
    _after = {_l: _lsp_cfg.lsp_config(_l) for _l in _langs}
    if _before != _after:
        restart_lsp(window)

    # Статус-бар: версия выбранного Python
    upd = getattr(window, "update_python_status", None)
    if upd is not None:
        try:
            upd()
        except Exception:
            pass
