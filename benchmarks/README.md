# Benchmarks

`soc_bench` 是面向优化回归的、无第三方依赖的 C17 benchmark。它通过公开
`soc` C ABI 测量同步单线程参考实现，覆盖完整 snapshot build、AABB 查询、
Hi-Z 读回以及资源生命周期。正确性仍由 `tests/` 负责；同时启用 benchmark 和
tests 时，CTest 会注册 `soc.bench.validate`，只运行确定性的 `--validate-only`
校验。性能采样不会加入 CTest，也不会成为跨机器的性能硬门禁。

## 构建

建议使用干净的 Release 共享库构建：

```sh
export SOC_APPLE_SDK="$(xcrun --sdk macosx --show-sdk-path)"

cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DSOC_BUILD_BENCHMARKS=ON \
  -DSOC_BUILD_TESTS=ON \
  -DSOC_BUILD_TOOLS=OFF \
  -DSOC_BUILD_SHARED=ON

cmake --build build-bench --config Release
ctest --test-dir build-bench --output-on-failure
```

`SOC_BUILD_BENCHMARKS` 默认是 `OFF`。benchmark 目标不参与安装。
只有同时设置 `SOC_BUILD_BENCHMARKS=ON` 和 `SOC_BUILD_TESTS=ON` 时，
`soc.bench.validate` 才会出现在 CTest 测试列表中。
Xcode 或 Ninja Multi-Config 的可执行文件通常位于
`build-bench/benchmarks/Release/`；Windows 顶层构建位于
`build-bench/bin/Release/soc_bench.exe`。其余命令相同。

## 运行

```sh
./build-bench/benchmarks/soc_bench --suite smoke
./build-bench/benchmarks/soc_bench --suite core --output candidate.json
./build-bench/benchmarks/soc_bench --suite full --filter query
./build-bench/benchmarks/soc_bench --validate-only
./build-bench/benchmarks/soc_bench --list
```

### 真实 OBJ snapshot build 基准

`soc_obj_bench` 读取带有 `# SOC benchmark OBJ v1` 元数据头的 OBJ，使用文件中
记录的分辨率、裁剪深度范围、深度方向、正面绕序、双面标志和
`camera_clip_from_world_col_major`。OBJ 解析、mesh/context 创建、深度读回、
结果校验和 snapshot 销毁均位于计时区外；每个 operation 计量一次完整的
`soc_occlusion_build()`，包含 snapshot 分配、Level 0 清除、光栅化和 Hi-Z 构建。

在 macOS 上，从仓库根目录复制执行以下一段 Shell，即可用 `test002.obj` 完成
Release 静态构建和性能测试：

```sh
cmake -S . -B build-bench-obj \
  -DCMAKE_BUILD_TYPE=Release \
  -DSOC_APPLE_SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)" \
  -DSOC_BUILD_BENCHMARKS=ON \
  -DSOC_BUILD_TESTS=OFF \
  -DSOC_BUILD_TOOLS=OFF \
  -DSOC_BUILD_SHARED=OFF && \
cmake --build build-bench-obj \
  --config Release --target soc_obj_bench --parallel && \
./build-bench-obj/benchmarks/soc_obj_bench \
  --input examples/test002.obj
```

默认执行 15 个样本，每个样本累计至少 200 ms 的 snapshot build 时间；可通过
`--samples N --sample-ms N` 调整。输出包含 median、P95、MAD、min/max，以及
输入/裁剪/光栅化三角形计数、写入深度的像素数和 Level 0 checksum。Linux 和
Windows 使用相同命令，但应省略 `SOC_APPLE_SDK_PATH` 这一行；多配置生成器的
可执行文件可能位于 `build-bench-obj/benchmarks/Release/`。

主要参数：

- `--suite smoke|core|full`：选择 5 项快速冒烟、42 项稳态回归或 51 项完整
  诊断集合。
- `--filter TEXT`：只运行名称包含 `TEXT` 的 case。
- `--samples N`：采样轮数，默认 15。
- `--sample-ms N`：每个样本的目标计时时长，默认 200 ms。
- `--seed N`：确定性合成数据种子，默认 `0x534F4301`。
- `--output FILE`：将 JSON 写入文件；未指定时写到标准输出。
- `--validate-only`：只构建工作负载并验证结果，不进行性能采样。
- `--list`：列出匹配的 case。

## Workloads

`soc_bench` 的内置场景都在内存中确定性生成，不依赖 OBJ、trace 或网络资源：

- 空 snapshot build：多种 POT/NPOT 分辨率以及 forward/reversed Z；原 clear/Hi-Z
  case 现在都计量公开 API 可观察的完整空 snapshot 构建。
- geometry snapshot build：16,384 个小三角形的视锥内、完全视锥外、近平面裁剪、
  背面和退化路径，以及 uint16/uint32 shared-index grid。
- fill snapshot build：全屏覆盖，以及 1/4/16 层的 near-to-far 与 far-to-near overdraw。
- instance snapshot build：固定 128 三角形网格的 1/16/256 实例扩展。
- query：每轮 65,536 个 AABB，分别以 1/64/4096/65536 批量调用，并覆盖
  全遮挡、全可见、60/25/10/5 的遮挡/可见/屏外/unknown 混合及大小投影；
  `query.perspective.mixed.small.65536` 还以 `w = world z` 的透视投影覆盖同一类
  mixed small AABB 分布。
- end-to-end：320×180、640×360 和 1280×720 三档代表性整帧。
- full suite 额外覆盖四种裁剪深度/深度方向组合、context create/resize、
  uint16/uint32 与不同 stride 的 mesh create，以及 Level 0/顶层 Hi-Z readback。

context/mesh 创建、resize 和读回会分配或复制内存，因此与 snapshot build 分开报告。
`soc_snapshot_hiz_level_query` 不计入普通 snapshot build 耗时。

## 测量与校验

输入生成、内存预触碰、context/mesh 资源分配、结果校验和验证用深度读回都在
计时区外；snapshot 及其深度金字塔的分配属于 `soc_occlusion_build()`，计入 build case。
`soc_occlusion_build_desc.frame` 是调用方持有的只读指针；这些 benchmark 会让对应的
`soc_frame_desc` 至少存活到同步 `soc_occlusion_build()` 返回。
每个性能 case 先预热至少 5 次和 250 ms，再自动标定迭代次数，使每个样本的计时
负载至少达到 `--sample-ms`。只读 query/readback 阶段会在每轮预热或样本内复用
同一个已构建的 Hi-Z 帧，避免计时外的重复建帧支配墙钟时间。默认保留全部 15
个样本，不删除离群值，并报告：

- median、P95、MAD、min 和 max；
- 每个样本的有效 operation 次数；
- 阶段吞吐所需的分辨率、三角形、实例与查询数量；
- `soc_build_stats`、`soc_query_stats`、visibility 分布和确定性 checksum。

若 `MAD / median > 3%`，结果会在 stderr 和 JSON 中标记为 noisy。每个 case
在采样前后都检查返回码、统计量、可见性分布及 checksum；snapshot build case 还会在
计时外读回并散列全部层级。校验失败时进程返回非零。P95 采用 nearest-rank；
默认 15 个样本时它等于 max，仅作为诊断数据。

JSON 根对象使用 `soc-bench-v1` schema，记录 suite、seed、原始样本、摘要、
场景参数和运行环境。环境包含 OS/kernel、架构、CPU、计时器、编译器及版本、
编译参数、IPO/LTO、fast-math、舍入模式、构建类型、共享/静态链接和 worker
数量。Git revision/dirty 信息在每次构建 `soc_bench` 时刷新，而不只在 CMake
configure 时捕获。机器电源模式无法可靠自动检测，发布结果时仍需另外记录。
`--validate-only` 会明确写出 `sample_count=0`、`sample_ms=0` 和空样本。

## 比较结果

先在同一台稳定机器、相同构建条件下保存一份干净标量 Release 基线，再比较
候选结果：

```sh
cmake \
  -DBASELINE=baseline.json \
  -DCANDIDATE=candidate.json \
  -DTHRESHOLD_PERCENT=5 \
  -P benchmarks/compare_results.cmake
```

比较器拒绝 validate-only 结果，只接受 schema、seed、采样协议、唯一 case
集合和关键环境字段一致的结果；每项参数、公开统计、visibility 分布和 checksum
也必须相同。一个 case 仅在以下条件同时成立时判为回退：

1. 候选 median 比基线慢超过 `THRESHOLD_PERCENT`（默认 5%）；
2. 绝对差大于 `3 * max(baseline MAD, candidate MAD)`。

默认只报告回退；添加 `-DFAIL_ON_REGRESSION=ON` 才返回失败。若任一侧结果被
标记为 noisy，比较器会将该项显示为 `NOISY`（满足回退规则时显示
`REGRESSION (noisy)`），并计入 noisy/inconclusive 数量；CI 可添加
`-DFAIL_ON_NOISY=ON` 要求重新测量。P95 用于诊断，不参与判定，也不生成会
掩盖单项退化的综合分数。

为减少噪声，应关闭高负载后台任务、保持 CPU 电源/散热状态一致、避免同时
运行多个 benchmark，并在同一启动方式下交替测量基线与候选。跨机器或不同
编译器的结果只适合记录，不应直接作为回归结论。
