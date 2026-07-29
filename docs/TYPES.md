# Public types

Public ABI data types are declared in `include/soc/soc_types.h`. They use
fixed-width scalar fields so their native and managed layouts can be verified.

## C and C# mapping

| C type | C# representation |
| --- | --- |
| `uint8_t`, `soc_bool`, `soc_visibility` | `byte` |
| `int32_t`, `soc_result` | `int` |
| `uint32_t` and typed constants based on it | `uint` |
| `uint64_t` | `ulong` |
| `float` | `float` |
| `const void*` | `IntPtr` |
| `soc_context*`, `soc_mesh*` | `IntPtr` or `SafeHandle` |

Do not marshal `soc_bool` as C# `bool`; its platform marshalling rules do not
guarantee the one-byte ABI layout used here.

## Matrix convention

`soc_mat4` contains four column vectors named `col0` through `col3`.
Positions are transformed as:

```text
clip_position = clip_from_world * world_position
```

The C# binding must copy Unity matrices according to this convention instead
of relying on the managed structure's physical layout.

## Visibility convention

`SOC_VISIBILITY_UNKNOWN` has value zero, making zero-initialized result buffers
fail open. Callers cull an object only when its result is exactly
`SOC_VISIBILITY_OCCLUDED`; both `UNKNOWN` and `VISIBLE` are rendered.

## Descriptor convention

Extensible descriptors start with `struct_size`. Callers zero-initialize the
complete structure, set `struct_size` to `sizeof` the caller's structure, and
then fill supported fields. Unknown flags and reserved values remain zero.

Pointers in `soc_mesh_desc` are borrowed input. The current framework validates
their metadata but does not retain or consume their payload. The rasterization
implementation will copy required data into native-owned storage during
`soc_mesh_create()`; the caller retains ownership of the input buffers.

## Hi-Z Level information

`soc_hiz_level_info` describes one queryable Level:

- `level` is zero for the base depth image.
- `width` and `height` are the selected Level dimensions.
- `required_element_count` is the number of tightly packed `float` elements
  required for a copy.

Callers initialize `struct_size` before both metadata-only and data-copy
queries.
