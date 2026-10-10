;; sbcl_check.lisp — проверка Common Lisp-файла через SBCL.
;;
;; Запуск:  sbcl --noinform --non-interactive --load sbcl_check.lisp -- <файл>
;; Читает файл по top-level формам, каждую eval-ит с перехватом
;; WARNING/ERROR и печатает на stdout по строке на сообщение:
;;     <строка>:<колонка>:<severity>:<текст>
;; Позиция — начало top-level формы: SBCL не даёт точных позиций при
;; поштучном eval (колонка всегда 0).
(in-package :cl-user)

(defun line-col (stream pos)
  "Строка и колонка байтовой позиции POS в потоке (1-based)."
  (let ((saved (file-position stream)))
    (file-position stream 0)
    (let ((text (make-string pos)))
      (read-sequence text stream)
      (file-position stream saved)
      (let ((line 1) (col 0))
        (loop for ch across text
              do (if (char= ch #\Newline)
                     (progn (incf line) (setf col 0))
                     (incf col)))
        (cons line col)))))

(defun eval-form (form)
  "Возвращает список сообщений (severity . text) для формы или nil."
  (let ((msgs '()))
    (handler-bind ((warning (lambda (c)
                              (push (cons "warning" (write-to-string c)) msgs)
                              (muffle-warning c))))
      (handler-case
          (eval form)
        (error (c) (push (cons "error" (write-to-string c)) msgs))))
    (nreverse msgs)))

(defun check-file (path)
  "Все сообщения (line col severity text) для файла."
  (let ((results '()))
    (with-open-file (in path :element-type 'character :external-format :utf-8)
      (loop
        (let ((start (file-position in)))
          (handler-case
              (let ((form (read in nil :eof)))
                (when (eq form :eof) (return))
                (dolist (m (eval-form form))
                  (let ((lc (line-col in start)))
                    (push (list (car lc) (cdr lc) (car m) (cdr m))
                          results))))
            ;; Недочитанная форма (незакрытая скобка в конце файла) —
            ;; это синтаксическая ошибка, сообщаем о ней
            (end-of-file ()
              (let ((lc (line-col in start)))
                (push (list (car lc) (cdr lc) "error"
                            "unexpected end of file inside a form")
                      results))
              (return)))))
    (nreverse results))))

(let* ((path (first (cddr sb-ext:*posix-argv*)))
       (items (check-file path)))
  (dolist (it items)
    (format t "~d:~d:~a:~a~%"
            (first it) (second it) (third it) (fourth it)))
  (format t "~d problems~%" (length items)))
