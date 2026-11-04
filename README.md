# NetBuild

NetBuild is a lightweight distributed C++ compilation tool for Linux, built with C++17 and SDL_net 3.

It uses UDP to discover available workers and TCP to distribute compilation jobs. Source files are preprocessed locally, compiled on remote workers using GCC, and linked on the coordinator.

## Dependencies

- GCC (g++)
- Meson
- SDL3
- SDL3_net
- nlohmann/json

## Build

```bash
meson setup build
meson compile -C build
```

## Usage

Start one or more workers:

```bash
./build/worker 41000 41001
./build/worker 41002 41003
```

Compile a C++ project:

```bash
./build/coordinator main.cpp math.cpp utils.cpp
```

Run the resulting executable:

```bash
./result_app
```

Worker addresses and UDP ports are currently configured in `src/coordinator.cpp`.

To accept connections from other computers on the local network, start a worker with a bind address:

```bash
./build/worker 41000 41001 0.0.0.0
```

Use remote workers only on trusted networks. All machines must have compatible GCC toolchains and target architectures.
