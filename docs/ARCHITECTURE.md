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
- `core` 拥有不透明上下文、不可变的原生网格快照，并负责库的整体协调。
- `occlusion` 负责可见性策略，并且可以使用光栅化后的深度数据。
- `raster` 使用原生网格快照，并负责裁剪、三角形设置、深度测试和光栅存储。
- `math` 和 `platform` 是内部叶子模块。
- 内部模块绝不依赖 C# 或 Unity API。
- 公共 API 的使用方绝不包含来自 `src/` 的头文件。

第 0 层级的标量深度光栅化器是正确性参考实现。SIMD、多线程、分块和分层路径
均为内部优化，不得要求变更 ABI。

公共的矩阵布局、坐标系手性、绕序、裁剪空间深度范围和深度方向约定记录于
`docs/TYPES.md`。
