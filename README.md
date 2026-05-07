# KILN: Kiln Interprets Lists Natively

C/C++ build system with CMake as an input language.

## Why

Modern C/C++ development is editor-first:

* Language Servers rely on `compile_commands.json`
* Builds are incremental and frequent
* Developers expect fast, predictable errors

CMake is a mature and widely-supported build system generator, but the configure-then-generate model adds quarks and occationally causes troubles. `kiln` keeps CMake as the input language while changing the execution model:


* **It's a build system** - CMakeLists.txt is interpreted directly on every build. Gone are stale cache surprises, better integration, no per-build system jank. With aggressively invalidated cache for heavy built-ins
* **Faster interpretation** - kiln's interpreter is significantly faster than CMake's, with 10x+ speedups in some workloads
* **Better error messagess** - no more looking at cryptic errors and guessing where it originates from


## Building

The project has a few dependencies:

* CLI11 (https://github.com/CLIUtils/CLI11)
* Catch2 3.x (https://github.com/catchorg/Catch2)
* PCRE2 (https://github.com/PCRE2Project/pcre2)
* Glaze (https://github.com/stephenberry/glaze)
* pugixml (https://github.com/zeux/pugixml)
* libcurl
* libarchive
* linenoise
* C++23 capable compiler (GCC 13+)
* CMake (kiln is an execution engine, you still need the CMake shipped modules)

To build `kiln` for the first time using CMake:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Usage

`kiln` provides a modern, verb-based CLI inspired by tools like `cargo` or `go`.

### Basic Build
Build the current project (the `all` virtual target):
```bash
kiln
```

Build specific targets:
```bash
kiln my_lib my_app
```

### Running Targets
Build and execute an executable target in one step:
```bash
kiln run my_app -- --arg1 --arg2
```

### Testing
Run tests defined with `add_test()`:
```bash
kiln test                 # Run all tests
kiln test "RegexPattern"  # Run matching tests
```
Tests run in parallel with buffered output to keep the terminal clean.

### Installing
Install the project to a prefix:
```bash
kiln install
kiln install --prefix /usr/local
```

### Cleaning
Remove build artifacts:
```bash
kiln clean
```

### Options
Common flags:
- `-j N`: Set number of parallel jobs (defaults to CPU count)
- `-C <dir>`: Set project directory
- `-B <dir>`: Set build root directory
- `-P <script.cmake>`: Script mode (like CMake -P)
- `-DVAR=VAL`: Define a CMake variable
- `--config <debug|release|relwithdebinfo|minsizerel>`: Set build configuration
- `--profile`: Output a Chrome Trace Event Format compatible profile that can be loaded into [Perfetto](https://ui.perfetto.dev/) and others
- `--trace`: Print commands as they are executed
- `--trace-expand`: Like `--trace` but also shows variable expansion
- `--debugger`: Launch in debug mode (use `help` inside for commands, or see below)
- `--config-only`: Parse and cache only, skip the build step
- `--fresh`: Skip persistent cache (force re-evaluation of find_* and try_compile)

### Debugging CMake
Launch in debug mode (GDB-like commands with breaking and other features). The program is then immediately broken on the first command. Setup breakpoints here and use `c` or `continue` to start execution. Debug mode also automatically breaks on fatal errors.

```plaintext
❯ ./kiln .. --debugger
(kiln) /home/marty/Documents/kiln/CMakeLists.txt:1  cmake_minimum_required(VERSION 3.15)
(kiln) > help
Commands:
  break <line>         (b) Set breakpoint at line in current file
  break <file>:<line>  (b) Set location breakpoint
  break <command>      (b) Break on command name
  continue             (c) Run until next breakpoint
  step                 (s) Step to next command
  next                 (n) Step over (stay at current call depth)
  print <var>          (p) Print variable value
  backtrace            (bt) Show call stack
  list                 (l) Show source around current line/frame
  frame [N]            (f) Select stack frame N (show current if no arg)
  up                       Move up one stack frame (toward caller)
  down                     Move down one stack frame (toward callee)
  info variables           List all visible variables
  info breakpoints         List all breakpoints
  break-on-message <p> (bm) Break when message matches pattern
  watch <var>          (w) Break when variable changes
  delete <n>           (d) Delete breakpoint by ID
  quit                 (q) Exit kiln
  help                 (h) Show this help
(kiln) >
```

## Self Hosting

Since `kiln` uses CMake as its input language and is itself built using CMake, it can build itself!

```bash
# Assuming you have a kiln binary in build/
./build/kiln --config release
```

This will produce a release binary at `build/release/kiln`.

## Roadmap / TODOs

- [ ] clang support
- [ ] TCC (Tiny C Compiler) support
- [ ] ICC support
- Alternative language support
   - [ ] CUDA
   - [ ] HIP
   - [ ] SYCL
   - [ ] Fortran
   - [ ] Objective C(++)
   - [ ] C#
- [ ] Proper CMake policy support
- [ ] Running on RISC-V
- [ ] vcpkg support
- [ ] CPack support?
- [ ] Bootstrapping from just a compiler
- OS support
   - [ ] Remove Filesystem Hierarchy Standard assumptions
   - [ ] macOS support
   - [ ] FreeBSD support
   - [ ] OpenBSD support
   - [ ] Windows support (MSVC)
   - [ ] Windows support (Cygwin/MSYS2)
- [ ] C++ module support

Explicitely non-goals:

- Shipping CMake modules in Kiln (unless it breaks too often)
