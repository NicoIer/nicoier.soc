# Architecture

The library keeps the public ABI thin and independent from rasterization,
occlusion, and platform implementation details.

```text
Unity / C#
    |
    v
include/soc + src/abi
    |
    v
src/core
    |
    +--------> src/occlusion
                   |
                   v
               src/raster
                   |
                   +--------> src/math
                   |
                   +--------> src/platform
```

## Dependency rules

- `abi` validates external data and forwards work to `core`.
- `core` owns the opaque context and coordinates library subsystems.
- `occlusion` owns visibility policy and may consume rasterized depth data.
- `raster` owns clipping, triangle setup, depth testing, and raster storage.
- `math` and `platform` are internal leaf modules.
- Internal modules never depend on C# or Unity APIs.
- Public callers never include headers from `src/`.

The initial scalar implementation is the correctness reference. SIMD,
multithreaded, tiled, and hierarchical paths are added as internal
optimizations and must not require ABI changes.

Before public geometry submission APIs are added, the project must document
matrix layout, handedness, winding order, clip-space depth range, and
reversed-Z behavior.
