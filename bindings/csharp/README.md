# C# binding

`soc.cs` contains the raw ABI 2 `DllImport` declarations. A higher-level managed
layer should use distinct handle types for `soc_context`, `soc_mesh`, and
`soc_snapshot`, but their ownership is not identical. A mesh handle must retain
its owning context's native lifetime until the mesh is released, for example with
`SafeHandle.DangerousAddRef`/`DangerousRelease` or an ownership layer that delays
context disposal while child meshes exist. Destroying a context also destroys all
meshes still attached to it, so a later release call through a stale mesh handle
is invalid. A snapshot is independent: it owns its depth pyramid and remains valid
after its source mesh is destroyed or its source context is resized or destroyed.

`soc_occlusion_build_desc.frame` is a borrowed pointer. Keep the frame descriptor,
group records, and transform arrays pinned or otherwise address-stable until the
synchronous `soc_occlusion_build` call returns. The resulting snapshot copies the
frame convention and retains none of those managed pointers.

Build one immutable snapshot with `soc_occlusion_build`, then call
`soc_snapshot_test_aabbs`, `soc_snapshot_get_build_stats`, or
`soc_snapshot_hiz_level_query`. Query counters are returned through
`soc_query_stats` for that call only; queries do not mutate the snapshot. Dispose
the result with `soc_snapshot_destroy`.

The current native implementation completes the build synchronously and uses the
single-threaded scalar rasterizer. The snapshot-shaped ABI leaves room for future
parallel or asynchronous builders without changing query ownership.

The native library base name is `libsoc`. Unity iOS builds may map the same entry
points to `__Internal`; desktop and Android builds load the platform-specific
`libsoc` binary.
