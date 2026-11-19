# 实验方法与可复现性

## 环境

数据在本机（Linux x86-64 VM，8 逻辑核，11 GB RAM）采集；编译器 GCC 15.2，CMake 4.2.3，构建配置 `-DCMAKE_BUILD_TYPE=Release`。编译命令与 commit 一起保存在仓库中，重跑请按下面的步骤。

## 测量协议

- 驱动：`tools/bench.cpp`，输出 CSV 一行。
- 每项配置重复 2–3 次，报告**最短**耗时（降低宿主调度噪声）；重复次数记录在 CSV 的 `reps` 列。
- 每次运行都报告 `primes`，与已知 π(n) 对照；不正确的数据不会被采用。
- `marks` 是确定性的位清除计数，不受调度影响，作为时间之外的第二指标。
- 每项实验只改变一个参数（单变量对照）。

## 复现

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure && cd ..
bash experiments/sweep.sh
```

`sweep.sh` 会重新执行 A/B/C/D 四组扫描并把新行追加到 `experiments/RESULTS.csv`；历史数据保留，便于跨机器对比。

单机单次测量示例：

```sh
./build/bench --n 1g --wheel 210 --bucket 1 --seg-kb 32 --reps 3
# limit,wheel,bucket,segment_kb,reps,seconds,primes,segments,marks
# 1000000000,210,1,32,3,1.9190,50847534,872,313898259
```

## 已知限制

- 宿主是共享 VM，绝对时间不可与其他机器直接比较；结论只看同一机器上的相对差异。
- 段大小扫描的 2% 差异接近噪声，未做统计显著性检验。
- 单机多线程实验（`--threads`）已加入；跨机切分不在本次研究范围内。
