# soc：基于 CPU Hi-Z 的遮挡剔除库

> [!IMPORTANT]
> `soc` 指 **Software Occlusion Culling（软件遮挡剔除）**。本项目不是用于输出
> 图像的通用软光栅器；它使用 CPU 对遮挡物进行仅深度光栅化，目标是在 CPU
> 上构建 Hi-Z（Hierarchical Z-Buffer，分层深度缓冲）并完成遮挡剔除。

`soc` 是一个使用 C17 编写的 CPU Hi-Z 遮挡剔除库。它提供稳定的 C ABI，
可供原生程序调用，也支持与 Unity/C# 互操作。

![soc 在 Unity/Android 真机中的运行效果](docs/images/unity-android-demo.jpg)

深度契约固定为 Reverse Z：近裁剪面映射到 `1`，远裁剪面趋近 `0`，
深度缓冲清除为 `0`，光栅化使用严格的 `GREATER` 比较。调用方必须直接
提供 Reverse-Z 投影矩阵

## 构建

```sh
cmake -S . -B build -DSOC_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

需要静态库时，可设置 `SOC_BUILD_SHARED=OFF`

在Mac上构建所有产物

```sh
export SOC_ANDROID_NDK="/absolute/path/to/android-ndk"
export SOC_MACOS_SDK="/absolute/path/to/MacOSX.sdk"
export SOC_IOS_DEVICE_SDK="/absolute/path/to/iPhoneOS.sdk"
export SOC_IOS_SIMULATOR_SDK="/absolute/path/to/iPhoneSimulator.sdk"
./scripts/build-all-macos.sh
```

## 性能基准

benchmark 默认不构建。使用
`-DSOC_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release` 启用

## soc_cli 调试工具

`soc_cli` 可将 OBJ 遮挡物的 Level 0 深度输出为 8 位对数灰度 PNG。
白色表示未覆盖像素，覆盖深度使用 `1..254`，以便在远近裁剪面比例很大时
仍能看清近处的深度变化。

```sh
./build/tools/soc_cli --input model.obj --output depth.png \
  --width 1280 --height 720 --fov 60
```

## 通过 ADB 在 Android 上测试性能

连接 `arm64-v8a` 设备并启用 USB 调试。benchmark 与 `soc` 静态链接，推送
单个可执行文件即可运行：

```sh
export SOC_ANDROID_NDK="/absolute/path/to/android-ndk"

cmake --preset android-arm64-v8a \
  -DSOC_BUILD_SHARED=OFF \
  -DSOC_BUILD_BENCHMARKS=ON
cmake --build --preset android-arm64-v8a --target soc_bench --parallel

adb wait-for-device
adb shell getprop ro.product.cpu.abi
adb push build/platform/android-arm64-v8a/benchmarks/soc_bench \
  /data/local/tmp/soc_bench
adb shell chmod 755 /data/local/tmp/soc_bench
adb shell /data/local/tmp/soc_bench \
  --suite full --samples 15 --sample-ms 200 \
  --output /data/local/tmp/soc-bench.json
adb pull /data/local/tmp/soc-bench.json ./soc-bench-android.json
```

## 参考

https://www.intel.com/content/www/us/en/developer/articles/technical/masked-software-occlusion-culling.html
