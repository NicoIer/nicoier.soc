# soc_cli 调试工具

`soc_cli` 用于检查 `soc` 的第 0 层级深度光栅化结果。它读取一个 OBJ 网格，
调用公共 C ABI 进行深度光栅化，并写出灰度 PNG；它不是通用 OBJ 渲染器。

## 构建

顶层构建默认包含调试工具，也可显式启用：

```sh
cmake -S . -B build -DSOC_BUILD_TOOLS=ON
cmake --build build --target soc_cli
```

## 基本用法

```sh
./build/tools/soc_cli --input model.obj --output depth.png \
  --width 1280 --height 720 --fov 60
```

`--input` 指定 OBJ，`--output` 指定 PNG。未提供相机参数时，工具根据 OBJ
包围盒计算观察目标、相机距离和裁剪范围，使模型自动进入画面。
执行 `cmake --install build` 后，也可以直接使用安装前缀 `bin` 目录中的
`soc_cli`。

## 参数

| 参数 | 说明 |
| --- | --- |
| `--input PATH` | 输入 OBJ 路径。 |
| `--output PATH` | 输出灰度 PNG 路径。 |
| `--width N` | 输出宽度。 |
| `--height N` | 输出高度。 |
| `--fov DEG` | 透视相机视场角，单位为度。 |
| `--eye X Y Z` | 显式指定相机位置。 |
| `--target X Y Z` | 显式指定相机观察目标。 |
| `--up X Y Z` | 显式指定相机上方向。 |
| `--near N` | 显式指定近裁剪面距离。 |
| `--far N` | 显式指定远裁剪面距离。 |
| `--two-sided` | 双面绘制，不做背面剔除。 |
| `--reversed-z` | 使用反向 Z 深度方向。 |
| `--front-face ccw\|cw` | 选择正面绕序。 |

显式相机参数可用于复现特定视角；不需要固定视角时，省略它们即可使用
基于包围盒的自动取景。调试输出最多包含 67,108,864 个像素，以避免错误
参数造成不可控的内存占用。

## 输出

PNG 为 8 位灰度图。工具先把透视深度线性化到 near/far 距离范围：靠近
相机的深度显示为黑色，远处显示为白色，未被遮挡物写入、仍保持清除值的
像素也是白色。该可视化约定在正向 Z 和反向 Z 下保持一致。

工具会向标准输出打印摘要，其中包括 `drawn_pixels=N`，便于脚本确认
模型确实产生了深度覆盖。

## OBJ 支持范围

当前解析 OBJ 的 `v x y z [w]` 顶点和 `f` 面；面索引支持 `v`、`v/vt`、
`v//vn`、`v/vt/vn` 以及负索引。多边形面以第一个顶点为扇心做扇形
三角化，因此多边形应为简单、凸且近似共面。材质、纹理坐标、法线和其他
OBJ 指令不参与深度绘制。
