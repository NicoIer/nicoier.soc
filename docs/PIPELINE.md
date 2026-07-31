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
- `soc_visibility_test_aabbs` 将每项结果写为 `SOC_VISIBILITY_UNKNOWN`。
- `soc_hiz_level_query` 返回第 0 层级的光栅化深度，或由它实际归约得到的任一
  Hi-Z 派生层级。
- `soc_frame_end` 将上下文恢复为 `IDLE` 状态。

在投影 AABB 测试实现之前，可见性仍采用默认放行策略。

统计信息会记录提交的源三角形、因齐次裁剪而发生变化或被拒绝的源三角形，
以及通过面朝向与退化检查的裁剪后扇形剖分三角形。光栅化三角形的计数并不
意味着该三角形曾在任一采样点的深度测试中胜出。

## 光栅化范围

当前用于保证正确性的标量路径提供以下功能：

1. 帧初始化和深度清除；
2. 逐实例执行从对象空间到裁剪空间的变换；
3. 齐次三角形裁剪；
4. 根据正面绕序执行背面剔除，并支持按网格启用双面处理；
5. 正向 Z 或反向 Z 深度光栅化；
6. 在结束遮挡物记录时同步构建完整的 Hi-Z 深度金字塔；
7. 帧清理。

投影 AABB 可见性测试仍是后续的内部阶段。经过优化的 SIMD、分块和多线程路径
必须保持标量路径的可观测行为。

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
