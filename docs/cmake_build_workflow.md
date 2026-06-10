
# MiniDB Build Workflow

## 1. Configure the project
Run from the MiniDB project root.
```bash
cmake -S . -B build
```
- `-S .`: Use the current directory as the source root.
- `-B build`: Store generated build files in `build/`.
- Run this again when `CMakeLists.txt` changes.

## 2. Build the project
```bash
cmake --build build
```
- Compiles C++ source files and links the executable.
- Rebuilds what needs updating.

## 3. Run the executable
```bash
./build/minidb
```
- Executes the compiled MiniDB program.

## Typical workflow
Configure -> Build -> Run

After changing C++ code:
```bash
cmake --build build && ./build/minidb
```

After changing CMake configuration:
```bash
cmake -S . -B build && cmake --build build
```

## Important
- Run commands from the project root: `MiniDB/`.
- Keep generated files in `build/`, not alongside source files.
- Do not edit generated Makefiles or files inside `build/` manually.
