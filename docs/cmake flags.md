# CMake options for CLION

Build, Execution, Deployment > CMake > Debug-eabi
```
-DPICO_COPY_TO_RAM=1 -DPICO_CXX_ENABLE_EXCEPTIONS=0 -DPICO_NO_FLASH=0 -DPICO_NO_HARDWARE=0 -DCMAKE_C_FLAGS_DEBUG="-g -O8" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

`-DPICO_COPY_TO_RAM=1`
Builds firmware to execute RAM (faster in some cases, uses more RAM).
Recommended for can2040 CAN bus.

`-DPICO_CXX_ENABLE_EXCEPTIONS=0`
Disables C++ exceptions in Pico SDK build settings. Reduces code size/runtime overhead.
1 enables try/catch exception support.

`-DPICO_NO_FLASH=0`
Indicates flash is available/used normally.
1 is for no-flash workflows (e.g., RAM-only style configs).

`-DPICO_NO_HARDWARE=0`
Enables normal hardware support (GPIO, clocks, peripherals, etc.).
1 disables hardware-dependent parts for host/test-style builds.

`-DCMAKE_C_FLAGS_DEBUG="-g3 -Og"`
-g3 emits full debug info (more than plain -g), including macro info.
-Og enables optimizations that keep debugging reasonable.

`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`
Generates compile_commands.json in the build directory, used by CLion/clangd/tools for accurate per-file include paths, defines, and flags.
This helps avoid false editor errors like missing headers.
