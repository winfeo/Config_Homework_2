import argparse
import os
import subprocess
from collections import defaultdict

def parse_apkindex(apkindex_path):
    """Парсит зависимости из файла APKINDEX."""
    dependencies = defaultdict(list)
    package = None

    with open(apkindex_path, 'r') as file:
        for line in file:
            line = line.strip()
            if line.startswith('P:'):
                package = line[2:].strip()
            elif line.startswith('D:') and package:
                deps = line[2:].strip().split()
                dependencies[package].extend(dep.split('=')[0] for dep in deps)
            elif not line:
                package = None  # Конец записи о пакете

    return dependencies

def resolve_dependencies(dependencies, package_name, resolved=None, seen=None):
    """Рекурсивно разрешает все транзитивные зависимости."""
    if resolved is None:
        resolved = set()
    if seen is None:
        seen = set()

    if package_name in seen:
        return resolved
    seen.add(package_name)

    for dep in dependencies.get(package_name, []):
        if dep not in resolved:
            resolved.add(dep)
            resolve_dependencies(dependencies, dep, resolved, seen)

    return resolved

def generate_plantuml_graph(package_name, dependencies):
    """Генерирует граф в формате PlantUML."""
    graph_lines = ["@startuml", "skinparam linetype ortho"]
    all_dependencies = resolve_dependencies(dependencies, package_name)

    graph_lines.append(f'"{package_name}" --> "{package_name}"')  # Узел пакета

    for dep in all_dependencies:
        graph_lines.append(f'"{package_name}" --> "{dep}"')

    graph_lines.append("@enduml")
    return "\n".join(graph_lines)

def visualize_plantuml(plantuml_content, plantuml_path, output_file):
    """Сохраняет граф в формате PNG с помощью PlantUML."""
    uml_file = "temp_graph.puml"
    with open(uml_file, 'w') as file:
        file.write(plantuml_content)

    try:
        subprocess.run([plantuml_path, uml_file, "-o", os.path.dirname(output_file)], check=True)
        os.rename(os.path.join(os.path.dirname(uml_file), "temp_graph.png"), output_file)
    finally:
        os.remove(uml_file)

def main():
    parser = argparse.ArgumentParser(description="Генератор графов зависимости пакетов Alpine Linux.")
    parser.add_argument("-i", "--input", required=True, help="Путь к файлу APKINDEX")
    parser.add_argument("-p", "--package", required=True, help="Имя анализируемого пакета")
    parser.add_argument("-v", "--visualizer", required=True, help="Путь к программе для визуализации PlantUML")
    parser.add_argument("-o", "--output", required=True, help="Путь для сохранения графа зависимостей (PNG)")
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"Ошибка: файл {args.input} не найден.")
        return

    if not os.path.isfile(args.visualizer):
        print(f"Ошибка: визуализатор PlantUML {args.visualizer} не найден.")
        return

    dependencies = parse_apkindex(args.input)

    if args.package not in dependencies:
        print(f"Ошибка: пакет {args.package} не найден в APKINDEX.")
        return

    plantuml_content = generate_plantuml_graph(args.package, dependencies)
    visualize_plantuml(plantuml_content, args.visualizer, args.output)
    print(f"Граф зависимости для пакета {args.package} успешно сохранён в {args.output}")

if __name__ == "__main__":
    main()
