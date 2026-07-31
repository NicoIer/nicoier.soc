# 渲染流程框架

公共帧 API 是同步且有状态的：

```text
IDLE
  |
  | soc_frame_begin
  v
RECORDING_OCCLUDERS
  |
  | soc_occluders_submit（零次或多次）
  | soc_occluders_finish
  v
QUERY_READY
  |
  | soc_visibility_test_aabbs（零次或多次）
  | soc_hiz_level_query（零次或多次）
  | soc_frame_end
  v
IDLE
```

仅允许在 `IDLE` 状态下调整上下文大小，以及创建或销毁网格。
上下文可随时销毁，销毁时会释放其关联的网格。
创建网格时，会同步将位置和索引快照到上下文自有的存储空间，
因此提交帧时绝不会读取调用方的原始缓冲区。

## 当前框架行为

- `soc_frame_begin` 验证帧描述并清空第 0 层级。
- `soc_occluders_submit` 对每个实例执行变换，在齐次裁剪体中裁剪三角形，
  并依据正面绕序设置执行背面剔除，或按网格启用双面处理，再以标量方式将深度
  光栅化到第 0 层级。
- `soc_occluders_finish` 结束记录，并同步构建全部 Hi-Z 派生层级；只有构建完成后，
  上下文才进入 `QUERY_READY`。
- `soc_visibility_test_aabbs` 投影有效的世界空间 AABB，保守选择 Hi-Z 层级，并仅在
  严格证明完整投影位于遮挡深度之后时将结果写为 `SOC_VISIBILITY_OCCLUDED`。
- `soc_hiz_level_query` 返回第 0 层级的光栅化深度，或由它实际归约得到的任一
  Hi-Z 派生层级。
- `soc_frame_end` 将上下文恢复为 `IDLE` 状态。

可见性查询采用保守的默认放行策略：非法、非有限、横跨近平面或无法可靠投影的
AABB 返回 `SOC_VISIBILITY_UNKNOWN`；可投影但无法严格证明被遮挡的 AABB 返回
`SOC_VISIBILITY_VISIBLE`。调用方只应剔除恰好返回
`SOC_VISIBILITY_OCCLUDED` 的对象。

统计信息会记录提交的源三角形、因齐次裁剪而发生变化或被拒绝的源三角形，
以及通过面朝向与退化检查的裁剪后扇形剖分三角形。光栅化三角形的计数并不
意味着该三角形曾在任一采样点的深度测试中胜出。统计还会记录成功查询的
AABB 总数以及其中被严格证明遮挡的数量；这些计数随每帧开始而清零。

## 光栅化范围

当前用于保证正确性的标量路径提供以下功能：

1. 帧初始化和深度清除；
2. 逐实例执行从对象空间到裁剪空间的变换；
3. 齐次三角形裁剪；
4. 根据正面绕序执行背面剔除，并支持按网格启用双面处理；
5. 正向 Z 或反向 Z 深度光栅化；
6. 在结束遮挡物记录时同步构建完整的 Hi-Z 深度金字塔；
7. 使用投影 AABB 和 Hi-Z 执行保守遮挡判定；
8. 帧清理。

经过优化的 SIMD、分块和多线程路径必须保持标量路径的可观测行为。

## AABB 遮挡判定

查询先验证世界空间 AABB 的所有分量均为有限值且 `min <= max`，再使用当前帧的
`clip_from_world` 变换八个角点。任何非有限的变换结果，以及横跨近平裁剪平面或
其他无法形成可靠透视投影的情况，都直接得到 `SOC_VISIBILITY_UNKNOWN`。这类结果
不会被用于剔除。

对于可安全投影的 AABB，查询保守计算其屏幕矩形与最靠近相机的包围盒深度，并选择
能够覆盖该矩形的 Hi-Z 层级。选择的层级及其覆盖采样不得漏掉投影触及的第 0 层级
像素。只有所有必要的 Hi-Z 深度都能证明包围盒在遮挡物之后时，查询才返回
`SOC_VISIBILITY_OCCLUDED`。正向 Z 使用严格的“包围盒深度大于遮挡深度”关系，
反向 Z 使用严格的“包围盒深度小于遮挡深度”关系；相等或无法证明时返回
`SOC_VISIBILITY_VISIBLE`。

两种裁剪深度范围均沿用遮挡物光栅化的帧约定：
`SOC_CLIP_DEPTH_ZERO_TO_ONE` 直接使用 `[0, 1]` 深度，
`SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE` 则在投影后映射到 `[0, 1]`。因此同一套
严格遮挡规则可以与 `SOC_DEPTH_FORWARD` 和 `SOC_DEPTH_REVERSED` 的任意受支持
组合配合使用。

## Hi-Z 层级查询

查询仅在 `QUERY_READY` 状态下有效，且必须位于 `soc_occluders_finish()`
之后、`soc_frame_end()` 之前。

首先在不提供目标缓冲区的情况下查询元数据：

```c
soc_hiz_level_info info = {
    .struct_size = sizeof(soc_hiz_level_info),
};

soc_hiz_level_query(context, level, &info, NULL, 0u);
```

然后分配可容纳 `info.required_element_count` 个 `float` 的空间，并再次查询：

```c
soc_hiz_level_query(
    context,
    level,
    &info,
    depth,
    info.required_element_count
);
```

深度数据采用紧密排列，并以行主序存储。第 0 层级使用上下文分辨率，
其中包含以标量方式光栅化的深度图像。若上一层尺寸为 `w x h`，下一层尺寸为
`ceil(w / 2) x ceil(h / 2)`，如此递归直至 `1 x 1`。

每个派生层级像素归约上一层对应的 `2 x 2` 子区域。对于非二次幂（NPOT）尺寸，
右边缘或下边缘可能不足四个子项；归约只包含仍在上一层范围内的有效子项，不会
使用清除深度或其他虚拟值进行填充。正向 Z 取有效子项的最大深度，反向 Z 取
有效子项的最小深度。正向 Z 的第 0 层级清除深度为 `1.0`，反向 Z 为 `0.0`。
`soc_occluders_finish()` 返回成功时，所有这些层级均已同步构建完成并可供查询。
