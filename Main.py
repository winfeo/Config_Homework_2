import subprocess
import sys
import os


def get_dependencies(package_name, dependencies=None):
    """Рекурсивно получает все зависимости для указанного пакета."""
    if dependencies is None:
        dependencies = set()

    try:
        # Получение списка зависимостей для пакета
        output = subprocess.check_output(["apk", "info", "-R", package_name], text=True)
        lines = output.strip().split("\n")

        # Пропускаем первую строку, так как это название пакета
        for line in lines[1:]:
            dep = line.strip()

            # Пропуск строк, не являющихся именами пакетов
            if dep.endswith("depends on:") or not dep:
                continue

            if dep not in dependencies:
                dependencies.add(dep)
                # Рекурсивно получаем зависимости для текущей зависимости
                get_dependencies(dep, dependencies)
    except subprocess.CalledProcessError as e:
        print(f"Ошибка получения зависимостей для пакета {package_name}: {e}")
        sys.exit(1)

    return dependencies


def generate_plantuml(package_name, dependencies, output_file):
    """Генерирует файл PlantUML для визуализации зависимостей."""
    with open(output_file, "w") as f:
        f.write("@startuml\n")
        f.write(f'[{package_name}]\n')

        for dep in dependencies:
            f.write(f'[{package_name}] --> [{dep}]\n')

        f.write("@enduml\n")


def visualize_graph(plantuml_path, uml_file, output_image):
    """Запускает внешнюю программу для генерации изображения графа."""
    try:
        subprocess.check_call([plantuml_path, "-tpng", uml_file, "-o", os.path.dirname(output_image)])
        print(f"Граф зависимостей сохранен в файл: {output_image}")
    except subprocess.CalledProcessError as e:
        print(f"Ошибка при генерации изображения: {e}")
        sys.exit(1)


def main():
    if len(sys.argv) != 4:
        print("Использование: python script.py <path_to_plantuml> <package_name> <output_image>")
        sys.exit(1)

    plantuml_path = sys.argv[1]
    package_name = sys.argv[2]
    output_image = sys.argv[3]
    uml_file = "dependencies.puml"

    # Получение зависимостей
    dependencies = get_dependencies(package_name)

    # Генерация PlantUML файла
    generate_plantuml(package_name, dependencies, uml_file)

    # Генерация изображения графа
    visualize_graph(plantuml_path, uml_file, output_image)

    # Удаление временного PlantUML файла
    os.remove(uml_file)


if __name__ == "__main__":
    main()
