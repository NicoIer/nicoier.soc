# Naming conventions

## Public ABI

- Public functions use `soc_<noun>_<verb>`, for example
  `soc_context_create`.
- Public types use lower snake case with the `soc_` prefix, for example
  `soc_context` and `soc_config`.
- Public macros and constants use upper snake case with the `SOC_` prefix,
  for example `SOC_ABI_VERSION` and `SOC_RESULT_OK`.
- Every exported symbol must be declared in `include/soc/soc.h`.
- Shared public data types are declared in `include/soc/soc_types.h`.
- ABI functions use `SOC_API` and `SOC_CALL`.

## Internal C code

- Source and header files use `soc_<module>.c` and `soc_<module>.h`.
- File-local functions are `static` and use lower snake case.
- Cross-file internal functions retain the `soc_` prefix. Add `_internal`
  when an internal operation backs a public function with the same name.
- Struct fields and local variables use lower snake case.
- Header guards use `SOC_<NAME>_H_INCLUDED`.
- A module must not expose internal headers outside `src/`.

## ABI data rules

- Use fixed-width integer types for ABI-visible data.
- Do not expose C `bool`, compiler-dependent enum storage, flexible arrays,
  platform handles, or internal object layouts.
- Public structs begin with `struct_size` when they may grow in later ABI
  versions.
- Reserved fields and unknown flags must be zero unless documented otherwise.
- Functions returning `soc_result` return `SOC_RESULT_OK` on success and a
  negative result on failure.
- The caller owns input buffers unless an API explicitly documents a transfer
  of ownership.
- Native code must not retain managed memory passed by C# unless the API
  explicitly establishes a pinned-lifetime contract.

## CMake

- CMake targets use lower-case names: `soc` and `soc_core`.
- Project options and compile definitions use the `SOC_` prefix.
- Public consumers link with the `soc::soc` alias.
