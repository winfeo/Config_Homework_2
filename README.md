# Домашняя работа №2
Визуализация графа зависимостей
## Вариант 22

### Задание
Разработать инструмент командной строки для визуализации графа зависимостей, включая транзитивные зависимости. Сторонние средства для получения зависимостей использовать нельзя. Зависимости определяются по имени пакета ОС Alpine Linux (apk). Для описания графа зависимостей используется представление PlantUML. Визуализатор должен выводить результат в виде сообщения об успешном выполнении и сохранять граф в файле формата png.

Ключами командной строки задаются:
- Путь к программе для визуализации графов.
- Имя анализируемого пакета.
- Путь к файлу с изображением графа зависимостей.

Все функции визуализатора зависимостей должны быть покрыты тестами.

## Описание
Инструмент командной строки для визуализации графа зависимостей пакетов ОС Alpine Linux. Инструмент анализирует зависимости пакетов и генерирует граф в формате PlantUML, который затем преобразуется в изображение формата PNG.

### Возможности
- Анализ зависимостей пакетов ОС Alpine Linux.
- Генерация графа зависимостей в формате PlantUML.
- Визуализация графа в формате PNG.
- Поддержка транзитивных зависимостей.

### Подготовка к работе
Для работы инструмента требуется Python и библиотека `subprocess`. Убедитесь, что у вас установлен Java для запуска `PlantUML`. Также требуется файл `APKINDEX` с зависимостями.

### Использование
Инструмент запускается из командной строки с указанием следующих параметров:
- Путь к программе для визуализации графов (PlantUML).
- Имя анализируемого пакета.
- Путь к файлу с изображением графа зависимостей.

Пример запуска:
`python dependency_visualizer.py -v path/to/plantuml.jar -p ncurses-dev -o output.png`

## Функции и методы файла Main.py
- parse_apkindex(apkindex_path) - парсит зависимости из файла APKINDEX.

![image](https://github.com/user-attachments/assets/2fe79991-fd96-4b97-981f-bcdef91a474b)

- resolve_dependencies(dependencies, package_name) - рекурсивно разрешает все транзитивные зависимости.

![image](https://github.com/user-attachments/assets/5733670e-ca84-4a07-81f0-f8dddfacb74d)

- generate_plantuml_graph(package_name, dependencies) - генерирует граф в формате PlantUML.

![image](https://github.com/user-attachments/assets/611ad66b-16f9-4018-a05a-0b700c6b7d68)

- visualize_plantuml(plantuml_content, plantuml_path, output_file) - сохраняет граф в формате PNG с помощью PlantUML.

![image](https://github.com/user-attachments/assets/c4048c05-4fbb-4c6a-af4b-673ff947fa56)

##Граф зависимостей
Результат выполнения программы, итоговый png файл.

![image](https://github.com/user-attachments/assets/2212e8be-5e26-453a-bd24-3efcb3833713)

## Тестирование
Модуль тестирования содержит комплексный набор тестов для функций визуализатора зависимостей. Тесты реализованы с использованием библиотеки `unittest` на Python.
Тесты:
- test_parse_apkindex - проверяет правильность парсинга зависимостей из файла APKINDEX.
- test_resolve_dependencies - проверяет правильность разрешения транзитивных зависимостей.
- test_generate_plantuml_graph - проверяет правильность генерации графа в формате PlantUML.
- test_visualize_plantuml - проверяет правильность сохранения графа в формате PNG.
- test_plantuml_jar_usage - проверяет корректность использования jar файла PlantUML.

![image](https://github.com/user-attachments/assets/70871d84-cf76-45a5-bd8e-1563fb8355c3)
