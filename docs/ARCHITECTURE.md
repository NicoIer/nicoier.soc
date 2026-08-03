# 架构

本库保持公共 ABI 精简，并使其独立于光栅化、遮挡和平台实现细节。

```text
Unity / C#
    |
    v
include/soc + src/abi
    |
    v
src/core
    |
    +--------> src/occlusion
                   |
                   v
               src/raster
                   |
                   +--------> src/math
                   |
                   +--------> src/platform
```

## 依赖规则

- `abi` 验证外部数据，并将工作转发给 `core`。
- `core` 拥有不透明 context、不可变原生 mesh，以及 build 后独立拥有深度金字塔的
  不可变 snapshot，并负责库的整体协调。
- `occlusion` 负责可见性策略，并且可以使用光栅化后的深度数据。
- `raster` 使用原生网格快照，并负责裁剪、三角形设置、深度测试和光栅存储。
- `math` 和 `platform` 是内部叶子模块。
- 内部模块绝不依赖 C# 或 Unity API。
- 公共 API 的使用方绝不包含来自 `src/` 的头文件。

## ABI 2 所有权边界

context 只保存未来 build 的尺寸和仍附属于它的 mesh。一次
`soc_occlusion_build()` 同步借用 frame 描述符指针和全部 occluder groups，并生成
一个 `soc_snapshot`。snapshot 复制 frame 约定并拥有自己的 Level 0、Hi-Z 和 build
stats，不借用 context、mesh 或调用方描述符存储，因此可以跨 mesh 销毁、context
resize 和 context 销毁继续存在。

查询只读取 snapshot。build stats 随 snapshot 冻结，query stats 由每次 AABB 查询
单独返回，避免在共享 snapshot 中维护可变计数器。

第 0 层级当前仍由单线程标量深度光栅化器生成，它是正确性参考实现。一次性 build
和 snapshot 模型只是为 SIMD、多线程、分块和未来异步构建建立稳定所有权边界；
当前实现本身尚未启用这些优化，且这些内部优化不应再次要求更改查询 ABI。

公共的矩阵布局、坐标系手性、绕序、裁剪空间深度范围和深度方向约定记录于
`docs/TYPES.md`。
