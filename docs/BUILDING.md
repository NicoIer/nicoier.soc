# 跨平台构建

项目使用同一套目标定义，并通过 `CMakePresets.json` 为不同平台选择编译器、
SDK、架构和独立构建目录。不要在源码目录内直接构建。

通用要求为 CMake 3.21 或更高版本及 Ninja。Windows 原生 MSVC preset 使用
Visual Studio 2022 生成器，不要求 Ninja。

## 通用命令

查看当前主机可用的配置：

```sh
cmake --list-presets
```

每个平台都使用同名的 configure/build preset：

```sh
cmake --preset <preset>
cmake --build --preset <preset>
cmake --install build/platform/<preset>
```

构建目录为 `build/platform/<preset>`，安装后的头文件、库和 CMake package
位于 `build/install/<preset>`。交叉编译 preset 只构建 `soc` 库，不构建无法在
主机执行的测试、CLI 或 benchmark。

## Android

需要 Android NDK。将以下任一变量指向 NDK 根目录：

- CMake 变量 `SOC_ANDROID_NDK`；
- 环境变量 `ANDROID_NDK_HOME`、`ANDROID_NDK_ROOT` 或 `NDK_PATH`。

例如使用 Unity 随附的 NDK：

```sh
export ANDROID_NDK_HOME="/Applications/Unity/Hub/Editor/6000.3.9f1/PlaybackEngines/AndroidPlayer/NDK"

cmake --preset android-arm64-v8a
cmake --build --preset android-arm64-v8a
```

可用 preset：

- `android-armeabi-v7a`
- `android-arm64-v8a`
- `android-x86`
- `android-x86_64`

最低 API 为 21。所有 ABI 都启用 NDK 的 flexible page size 支持，以便生成可
用于 16 KiB page-size 设备的共享库。产物为 `libsoc.so`。

## Linux

Linux 交叉编译使用 Zig，因此同一台 macOS、Linux 或 Windows 主机可生成两种
ELF 架构。先确保 `zig` 位于 `PATH`：

```sh
cmake --preset linux-x86_64
cmake --build --preset linux-x86_64

cmake --preset linux-aarch64
cmake --build --preset linux-aarch64
```

产物分别为 x86-64 和 AArch64 的 `libsoc.so`。

## macOS

需要 Xcode Command Line Tools：

```sh
cmake --preset macos-universal
cmake --build --preset macos-universal
```

该配置生成最低支持 macOS 11.0、同时包含 `arm64` 和 `x86_64` slice 的
Universal 2 `libsoc.dylib`。

## iOS

需要完整 Xcode。iPhoneOS 与 iPhoneSimulator 是不同平台，必须分开构建：

```sh
cmake --preset ios-device-arm64
cmake --build --preset ios-device-arm64

cmake --preset ios-simulator-universal
cmake --build --preset ios-simulator-universal
```

两个 preset 都生成静态 `libsoc.a`；设备库为 `arm64`，模拟器库包含 `arm64`
和 `x86_64`。不要把 device 与 simulator slice 用 `lipo` 合并。需要分发单一
Apple 包时，可创建 XCFramework：

```sh
xcodebuild -create-xcframework \
  -library build/platform/ios-device-arm64/src/libsoc.a \
  -headers include \
  -library build/platform/ios-simulator-universal/src/libsoc.a \
  -headers include \
  -output build/install/soc.xcframework
```

Unity iOS 会把静态库链接进最终应用；C# 绑定在 iOS 侧应使用 `__Internal`。

## Windows

在 macOS/Linux 上交叉编译需要 MinGW-w64：

```sh
cmake --preset windows-mingw-x86_64
cmake --build --preset windows-mingw-x86_64
```

在 Windows 上使用 Visual Studio 2022：

```powershell
cmake --preset windows-msvc-x64
cmake --build --preset windows-msvc-x64
cmake --install build/platform/windows-msvc-x64 --config Release
```

两个配置都生成与 `DllImport("soc")` 对应的 `soc.dll`。MinGW 的导入库也会
保留在构建目录中。

## 原生开发构建

原有的本机测试流程保持不变：

```sh
cmake -S . -B build/dev -DSOC_BUILD_TESTS=ON
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```
