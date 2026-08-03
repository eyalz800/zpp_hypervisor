# Merges compile_commands.json from all sub-builds into the project root.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "
import json, glob, os
merged = []
for f in sorted(glob.glob(os.path.join('${BINARY_DIR}', '*/compile_commands.json'))):
    with open(f) as fp:
        merged.extend(json.load(fp))
with open(os.path.join('${SOURCE_DIR}', 'compile_commands.json'), 'w') as fp:
    json.dump(merged, fp, indent=2)
"
    )
else()
    message(WARNING "Python3 not found; skipping compile_commands.json merge")
endif()
