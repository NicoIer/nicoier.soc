# 跨平台构建

项目使用同一套目标定义，并通过 `CMakePresets.json` 为不同平台选择编译器、
SDK、架构和独立构建目录。不要在源码目录内直接构建。

通用要求为 CMake 3.21 或更高版本及 Ninja。Windows 原生 MSVC preset 使用
Visual Studio 2022 生成器，不要求 Ninja。

## 在一台 Mac 构建全部平台

macOS 主机安装完整 Xcode、CMake、Ninja、Zig 和 Android NDK 后，需要先
显式提供四个 SDK/NDK 的绝对路径，再构建并安装全部平台变体：

```sh
export SOC_ANDROID_NDK="/absolute/path/to/android-ndk"
export SOC_MACOS_SDK="/absolute/path/to/MacOSX.sdk"
export SOC_IOS_DEVICE_SDK="/absolute/path/to/iPhoneOS.sdk"
export SOC_IOS_SIMULATOR_SDK="/absolute/path/to/iPhoneSimulator.sdk"
./scripts/build-all-macos.sh
```

也可用 `--android-ndk`、`--macos-sdk`、`--ios-device-sdk` 和
`--ios-simulator-sdk` 在命令行逐项传入。脚本不会猜测默认位置、扫描 Unity
Hub 或回退到活动 Xcode 的 SDK；缺少任一路径都会在 configure 前失败。脚本
还会读取每个显式 Apple SDK 的 `SDKSettings.json`，拒绝把 macOS、iOS device
和 iOS simulator SDK 互换使用。

该入口依次构建：

| 平台 | 架构或目标 |
| --- | --- |
| Android | armeabi-v7a、arm64-v8a、x86、x86_64 |
| Linux | x86_64、AArch64 |
| macOS | Universal 2（arm64 + x86_64） |
| iOS | device arm64、simulator arm64 + x86_64 |
| Windows | x86_64、ARM64 |

Linux 与两个 Windows 目标都由 Zig 交叉编译，因此不需要 Linux 或 Windows
主机，也不需要 Visual Studio。

每个目标会同时执行 configure、build 和 install。最终产物分别位于
`build/install/<preset>`；脚本在结束前会检查 11 个目标产物是否齐全，并验证
ELF/PE 架构及 Apple universal library 的架构切片。

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

需要显式提供 Android NDK 根目录，且路径必须存在。支持：

- CMake 变量 `SOC_ANDROID_NDK_PATH`；
- 环境变量 `SOC_ANDROID_NDK`。

不会检查任何默认安装目录，也不会自动扫描 Unity。CMake 的一次性输入使用
`SOC_ANDROID_NDK_PATH`，环境输入使用 `SOC_ANDROID_NDK`；前者在 toolchain
开始时就从 cache 移除，即使后续 configure 失败也不会被下一次配置静默复用。
持续配置和自动重新配置时建议导出环境变量。示例：

```sh
export SOC_ANDROID_NDK="/absolute/path/to/android-ndk"

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

需要 Xcode Command Line Tools，并通过 `SOC_APPLE_SDK` 环境变量或
`-DSOC_APPLE_SDK_PATH:PATH=...` 显式提供 macOS SDK 的绝对路径：

```sh
export SOC_APPLE_SDK="/absolute/path/to/MacOSX.sdk"
cmake --preset macos-universal
cmake --build --preset macos-universal
```

路径不会从 `SDKROOT`、`CMAKE_OSX_SYSROOT` 或活动 Xcode 推导。CMake 的
`SOC_APPLE_SDK_PATH` 输入只对当次 configure 有效，并在 toolchain 开始时
立即从 cache 移除；即使后续配置失败也不会成为下一次配置的隐式输入。

该配置生成最低支持 macOS 11.0、同时包含 `arm64` 和 `x86_64` slice 的
Universal 2 `libsoc.dylib`。

## iOS

需要完整 Xcode。iPhoneOS 与 iPhoneSimulator 是不同平台，必须分别显式
传入对应 SDK 的绝对路径：

```sh
export SOC_APPLE_SDK="/absolute/path/to/iPhoneOS.sdk"
cmake --preset ios-device-arm64
cmake --build --preset ios-device-arm64

export SOC_APPLE_SDK="/absolute/path/to/iPhoneSimulator.sdk"
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

在 macOS 上使用 Zig 交叉编译两个 Windows 架构：

```sh
cmake --preset windows-zig-x86_64
cmake --build --preset windows-zig-x86_64
cmake --install build/platform/windows-zig-x86_64

cmake --preset windows-zig-arm64
cmake --build --preset windows-zig-arm64
cmake --install build/platform/windows-zig-arm64
```

原有的 `windows-mingw-x86_64` preset 仍可用于已安装 MinGW-w64 的环境，但
macOS 全平台构建入口不依赖它。

在 Windows 上使用 Visual Studio 2022。x64 构建：

```powershell
cmake --preset windows-msvc-x64
cmake --build --preset windows-msvc-x64
cmake --install build/platform/windows-msvc-x64 --config Release
```

ARM64 构建需要安装 Visual Studio 的 C++ ARM64 构建工具和 Windows SDK：

```powershell
cmake --preset windows-msvc-arm64
cmake --build --preset windows-msvc-arm64
cmake --install build/platform/windows-msvc-arm64 --config Release
```

这些配置都生成与 `DllImport("libsoc")` 对应的 `libsoc.dll`。导入库也统一
使用 `libsoc` 基名，并保留在构建或安装目录中。

## 原生开发构建

共享库和静态库的本机调试验证分别使用 `dev` 和 `dev-static`
preset。在 macOS 上也必须先设置 `SOC_APPLE_SDK` 为 macOS SDK 的绝对路径；
原生 configure 不会选择活动 Xcode 的默认 SDK：

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev

cmake --preset dev-static
cmake --build --preset dev-static
ctest --preset dev-static
```

这些验证 preset 会设置 `SOC_WARNINGS_AS_ERRORS=ON`；普通库构建仍保持该选项
默认关闭。

Linux 和 macOS 上还可用 AddressSanitizer 与 UndefinedBehaviorSanitizer
验证同一组测试：

```sh
cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize
```

## 持续集成

`.github/workflows/ci.yml` 在 Linux、macOS 和 Windows 上构建并运行
CTest，且把编译器警告视为错误；它覆盖共享库与静态库。Host CTest 还会
运行确定性的 benchmark workload 校验。Linux 另外运行
ASan/UBSan，并在安装共享库和静态库后用独立 CMake 项目验证
`find_package(soc CONFIG)` 和 `soc::soc`。

`macos-all-platforms` 作业直接调用 `scripts/build-all-macos.sh`，持续验证同一台
Mac 生成全部 11 个发布平台变体，包括 Windows x86_64/ARM64 DLL。调用前，
workflow 会在 job 环境中固定 Xcode 16.4、NDK 29.0.14206865、macOS 15.5
SDK 与 iOS 18.5 device/simulator SDK 的绝对路径；镜像不再提供任一路径时
直接失败。CI 还验证缺失、相对、冲突、cache 复用和 Apple 平台互换均会失败。
构建脚本自身不做路径发现或回退。整个 workflow 只需要仓库的只读内容权限，
不依赖 secrets 或额外的第三方、付费服务。
