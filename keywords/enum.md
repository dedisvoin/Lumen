# enum
---

Объявляет перечисление — набор именованных констант (`Null`-значения-варианты).

Варианты нумеруются с нуля в порядке объявления. Обращение через `::`:
```lumen
enum Color { Red, Green, Blue }

echo Color::Red;         // 0
echo Color::Blue;        // 2
let fav = Color::Green;
echo fav;                // 1
```

Перечисления участвуют в сравнениях и `if`:
```lumen
if (fav == Color::Green) {
    echo "зелёный";
}
```

Вариант — это значение с типом самого перечисления:
```lumen
echo typeof(Color::Red);      // Color
```
