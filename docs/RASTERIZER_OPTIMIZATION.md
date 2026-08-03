# `soc_rasterizer_submit_occluders` 优化路线

本文记录 `soc_rasterizer_submit_occluders()` 的性能现状、可实施的优化方案、
正确性约束和推荐实施顺序。目标是在不改变公共 ABI 和保守遮挡语义的前提下，
降低遮挡物提交阶段的顶点处理、裁剪、三角形设置和深度光栅化成本。

## 当前实现

主要实现位于 `src/raster/soc_rasterizer.c`。一次提交按以下顺序执行：

1. 遍历实例和索引三角形；
2. 对每个三角形的三个顶点执行 object-to-world 和 world-to-clip 两次矩阵变换；
3. 对三角形执行六平面 Sutherland-Hodgman 齐次裁剪；
4. 将裁剪后的多边形按 triangle fan 拆分；
5. 执行透视除法、绕序和退化检查；
6. 计算屏幕包围盒并逐像素求三个边函数；
7. 插值深度并执行 forward-Z 或 reversed-Z 深度测试。

当前路径是同步、单线程、双精度中间计算的标量正确性参考实现。网格位置和索引
已在 `soc_mesh_create()` 时复制到库拥有的不可变存储，因此提交阶段不读取调用方
的原始 vertex/index buffer。

### 现有 C 方法与职责

| 文件 | 方法 | 当前职责 | 主要成本或限制 |
| --- | --- | --- | --- |
| `src/raster/soc_rasterizer.c` | `finite_double()` / `finite_clip_vertex()` | 拒绝 NaN 和无穷 clip vertex | fast path 仍必须保持非有限输入行为 |
| `src/raster/soc_rasterizer.c` | `transform_vertex()` | 执行一次 4×4 matrix-vector 变换 | 每个 corner 连续调用两次；共享顶点重复计算 |
| `src/raster/soc_rasterizer.c` | `read_mesh_index()` | 根据 index type 读取一个索引 | 每个 corner 重复判断 uint16/uint32，并使用 `memcpy()` |
| `src/raster/soc_rasterizer.c` | `clip_plane_distance()` | 计算顶点到指定齐次裁剪平面的有符号距离 | 每个 polygon vertex、每个平面重复进入 switch |
| `src/raster/soc_rasterizer.c` | `interpolate_clip_vertex()` | 计算裁剪边与平面的交点 | 只应在部分相交的三角形上调用 |
| `src/raster/soc_rasterizer.c` | `clip_polygon_against_plane()` | 对一个平面执行 Sutherland-Hodgman 裁剪 | 当前被所有三角形对全部六个平面调用 |
| `src/raster/soc_rasterizer.c` | `clip_triangle()` | 有限值检查、六平面裁剪、复制输出 polygon | 缺少 trivial-accept、trivial-reject 和 active-plane mask |
| `src/raster/soc_rasterizer.c` | `edge_function()` | 在指定 sample 上求二维边函数 | 当前内层每像素调用三次 |
| `src/raster/soc_rasterizer.c` | `is_top_left_edge()` | 判断共享边的 top-left 归属 | 优化后仍必须保持相同规则 |
| `src/raster/soc_rasterizer.c` | `edge_contains_sample()` | 根据边函数和 top-left 标志判断 coverage | 当前每像素最多调用三次 |
| `src/raster/soc_rasterizer.c` | `clamp_double()` | 限制 NDC、depth 和屏幕范围 | 优化 bbox/depth 时仍需保持边界结果 |
| `src/raster/soc_rasterizer.c` | `rasterize_triangle()` | 透视除法、面剔除、viewport、bbox、coverage、depth | 方法过于集中，难以独立替换 setup、block 和 SIMD 路径 |
| `src/raster/soc_rasterizer.c` | `soc_rasterizer_initialize()` / `resize()` / `shutdown()` | 管理 rasterizer 生命周期和借用的 Level 0 指针 | 后续 scratch/tile storage 必须在这些方法中事务性管理 |
| `src/raster/soc_rasterizer.c` | `soc_rasterizer_begin_frame()` | 保存 frame 配置并清除 Level 0 | 后续 tile state 也应在这里重置 |
| `src/raster/soc_rasterizer.c` | `soc_rasterizer_submit_occluders()` | 遍历实例/三角形并串联全部阶段 | 当前优化的主要入口 |
| `src/raster/soc_rasterizer.c` | `soc_rasterizer_finish_occluders()` | 当前只验证状态 | 可作为未来内部任务收尾点；公开统计仅在 snapshot 发布后可见 |
| `src/core/soc_mesh.c` | `soc_mesh_create_internal()` | 验证并复制 position/index snapshot | 可计算 bounds、cluster 和顶点复用信息 |
| `src/core/soc_pipeline.c` | `soc_occlusion_build_internal()` | 验证完整 group 数组、驱动 rasterizer/Hi-Z 并发布 snapshot | 候选 snapshot 必须完整成功后再公开 |
| `src/core/soc_context.c` | `soc_context_create_internal()` | 保存未来 build 的尺寸、worker 配置和 mesh owner | 当前拒绝 `worker_count > 1` |

### 推荐的内部调用分层

为了让每类优化可以独立 benchmark 和回退，建议逐步把当前单体调用链拆成：

```text
soc_rasterizer_submit_occluders()
  -> submit_instances_u16() / submit_instances_u32()
      -> transform_or_fetch_clip_vertex()
      -> classify_clip_triangle()
      -> clip_triangle_masked()          # 仅部分相交时
      -> setup_raster_triangle()
      -> rasterize_triangle_reference()  # 正确性基线
         或 rasterize_triangle_blocks() # 优化路径
```

这些名称是建议新增的 `static` 内部方法，不属于公共 ABI。初期可以继续保留
`rasterize_triangle()` 作为外层包装，根据三角形特征或 CPU capability 选择内部路径。

## 性能基线

以下数据来自 Apple M4 Pro、AppleClang、Release `-O3 -DNDEBUG`、单线程静态构建。
数据仅用于判断热点和保存本机优化前基线，不应直接作为跨机器性能指标。

### 合成场景

| 场景 | Median |
| --- | ---: |
| 16,384 个视锥内三角形 | 1.076 ms |
| 16,384 个近平面裁剪三角形 | 1.330 ms |
| 16,384 个背面三角形 | 0.636 ms |
| 16,384 个退化三角形 | 0.618 ms |
| 640×360 全屏三角形 | 0.627 ms |
| 全屏 16 层 front-to-back overdraw | 9.672 ms |
| 全屏 16 层 back-to-front overdraw | 9.938 ms |
| 256 个实例，共 32,768 个三角形 | 2.264 ms |

背面和退化三角形在最终没有进入光栅化的情况下仍需约 0.62 ms，说明顶点变换和
无条件六平面裁剪占有明显成本。16 层 front-to-back 与 back-to-front 时间接近，
说明当前深度测试只能拒绝单个像素的写入，不能提前跳过被完全遮挡的 block/tile。

### `test002.obj` 真实场景

`examples/test002.obj` 的基准元数据为：

- 312×144；
- 4,375 个顶点、2,068 个源三角形；
- negative-one-to-one clip range；
- forward-Z、CW、双面；
- 12 个源三角形发生裁剪；
- 2,072 个裁剪后扇形三角形进入光栅化；
- 27,624 个像素写入深度。

当前默认 15 样本、每样本至少 200 ms 的光栅阶段基线为：

| 指标 | 结果 |
| --- | ---: |
| Median | 305.652 us |
| P95 | 311.712 us |
| MAD | 3.013 us |
| Min | 301.261 us |
| Max | 311.712 us |
| Level 0 checksum | `2db3aca647f29990` |

运行方法见 `benchmarks/README.md` 的真实 OBJ 基准。该数字来自 ABI 2 切换前对
内部遮挡物光栅阶段的独立计时；OBJ 解析、mesh/context 创建、Level 0 清除、Hi-Z
构建、深度读回和校验均位于计时区外。ABI 2 的公开 `soc_occlusion_build()` 会同步
包含清除、光栅和 Hi-Z，因此不能把该数字直接解释为完整公开 build 延迟。

## 第一阶段：裁剪和提交快速路径

当前实现状态：outcode trivial accept/reject 和受保护的 active-plane clipping 已完成；
uint16/uint32 提交循环专门化仍未实施。部分相交路径会按原平面顺序处理 active planes，
并在插值后的 polygon 因浮点舍入落到被跳过平面之外时回退原六平面裁剪，保持参考
路径的保守性和精确输出。

### 1. 齐次裁剪 outcode

为每个 clip-space 顶点计算六位 outcode，每一位表示顶点位于对应裁剪平面之外。
对三个顶点的 outcode 执行：

```text
union_code        = code0 | code1 | code2
intersection_code = code0 & code1 & code2
```

- `union_code == 0`：三角形完全位于裁剪体内，直接进入三角形设置；
- `intersection_code != 0`：三个顶点位于同一平面之外，直接拒绝；
- 其他情况：进入精确多边形裁剪。

该方案避免视锥内三角形无条件遍历六个平面，也是最推荐优先实现的优化。

必须保持现有统计语义：完全在裁剪体内的源三角形不增加
`clipped_triangle_count`，被 trivial-reject 或精确裁剪改变的源三角形应增加该计数。

**对应现有方法**

- 修改 `soc_rasterizer_submit_occluders()`：在三个顶点变换完成后聚合 outcode；
- 修改或包装 `clip_triangle()`：只让部分相交三角形进入 polygon clipping；
- 复用 `clip_plane_distance()` 的平面定义，确保 outcode 与精确裁剪的 inside 条件一致。

**当前内部方法和类型**

```c
typedef uint8_t soc_clip_outcode;

typedef enum soc_clip_classification {
    SOC_CLIP_CLASSIFICATION_NONFINITE = 0,
    SOC_CLIP_CLASSIFICATION_ACCEPT,
    SOC_CLIP_CLASSIFICATION_REJECT,
    SOC_CLIP_CLASSIFICATION_PARTIAL,
} soc_clip_classification;

static soc_clip_outcode compute_clip_outcode(
    const soc_clip_vertex* vertex,
    soc_clip_depth_range depth_range
);

static soc_clip_classification classify_clip_triangle(
    const soc_clip_vertex vertices[3],
    soc_clip_depth_range depth_range,
    soc_clip_outcode* out_active_planes
);
```

`compute_clip_outcode()` 的 bit 和当前 `clip_plane_distance()` 的 plane 编号保持一致：

| Bit/plane | Outside 条件 |
| --- | --- |
| 0，left | `x + w < 0` |
| 1，right | `w - x < 0` |
| 2，bottom | `y + w < 0` |
| 3，top | `w - y < 0` |
| 4，near ZO | `z < 0` |
| 4，near NO | `z + w < 0` |
| 5，far | `w - z < 0` |

边界距离等于零时仍属于 inside，必须使用 `< 0.0`，不能改成 `<= 0.0`。
任一顶点非有限时应沿用当前 `clip_triangle()` 的行为：源三角形计为 clipped 并被拒绝。

提交入口的目标控制流为：

```text
active_planes = code0 | code1 | code2;
common_planes = code0 & code1 & code2;

if (any_vertex_non_finite) {
    ++rasterizer->clipped_triangle_count;
    continue;
}
if (common_planes != 0u) {
    ++rasterizer->clipped_triangle_count;
    continue;
}
if (active_planes == 0u) {
    rasterize original triangle directly;
    continue;
}

++rasterizer->clipped_triangle_count;
clip only active_planes, then rasterize the fan;
```

### 2. 只处理 active clip planes

需要精确裁剪时，优先只遍历 `union_code` 中置位的平面。数学上，若原始三角形的
所有顶点均在某个凸半空间内，裁剪其他平面所产生的多边形仍是原三角形的子集；但
浮点插值可能让新顶点因舍入略微落到原本 inactive 的平面之外。当前实现在首次产生
交点后检查每个将被跳过的平面；若当前 polygon 不再完全位于其内，就从原三角形
回退六平面参考裁剪。

**对应现有方法**

- 将 `clip_triangle()` 改为接收 `soc_clip_outcode active_planes`；
- 保持 `clip_polygon_against_plane()` 和 `interpolate_clip_vertex()` 的数学行为不变；
- `clip_plane_distance()` 可以继续由精确裁剪使用，第一版不必展开成六个专用方法。

**建议签名**

```c
static uint32_t clip_triangle_masked(
    const soc_rasterizer* rasterizer,
    const soc_clip_vertex input_triangle[3],
    soc_clip_outcode active_planes,
    soc_clip_vertex output_polygon[SOC_MAX_CLIPPED_VERTICES]
);
```

循环仍按 plane 0 到 5 的原始顺序执行，只跳过能够证明当前 polygon 仍完全位于其内
的 inactive plane。调用方已经完成原始三个顶点的有限值检查和分类；masked path 的
额外检查只用于决定是否需要回退 `clip_triangle_all_planes()`。

### 3. 提交循环专门化

将以下不变量移到内部循环之外：

- `triangle_count`；
- `two_sided`；
- clip range、depth direction 和 front face；
- viewport scale；
- Level 0 depth pointer 和 width/height。

按 `SOC_INDEX_UINT16` 和 `SOC_INDEX_UINT32` 建立两个内部提交路径，避免每个 corner
重复判断 index type。索引已复制到由库拥有且满足 `malloc` 对齐要求的连续存储，
可以让专门化路径使用对应的 typed load；仍需保持 mesh 创建阶段的索引范围验证。

这项优化风险较低，但预期收益小于 outcode 快速路径，适合与第一阶段一起实现。

**对应现有方法**

- `soc_rasterizer_submit_occluders()`：只负责参数验证、提取不变量和 index type dispatch；
- `read_mesh_index()`：从最热的 corner loop 移除；可以保留给慢路径或最终删除；
- `soc_mesh_create_internal()`：继续负责索引范围验证，不把验证移动到 submit。

**建议新增方法**

```c
static void submit_instances_u16(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count,
    soc_bool two_sided
);

static void submit_instances_u32(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world,
    uint32_t instance_count,
    soc_bool two_sided
);
```

两条路径应一次读取一个完整三角形的三个 index，避免 corner loop 中重复计算
`triangle * 3 + corner`。若不希望复制两份主体逻辑，可以用仅在本 `.c` 文件内使用的
宏生成两条 typed path，但不要在热循环中改用函数指针 reader。

## 第二阶段：减少顶点和三角形设置成本

### 4. 缓存 indexed vertex 的变换结果

当前共享顶点会被每个引用它的三角形重复变换。可以为当前实例准备 clip-space
vertex cache：

```text
mesh vertex index -> transformed clip-space vertex
```

三角形循环随后只读取三个缓存项。该方案保持原有两次矩阵向量乘法的计算顺序，
比矩阵预合并更容易保持逐位结果一致。

需要考虑：

- triangle soup 没有顶点复用，缓存收益可能为零；
- scratch memory 不宜在计时中的 submit 临时分配；
- 可在调用方保证没有并发 context 操作时或 mesh 创建阶段扩容 reusable scratch；
- 应增加 shared-grid/shared-index benchmark，避免只用 triangle soup 评估。

**对应现有方法和结构**

- `transform_vertex()`：继续作为单顶点参考变换方法；
- `soc_rasterizer_submit_occluders()`：实例开始时先填充 transformed vertex cache；
- `soc_rasterizer_initialize()` / `soc_rasterizer_shutdown()`：初始化和释放 scratch；
- `soc_mesh_create_internal()`：若未来将 reusable scratch 明确放入 context，则在
  调用方保证同一 context 没有并发 build、resize 或 destroy 时，根据新 mesh 的
  `vertex_count` 请求扩容；
- `soc_rasterizer`：记录 scratch pointer 和 capacity。

一种不把 `soc_clip_vertex` 暴露到头文件的内部存储方式为：

```c
typedef struct soc_rasterizer {
    /* existing fields ... */
    double* transformed_clip_xyzw;
    size_t transformed_vertex_capacity;
} soc_rasterizer;
```

每个顶点占连续四个 double。`soc_rasterizer.c` 内部再通过小型 load/store helper
转换为 `soc_clip_vertex`。也可以把 `soc_clip_vertex` 移到内部头文件并保存 typed
pointer，但不应放进公共 `include/soc/`。

**建议新增方法**

```c
soc_result soc_rasterizer_reserve_vertex_scratch(
    soc_rasterizer* rasterizer,
    size_t vertex_count
);

static soc_clip_vertex transform_mesh_vertex(
    const soc_rasterizer* rasterizer,
    const soc_mat4* object_to_world,
    const float* position_xyz
);

static void transform_mesh_vertices(
    soc_rasterizer* rasterizer,
    const soc_mesh* mesh,
    const soc_mat4* object_to_world
);
```

`transform_mesh_vertex()` 第一版应原样调用两次 `transform_vertex()`，以保持算术顺序。
`transform_mesh_vertices()` 按 position 的线性顺序读取，可以改善 cache locality；
triangle loop 再按 index 读取已经变换的 clip vertex。

scratch 扩容必须是事务性的：先分配 replacement，成功后再替换旧 pointer。若选择在
`soc_mesh_create_internal()` 中扩容，失败应使 mesh 创建返回
`SOC_RESULT_OUT_OF_MEMORY`，不得把半初始化 mesh 挂入 `context->meshes`。这样 submit
仍不会新增内存分配和新的运行时失败点。

可依据 `index_count / vertex_count` 决定是否启用缓存。例如 index corner 数没有明显
高于 vertex 数时，直接 triangle transform 可能更快；阈值必须由 shared-index、
triangle-soup 和 `test002.obj` 三类数据共同确定，不应凭经验写死。

### 5. 预合并 `clip_from_object`

每个实例预先计算：

```text
clip_from_object = clip_from_world * object_to_world
clip_position = clip_from_object * object_position
```

这会将每顶点两次矩阵向量乘法减少为一次。对于顶点较多的实例，矩阵乘法本身的
固定成本很快可以摊薄。

该方案会改变浮点运算结合顺序，可能影响裁剪边界、共享边 coverage 或最终 float
depth bit pattern。因此应作为独立改动实施，并通过 Level 0 checksum、随机矩阵和
边界三角形差分测试确认行为。若无法保持严格一致，应保留参考路径或采用明确的
保守误差策略，不能静默扩大遮挡覆盖。

**对应现有方法**

- 替换 `soc_rasterizer_submit_occluders()` 内连续两次 `transform_vertex()` 的路径；
- 保留 `transform_vertex()` 作为精确参考和 fallback；
- 若已实现 transformed vertex cache，则在 `transform_mesh_vertices()` 中选择
  reference 或 composed matrix 路径。

**建议新增类型和方法**

```c
typedef struct soc_clip_matrix {
    double values[16];
} soc_clip_matrix;

static soc_clip_matrix compose_clip_from_object(
    const soc_mat4* clip_from_world,
    const soc_mat4* object_to_world
);

static soc_clip_vertex transform_object_vertex(
    const soc_clip_matrix* clip_from_object,
    const float position_xyz[3]
);
```

组合矩阵使用 double 保存，可以避免把组合后的系数再次舍入到 float；但它仍改变
`clip * (object * position)` 的结合顺序。因此必须在同一个版本中保留可选 reference
路径，以便差分运行：同一批输入分别执行 reference/composed path，并比较 clip
分类、Level 0 depth 和公开统计。

固定成本为每实例一次 4×4 matrix multiply。对于极小 mesh，组合成本可能高于节省的
顶点变换，应通过 vertex count 阈值或 identity/affine 特例决定是否启用。identity
特例也必须验证 signed zero、NaN 和统计行为，不能仅用 `memcmp()` 后无条件改变路径。

### 6. 提前背面和退化拒绝

对于 trivial-accept 且所有 `w > 0` 的三角形，可使用齐次坐标行列式判断投影后
绕序，尝试在三次透视除法和 viewport setup 前拒绝单面网格的背面三角形。

接近零面积、非有限或数值不确定的情况应回退现有路径，以避免改变退化分类和
边界行为。发生精确裁剪的三角形仍应在裁剪后判断绕序。

**对应现有方法**

- 当前绕序和退化判断位于 `rasterize_triangle()` 的 NDC area 部分；
- outcode trivial-accept 后，在调用 `rasterize_triangle()` 前增加可证明安全的 early
  classification；
- 部分裁剪三角形继续使用 `clip_triangle_masked()` 输出，再走现有 NDC 判断。

**建议新增方法**

```c
static double homogeneous_orientation(
    const soc_clip_vertex* vertex0,
    const soc_clip_vertex* vertex1,
    const soc_clip_vertex* vertex2
);

static soc_bool can_early_reject_face(
    const soc_rasterizer* rasterizer,
    const soc_clip_vertex vertices[3],
    soc_bool two_sided,
    soc_bool* out_rejected
);
```

当三个 `w` 都严格大于零时，投影后 NDC area 的符号等于下列 determinant 的符号：

```text
det([x0 y0 w0],
    [x1 y1 w1],
    [x2 y2 w2])
```

`two_sided == true`、任一 `w <= 0`、determinant 非有限，或 determinant 接近数值误差
范围时，`can_early_reject_face()` 应返回“不能确定”，让现有 `rasterize_triangle()`
完成判断。只有能够严格证明是背面时才提前拒绝。`rasterized_triangle_count` 仍只在
通过绕序和退化检查时增加，因此 early-reject 不得增加该计数。

### 7. 实例和 cluster 粗剔除

mesh 创建时可保存 object-space bounds；提交实例时先将 bounds 变换到 clip space，
对完全位于裁剪体外的实例整体拒绝。更进一步可在 mesh 创建时生成 meshlet/cluster：

- cluster bounds 用于视锥粗剔除；
- 单面网格可使用 normal cone 做整簇背面剔除；
- 只有通过粗剔除的 cluster 才进入逐三角形路径。

该方案适合大型真实网格，但数据结构、内存占用和非均匀缩放下的 normal cone
处理复杂度更高，不建议作为第一项修改。

**对应现有方法和结构**

- `soc_mesh_create_internal()`：计算 object-space AABB、构建 cluster metadata；
- `free_mesh_storage()`：释放新增的 cluster/bounds storage；
- `soc_mesh`：新增 bounds、cluster pointer 和 cluster count；
- `soc_rasterizer_submit_occluders()`：在实例和 cluster 循环入口执行粗分类；
- `compute_clip_outcode()`：可复用于变换后 bounds corner 的视锥分类。

**建议新增方法**

```c
static void calculate_mesh_bounds(
    const float* positions_xyz,
    uint32_t vertex_count,
    soc_aabb* out_bounds
);

static soc_bool instance_bounds_outside_clip(
    const soc_aabb* object_bounds,
    const soc_mat4* object_to_world,
    const soc_mat4* clip_from_world,
    soc_clip_depth_range depth_range
);
```

对 AABB 八角点取 outcode 后，只有所有角点共享至少一个 outside plane bit 时才能
拒绝整个实例。不能只测试 AABB 中心。若 matrix 或 bounds 含非有限值，应回退逐三角形
路径，而不是拒绝实例，因为当前遮挡语义要求无法证明时 fail-open。

cluster 数据可以后续定义为内部结构：

```c
typedef struct soc_mesh_cluster {
    soc_aabb bounds;
    uint32_t first_triangle;
    uint32_t triangle_count;
    /* optional normal cone */
} soc_mesh_cluster;
```

若 cluster 需要重排 index，应在 mesh 创建时完成，并验证公开统计仍按源三角形计数。

## 第三阶段：降低逐像素光栅成本

### 8. 增量边函数和深度平面

当前每个像素重新计算三个边函数，并以 `screen_area` 做深度除法。三角形 setup
阶段可生成：

```text
edge(x, y)  = A*x + B*y + C
depth(x, y) = depth_origin + dzdx*x + dzdy*y
```

扫描一行时只需增量加法：向右移动加 `d/dx`，换行加 `d/dy`；
`1 / screen_area` 每个三角形只计算一次。

直接以浮点增量累加可能产生与逐像素重新求值不同的舍入误差。可选择：

- 每行重新计算起点，只在行内增量，限制累计误差；
- 边函数采用固定点表示，深度仍采用浮点平面；
- 对边缘 block 回退参考算法；
- 使用随机差分和共享边测试确认 top-left rule。

**对应现有方法**

- 拆分当前 `rasterize_triangle()`；
- `edge_function()` 保留给 setup、reference path 和每行起点计算；
- `is_top_left_edge()` 继续产生每条边的填充偏置；
- `edge_contains_sample()` 保留给 reference/partial block；
- `swap_screen_vertices()` 仍用于把 screen area 规范为正值。

**建议新增内部结构**

```c
typedef struct soc_edge_equation {
    double a;
    double b;
    double c;
    soc_bool top_left;
} soc_edge_equation;

typedef struct soc_raster_triangle_setup {
    soc_edge_equation edges[3];
    double depth_origin;
    double depth_dx;
    double depth_dy;
    uint32_t minimum_x;
    uint32_t maximum_x;
    uint32_t minimum_y;
    uint32_t maximum_y;
} soc_raster_triangle_setup;
```

**建议拆分方法**

```c
static soc_bool setup_raster_triangle(
    const soc_rasterizer* rasterizer,
    const soc_clip_vertex* clip0,
    const soc_clip_vertex* clip1,
    const soc_clip_vertex* clip2,
    soc_bool two_sided,
    soc_raster_triangle_setup* out_setup
);

static void rasterize_triangle_reference(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
);

static void rasterize_triangle_incremental(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
);
```

`setup_raster_triangle()` 吸收当前 `rasterize_triangle()` 中从透视除法到 bbox/top-left
计算的部分；返回 false 表示 w、非有限、退化或背面拒绝。外层只在返回 true 后增加
`rasterized_triangle_count`。第一步重构应让 `rasterize_triangle_reference()` 复现
当前逐像素算法和 checksum，再单独引入 incremental path，避免结构重构和算术变化
混在一个提交中。

增量路径每行用 `edge_function()` 或 `A*x+B*y+C` 重新计算首个 sample，行内才累加
`a`；depth 也每行从平面方程重新求起点。这样比跨整个 bbox 连续累加更容易限制误差。

### 9. 4×4 或 8×8 block rasterization

对 block 四角求边函数并分类：

- 整块在任一边外：跳过整个 block；
- 整块完全在三角形内：进入无 coverage 分支的快速循环；
- 与三角形边界相交：回退逐像素 coverage 判断。

该方案对全屏和大三角形收益明显，也是后续 SIMD 和 tile early-Z 的基础。为了避免
漏画或错误扩张，应采用能够证明 inside/outside 的保守判定；不确定时必须回退。

**对应现有方法**

- `rasterize_triangle()`：作为 dispatch wrapper；
- `setup_raster_triangle()`：提供边方程、bbox 和 depth plane；
- `edge_contains_sample()`：partial block 的逐像素 fallback；
- `soc_rasterizer_begin_frame()`：若 block path 有辅助状态，在帧开始时重置。

**建议新增方法**

```c
typedef enum soc_block_classification {
    SOC_BLOCK_OUTSIDE,
    SOC_BLOCK_INSIDE,
    SOC_BLOCK_PARTIAL
} soc_block_classification;

static soc_block_classification classify_raster_block(
    const soc_raster_triangle_setup* setup,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height
);

static void rasterize_full_block(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup,
    uint32_t block_x,
    uint32_t block_y,
    uint32_t block_width,
    uint32_t block_height
);

static void rasterize_partial_block(/* same location arguments */);
```

block 与 framebuffer 右/下边缘相交时，`block_width`/`block_height` 必须缩短，不能
读写 padding。classification 对每条线性边函数只需选择使其最小/最大的 block corner；
只有所有 sample 都能被证明 inside 才进入 full block。落在 top-left 边界上的 block
优先分类为 partial，以复用逐像素精确规则。

### 10. Tile Early-Z

为每个 tile 维护是否完全覆盖，以及当前深度的保守界限：

- forward-Z 维护 tile 中最远的已覆盖深度；
- reversed-Z 维护对应的反向界限；
- 只有 tile 已完全覆盖，且新三角形在整个 tile 上都严格更远时，才允许整块跳过。

三角形在 tile 上的最近深度必须使用保守 bound。部分覆盖 tile 或深度关系无法严格
证明时继续执行像素级路径。该优化主要改善 front-to-back overdraw。

**对应现有方法和结构**

- `soc_rasterizer`：新增 tile dimensions、coverage 和 depth bound storage；
- `soc_rasterizer_initialize()`：分配初始 tile storage；
- `soc_rasterizer_resize()`：事务性创建匹配新尺寸的 tile storage；
- `soc_rasterizer_shutdown()`：释放 tile storage；
- `soc_rasterizer_begin_frame()`：重置 coverage/depth，或递增 lazy-clear generation；
- `rasterize_full_block()` / `rasterize_partial_block()`：更新 tile 状态；
- `rasterize_triangle_blocks()`：在进入像素循环前查询 early-Z。

**建议新增类型和方法**

```c
typedef struct soc_depth_tile {
    float depth_bound;
    uint64_t coverage_mask;
    uint32_t generation;
} soc_depth_tile;

static soc_bool tile_can_reject_triangle(
    const soc_rasterizer* rasterizer,
    const soc_depth_tile* tile,
    const soc_raster_triangle_setup* setup,
    uint32_t tile_x,
    uint32_t tile_y
);

static void update_depth_tile(
    soc_rasterizer* rasterizer,
    uint32_t tile_index
);
```

若采用 8×8 tile，`coverage_mask` 可用 64 bit 表示每个像素是否已被任一有效遮挡
深度覆盖。只有 mask 全 1 时才允许依据 `depth_bound` 整 tile 拒绝。第一版
`update_depth_tile()` 可以在 tile 写入后扫描其 64 个 Level 0 depth，换取简单正确；
确认收益后再改为增量维护。forward-Z 的 bound 是已覆盖像素中的最大值，reversed-Z
则是最小值。

### 11. SIMD

在数据布局和 block path 稳定后，可增加运行时选择的 SIMD 实现：

- 批量顶点变换；
- 多像素边函数计算；
- 多像素 depth compare/store；
- NEON、SSE2 和 AVX2 后端。

标量路径应继续作为参考实现和不支持 SIMD 平台的 fallback。优先保持 double
中间精度和现有运算顺序；直接切换为 float SIMD 需要单独评估保守性和 checksum。

**对应现有方法和文件**

- `transform_mesh_vertices()`：增加批量顶点变换后端；
- `rasterize_full_block()`：增加多像素 edge/depth 后端；
- `rasterize_triangle()`：根据 dispatch table 选择 scalar/SIMD；
- `src/platform/`：增加 CPU feature detection；
- `src/CMakeLists.txt`：按平台编译独立 SIMD translation unit。

建议文件拆分：

```text
src/raster/soc_rasterizer.c           # 公共内部入口、reference path
src/raster/soc_rasterizer_internal.h  # setup/dispatch 内部类型
src/raster/soc_rasterizer_neon.c      # AArch64 NEON
src/raster/soc_rasterizer_sse2.c      # x86-64 baseline SIMD
src/raster/soc_rasterizer_avx2.c      # 可选 AVX2
src/platform/soc_cpu_features.c       # runtime capability
```

建议定义一个内部 dispatch table，而不是在每个 block 内判断 CPU：

```c
typedef struct soc_raster_dispatch {
    void (*transform_vertices)(/* ... */);
    void (*rasterize_full_block)(/* ... */);
} soc_raster_dispatch;
```

dispatch 在 `soc_rasterizer_initialize()` 时选择一次。不同后端必须共享相同的 setup、
clip 和统计逻辑；SIMD 只替换能够逐位或保守等价的计算内核。

## 第四阶段：分块并行

公共 `soc_config.worker_count` 已存在，但当前 `worker_count > 1` 返回
`SOC_RESULT_UNSUPPORTED`。若要启用多线程，推荐先完成 tile binning：

1. 并行执行顶点变换、裁剪和 triangle setup；
2. 将三角形引用写入覆盖的 tile bin；
3. 每个 tile 由单一 worker 光栅化，避免 Level 0 写冲突；
4. 所有 tile 完成后同步构建 Hi-Z。

按不相交 tile 分配写入比多个线程直接竞争同一深度缓冲区更容易保持确定性。
需要额外验证统计汇总、任务调度开销、小工作负载 fallback 和 context 生命周期。

**对应现有方法和结构**

- `soc_context_create_internal()`：当前在 `worker_count > 1` 时返回 unsupported；未来在
  这里创建 worker pool，并保存实际 worker count；
- `soc_context_destroy_internal()`：停止并回收 worker；
- `soc_context_resize_internal()`：只改变未来 build 尺寸；已有 snapshot 与任务存储独立；
- `soc_rasterizer_submit_occluders()`：完成 transform/clip/setup 和 tile binning；
- `soc_rasterizer_finish_occluders()`：等待 tile jobs，并在返回前保证 Level 0 完整；
- `soc_occlusion_build_internal()`：一次看到全部 group，并且只在 raster、Hi-Z 和
  `soc_build_stats` 均完整后发布不可变 snapshot。

**建议新增平台和 raster 方法**

```c
soc_result soc_thread_pool_initialize(
    soc_thread_pool* pool,
    uint32_t worker_count
);

void soc_thread_pool_shutdown(soc_thread_pool* pool);

static soc_result bin_raster_triangle(
    soc_rasterizer* rasterizer,
    const soc_raster_triangle_setup* setup
);

static void rasterize_tile_job(void* user_data, uint32_t tile_index);
```

ABI 2 当前仍要求 `soc_occlusion_build()` 同步返回，因此第一版并行实现应在内部完成
setup/binning、派发 tile jobs、等待 Level 0 与 Hi-Z 完成，再一次性发布 snapshot。
未来若增加异步 build，可以复用相同 snapshot 发布边界；build stats 与 query stats
已经分离，不需要维护 ABI 1 的中途统计可见时机。

每个 tile 只能由一个 job 写入，triangle 在 tile 内的顺序最好保持原 group/instance 顺序。
虽然严格 min/max depth 在数学上与顺序无关，但浮点 tie、统计和未来扩展都更容易在
稳定顺序下保持确定性。

## 不建议优先采用的方案

### 全局启用 fast-math

当前实现显式处理 NaN、无穷值、严格深度比较和退化情况。`-ffast-math` 可能破坏
这些假设，不适合作为首轮优化。

### 无保护地将 double 改为 float

float 可以提高 SIMD 宽度并减少带宽，但可能改变裁剪交点、边函数符号和深度值。
遮挡器的错误扩张可能使本应可见的对象被错误剔除，因此不能只以画面“看起来一样”
作为验收条件。

### 一开始就引入多线程

当前主要标量热点尚未优化。若先引入线程池和共享写入，会增加性能噪声和调试范围，
也会掩盖单线程算法收益。建议先完成快速裁剪和 block rasterization。

## 推荐实施顺序

### 里程碑 A：低风险提交优化

1. 增加 outcode trivial-accept/trivial-reject；
2. 只裁剪 active planes；
3. hoist 提交不变量并专门化 uint16/uint32 index path；
4. 增加 fully-outside 和 shared-index 合成 benchmark；
5. 使用 `test002.obj` 和全部测试验证统计及 checksum。

对应代码改动集中在：

- `compute_clip_outcode()`：新增；
- `classify_clip_triangle()`：新增；
- `clip_triangle()` → `clip_triangle_masked()`：改造；
- `soc_rasterizer_submit_occluders()`：接入三条 clip classification 分支；
- `submit_instances_u16()` / `submit_instances_u32()`：新增；
- `read_mesh_index()`：退出 raster hot loop。

### 里程碑 B：顶点和 setup 优化

1. 引入 reusable transformed-vertex cache；
2. 单独评估 `clip_from_object` 预合并；
3. 为 trivial-accept 单面三角形增加保守的 early backface path；
4. 分别测量 triangle soup、共享网格和多实例场景。

对应代码改动集中在：

- `soc_rasterizer_reserve_vertex_scratch()`：新增；
- `transform_mesh_vertices()`：新增；
- `soc_mesh_create_internal()`：请求 scratch capacity；
- `soc_rasterizer_initialize()` / `soc_rasterizer_shutdown()`：管理 scratch；
- `compose_clip_from_object()`：作为独立实验路径新增；
- `homogeneous_orientation()` / `can_early_reject_face()`：新增。

### 里程碑 C：光栅核心优化

1. 预计算边函数和深度平面；
2. 引入 block inside/outside 分类；
3. 加入 tile coverage/depth 状态和 early-Z；
4. 在稳定的数据布局上增加 SIMD 后端。

对应代码改动集中在：

- `rasterize_triangle()`：拆为 setup 和 execution；
- `setup_raster_triangle()`：新增；
- `rasterize_triangle_reference()`：由当前逐像素实现迁移；
- `rasterize_triangle_incremental()`：新增；
- `classify_raster_block()`、`rasterize_full_block()`、
  `rasterize_partial_block()`：新增；
- `soc_rasterizer_initialize()` / `resize()` / `begin_frame()` / `shutdown()`：
  管理 tile storage；
- SIMD translation units 和 `soc_raster_dispatch`：新增。

### 里程碑 D：大规模场景

1. mesh/cluster bounds；
2. meshlet 和 normal cone；
3. tile binning；
4. 启用 `worker_count > 1`。

对应代码改动集中在：

- `soc_mesh_create_internal()` / `free_mesh_storage()`：管理 bounds/cluster；
- `instance_bounds_outside_clip()`：新增；
- `bin_raster_triangle()` / `rasterize_tile_job()`：新增；
- `soc_context_create_internal()` / `destroy_internal()`：管理 thread pool；
- `soc_rasterizer_finish_occluders()`：在需要时完成 job synchronization。

## 方法级改动总表

| 优化 | 主要修改的现有方法 | 建议新增方法 | 需要新增的状态 |
| --- | --- | --- | --- |
| Clip outcode | `soc_rasterizer_submit_occluders()`, `clip_triangle()` | `compute_clip_outcode()`, `classify_clip_triangle()` | 无 |
| Active planes | `clip_triangle()`, `clip_polygon_against_plane()` 调用点 | `clip_triangle_masked()` | 无 |
| Index specialization | `soc_rasterizer_submit_occluders()`, `read_mesh_index()` | `submit_instances_u16()`, `submit_instances_u32()` | 无 |
| Vertex transform cache | `transform_vertex()`, submit、initialize/shutdown、mesh create | `soc_rasterizer_reserve_vertex_scratch()`, `transform_mesh_vertices()` | transformed vertex scratch |
| Matrix composition | submit 或 `transform_mesh_vertices()` | `compose_clip_from_object()`, `transform_object_vertex()` | 每实例临时 double matrix |
| Early face reject | `rasterize_triangle()` 调用前 | `homogeneous_orientation()`, `can_early_reject_face()` | 无 |
| Instance/cluster cull | mesh create/free、submit | `calculate_mesh_bounds()`, `instance_bounds_outside_clip()` | mesh bounds/cluster array |
| Incremental raster | `rasterize_triangle()`, `edge_function()` 调用方式 | `setup_raster_triangle()`, `rasterize_triangle_reference()`, `rasterize_triangle_incremental()` | triangle setup struct |
| Block raster | `rasterize_triangle()` | `classify_raster_block()`, `rasterize_full_block()`, `rasterize_partial_block()` | 可选 block constants |
| Tile Early-Z | initialize/resize/begin/shutdown、block methods | `tile_can_reject_triangle()`, `update_depth_tile()` | tile coverage/depth/generation |
| SIMD | raster dispatch、CMake | NEON/SSE2/AVX2 kernels、CPU detection | dispatch table |
| Multithreading | context create/destroy/resize、submit/finish | thread pool、`bin_raster_triangle()`, `rasterize_tile_job()` | worker pool、tile bins |

## 正确性约束

每项优化都必须保持以下行为：

- `(x + 0.5, y + 0.5)` 像素中心采样；
- top-left fill rule；
- CCW/CW front face 和 two-sided 行为；
- zero-to-one 与 negative-one-to-one clip range；
- forward-Z 严格小于和 reversed-Z 严格大于；
- 裁剪、退化和非有限输入处理；
- `clipped_triangle_count` 与 `rasterized_triangle_count` 的公开语义；
- Level 0 和 Hi-Z 的保守遮挡结果；
- 无法严格证明被遮挡时 fail-open。

优化后的 rasterizer 不得错误增加遮挡覆盖，也不得把遮挡物深度错误地移动到更靠近
相机的位置；这两类错误都可能产生 false occlusion。

## 验收方法

每个独立优化应至少执行：

1. Release 全量构建；
2. `ctest --output-on-failure`；
3. `soc_bench --validate-only`；
4. 合成 geometry、fill、overdraw 和 instance 基准；
5. `test002.obj` submit 基准；
6. 对比公开统计和 Level 0 checksum；
7. 同机交替运行 baseline/candidate，使用 median 和 MAD 判断收益；
8. sanitizer 构建检查越界、未定义行为和生命周期问题。

### 现有测试与目标方法的对应关系

| 测试 | 主要覆盖的方法或语义 |
| --- | --- |
| `test_single_triangle_level_zero()` | `setup_raster_triangle()`、reference/block coverage 和 Level 0 写入 |
| `test_forward_and_reversed_depth()` | depth compare、tile depth bound、SIMD compare/store |
| `test_negative_one_to_one_depth_mapping()` | `compute_clip_outcode()` 的 near plane、NDC depth mapping |
| `test_homogeneous_scale_invariance()` | clip classification、透视除法和 matrix 优化 |
| `test_instance_transform_depth()` | `transform_vertex()`、vertex cache、matrix composition |
| `test_front_face_culling()` | `homogeneous_orientation()`、`can_early_reject_face()` |
| `test_two_sided_winding_and_frame_clear()` | two-sided dispatch、top-left、`soc_rasterizer_begin_frame()` |
| `test_clipped_oversized_triangle()` | `clip_triangle_masked()`、fan rasterization、fullscreen block path |
| `test_zero_to_one_near_plane_clipping()` | ZO near outcode 和精确交点插值 |
| `test_resize_and_empty_frame_clear()` | tile/scratch resize、clear 和生命周期 |
| `test_uint16_storage_and_explicit_destroy()` | `submit_instances_u16()` 的 storage 前置条件 |
| `test_uint32_unaligned_storage_and_context_destroy()` | mesh create 的 unaligned input copy 和 u32 submit path |

### 建议补充的定向测试

在 `tests/test_depth.c` 中建议新增：

```c
static int test_clip_outcode_trivial_accept(void);
static int test_clip_outcode_trivial_reject_each_plane(void);
static int test_clip_outcode_boundaries_are_inside(void);
static int test_clip_outcode_non_finite_rejection(void);
static int test_active_plane_clipping_matches_reference(void);
static int test_block_raster_matches_reference(void);
static int test_tile_early_z_matches_reference(void);
```

在 `tests/test_mesh.c` 中建议新增 shared-index mesh，分别强制走 uint16 和 uint32
提交路径；在 `benchmarks/soc_bench.c` 的 `g_cases` 中增加 fully-outside、shared-grid
和 mixed-size triangle case。随机差分测试可保留 scalar reference path，通过内部
测试入口将相同输入分别送入 reference 和 candidate，并逐元素比较 Level 0 depth。

性能测试对应关系：

- `benchmarks/soc_bench.c::workload_run_timed()` 测量合成 geometry/fill/overdraw/
  instance 路径；
- `benchmarks/soc_obj_bench.c::run_submit()` 是历史命名，测量带真实 OBJ metadata 的
  内部光栅工作；完整 ABI 2 build 还包括 clear、Hi-Z 和 snapshot 分配；
- `benchmarks/soc_obj_bench.c::capture_validation()` 在计时前后核对统计、drawn pixels
  和 Level 0 checksum。

第一阶段默认要求 `test002.obj` checksum 保持为
`2db3aca647f29990`。若某项优化有意改变精确深度结果，必须先证明新结果仍满足保守
遮挡约束，并将其作为单独的行为变更评审，不能与普通性能优化混合提交。
