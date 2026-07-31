# 公共类型

公共 ABI 数据类型在 `include/soc/soc_types.h` 中声明。它们使用固定位宽的标量字段，
以便验证其原生布局和托管布局。

## C 与 C# 的映射

| C 类型 | C# 表示形式 |
| --- | --- |
| `uint8_t`, `soc_bool`, `soc_visibility` | `byte` |
| `int32_t`, `soc_result` | `int` |
| `uint32_t` 以及基于该类型定义的类型化常量 | `uint` |
| `uint64_t` | `ulong` |
| `float` | `float` |
| `const void*` | `IntPtr` |
| `soc_context*`, `soc_mesh*` | `IntPtr` 或 `SafeHandle` |

不要将 `soc_bool` 封送为 C# `bool`；其平台封送规则无法保证这里所使用的单字节 ABI
布局。

## 矩阵约定

`soc_mat4` 包含四个列向量，名称从 `col0` 到 `col3`。每个提交的实例按以下方式变换：

```text
world_position = object_to_world[instance] * object_position
clip_position = clip_from_world * world_position
```

C# 绑定必须按照此约定复制 Unity 矩阵，而不能依赖托管结构的物理布局。

## 光栅化约定

本库不强制规定世界空间的手性；它由调用方的矩阵定义。三角形会在齐次坐标中，
依据 `clip_depth_range` 所选择的裁剪体进行裁剪。`front_face` 选择投影后正面的绕序，
而 `SOC_MESH_FLAG_TWO_SIDED` 会禁用该网格的面剔除。绕序在视口进行 Y 轴翻转之前，
于 Y 轴正方向朝上的归一化设备 XY 坐标中计算。

第 0 层级采用紧密排列的行主序布局，第 0 行位于顶部。光栅化在
`(x + 0.5, y + 0.5)` 处对像素中心采样，并使用左上填充规则。正向 Z 的清除
深度为 `1.0`，并使用严格小于的深度比较；反向 Z 的清除深度为 `0.0`，并使用
严格大于的深度比较。调用方的投影矩阵必须与所选的裁剪深度范围和深度方向一致。

## 可见性约定

`SOC_VISIBILITY_UNKNOWN` 的值为零，因此以零初始化的结果缓冲区会默认放行。只有当对象的
结果恰好为 `SOC_VISIBILITY_OCCLUDED` 时，调用方才剔除该对象；`UNKNOWN` 和 `VISIBLE`
都会被渲染。

`soc_aabb.min` 和 `soc_aabb.max` 表示世界空间包围盒。对于分量有限且每个
`min` 分量都不大于对应 `max` 分量的 AABB，查询会使用当前帧的
`clip_from_world` 变换其八个角点，将可安全投影的包围盒保守地映射到屏幕矩形，
并选择能够覆盖该矩形的 Hi-Z 层级。只有所选覆盖范围内的深度能够严格证明整个
投影包围盒都位于遮挡深度之后时，结果才是 `SOC_VISIBILITY_OCCLUDED`：

- 正向 Z 要求包围盒最靠近相机的深度严格大于覆盖范围的遮挡深度；
- 反向 Z 要求包围盒最靠近相机的深度严格小于覆盖范围的遮挡深度。

严格比较使深度相等时保持可见。`SOC_CLIP_DEPTH_ZERO_TO_ONE` 和
`SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE` 均受支持；后一种范围在比较前映射到内部的
`[0, 1]` 深度范围。

次序非法的 AABB、任何非有限输入或变换结果、横跨近平裁剪平面，或其他无法安全
投影的情况都会返回 `SOC_VISIBILITY_UNKNOWN`，不会据此剔除。其余能够安全投影但
无法满足严格遮挡证明的 AABB 返回 `SOC_VISIBILITY_VISIBLE`。

每帧的 `soc_stats.tested_aabb_count` 累计成功查询所包含的 AABB 数量，
`occluded_aabb_count` 则累计其中返回 `SOC_VISIBILITY_OCCLUDED` 的数量。

## 描述符约定

可扩展描述符以 `struct_size` 开头。调用方先将完整结构以零初始化，再将
`struct_size` 设为使用 `sizeof` 计算出的调用方结构大小，然后填充受支持的字段。
未知标志和保留值保持为零。

`soc_mesh_desc` 中的指针是借用的只读输入，由 `soc_mesh_create()` 同步使用。
`vertices` 描述了 `vertex_count` 条记录，各记录间隔 `vertex_stride` 字节。每个位置
由 `position_offset` 处三个连续的 `float` 值组成。`indices` 包含 `index_count` 个
紧密排列的 `uint16_t` 或 `uint32_t` 值，具体类型由 `index_type` 选择；每个索引都必须
小于 `vertex_count`。

此调用会将位置和索引复制到由原生代码拥有的不可变快照中。它不会保留其他交错的顶点属性，
也不会保留调用方的任何指针。调用成功后，调用方可以立即重用、修改、释放两个输入缓冲区，
或解除其固定状态。调用失败时，`*out_mesh` 保持为空，且不会将未完整创建的网格附加到上下文。

版本 1 不提供顶点缓冲区或索引缓冲区的字节数。调用方必须确保顶点缓冲区至少可读取
`(vertex_count - 1) * vertex_stride + position_offset + 3 * sizeof(float)` 字节，
并确保索引缓冲区包含 `index_count` 个完整元素。在整个调用期间，缓冲区必须保持有效且
不被修改；本库无法从 V1 描述符验证已分配内存的长度。

## Hi-Z 层级信息

`soc_hiz_level_info` 描述一个可查询的层级：

- `level` 是层级索引；零表示标量光栅化的第 0 层级，大于零表示实际构建的
  派生层级。
- `width` 和 `height` 是所选层级的尺寸。
- `required_element_count` 是复制时所需的紧密排列 `float` 元素数量。

从第 0 层级开始，每个下一层的宽、高分别为上一层的 `ceil(width / 2)` 和
`ceil(height / 2)`，直至 `1 x 1`。每个派生元素归约上一层对应 `2 x 2` 区域中
实际存在的子项：正向 Z 取最大值，反向 Z 取最小值。对于非二次幂（NPOT）尺寸的
右侧和底部边界，不存在的子项不会参与归约，也不会以清除深度补齐。

调用方必须在仅查询元数据和复制数据这两种查询之前初始化 `struct_size`。
