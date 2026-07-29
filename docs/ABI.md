# ABI policy

The public ABI is declared in `include/soc/soc.h`, with shared data layouts in
`include/soc/soc_types.h`. Public symbols and ABI-visible data layouts are
compatibility commitments once the project reaches its first stable release.

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

Callers initialize `soc_config.struct_size` to `sizeof(soc_config)`. Version 1
requires at least `SOC_CONFIG_SIZE_V1` bytes. A native library may accept a
larger structure from a newer caller but must reject a structure too small for
the fields it reads.

## Mesh lifetime

`soc_mesh_create()` creates an opaque mesh owned by its context.
`soc_mesh_destroy()` releases it. Destroying a context also releases any meshes
that remain attached to it. Mesh creation and destruction are allowed only
while the context is idle.

The current framework validates mesh metadata but does not yet copy or consume
vertex and index payloads. Required data will be copied into native-owned
storage when rasterization is implemented; callers retain ownership of their
input buffers.

## Frame lifetime

Calls follow this order:

```text
soc_frame_begin
soc_occluders_submit (zero or more calls)
soc_occluders_finish
soc_visibility_test_aabbs (zero or more calls)
soc_frame_end
```

Out-of-order calls return `SOC_RESULT_INVALID_STATE`. The framework currently
performs no rasterization, builds no Hi-Z levels, and returns
`SOC_VISIBILITY_UNKNOWN` for every tested AABB.

## Hi-Z image query

`soc_hiz_level_query()` copies a selected Level into caller-owned `float`
storage. It never exposes an internal depth pointer. A metadata-only call uses
`out_depth = NULL` and `out_depth_count = 0`; the returned
`soc_hiz_level_info.required_element_count` gives the required element count.

The query is valid only after `soc_occluders_finish()` and before
`soc_frame_end()`. An undersized destination returns
`SOC_RESULT_BUFFER_TOO_SMALL` while still returning Level metadata.

## Compatibility constraints

- Exported functions use the C calling convention represented by `SOC_CALL`.
- C# declarations use the native library base name `soc`.
- No exception, allocator-specific object, or internal pointer crosses the ABI.
- Input memory remains caller-owned for the duration of a call unless a
  function explicitly documents a different lifetime.
- New flags default to disabled, and all reserved data is initialized to zero.
- Batch APIs are preferred over per-object calls for managed/native interop.
