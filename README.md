# soc：基于 CPU Hi-Z 的遮挡剔除库

> [!IMPORTANT]
> `soc` 指 **Software Occlusion Culling（软件遮挡剔除）**。本项目不是用于输出
> 图像的通用软光栅器；它使用 CPU 对遮挡物进行仅深度光栅化，目标是在 CPU
> 上构建 Hi-Z（Hierarchical Z-Buffer，分层深度缓冲）并完成遮挡剔除。

`soc` 是一个使用 C17 编写的 CPU Hi-Z 遮挡剔除库。它提供稳定的 C ABI，
可供原生程序调用，也支持与 Unity/C# 互操作。

## 构建

```sh
cmake -S . -B build -DSOC_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

需要静态库时，可设置 `SOC_BUILD_SHARED=OFF`
