# NetBuild — Stage 1

Minimal single-task distributed GCC compilation demo using SDL3_net.

## Requirements

Linux, GCC/g++, Meson, SDL3, SDL3_net 3.0+, nlohmann-json.

## Build

```sh
meson setup build
meson compile -C build
```

Run the worker in terminal 1:

```sh
./build/worker
```

Run the coordinator in terminal 2, from the directory containing `main.cpp`:

```sh
./build/coordinator
./result_app
```

The source filename is fixed to `main.cpp`, the address to `127.0.0.1`, and the output to `result_app` in this stage.

## Protocol

UDP JSON `PING` / `PONG` (41000); TCP `JOB` / `RESULT` / `ERROR` (41001).
TCP framing: four-byte big-endian header length, JSON header, binary payload.

## Stage 1 limitations

* One source file, one worker, sequential handling, no retries.
* Workers are loopback-only. **Do not expose this compiler service to untrusted networks.**
* Output of GCC compilation errors is written to the worker's stderr.
* Preprocessing, remote object generation, then local linking. Compatible GCC target and ABI are assumed.
* Temporary `.ii` and `.o` files are deleted automatically on normal program unwinding.
* Network reads and writes have bounded timeouts and frame-size caps, but a local compiler process itself is not time-limited in this stage.

## Suggested manual checks

1. Run worker then coordinator; verify `result_app` executes.
2. Run coordinator without worker; expect a clean timeout.
3. Introduce a syntax error in `main.cpp`; expect a nonzero exit from preprocessing or worker compilation.
4. Send malformed TCP frames to the worker; it should close the connection and continue serving.
5. Use a working directory containing spaces in its pathname; building should still work.
