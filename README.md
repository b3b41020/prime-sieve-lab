# SIE-Lab：分段素数筛的参数取舍实验

一个**自主设计与实现**的素数计数实验仓库。研究问题很具体：

> 在固定内存预算下，**轮宽（wheel modulus）**、**分段大小**和**跨段素数队列**分别如何影响分段筛的吞吐？标记操作数是主要成本吗？

仓库包含三样东西：一个可参数化的筛法实现（`src/`），一组**真实测量**的数据（`experiments/RESULTS.csv`），以及可复现的实验脚本（`experiments/sweep.sh`）。不依赖任何第三方素数库；正确性用两个独立写的朴素计数器交叉验证。

## 设计概要

- **余数位图**：以 `m ∈ {2, 30, 210}` 为模，每段只存与 `m` 互素的余数位。实验中的 `m=2` 就是常见的“只存奇数”方案，`m=210` 每段 210 个整数压成 48 bit（162/210 = 77% 的整数不占空间）。
- **分段驱动**：段大小可从命令行设置；内存占用与段大小线性相关，与 `limit` 无关。
- **跨段队列**：`p²` 落在未来段的素数进入一个按段索引的队列，被消费后按轮增量重新入队，而不是每段重新扫描。可开关以做对照。
- **独立性**：轮表、队列、位图和两个对照计数器都在这里从零实现；`tests/check.cpp` 对三种轮宽 × 两种队列模式做全组合校验，并对照 π(10⁹)=50847534 等已知值。

设计取舍与算法细节见 [`docs/design.md`](docs/design.md)，实验方法与环境见 [`docs/methodology.md`](docs/methodology.md)。

## 主要结果（真实测量，非估算）

完整数据：[`experiments/RESULTS.csv`](experiments/RESULTS.csv)，解读：[`experiments/RESULTS.md`](experiments/RESULTS.md)。

| 实验 | 结论 |
|------|------|
| 轮宽对照（n=10⁸，32KB 段） | 轮 210：**0.170s**，奇-only：**0.619s**，快 **3.6×**；位清除次数从 9628 万降到 2856 万（−70%） |
| 段大小扫描（n=10⁹） | 16KB→128KB 吞吐变化 <2%，32KB 附近无显著最优值；说明本实现瓶颈不在段缓存局部性 |
| 跨段队列（n=10¹⁰） | 开启 21.41s，关闭 22.73s，约 **6%** 收益；两种方式的位清除次数完全相同，收益来自减少素数遍历 |
| 规模扩展 | 10⁶→10¹⁰ 吞吐从 7.7 亿降到 4.7 亿整数/秒，随 n 增大对数因子上升，符合预期 |

## 编译与验证

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```

## 复现实验

```sh
bash experiments/sweep.sh          # 重新跑全部扫描，结果追加到 experiments/RESULTS.csv
./build/bench --n 1g --wheel 210 --bucket 1 --seg-kb 32 --reps 3
```

`bench` 输出一行的 CSV：`limit,wheel,bucket,segment_kb,reps,seconds,primes,segments,marks,threads`。取 `reps` 次中的**最短**耗时以降低噪声；正确性每行都核对 `primes`。

## 并行化（多线程，无竞态）

`--threads N` 走并行引擎：**协调器单线程簿记跨段队列 + 工作线程只读不可变作业清单 + 位图按块切片空间隔离 + 层级规则时间隔离**。竞态从结构上消除，并用「marks 与线程数无关」这一可证判据守住（`tests/check_parallel.cpp`）。

实测（8 逻辑核 VM）：10⁹ → 1.91s → 0.50s（8 线程，3.8×）；10¹⁰ → 21.2s → 7.4s（2.9×）。设计论证见 [`docs/parallelization.md`](docs/parallelization.md)，原始数据 [`experiments/PARALLEL.csv`](experiments/PARALLEL.csv)。

```sh
./build/bench --n 1g --wheel 210 --bucket 1 --seg-kb 32 --threads 8
```

## Bot 集成（DevinBot 等自动化 Agent）

仓库自带一层 Bot 适配：Bot 通过**原生 function calling** 调三个 HTTP 工具，不接触 C++ 或 CSV。完整联调说明见 [`docs/DEVINBOT_INTEGRATION.md`](docs/DEVINBOT_INTEGRATION.md)。

| 组件 | 作用 |
|------|------|
| `bot/bridge.py` | stdlib HTTP 服务（默认 127.0.0.1:8787），把 count / autotune / selftest 转成对真实二进制的调用，带超时与参数白名单 |
| `bot/tools.json` | 标准 JSON Schema 工具定义，直接注册给 Bot |
| `bot/selftest_hook.sh` | 任务前 preflight：正确性套件 + 输出契约 + 工具 schema + git sha |
| `tests/test_bot_bridge.py` | 端到端测试：真起服务、真跑二进制，覆盖成功与错误路径 |
| `.github/workflows/ci.yml` | 构建 → ctest → preflight → bridge 端到端，全绿才算通过 |

自动调参是**两阶段受控搜索**：10⁷ 上粗筛 18 组（轮宽 × 跨段队列 × 段大小），再在目标 n 上复验 Top-3，预算硬截断。示例（n=10⁹，预算 60s，实测 12.47s）：18 组筛完，winner `wheel=210, seg=64KB`。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
bash bot/selftest_hook.sh
python3 tests/test_bot_bridge.py
```

## 目录

```
src/               轮表、跨段队列、筛法核心、独立对照计数器
tools/bench.cpp    测量驱动（CLI → CSV）
tests/check.cpp    正确性全组合测试
bot/               Bot 适配层：HTTP bridge、工具 schema、preflight hook
experiments/       原始 CSV、实验脚本、结果解读
docs/              设计、方法学、Bot 联调说明
```

## 许可

MIT（见 `LICENSE`）。
