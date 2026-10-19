# AGENTS.md

QEMU 11.1.2 source tree, ARM/AArch64 focused (`target/arm`, `hw/arm`,
`tests/functional/arm`). The tree is a release tarball imported as a **single
git commit** with no remote and no tags: `git log`, `git blame`, `git describe`
and `get_maintainer.pl --git` have no useful history, so don't try to diff
against upstream. Work from the files.

## Build

```sh
mkdir build && cd build
../configure --target-list=arm-softmmu,aarch64-softmmu
make
```

- Default `./configure` builds **every** target; always pass `--target-list`
  for a focused build (this tree only needs the two above).
- Running `./configure` from the source root auto-creates `build/` plus a
  `GNUmakefile` wrapper and re-execs inside `build/`. It refuses to reuse an
  existing `build/` it did not create, and Meson refuses in-source artifacts,
  so keep builds in `build/` or a separate out-of-tree directory.
- Meson always comes from `build/pyvenv/bin/meson` (never `PATH`); Ninja is
  probed on `PATH` (`ninja`, `ninja-build`, or `samu`). Use `build/run <cmd>`
  to get the venv and built binaries on `PATH`.
- Host prerequisites: `pkg-config`, `ninja-build`, `python3-venv`,
  `libglib2.0-dev`, `libpixman-1-dev` (plus `gcc`, `make`, `python3`). Missing
  `python3-venv` makes `configure` fail at "python venv creation failed".
- `make` needs `-j$(nproc)` for parallelism; Ninja parallelizes by default.
- `roms/` and `tests/lcitool/libvirt-ci` are vendored as ordinary tracked
  files, not real git submodules — `git submodule update` is a no-op here.

## Tests

Build the emulators first (`make`) or some tests fail with obscure errors.
`make check-help` lists every target.

```sh
make check                  # block + qapi-schema + unit + softfloat + qtest + decodetree
make check-unit             # tests/unit
make check-qtest-arm        # qtests for arm-softmmu (suite qtest-<target>)
make check-functional-arm   # ARM functional tests; forces SPEED=thorough
make check-rust             # Rust integration tests, if --enable-rust
```

Single tests from the build directory:

```sh
pyvenv/bin/meson test qtest-arm/arm-cpu-features   # one qtest binary
pyvenv/bin/meson test check-qdict                  # one unit test
QEMU_TEST_QEMU_BINARY=qemu-system-arm \
  ./run ../tests/functional/arm/test_virt.py ArmVirtMachine.test_arm_virt
```

Run functional tests through `./run` (or `meson devenv`), not bare `python3`:
it supplies the `qemu_test` module on `PYTHONPATH`. Use `-k <pattern>` to
filter by test name.

Block tests use golden output; run a single case from the build directory:

```sh
cd tests/qemu-iotests && ./check -qcow2 001
```

Functional-test quirks:
- Tests download guest images/assets into `~/.cache/qemu/download`
  (`QEMU_TEST_CACHE_DIR` overrides). `make check-functional` precaches and sets
  `QEMU_TEST_NO_DOWNLOAD=1`.
- Large-image tests are skipped unless `QEMU_TEST_ALLOW_LARGE_STORAGE=1`.
- `QEMU_TEST_KEEP_SCRATCH=1` preserves scratch/logs under
  `build/tests/functional/<arch>/`.
- Slow hosts: `TIMEOUT_MULTIPLIER=3 make check`. Speed selection is
  `SPEED=quick|slow|thorough` (default quick).

Golden-output regeneration (review the diff before committing):
- `QEMU_TEST_REGENERATE=1 make check-qapi-schema`
- `QEMU_TEST_REGENERATE=1 make check-tracetool`
- ACPI tables: add changed names to `tests/qtest/bios-tables-test-allowed-diff.h`,
  run `make check V=2`, then `tests/data/acpi/rebuild-expected-aml.sh`; the
  allowed-diff list must be empty before a series is sent.

## Adding code

- A new `hw/arm/*.c` file needs an entry in `hw/arm/meson.build` **and** a
  Kconfig symbol in `hw/arm/Kconfig` (boards gate on `CONFIG_*`; a board with
  `default y` is built unless disabled in
  `configs/devices/<target>/default.mak`). Same pattern applies to other
  `hw/*` and `target/*` directories.
- Register new tests where they are enumerated, not just by adding a file:
  `tests/unit/meson.build`, `tests/qtest/meson.build` (per-arch `qtests_*`
  lists), and `tests/functional/arm/meson.build` (`tests_arm_system_quick` /
  `tests_arm_system_thorough`).
- `target/arm` builds from source-sets (`arm_ss`, `arm_common_ss`,
  `arm_system_ss`, ...) in `target/arm/meson.build`; pick the set matching
  whether the code is user-mode, system-mode, or both.
- Instruction decode is generated from `target/arm/tcg/*.decode` by
  `scripts/decodetree.py` (`docs/devel/decodetree.rst`); do not hand-edit
  generated decoders.
- QAPI commands/types come from `qapi/*.json` (`docs/devel/qapi-code-gen.rst`).
- Trace events live in per-directory `trace-events`; include the local
  `"trace.h"` (`docs/devel/tracing.rst`).
- Never edit generated output in `build/` (trace headers, QAPI C, config
  headers). `target/arm/cpu-sysregs.h.inc` is checked in, not generated.

## Style and checks

- 4-space indent, no tabs (except Makefiles/asm), ~80 columns; `.editorconfig`
  is authoritative.
- `scripts/checkpatch.pl --terse -f path/to/file.c` for a file,
  `scripts/checkpatch.pl <patch>` for a patch. Warnings on untouched code are
  normal; errors are not.
- Commits need `Signed-off-by:` (`.gitlab-ci.d/check-dco.py`); QEMU uses an
  email-list workflow (`docs/devel/submitting-a-patch.rst`), not PRs.
- Rust (optional, `--enable-rust`, MSRV 1.83.0): use `make clippy`,
  `make rustdoc`, `make rustfmt`, `make check-rust`. Plain `cargo test` cannot
  build these tests because they need QEMU's generated C.

## Reference docs

`docs/devel/style.rst`, `docs/devel/build-system.rst`,
`docs/devel/codebase.rst`, `docs/devel/testing/{main,qtest,functional}.rst`,
`docs/devel/kconfig.rst`.
