# Benchmarks

`soc_bench` 是面向优化回归的、无第三方依赖的 C17 benchmark。它通过公开
`soc` C ABI 测量同步单线程参考实现，覆盖整帧以及 clear、遮挡物提交、Hi-Z
构建和 AABB 查询等阶段。正确性仍由 `tests/` 负责；benchmark 不会默认加入
CTest，也不会成为跨机器的性能硬门禁。

## 构建

建议使用干净的 Release 共享库构建：

```sh
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

主要参数：

- `--suite smoke|core|full`：选择 5 项快速冒烟、38 项稳态回归或 47 项完整
  诊断集合。
- `--filter TEXT`：只运行名称包含 `TEXT` 的 case。
- `--samples N`：采样轮数，默认 15。
- `--sample-ms N`：每个样本的目标计时时长，默认 200 ms。
- `--seed N`：确定性合成数据种子，默认 `0x534F4301`。
- `--output FILE`：将 JSON 写入文件；未指定时写到标准输出。
- `--validate-only`：只构建工作负载并验证结果，不进行性能采样。
- `--list`：列出匹配的 case。

## Workloads

所有场景都在内存中确定性生成，不依赖 OBJ、trace 或网络资源：

- 空帧 clear 和 Hi-Z：多种 POT/NPOT 分辨率以及 forward/reversed Z。
- geometry：16,384 个小三角形的视锥内、近平面裁剪、背面和退化路径。
- fill：全屏覆盖，以及 1/4/16 层的 near-to-far 与 far-to-near overdraw。
- instance：固定 128 三角形网格的 1/16/256 实例扩展。
- query：每轮 65,536 个 AABB，分别以 1/64/4096/65536 批量调用，并覆盖
  全遮挡、全可见、60/25/10/5 的遮挡/可见/屏外/unknown 混合及大小投影。
- end-to-end：320×180、640×360 和 1280×720 三档代表性整帧。
- full suite 额外覆盖四种裁剪深度/深度方向组合、context create/resize、
  uint16/uint32 与不同 stride 的 mesh create，以及 Level 0/顶层 Hi-Z readback。

context/mesh 创建、resize 和读回会分配或复制内存，因此与稳态帧分开报告。
`soc_hiz_level_query` 不计入普通帧和阶段耗时。

## 测量与校验

输入生成、内存预触碰、资源分配、结果校验和验证用深度读回都在计时区外。
每个性能 case 先预热至少 5 次和 250 ms，再自动标定迭代次数，使每个样本的计时
负载至少达到 `--sample-ms`。默认保留全部 15 个样本，不删除离群值，并报告：

- median、P95、MAD、min 和 max；
- 每个样本的有效 operation 次数；
- 阶段吞吐所需的分辨率、三角形、实例与查询数量；
- `soc_stats`、visibility 分布和确定性 checksum。

若 `MAD / median > 3%`，结果会在 stderr 和 JSON 中标记为 noisy。每个 case
在采样前后都检查返回码、统计量、可见性分布及 checksum；Hi-Z 阶段还会在
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

默认只报告回退；添加 `-DFAIL_ON_REGRESSION=ON` 才返回失败。P95 用于诊断，
不参与判定，也不生成会掩盖单项退化的综合分数。

为减少噪声，应关闭高负载后台任务、保持 CPU 电源/散热状态一致、避免同时
运行多个 benchmark，并在同一启动方式下交替测量基线与候选。跨机器或不同
编译器的结果只适合记录，不应直接作为回归结论。
