# 遮挡构建与查询流程

ABI 2 不再把一个可变 context 暴露为逐步推进的帧状态机。调用方通过借用的 frame
描述符指针和完整 occluder groups 一次提交全部输入，库同步生成一个不可变 snapshot：

```text
context + frame + all occluder groups
                  |
                  | soc_occlusion_build
                  v
          immutable soc_snapshot
             /        |        \
            v         v         v
  test AABBs   get build stats   query Hi-Z
            \         |         /
                  read-only
                     |
                     v
          soc_snapshot_destroy
```

## 当前实现

`soc_occlusion_build()` 当前按以下顺序同步执行：

1. 验证 build、frame、group stride、mesh owner 和计数；
2. 按 context 当前尺寸为候选 snapshot 分配完整深度金字塔；
3. 根据正向或反向 Z 清除 Level 0；
4. 依次处理所有非空 group 和实例；
5. 执行对象到世界、世界到裁剪空间的变换；
6. 在齐次裁剪体中裁剪三角形；
7. 执行背面和退化剔除，并以标量方式写入 Level 0 深度；
8. 同步构建全部派生 Hi-Z 层级；
9. 冻结 frame 与 build stats，并发布 snapshot。

这仍是同步、单线程、双精度中间计算的标量正确性路径。一次性 build 和 snapshot
所有权为未来的 SIMD、分块、多线程或异步构建提供稳定边界，但 ABI 2 的当前实现
尚未执行这些优化。

## Occluder groups

每个 `soc_occluder_group` 将一个不可变 mesh 与连续的 `object_to_world` 矩阵数组
配对。`soc_occlusion_build_desc` 的 group 数组代表本次 build 的完整遮挡物输入，
因此调用方应在一次调用中提供所有 group，而不是逐对象调用库。

`group_count == 0` 会构建一个只有清除深度的有效 snapshot。单个 group 的
`instance_count == 0` 也是合法 no-op，其 mesh 和 transform 指针可以为空。
非空 group 的 mesh 必须属于执行 build 的 context。

frame 描述符、group、transform 和 mesh 仅在同步 build 期间被读取。成功返回后
snapshot 已复制 frame 约定并拥有完整结果，不再依赖这些输入。

## Snapshot 独立性

snapshot 保存自己的深度金字塔、frame 约定和 build stats。它不会借用 context 的
Level 0，也不会引用参与 build 的 mesh。因此下列操作都不会使已有 snapshot 失效：

- 销毁参与 build 的 mesh；
- 调整 context 尺寸；
- 使用同一 context 构建其他 snapshot；
- 销毁 context。

resize 仅决定后续 snapshot 的尺寸。多个 snapshot 可以拥有不同尺寸并同时被查询。

## Build 原子性与统计

库只在 Level 0、Hi-Z 和统计全部完成后发布 snapshot。build 失败时输出 snapshot
保持为空，已存在的 snapshot 和 context/mesh 逻辑状态不变。

`soc_build_stats` 只描述构建阶段，并随 snapshot 冻结。它记录源三角形数、因齐次
裁剪改变或拒绝的源三角形数，以及通过朝向和退化检查的裁剪后扇形三角形数。
`rasterized_triangle_count` 不表示三角形曾在任何采样点赢得深度测试。

`soc_query_stats` 则只描述一次 `soc_snapshot_test_aabbs()` 调用。查询不会修改
snapshot，也不会在 context 或 snapshot 中累计计数。这使同一 snapshot 可以被多个
调用方并发只读查询。

## AABB 遮挡判定

snapshot 构建时会把 `clip_from_world` 转为查询专用的双精度列向量、六个世界空间
裁剪面和数值误差尺度，后续所有批次直接复用。每个 AABB 验证所有分量均为有限值
且 `min <= max` 后，直接选择各裁剪面的正/负顶点求解析极值；处于数值误差带内的
边界情况回退到逐角点判定，以保持原有边界语义。非有限变换结果、横跨近平裁剪面
或其他无法可靠投影的情况得到 `SOC_VISIBILITY_UNKNOWN`，保持 fail-open。

对于可安全投影的 AABB，只完整变换最小角点，再用三条轴增量重建其余七个角点；
每个角点只求一次 `1 / w`，复用于 NDC X、Y 和深度。投影范围按误差界向外扩张，
随后选择覆盖屏幕矩形的 Hi-Z 层级。只有全部必要采样都严格证明包围盒位于遮挡深度
之后，才返回 `SOC_VISIBILITY_OCCLUDED`。正向 Z 使用严格的大于关系，反向 Z 使用
严格的小于关系；相等或无法证明时返回 `SOC_VISIBILITY_VISIBLE`。查询结果的
visible/occluded/unknown 计数在同一主循环中累加，不再二次扫描输出数组。

## Hi-Z 层级

先查询元数据：

```c
soc_hiz_level_info info = {
    .struct_size = sizeof(soc_hiz_level_info),
};

soc_snapshot_hiz_level_query(snapshot, level, &info, NULL, 0u);
```

再分配 `info.required_element_count` 个 `float` 并复制数据：

```c
soc_snapshot_hiz_level_query(
    snapshot,
    level,
    &info,
    depth,
    info.required_element_count
);
```

输出采用紧密行主序。Level 0 使用 build 时的 context 尺寸；每个下一层宽高均为
上一层的 `ceil(value / 2)`，直至 `1 x 1`。正向 Z 取对应有效子项的最大深度，
反向 Z 取最小深度；NPOT 的右侧和底部不会以虚拟子项补齐。
