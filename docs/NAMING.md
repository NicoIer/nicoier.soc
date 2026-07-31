# 命名规范

## 公共 ABI

- 公共函数采用 `soc_<noun>_<verb>` 格式，例如
  `soc_context_create`。
- 公共类型采用带 `soc_` 前缀的小写蛇形命名法，例如
  `soc_context` 和 `soc_config`。
- 公共宏和常量采用带 `SOC_` 前缀的大写蛇形命名法，
  例如 `SOC_ABI_VERSION` 和 `SOC_RESULT_OK`。
- 每个导出符号都必须在 `include/soc/soc.h` 中声明。
- 共享的公共数据类型在 `include/soc/soc_types.h` 中声明。
- ABI 函数使用 `SOC_API` 和 `SOC_CALL`。

## 内部 C 代码

- 源文件和头文件采用 `soc_<module>.c` 和 `soc_<module>.h` 格式。
- 文件内函数使用 `static` 修饰，并采用小写蛇形命名法。
- 跨文件内部函数保留 `soc_` 前缀。当内部操作为同名公共函数提供实现时，
  添加 `_internal` 后缀。
- 结构体字段和局部变量采用小写蛇形命名法。
- 头文件保护宏采用 `SOC_<NAME>_H_INCLUDED` 格式。
- 模块不得将内部头文件暴露到 `src/` 之外。

## ABI 数据规则

- ABI 可见的数据使用定宽整数类型。
- 不得暴露 C 的 `bool`、存储大小取决于编译器的枚举、柔性数组、
  平台句柄或内部对象布局。
- 如果公共结构体可能在后续 ABI 版本中扩展，则以 `struct_size` 作为首个字段。
- 除非文档另有说明，保留字段和未知标志必须为零。
- 返回 `soc_result` 的函数在成功时返回 `SOC_RESULT_OK`，失败时返回负值。
- 除非 API 明确说明所有权发生转移，否则输入缓冲区归调用方所有。
- 将输入复制到原生代码自有的存储空间时，不得保留调用方的指针。
- 除非 API 明确建立了固定内存生命周期契约，否则原生代码不得保留
  从 C# 传入的托管内存。

## CMake

- CMake 目标使用小写名称：`soc` 和 `soc_core`。
- 项目选项和编译定义使用 `SOC_` 前缀。
- 外部使用方通过 `soc::soc` 别名进行链接。
