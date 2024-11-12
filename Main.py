import networkx as nx
import os
import argparse
import subprocess

def get_dependencies(package_name):
    """Получение зависимостей пакета."""
    result = subprocess.run(['apk', 'info', '--depends', package_name], capture_output=True, text=True)
    if result.returncode != 0:
        raise Exception(f"Failed to get dependencies for {package_name}: {result.stderr}")
    dependencies = result.stdout.strip().split('\n')
    return [dep.split(' ')[0] for dep in dependencies]

def build_dependency_graph(package_name):
    """Построение графа зависимостей."""
    G = nx.DiGraph()
    stack = [package_name]
    visited = set()

    while stack:
        current = stack.pop()
        if current in visited:
            continue
        visited.add(current)
        dependencies = get_dependencies(current)
        for dep in dependencies:
            G.add_edge(current, dep)
            if dep not in visited:
                stack.append(dep)

    return G

def generate_plantuml(graph):
    """Генерация PlantUML из графа."""
    plantuml = "@startuml\n"
    for node in graph.nodes:
        plantuml += f"package {node} {{\n}}\n"
    for edge in graph.edges:
        plantuml += f"{edge[0]} --> {edge[1]}\n"
    plantuml += "@enduml\n"
    return plantuml

def save_plantuml_to_png(plantuml, output_file, plantuml_path):
    """Сохранение PlantUML в PNG."""
    with open("temp.puml", "w") as f:
        f.write(plantuml)
    subprocess.run([plantuml_path, 'temp.puml'], check=True)
    os.rename("temp.png", output_file)
    os.remove("temp.puml")

def main():
    parser = argparse.ArgumentParser(description="Visualize Alpine Linux package dependencies.")
    parser.add_argument('--plantuml-path', required=True, help="Path to the PlantUML executable.")
    parser.add_argument('--package', required=True, help="Name of the package to analyze.")
    parser.add_argument('--output', required=True, help="Path to save the dependency graph image.")
    args = parser.parse_args()

    # Строим граф зависимостей
    graph = build_dependency_graph(args.package)

    # Генерируем PlantUML
    plantuml = generate_plantuml(graph)

    # Сохраняем PlantUML в PNG
    save_plantuml_to_png(plantuml, args.output, args.plantuml_path)

    print(f"Graph saved to {args.output}")

if __name__ == "__main__":
    main()