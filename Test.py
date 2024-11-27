import unittest
import os
import subprocess
from collections import defaultdict
from Main import parse_apkindex, resolve_dependencies, generate_plantuml_graph, visualize_plantuml

class TestDependencyVisualizer(unittest.TestCase):

    def setUp(self):
        # Создаем временный APKINDEX файл для тестов
        self.temp_apkindex_path = "test_apkindex.txt"
        with open(self.temp_apkindex_path, 'w') as f:
            f.write("P:ncurses-dev\n")
            f.write("D:libformw libmenuw libncursesw libpanelw pkgconfig\n")
            f.write("\n")
            f.write("P:libformw\n")
            f.write("D:ncurses-terminfo-base so:libc.musl-x86_64.so.1 so:libncursesw.so.6\n")
            f.write("\n")
            f.write("P:libmenuw\n")
            f.write("D:ncurses-terminfo-base so:libc.musl-x86_64.so.1 so:libncursesw.so.6\n")
            f.write("\n")
            f.write("P:libncursesw\n")
            f.write("D:ncurses-terminfo-base so:libc.musl-x86_64.so.1\n")
            f.write("\n")
            f.write("P:libpanelw\n")
            f.write("D:ncurses-terminfo-base so:libc.musl-x86_64.so.1 so:libncursesw.so.6\n")
            f.write("\n")
            f.write("P:pkgconfig\n")
            f.write("D:\n")
            f.write("\n")

    def tearDown(self):
        # Удаляем временный APKINDEX файл после тестов
        if os.path.exists(self.temp_apkindex_path):
            os.remove(self.temp_apkindex_path)

    def test_parse_apkindex(self):
        dependencies = parse_apkindex(self.temp_apkindex_path)
        expected_dependencies = {
            'ncurses-dev': ['libformw', 'libmenuw', 'libncursesw', 'libpanelw', 'pkgconfig'],
            'libformw': ['ncurses-terminfo-base', 'so:libc.musl-x86_64.so.1', 'so:libncursesw.so.6'],
            'libmenuw': ['ncurses-terminfo-base', 'so:libc.musl-x86_64.so.1', 'so:libncursesw.so.6'],
            'libncursesw': ['ncurses-terminfo-base', 'so:libc.musl-x86_64.so.1'],
            'libpanelw': ['ncurses-terminfo-base', 'so:libc.musl-x86_64.so.1', 'so:libncursesw.so.6'],
            'pkgconfig': []
        }
        self.assertEqual(dependencies, expected_dependencies)

    def test_resolve_dependencies(self):
        dependencies = parse_apkindex(self.temp_apkindex_path)
        resolved = resolve_dependencies(dependencies, 'ncurses-dev')
        expected_resolved = {
            'libformw', 'libmenuw', 'libncursesw', 'libpanelw', 'pkgconfig',
            'ncurses-terminfo-base', 'so:libc.musl-x86_64.so.1', 'so:libncursesw.so.6'
        }
        self.assertEqual(resolved, expected_resolved)

    def test_generate_plantuml_graph(self):
        dependencies = parse_apkindex(self.temp_apkindex_path)
        plantuml_content = generate_plantuml_graph('ncurses-dev', dependencies)
        expected_start = "@startuml\nskinparam linetype ortho\n"
        self.assertTrue(plantuml_content.startswith(expected_start))
        self.assertIn('class "ncurses-dev" as ncurses-dev', plantuml_content)
        self.assertIn('ncurses-dev --> libformw', plantuml_content)
        self.assertIn('@enduml', plantuml_content)

        # Сравниваем с ожидаемой структурой
        expected_puml = """@startuml
skinparam linetype ortho
class "ncurses-dev" as ncurses-dev {
}
class "libformw" as libformw {
}
class "ncurses-terminfo-base" as terminfo-base {
}
class "so:libc.musl-x86_64.so.1" as so_libc_musl-x86_64_so_1 {
}
class "so:libncursesw.so.6" as so_libncursesw_so_6 {
}
ncurses-dev --> libformw
libformw --> terminfo-base
libformw --> so_libc_musl-x86_64_so_1
libformw --> so_libncursesw_so_6
class "libmenuw" as libmenuw {
}
ncurses-dev --> libmenuw
libmenuw --> terminfo-base
libmenuw --> so_libc_musl-x86_64_so_1
libmenuw --> so_libncursesw_so_6
class "libncurses++" as libncursespp {
}
ncurses-dev --> libncursespp
libncursespp --> terminfo-base
libncursespp --> so_libc_musl-x86_64_so_1
libncursespp --> libformw
libncursespp --> "so:libgcc_s.so.1"
libncursespp --> "so:libmenuw.so.6"
libncursespp --> so_libncursesw_so_6
libncursespp --> "so:libpanelw.so.6"
libncursespp --> "so:libstdc++.so.6"
class "libncursesw" as libncursesw {
}
ncurses-dev --> libncursesw
libncursesw --> terminfo-base
libncursesw --> so_libc_musl-x86_64_so_1
class "libpanelw" as libpanelw {
}
ncurses-dev --> libpanelw
libpanelw --> terminfo-base
libpanelw --> so_libc_musl-x86_64_so_1
libpanelw --> so_libncursesw_so_6
class "pkgconfig" as pkgconfig {
}
ncurses-dev --> pkgconfig
@enduml"""

        self.assertEqual(plantuml_content.strip(), expected_puml.strip())

    def test_visualize_plantuml(self):
        dependencies = parse_apkindex(self.temp_apkindex_path)
        plantuml_content = generate_plantuml_graph('ncurses-dev', dependencies)
        output_file = "test_output.png"
        # Исправленный путь к plantuml.jar
        plantuml_jar_path = "C:\\Users\\User\\Desktop\\ДЗ_ИНФА\\Конфиг\\Config_Homework_2\\plantuml\\plantuml-lgpl-1.2024.7.jar"
        visualize_plantuml(plantuml_content, plantuml_jar_path, output_file)
        self.assertTrue(os.path.exists(output_file))
        os.remove(output_file)  # Удаляем созданный файл после теста

if __name__ == "__main__":
    unittest.main()