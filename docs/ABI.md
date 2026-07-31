# ABI 策略

公共 ABI 声明于 `include/soc/soc.h`，共享数据布局则位于
`include/soc/soc_types.h`。项目发布首个稳定版本后，公共符号和 ABI
可见的数据布局即构成兼容性承诺。

## 版本管理

`soc_get_abi_version()` 返回 `SOC_ABI_VERSION`。高 16 位表示主版本号，
低 16 位表示次版本号。

- 主版本变更可以移除或重新解释 ABI 元素。
- 次版本变更可以添加函数、结果码、标志或可选的结构体字段，但必须保持现有行为。
- 原生库的软件包版本与 ABI 版本相互独立。

## 上下文生命周期

`soc_context_create()` 创建一个不透明上下文。调用成功后，该上下文的所有权
转移给调用方。`soc_context_destroy()` 释放该上下文，并且接受空指针。

调用方将 `soc_config.struct_size` 初始化为 `sizeof(soc_config)`。版本 1
要求至少提供 `SOC_CONFIG_SIZE_V1` 字节。原生库可以接受来自较新调用方的
更大结构体，但如果结构体小到不足以包含原生库需要读取的字段，则必须拒绝。

## 网格生命周期

`soc_mesh_create()` 创建一个由其上下文拥有的不透明网格。
`soc_mesh_destroy()` 释放该网格。销毁上下文时，也会释放仍附属于它的所有
网格。只有上下文处于空闲状态时，才允许创建和销毁网格。

`soc_mesh_create()` 会同步验证所描述的 XYZ 坐标和索引流，并将其复制到
原生代码拥有的存储空间中。在调用期间，描述符和输入缓冲区是借用的只读输入。
成功返回后，库不会保留任何指向调用方内存的指针，因此调用方可以立即重用、
修改、释放这些缓冲区或解除其固定，而不会改变网格快照。

创建操作具有原子性。失败时，`*out_mesh` 保持为空，此前已分配的所有原生
内存都会被释放，且上下文保持不变。版本 1 的 `soc_mesh_desc` 不含缓冲区
大小字段，因此调用方必须提供足够大的可读缓冲区，并且不得在
`soc_mesh_create()` 执行期间并发修改这些缓冲区。

## 帧生命周期

调用遵循以下顺序：

```text
soc_frame_begin
soc_occluders_submit（零次或多次调用）
soc_occluders_finish
soc_visibility_test_aabbs（零次或多次调用）
soc_frame_end
```

乱序调用会返回 `SOC_RESULT_INVALID_STATE`。提交遮挡物时，会同步对三角形执行
变换、齐次裁剪、面剔除和标量光栅化，并将结果写入第 0 层级的深度图像。此时尚未
构建派生 Hi-Z 层级，每个被测试的 AABB 结果仍为
`SOC_VISIBILITY_UNKNOWN`。

## Hi-Z 图像查询

`soc_hiz_level_query()` 将选定层级的深度数据复制到调用方拥有的 `float` 存储空间。
它绝不会暴露内部深度指针。只查询元数据的调用应使用 `out_depth = NULL` 和
`out_depth_count = 0`；返回的
`soc_hiz_level_info.required_element_count` 给出所需的元素数量。

第 0 层级包含经标量光栅化得到的深度图像。更高层级目前会返回其逻辑尺寸，
并以清除深度占位值填充对应层级的数据。

仅在 `soc_occluders_finish()` 之后、`soc_frame_end()` 之前查询才有效。
目标缓冲区过小时会返回 `SOC_RESULT_BUFFER_TOO_SMALL`，但仍会返回层级
元数据。

## 兼容性约束

- 导出函数使用由 `SOC_CALL` 表示的 C 调用约定。
- C# 声明使用原生库基本名称 `soc`。
- 任何异常、分配器专用对象或内部指针都不会跨越 ABI。
- 除非函数明确记录了不同的生命周期，否则输入内存在调用期间始终由调用方拥有。
- 新标志默认禁用，所有保留数据均初始化为零。
- 对于托管代码与原生代码互操作，优先使用批处理 API，而非逐对象调用。
