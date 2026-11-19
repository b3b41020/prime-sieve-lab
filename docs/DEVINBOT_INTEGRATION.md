# DevinBot 联调闭环

这一节回答一个问题：**Bot 到底怎么把这套筛法当成工具用，怎么知道结果可信，出了问题怎么自证。** 所有组件都在本仓库里，可以直接跑。

## 1. 集成方式：HTTP 工具适配层，不改 Bot 内核

选的是**Bot 通过原生 function calling 调 HTTP 工具**（`bot/tools.json` 是标准 JSON Schema 工具定义），而不是让 Bot 去编译 C++ 或解析 CSV：

```
DevinBot (LLM + 原生 tool_calls)
        │  POST /api/sieve/count | /api/sieve/autotune | /api/selftest
        ▼
bot/bridge.py (stdlib HTTP, 默认 127.0.0.1:8787)
        │  subprocess + 超时 + 参数白名单
        ▼
build/bench  /  build/check   ← 从本仓库源码编译
        │
        ▼
JSON：实测秒数、π(n)、段数、位清除次数、git sha
```

为什么这样接：Bot 只需认识三个 JSON 工具，不需要知道 C++、CMake 或实验细节；换 Bot 框架只要重新注册 `bot/tools.json`，不用改筛法。工具返回的每个数字都来自真实二进制，bridge 不生成任何“看起来合理”的假值。

## 2. Bot 自动跑哪组参数搜索

`sieve_autotune` 实现的是**两阶段受控搜索**（每次都真跑 `bench`）：

| 阶段 | 规模 | 搜索空间 | 用途 |
|------|------|----------|------|
| 粗筛 | `min(n, 10⁷)` | wheel ∈ {2,30,210} × bucket ∈ {0,1} × seg ∈ {16,32,64} KB = **18 组** | 用廉价规模淘汰明显差的组合，1 次/rep |
| 复验 | 目标 `n`（Bot 传入） | 粗筛 Top-3 | 用目标规模实测定榜，2 次/rep 取最小值 |

约束：总预算 `budget_seconds`（5–180s）硬性截断；超范围 `limit`、非法轮宽/段大小直接 400；每个子进程有 timeout。

真实运行示例（`n=10⁹`, budget=60s）：

```
POST /api/sieve/autotune {"limit":1000000000,"budget_seconds":60}
→ elapsed 12.47s, stage1_screened 18
  210 bucket=0 seg=64KB → 1.9350s  π=50847534  marks=313898259
  210 bucket=0 seg=32KB → 1.9548s  π=50847534  marks=313898259
  210 bucket=0 seg=16KB → 2.0186s  π=50847534  marks=313898259
  winner: wheel=210 bucket=false seg=64KB
```

注意：在 10⁹ 尺度上 bucket 开/关的差距（~2–3%）接近宿主 VM 噪声，所以自动调参可能选 `bucket=false`；在 10¹⁰ 尺度上同一搜索会稳定选出 `bucket=true`（21.41s vs 22.73s）。**Bot 必须按目标规模搜索，不能拿小 n 的结论外推**——这也是研究结论的一部分。

## 3. 自测 hook 挂在哪

三层，都在真实链路上：

1. **任务前 preflight**：Bot 每次测量前先调 `sieve_selftest` → `bot/check` 全组合正确性（3 轮宽 × 2 队列模式 vs 朴素试除 + π(10⁹/10¹⁰) 已知值），返回 `ok` 和 git sha。
2. **契约校验**：`bot/selftest_hook.sh` 额外检查 `bench` 输出必须是 9 列 CSV、π(10⁶)=78498、工具 schema 三个函数齐全。任何一项失败 → 非零退出码，CI/Bot 都不得继续。
3. **CI 门禁**：`.github/workflows/ci.yml` 在每次 push 上跑 C++ 构建 → ctest → preflight hook → `tests/test_bot_bridge.py`（真起 HTTP server，走完 health/selftest/count/autotune/错误参数全部路径）。

git sha 会出现在每次响应里（`git_sha` 字段）：Bot 拿到的结果能和具体的 commit 对上，避免出现“跑的是旧二进制”这种扯皮。

## 4. 失败与边界

| 情况 | 行为 |
|------|------|
| 未 build | bridge 启动即报错退出，不会返回假数据 |
| 编译过但源码改了 | preflight 的 sha 与 Bot 记录的 sha 不一致 → 拒绝使用结果，要求重建 |
| limit > 10¹⁰ / 非法轮宽 / 非法段大小 | 400 + 明确 error，不静默截断 |
| 子进程超时 / bench 崩溃 | 400 + 异常类型，不把半截输出当结果 |
| 端口 | 默认只绑 `127.0.0.1`；这是研究工具，不是公网服务 |
| 并行作业 | selftest 用全局锁串行；count/autotune 各自独立子进程，无共享状态 |

## 5.  Bot 侧的调用顺序（可直接写进系统提示词的工具编排）

```
1. sieve_selftest                       → ok=true 且 sha 与 CI 通过的那次一致
2. sieve_count(1e6, 210, true, 32, 1)   → π=78498 复核链路（便宜的端到端冒烟）
3. sieve_autotune(target_n, budget)     → 拿到 winner 与全部实测数据
4. 用 winner 再跑一次 sieve_count(target_n)，作为最终报告数字
5. 把 JSON（含 git_sha）写进实验报告 / 提交审核材料
```

第 2、4 步不是多余的：第 2 步证明这次会话里工具链是通的；第 4 步让报告里的数字与调参搜索使用同一配置、同一二进制。

## 6. 本地复现命令

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
bash bot/selftest_hook.sh
python3 tests/test_bot_bridge.py                     # 11 项端到端断言
SIEVE_PORT=8787 python3 bot/bridge.py &              # 起服务
curl -s http://127.0.0.1:8787/health
curl -s -X POST http://127.0.0.1:8787/api/sieve/count \
     -d '{"limit":1000000,"wheel":210,"bucket":true,"segment_kb":32,"reps":1}'
curl -s -X POST http://127.0.0.1:8787/api/sieve/autotune \
     -d '{"limit":1000000000,"budget_seconds":60}'
```

## 7. 一句话给审核方

> 这个工程不是一段孤立代码：它有可复现的正确性套件、真实测量的参数扫描数据、一个带 git sha 的 Bot 工具接口、任务前自测 hook 和 CI 门禁。Bot 的每一步都调用编译后的真实实现，并返回可核对的数字与 commit——从研究问题到实验数据到自动化调用，闭环是完整可演示的。
