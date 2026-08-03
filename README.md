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

在 macOS 上执行原生构建前，还必须显式设置 SDK 绝对路径；项目不会使用活动
Xcode 的默认 SDK：

```sh
export SOC_APPLE_SDK="/absolute/path/to/MacOSX.sdk"
```

需要静态库时，可设置 `SOC_BUILD_SHARED=OFF`

Linux、Android、macOS、iOS 和 Windows 的交叉编译 preset、工具链要求及
产物说明见 [跨平台构建](docs/BUILDING.md)。

在一台 Mac 上构建并安装当前支持的全部平台变体：

```sh
export SOC_ANDROID_NDK="/absolute/path/to/android-ndk"
export SOC_MACOS_SDK="/absolute/path/to/MacOSX.sdk"
export SOC_IOS_DEVICE_SDK="/absolute/path/to/iPhoneOS.sdk"
export SOC_IOS_SIMULATOR_SDK="/absolute/path/to/iPhoneSimulator.sdk"
./scripts/build-all-macos.sh
```

## 性能基准

benchmark 默认不构建。使用
`-DSOC_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release` 启用独立的
`soc_bench` 目标；工作负载、采样方法和结果比较方式见
[benchmarks/README.md](benchmarks/README.md)。

## soc_cli 调试工具

`soc_cli` 可将 OBJ 遮挡物的 Level 0 深度输出为灰度 PNG，方便检查相机、
裁剪和深度光栅结果：

```sh
./build/tools/soc_cli --input model.obj --output depth.png \
  --width 1280 --height 720 --fov 60
```

未指定相机时，工具会根据 OBJ 包围盒自动取景。完整参数和格式限制见
[docs/CLI.md](docs/CLI.md)。


## 参考

https://www.intel.com/content/www/us/en/developer/articles/technical/masked-software-occlusion-culling.html
