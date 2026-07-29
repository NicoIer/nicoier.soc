# Rendering-flow framework

The public frame API is synchronous and stateful:

```text
IDLE
  |
  | soc_frame_begin
  v
RECORDING_OCCLUDERS
  |
  | soc_occluders_submit (zero or more)
  | soc_occluders_finish
  v
QUERY_READY
  |
  | soc_visibility_test_aabbs (zero or more)
  | soc_hiz_level_query (zero or more)
  | soc_frame_end
  v
IDLE
```

Context resize and mesh creation or destruction are allowed only in `IDLE`.
Destroying the context is always allowed and releases its attached meshes.

## Current framework behavior

- `soc_frame_begin` validates and stores the frame description.
- `soc_occluders_submit` validates ownership and records input triangle counts.
- `soc_occluders_finish` calls the future Hi-Z construction hook.
- `soc_visibility_test_aabbs` writes `SOC_VISIBILITY_UNKNOWN` for every result.
- `soc_hiz_level_query` reports logical Level dimensions and returns a clear
  depth image until depth storage is implemented.
- `soc_frame_end` returns the context to `IDLE`.
- Statistics report submitted triangles, tested AABBs, and the logical Hi-Z
  Level count; rasterized and occluded counts remain zero.

This fail-open behavior allows the ABI, Unity integration, ownership, and call
ordering to be tested before the rasterization implementation exists.

## Future implementation hooks

The internal rasterizer already exposes hooks for:

1. frame initialization and depth clearing;
2. occluder transformation, clipping, binning, and depth writes;
3. Hi-Z hierarchy construction;
4. projected-AABB visibility testing;
5. frame cleanup.

These hooks are internal and can change without changing the public ABI.

## Hi-Z Level query

The query is valid only in `QUERY_READY`, after `soc_occluders_finish()` and
before `soc_frame_end()`.

First query metadata without a destination:

```c
soc_hiz_level_info info = {
    .struct_size = sizeof(soc_hiz_level_info),
};

soc_hiz_level_query(context, level, &info, NULL, 0u);
```

Then allocate `info.required_element_count` floats and query again:

```c
soc_hiz_level_query(
    context,
    level,
    &info,
    depth,
    info.required_element_count
);
```

Depth data is tightly packed and row-major. Level 0 uses the context
resolution; each following Level halves both dimensions with upward rounding,
until `1 x 1`. Forward-Z clear depth is `1.0`, and reversed-Z clear depth is
`0.0`.
