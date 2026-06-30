# Codex Notes

## Module Build and Flash

Build a firmware module from the repo root with `tools/build.sh` and the module name:

```bash
./tools/build.sh <module_name>
```

Flash it with the helper from inside the `tools/` directory. The script uses a UF2 path relative to `tools/`, so
running `./tools/flash.sh <module_name>` from the repo root can fail with
`Could not open '../cmake-build-debug-eabi/modules/scp_<module_name>.uf2'`.

```bash
cd tools
./flash.sh <module_name>
```

If `picotool` reports `Failed to initialise libUSB`, rerun the flash command with USB/device access.
