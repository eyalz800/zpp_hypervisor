#!/bin/bash
find {{project_root}} -name "*.cpp" -or -name "*.c" -or -name "*.h" | xargs clang-format -i -style=file
