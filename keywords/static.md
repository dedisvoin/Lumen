# static
---

Модификатор статического элемента: переменная/функция принадлежит типу/namespace, а не экземпляру.
```lumen
struct Util {
    static fn twice(v: i32) -> i32 : { ret v * 2; }
}
echo Util::twice(21);
```
