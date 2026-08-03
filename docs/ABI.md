# ABI 策略

公共 ABI 声明于 `include/soc/soc.h`，共享数据布局位于
`include/soc/soc_types.h`。当前 ABI 主版本为 2；它使用一次性
`soc_occlusion_build()` 构建不可变 `soc_snapshot`，不再公开分步帧状态机。

## 版本管理

`soc_get_abi_version()` 返回 `SOC_ABI_VERSION`。高 16 位表示主版本号，
低 16 位表示次版本号。

- 主版本变更可以移除或重新解释 ABI 元素；ABI 2 已移除 ABI 1 的
  begin/submit/finish/end 帧函数。
- 次版本变更可以添加函数、结果码、标志或可选结构字段，但必须保持现有行为。
- 原生库的软件包版本与 ABI 版本相互独立。

## 上下文与网格

`soc_context_create()` 创建一个不透明上下文，保存未来 build 使用的尺寸和网格。
`soc_context_resize()` 只改变未来 build 的尺寸，不会改变已经发布的 snapshot。
`soc_context_destroy()` 接受空指针，并释放仍附属于该 context 的网格。

`soc_mesh_create()` 同步验证位置和索引流，并将它们复制到库拥有的不可变存储中。
描述符和输入缓冲区只在调用期间借用；成功后调用方可以立即重用或释放原缓冲区。
创建失败时 `*out_mesh` 保持为空，context 不会挂接部分网格。

非空 `soc_occluder_group` 中的 mesh 必须由执行 build 的同一 context 创建。
mesh 和 transform 数组只需保持到 `soc_occlusion_build()` 返回。build 返回后，
得到的 snapshot 不再依赖任何 mesh。

## 一次性 build

`soc_occlusion_build_desc.frame` 是指向完整 `soc_frame_desc` 的同步借用指针，
`groups`、`group_count` 和 `group_stride` 描述整批遮挡物。通常将 `group_stride`
设为 `sizeof(soc_occluder_group)`；显式 stride 允许未来版本在 group 末尾追加字段。
frame 使用指针而不是按值内嵌，因此未来扩展 `soc_frame_desc` 不会移动 build 描述符
中后续字段的 ABI 偏移。

```c
soc_occluder_group groups[] = {
    {
        .mesh = wall_mesh,
        .object_to_world = wall_transforms,
        .instance_count = wall_count,
        .flags = SOC_OCCLUDER_GROUP_FLAG_NONE,
    },
};

soc_occlusion_build_desc build = {
    .struct_size = sizeof(soc_occlusion_build_desc),
    .flags = SOC_OCCLUSION_BUILD_FLAG_NONE,
    .frame = &frame,
    .groups = groups,
    .group_count = 1u,
    .group_stride = sizeof(soc_occluder_group),
};

soc_snapshot* snapshot = NULL;
soc_result result = soc_occlusion_build(context, &build, &snapshot);
```

frame 描述符、group 数组和实例 transform 数组只在同步 build 调用期间借用，必须
保持有效且不被修改直到函数返回。成功返回的 snapshot 会复制 frame 约定，不保留
这些输入指针。

`group_count == 0` 是合法的空 build；此时 `groups` 可以为空且
`group_stride` 可以为零。空 build 仍会生成具有清除深度和完整 Hi-Z 层级的
snapshot。group 的 `instance_count == 0` 时，该项不产生几何；其 mesh 和
transform 指针可以为空，但 flags 仍须有效。

当前实现同步完成全部工作：验证、清除 Level 0、逐实例变换、齐次裁剪、面剔除、
标量深度光栅化和 Hi-Z 构建都发生在 `soc_occlusion_build()` 返回之前。
当前 rasterizer 是单线程标量正确性路径，`worker_count > 1` 仍返回
`SOC_RESULT_UNSUPPORTED`。snapshot API 本身不表示当前已经异步、分块或 SIMD 化。

build 具有结果原子性。调用方提供有效 `out_snapshot` 时，函数首先将其置空；
任何验证、分配或执行失败都不会发布部分 snapshot，也不会改变已存在的 snapshot。

## Snapshot 生命周期

成功 build 返回的 `soc_snapshot` 原生拥有以下不可变数据：

- build 时使用的 frame 约定；
- Level 0 深度和全部派生 Hi-Z 层级；
- `soc_build_stats`。

snapshot 与创建它的 context 和 mesh 相互独立。可以先销毁 mesh、调整或销毁
context，再继续查询 snapshot；这些操作不会改变 snapshot 的尺寸、深度、frame
约定或统计。多个 snapshot 可以同时存在。

只读 snapshot 查询可以并发执行，但不得在查询仍进行时销毁同一 snapshot。
`soc_snapshot_destroy()` 接受空指针。

## Build 与查询统计

`soc_snapshot_get_build_stats()` 返回构建时冻结的 `soc_build_stats`：

- `input_triangle_count` 是所有非空 group、所有实例的源三角形数；
- `clipped_triangle_count` 是被齐次裁剪改变或拒绝的源三角形数；
- `rasterized_triangle_count` 是裁剪后通过朝向与退化检查的三角形数，不保证其在
  任一像素赢得深度测试；
- `hiz_level_count` 是该 snapshot 的实际层数。

`soc_snapshot_test_aabbs()` 的可选 `soc_query_stats` 只描述本次调用。
snapshot 不累计查询计数，因此查询不会改变 build stats，也不会使 snapshot
失去不可变性。成功调用时：

```text
tested_aabb_count == bounds_count
visible_aabb_count + occluded_aabb_count + unknown_aabb_count
    == tested_aabb_count
```

`bounds_count == 0` 是合法查询；bounds 和 visibility 指针可以为空，提供的合法
query stats 会被写为零。`out_stats == NULL` 表示不需要本次调用统计。

## 可见性与 Hi-Z 查询

`soc_snapshot_test_aabbs()` 同步投影世界空间 AABB 并保守查询 snapshot 的 Hi-Z。
只有能够严格证明完整投影位于遮挡深度之后时，结果才是
`SOC_VISIBILITY_OCCLUDED`。非有限、横跨近平面或无法安全投影的 AABB 返回
`SOC_VISIBILITY_UNKNOWN`；其余未被严格证明遮挡的 AABB 返回
`SOC_VISIBILITY_VISIBLE`。

`soc_snapshot_hiz_level_query()` 将选定层级复制到调用方的 `float` 缓冲区，绝不
暴露内部指针。元数据查询使用 `out_depth = NULL` 和 `out_depth_count = 0`。
目标缓冲区过小时返回 `SOC_RESULT_BUFFER_TOO_SMALL`，但仍填写有效的层级元数据。

第 0 层级采用紧密的行主序输出。后续层级尺寸为上一层的
`ceil(width / 2) x ceil(height / 2)`，直至 `1 x 1`。正向 Z 归约取最大值，
反向 Z 归约取最小值；NPOT 边缘只归约实际存在的子项。

## 兼容性约束

- 导出函数使用 `SOC_CALL` 指定的 C 调用约定。
- C# 原始绑定使用原生库基本名称 `libsoc`。
- 异常、分配器对象和内部数据指针不会跨越 ABI。
- 描述符和输出结构由调用方初始化 `struct_size`；未知 flags 和 reserved 字段为零。
- 除非函数明确记录不同生命周期，否则输入内存只在同步调用期间借用。
- 同一 context 的变更和 build 由调用方串行化；不可变 snapshot 可供并发只读查询。
