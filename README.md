# soc

`soc` is a C17 library for CPU software rasterization and software occlusion
culling. Its public surface is a stable C ABI intended for native callers and
Unity/C# interop.

The project currently contains the buildable library skeleton: ABI versioning,
an opaque context, internal module boundaries, and ABI smoke tests. Rasterization
and occlusion algorithms will be added behind this interface.

## Build

```sh
cmake -S . -B build -DSOC_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Set `SOC_BUILD_SHARED=OFF` when a static library is required, such as for some
Unity iOS or WebGL builds.

## Layout

- `include/soc/`: stable public C ABI
- `src/abi/`: ABI entry points and boundary validation
- `src/core/`: context and library orchestration
- `src/raster/`: CPU rasterization implementation
- `src/occlusion/`: occlusion and depth hierarchy implementation
- `src/math/`: internal scalar and SIMD math
- `src/platform/`: platform, CPU feature, and threading adapters
- `bindings/csharp/`: managed interop layer
- `tests/`: unit, integration, and ABI tests
- `benchmarks/`: performance benchmarks
- `examples/`: C and Unity integration examples
- `docs/`: architecture, ABI, and naming rules

See [docs/NAMING.md](docs/NAMING.md) and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) before adding a new module or
public API.
