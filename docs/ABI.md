# ABI policy

The public ABI is declared only in `include/soc/soc.h`. Public symbols and
ABI-visible data layouts are compatibility commitments once the project
reaches its first stable release.

## Versioning

`soc_get_abi_version()` returns `SOC_ABI_VERSION`. The high 16 bits contain the
major version and the low 16 bits contain the minor version.

- A major-version change may remove or reinterpret ABI elements.
- A minor-version change may add functions, result codes, flags, or optional
  struct fields while preserving existing behavior.
- The native library package version is separate from the ABI version.

## Context lifetime

`soc_context_create()` creates an opaque context. A successful call transfers
ownership of that context to the caller. `soc_context_destroy()` releases it
and accepts a null pointer.

Callers initialize `soc_config.struct_size` to `sizeof(soc_config)`. A native
library may accept a larger structure from a newer caller but must reject a
structure too small for the fields it reads.

## Compatibility constraints

- Exported functions use the C calling convention represented by `SOC_CALL`.
- C# declarations use the native library base name `soc`.
- No exception, allocator-specific object, or internal pointer crosses the ABI.
- Input memory remains caller-owned for the duration of a call unless a
  function explicitly documents a different lifetime.
- New flags default to disabled, and all reserved data is initialized to zero.
- Batch APIs are preferred over per-object calls for managed/native interop.
