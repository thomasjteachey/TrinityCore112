# Crash investigation: `SIGSEGV` with stack ending in `libc on_exit`

Date analyzed: 2026-03-27

## Symptom summary

Observed fatal log:

- `Caught fatal signal 11 (Segmentation fault)`
- stack includes `libc.so.6(on_exit+0)` and `__libc_start_main`
- worldserver frames appear at offsets around `+0xa983b7`, `+0xd3f6ef`, `+0xd3eae2`, `+0xec9ad7`

## What this strongly suggests

Because the trace includes `on_exit` in the unwind path, the crash is most likely happening during process teardown (or while unwinding through an exit-handler path), not during normal world update logic.

In TrinityCore, fatal signal handling installs `FatalSignalHandler`, logs the stack trace, restores default signal handling, and then re-raises the signal so native core-dump/debugger flow still happens.

That behavior means the displayed stack is from the faulting context, and can include libc shutdown machinery if the fault happens while destructors or cleanup handlers run.

## Relevant TrinityCore code paths

- Crash handlers are installed in worldserver startup (`Trinity::InitCrashSignalHandlers()`).
- `FatalSignalHandler` logs and then re-raises signal after resetting handler to default.
- `Crash()` intentionally dereferences null only in assert/fatal helper path, but your log indicates signal 11 happened first and was caught by signal handler.

## Most likely root-cause classes

1. **Use-after-free / invalid pointer during shutdown cleanup**
   - Typical when subsystems are destroyed out of dependency order.
2. **Double-free in module/plugin/static destructor**
   - Especially plausible if there are custom modules/scripts or patched branches.
3. **Race at shutdown**
   - Worker thread still touching memory after ownership moved/destroyed.
4. **ABI mismatch / stale binary vs libraries**
   - If worldserver binary and linked libs are not from same build set.

## How to get exact function+line for the offsets

Use the exact crashing binary and its debug symbols:

```bash
# Keep addresses from the crash and resolve against the same worldserver executable
addr2line -C -f -e /home/brokilodeluxe/wow/servers/tc-legionnaireplus/bin/worldserver 0xfaedb3 0xfaebe9 0xa983b7 0xd3f6ef 0xd3eae2 0xec9ad7 0x414b15

# If PIE is enabled and runtime addresses are needed, resolve with gdb from a core file:
gdb /home/brokilodeluxe/wow/servers/tc-legionnaireplus/bin/worldserver /path/to/core \
  -ex 'set pagination off' \
  -ex 'thread apply all bt full' \
  -ex 'info sharedlibrary' \
  -ex 'quit'
```

If `addr2line` returns `??:0`, install/build with debug symbols (`RelWithDebInfo` or `Debug`) and ensure symbols match the deployed binary exactly.

## Fast triage checklist

1. Verify binary and symbol parity (`sha256sum` of deployed worldserver vs symbolized one).
2. Reproduce shutdown path (clean stop, reload, script hot-reload if used).
3. Run with ASan/UBSan build in a staging env and repeat same action.
4. Disable custom modules/scripts temporarily to isolate branch-local regressions.
5. Capture first bad commit via `git bisect` if this started recently.

## Practical next step

Symbolize frame `[3]` (`+0xa983b7`) and `[4]/[5]` first. Those are usually the nearest user-code frames before libc teardown and will identify the concrete owner subsystem causing the segfault.

## Update after partial symbolization

The supplied symbols materially narrow the failing path:

- `Object::_ConcatFields(...) const`
- `WorldSession::LogoutPlayer(bool)`

`Object::_ConcatFields` is used in corpse persistence (`Corpse::SaveToDB`) to serialize corpse equipment fields. During logout, this can be reached on death/ghost transitions. This indicates the crash is likely a null/invalid `m_uint32Values` (or bounds issue) while serializing object update fields during logout teardown.

### Why the DWARF warning matters

`addr2line: DWARF error: invalid or unhandled FORM value: 0x23` indicates symbol/debug-info mismatch or incompatible toolchain for the binary's DWARF version. Function names are still useful, but line-level resolution is unreliable until toolchain/symbol parity is fixed.

### Hardening applied in this branch

To prevent this exact segfault class from taking down the process while deeper root-cause hunting continues:

- Added null-pointer guard in `Object::_ConcatFields`.
- Added bounds guards for `startIndex/size` against `m_valuesCount`.
- Added explicit error logs with object GUID/type and requested range.
- Iteration now uses direct `m_uint32Values[...]` only after guards pass.

This keeps the server alive and emits actionable logs instead of crashing in logout-path corpse serialization.
