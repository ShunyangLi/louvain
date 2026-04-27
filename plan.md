# Plan · case_community 研究的底层逻辑与执行路线

> **配套文档**：`case_community.md`（问题原始设想）、`case_community_findings.md`（分析结果报告）  
> **本文档的定位**：说明**为什么要这样做**（Part 1-2）+ **接下来怎么做**（Part 3-7）

---

## 目录

- [Part 1 · 详细原因：为什么要做这件事](#part-1--详细原因为什么要做这件事)
- [Part 2 · 方法论基础：为什么这个方法是可辩护的](#part-2--方法论基础为什么这个方法是可辩护的)
- [Part 3 · 已完成工作清单](#part-3--已完成工作清单)
- [Part 4 · 执行 Plan（按优先级）](#part-4--执行-plan按优先级)
- [Part 5 · 里程碑与时间线](#part-5--里程碑与时间线)
- [Part 6 · 风险与应对](#part-6--风险与应对)
- [Part 7 · 产出物与投稿规划](#part-7--产出物与投稿规划)

---

# Part 1 · 详细原因：为什么要做这件事

## 1.1 从"计数"到"归属"的问题跃迁

此前的俄乌 case 用 subgraph matching 回答了**计数问题** —— "2022 后俄罗斯的论文/基金掉了多少？"。这个数字很有冲击力，但回答不了一个更关键的问题：

> **剩下没掉的那 70% 科研，跑到哪里去了？谁接盘了空缺？是否涌现出新的"科研小集团"？**

这是 subgraph matching 做不到的事，因为它**需要预设 pattern**（"我去找俄罗斯+德国合作"），而"新阵营"本身就是未知的，没有预设 pattern。

**Community detection 是唯一能回答"归属"问题的工具**，因为它**自下而上涌现出阵营**，不要求预设标签。你给它一张图，它告诉你"根据连接密度，这些节点天然归成一类"。

这个从计数到归属的跃迁**就是整个研究的核心价值**。计数告诉你"损失量"，归属告诉你"重新分配的方向"。对制裁、战争、科技脱钩这类大事件，**方向远比量级重要** —— 因为方向才决定下一步的政策响应。

## 1.2 为什么 Louvain 是唯一合适的工具

候选算法比较：

| 算法 | 对我们的问题是否合适 | 不合适的原因 |
|---|---|---|
| **Louvain** | ✅ | 适合大图、产生层次化社区、对加权无向图原生支持、modularity 可解释 |
| Label Propagation | ❌ | 结果对初始化敏感，难以复现；无层级；无全局 objective function |
| Girvan-Newman | ❌ | O(n³) 复杂度，跑不动 29M 顶点的全图 |
| Spectral Clustering | ❌ | 需要预设 k（社区数），恰恰违背"自下而上涌现"的前提 |
| Stochastic Block Model | ⚠️ | 统计上更严谨，但需要先验分布假设；对大规模稀疏图数值不稳定 |
| Infomap | ⚠️ | 基于信息流，更适合 directed 图；我们的 co-author 本质无向 |
| Leiden | ✅✅ | Louvain 的改进版，修复了 disconnected community 问题；**未来可作为 robustness check** |

**当前选择 Louvain 的理由**：
1. NeuG 已有 extension 实现且跑通
2. 性能够用：239 国图毫秒级完成，29.7M 全图 13 分钟完成
3. 业内标准：引用超 25,000，审稿人不会质疑算法合理性
4. 产生的 modularity Q 有明确物理含义（vs 随机基线的偏离度）

**后续可加的 robustness**：Leiden 算法跑一遍，对比社区划分是否一致。这是 paper 附录常见的验证。

## 1.3 为什么必须投影到国家图（而非直接在原图跑）

**原图上跑 Louvain** 的问题：

- 顶点混杂：Publication / Organization / Author / Funding / Datasource 全部混在一起
- 边语义混杂：`hasAuthor` 和 `cites` 和 `fundedBy` 边的结构意义完全不同
- 结果难解释：一个社区里混着几万篇 pub、几千个 org、几百个 author —— 这"社区"到底是"同一领域"还是"同一国家"还是"同一资助机构"？无法分辨。

我们确实**也跑了全图 Louvain**作为 baseline（29.7M 顶点 → 541,008 社区），但结果**无法直接回答 case_community 的问题**，只能作为数据规模的可行性证明。

**投影到国家图** 的必要性：

case_community.md 的五个 case（RU、中美、Brexit、TR、IR）全部是**国家级问题**。要回答"俄罗斯去哪了"，必须有一张**节点 = 国家、边 = 国家间合作强度**的图。这张图不存在于原数据里，必须从 `Publication → Organization` 二部图**投影**出来。

投影的语义很清晰：

```
"两国 A 和 B 在 window W 内的合作强度"
  = Σ over pubs p in [W]: (A 和 B 都有作者单位在 p) × 1
```

这是一个**可解释、可验证、可 debug** 的定义，且学界使用已久（bibliometrics 标准做法）。

## 1.4 为什么这个问题值得做（学术影响力评估）

**科学意义**：
- **首次系统性地量化 sanctions 对科研网络的 topology 改造**。文献里有大量关于"sanctions → publication count 下降"的 paper，但**几乎没有从 community structure 角度看 realignment 的**。
- 通过 Case A + E 的合并叙事，建立一个 **"sanctions topology spectrum"** 的概念框架 —— 急性休克 vs 慢性下沉。这是**方法论贡献**（之后任何被制裁国家都可以放到这个谱上做比较）。
- Case B 的中美脱钩**在 CHIPS Act 时间轴上得到了精确的结构印记**，这对 science of science 领域是新材料。

**政策意义**：
- 指出"制裁让俄罗斯孤立"是**错误的政策诊断** —— 真正发生的是"西欧自愿离开"，这对理解制裁的实际效力至关重要。
- Pipeline lag（5+ 年的滞后）是**未被充分讨论的政策盲区**：政策制定者假设合作中断是瞬时的，实际是渐进的。

**传播潜力**：
- 所有 headline 都有反直觉张力，适合 Nature News / Science Feature / Wired 长文
- 可视化材料（Sankey / heatmap / trajectory）已在 `figures/` 准备好
- 故事化结论已在 `case_community_findings.md` 每个 case 下写好

这不是一个"小而美"的 paper，是**一个有政策含义、方法贡献、传播钩子的综合性研究**。值得投入 3 个月认真做。

---

# Part 2 · 方法论基础：为什么这个方法是可辩护的

这一节是为了**抢在审稿人之前回答可能的质疑**。

## 2.1 Louvain 在国家投影子图上的合法性

**形式要求 · Louvain 只需要满足三条**：

| 条件 | 我们的投影图 | 代码保证 |
|---|---|---|
| 无向图 | ✅ | `louvain_functions.h:780-781` `adj_[a].push_back({b,w}); adj_[b].push_back({a,w});` 双向填充 |
| 非负边权 | ✅ | 权重 = 共作次数累加，算术上 ≥ 0 |
| 有限节点 / 边 | ✅ | 239 节点，最多 C(239,2) = 28,441 可能边 |

**Modularity Q 对投影图仍良定义**：

$$Q = \frac{1}{2m} \sum_{i,j} \left[ A_{ij} - \frac{k_i k_j}{2m} \right] \delta(c_i, c_j)$$

公式只需要加权邻接矩阵 A、总权重 m、度数 k_i。这三个量**对任何加权无向图都存在且唯一**，与 A_ij 的语义来源无关。

**算法实现也是图语义无关的**：`LouvainComputer::Compute()` 的主循环只读 `adj_`，不关心它是来自全图还是投影图。Mode 1 和 Mode 2 共用完全相同的 `LocalMove` + coarsening 代码。

## 2.2 投影带来的语义代价（已识别 + 应对方案）

| 代价 | 影响 | 应对 |
|---|---|---|
| 信息压缩：454,601 orgs → 239 countries | 无法观察机构内部异质性 | 国家级结论不受影响；机构级留给未来 follow-up |
| "超级边"指数放大：k 国论文产生 k(k-1)/2 条边 | 大协作论文污染结构 | **已过滤** `max_coauthors > 30` 的论文；需要扫 {10, 20, 50, 100} 做敏感度 |
| 权重未按领域/IF 加权 | "阵营内部中心性"结论受影响 | 对"谁和谁聚一起"的 first-order 问题影响小，可接受 |
| Louvain 非确定性（seed 敏感） | 社区 id 跨 run 不同 | 方法论上用 Jaccard 对齐；多 seed 跑 consensus 是未来的 robustness |

## 2.3 时间切片的 rationale

**为什么用 2-year window 而不是 1-year 或 5-year**：

| window 长度 | 利 | 弊 |
|---|---|---|
| 1 年 | 时间精度高 | 每年 pub 数不够 → 图稀疏 → Louvain 抖动大 |
| **2 年** | **pub 密度够 + 时间精度可接受** | - |
| 3-5 年 | 结果稳定 | 跨窗边界事件被洗掉（如 2022 Feb 战争 vs 2022-2023 window） |

**2 年是 bibliometrics 的经验甜区**。2022-2023 window 同时包含战争、CHIPS Act、学术脱钩三个事件，信号强；但覆盖到 2022 Feb 战争开始的完整 23 个月。

**为什么选这 4 个 window**：

| Window | 政治含义 |
|---|---|
| 2018-2019 | JCPOA 崩溃 + 贸易战开启 + 脱欧前夜 |
| 2020-2021 | COVID 期 + 战前稳态 |
| 2022-2023 | 俄乌战争急性期 + CHIPS Act |
| 2024 | 战后稳态（数据不完整，作脚注）|

P1 阶段会加 2022、2023 单年 window 做更精细的时间定位。

## 2.4 跨窗口 Jaccard 对齐的必要性

**问题**：Louvain 每次运行的 community id 是**任意**的。2020-2021 的 community 3 和 2022-2023 的 community 3 **不是同一个社区**，只是两次运行各自编号到了 3。

**解决**：用"社区成员集合的 Jaccard 相似度"对齐：

> 对国家 c，定义其在 window w 的邻域 N(c, w) = {和 c 落在同一社区的其他国家}  
> 跨窗口稳定性 = |N(c, w) ∩ N(c, w+1)| / |N(c, w) ∪ N(c, w+1)|

Jaccard ≈ 1 ：邻居几乎不变 → 国家没迁移  
Jaccard ≈ 0 ：邻居大换血 → 重大结构迁移事件

这是我们识别"RU 在 2022 发生大迁移"的关键算法（观察到 Jaccard 从 0.64 跌到 0.34）。

## 2.5 Sanity checks 清单（做 robustness 时逐项验证）

| 检查项 | 期望 | 当前结果 |
|---|---|---|
| Iberoamérica（ES/PT/BR/AR/MX）在多 window 稳定 | 是 | 3/4 ✅（2022-2023 异常待解，见 §10.1）|
| DE-FR 永远同社区 | 是 | 4/4 ✅ |
| 中美 2022 前同社区、2022 后分开 | 是 | ✅ |
| RU 社区在 2022 发生 Jaccard 跃迁 | 是 | 0.64 → 0.34 ✅ |
| Modularity Q 单调递增（如果假设"阵营化"）| 否（我们的发现）| 非单调 —— 2020-2021 峰值 |
| Louvain 随机种子下结果稳定 | 待做 | P1 任务 |

---

# Part 3 · 已完成工作清单

## 3.1 代码实现

| 文件 | 行数 | 功能 |
|---|---|---|
| `extension/louvain/src/louvain_extension.cpp` | 64 | Louvain extension 注册到 NeuG |
| `extension/louvain/include/louvain_functions.h` | 1312 | 核心算法 + 两种 BuildGraph（全图 / 投影）+ Compute |
| `extension/louvain/test/test_louvain_install.cc` | 533 | 入口 + 全图 Louvain test + 4-window sweep + pivot CSV 输出 |

**已解决的关键工程问题**：
- ✅ READ_ONLY / READ_WRITE 模式的 LOAD extension 兼容性（`VertexTable::EnsureCapacity` rehash 崩溃已修）
- ✅ Louvain OpenMP 24 线程并行（Compute 阶段）
- ✅ 全图 380M 边的 `BuildGraph` 在 22 秒内完成（部分并行）
- ⚠️ `BuildCountryCoauthorProjection` 主循环仍串行（未来 P2 优化点）

## 3.2 数据产出

| 产物 | 位置 | 规模 |
|---|---|---|
| 全图 Louvain 结果 | `hk:/tmp/p/neug_louvain/louvain_1776937809000.csv` | 29.7M 行 |
| 分 label 顶点属性 | `hk:/tmp/p/neug_louvain/props_1776937815705/` | 7 个 CSV |
| 4 个 window 的 coauthor CSV | `hk:/tmp/p/neug_louvain/louvain_coauthor_*.csv` | 每个 239 行 |
| **Pivot CSV**（主分析产物）| `hk:/tmp/p/neug_louvain/pivot_country_community_1776937937289.csv` | 239 行 × 4 window |

**本地镜像**：`/tmp/louvain_results/pivot_country_community_1776937937289.csv`

## 3.3 分析与可视化

| 文件 | 功能 |
|---|---|
| `/tmp/louvain_results/analyze_pivot.py` | 7 面板 pivot 分析：社区大小、邻域对比、Jaccard 稳定性、迁移排名、RU 追踪 |
| `/Users/lsy/Desktop/neug/figures/make_figures.py` | 生成 6 张 paper-ready PNG |

**6 张图**：
1. `fig1_modularity_trajectory.png` —— Modularity 非单调轨迹
2. `fig2_ru_community_shrink.png` —— RU 社区规模 76→67→28→26 滑坡
3. `fig3_key_country_comembership.png` —— 31×31 co-membership 矩阵
4. `fig4_jaccard_stability.png` —— 31 国 × 3 转换 Jaccard 热力图
5. `fig5_sankey_2020_vs_2022.png` —— 2020-2021 → 2022-2023 Sankey
6. `fig6_distance_from_west_eu.png` —— RU/IR/CN/TR/BR 的距离轨迹

## 3.4 报告文档

| 文件 | 行数 | 用途 |
|---|---|---|
| `case_community.md` | 155 | 原始问题设想（5 个 case） |
| `case_community_findings.md` | 734 | 完整分析报告（12 章，含故事化结论） |
| `plan.md` (本文档) | ~500 | 原因 + 执行计划 |

---

# Part 4 · 执行 Plan（按优先级）

## 4.1 P0 · 写 paper 前必做（**必须在 2 周内完成**）

这一批是**blocker**：不解决这些，paper 的任何 claim 都可能被推翻。

### P0-1 · 验证 ES/PT 2022-2023 异常 ⚠️

**背景**：ES 和 PT 在 2018-19, 2020-21, 2024 都稳定在 Iberoamérica，**唯独 2022-2023 跳到 West-EU**（Jaccard 0.03，见 `case_community_findings.md` §10.1）。不解决这个异常，所有涉及 Iberoamérica 的结论都不可靠。

**执行步骤**：

```sql
-- Step 1: 检查 ES 机构的 2022-2023 合作国分布
MATCH (p:Publication)-[:hasAuthorInstitution]->(o:Organization {country_code:'ES'}),
      (p)-[:hasAuthorInstitution]->(o2:Organization)
WHERE p.year IN [2022, 2023] AND o2.country_code <> 'ES'
RETURN o2.country_code AS partner, count(DISTINCT p) AS n
ORDER BY n DESC LIMIT 20;

-- Step 2: 同样查 2020-2021 做对比
MATCH (p:Publication)-[:hasAuthorInstitution]->(o:Organization {country_code:'ES'}),
      (p)-[:hasAuthorInstitution]->(o2:Organization)
WHERE p.year IN [2020, 2021] AND o2.country_code <> 'ES'
RETURN o2.country_code AS partner, count(DISTINCT p) AS n
ORDER BY n DESC LIMIT 20;

-- Step 3: 检查拉美机构在 2022-2023 的 publication 覆盖率
MATCH (p:Publication)-[:hasAuthorInstitution]->(o:Organization)
WHERE p.year IN [2022, 2023] AND o.country_code IN ['BR', 'AR', 'MX', 'CL', 'CO']
RETURN o.country_code, count(DISTINCT p) AS n
ORDER BY n DESC;
```

**判断标准**：
- 如果 ES 在 2022-2023 的 top 合作国从拉美变成德法意荷 → **真实效应**（Horizon Europe 框架拉动），写进 paper 作子 finding
- 如果 BR/AR/MX 的 2022-2023 pub count 比 2020-2021 显著下降 → **OpenAIRE 覆盖率问题**，需要数据修正或说明局限

**预计工作量**：半天

### P0-2 · 跑单年 window 2022 / 2023 锁定 RU 迁移时间精度

**背景**：目前只能说"RU 在 2022-2023 某个时刻发生了结构迁移"，但不知道具体是战争开始就迁、还是 2023 才完成。对 paper 的"响应速度"论证至关重要。

**执行步骤**：

1. 修改 `test_louvain_install.cc:499-504` 的 `kWindows` 列表：

   ```cpp
   const std::vector<std::pair<int64_t, int64_t>> kWindows = {
       {2020, 2021},  // 战前基线
       {2022, 2022},  // 战争第一年
       {2023, 2023},  // 战争第二年
       {2022, 2023},  // 合并作对照
   };
   ```

2. 重新 build + run：`./extension/louvain/test/test_louvain_install`

3. SCP 新 pivot CSV 回来

4. 扩展 `analyze_pivot.py`，加一个 "RU-DE 同社区 timeline" 面板

**判断标准**：
- 如果 2022 单年 RU-DE 已 diff → 响应速度**秒级**（惊人发现，可加入 paper headline）
- 如果 2022 单年 RU-DE 仍 SAME、2023 才 diff → 响应速度**年级**（pipeline lag 的 1 年精度）

**预计工作量**：1 天（build + run + 分析）

### P0-3 · 跑 max_coauthors 敏感度扫描

**背景**：当前只试了 `max_coauthors = 30` 一个阈值。审稿人会问"你把这个阈值改成 10 或 100，结论是否还成立？"

**执行步骤**：

1. 在 `louvain_functions.h` 里暴露 `max_coauthors` 作为 CALL 参数（已经是 6 参数 overload 的一部分）
2. 对 2022-2023 window 扫阈值 {10, 20, 30, 50, 100, 无过滤}
3. 对每个阈值观察：
   - RU 社区成员是否稳定？
   - DE-FR 是否仍同社区？
   - 中美分裂是否仍可见？
   - Modularity 是否在同量级？

**判断标准**：如果 RU / DE-FR / 中美这三个核心 finding 在 {20, 30, 50} 这个 "合理区间" 内都稳定，说明结论不依赖阈值选择；写进 paper 的 supplementary。

**预计工作量**：2-3 天

### P0-4 · 数据清洗（ISO 码归一化 + 聚合码白名单）

**背景**：`case_community_findings.md` §10.3 列的污染源（URY vs UY、EU、OC、YU、AN、微国家）会让社区计数失真。写 paper 前必须清理。

**执行步骤**：

1. 在 `BuildCountryCoauthorProjection()`（`louvain_functions.h:646-655`）加归一化：

   ```cpp
   static const std::unordered_map<std::string, std::string> ISO3_TO_ISO2 = {
       {"URY", "UY"}, {"BRA", "BR"}, // ... 完整列表 ~250 项
   };
   static const std::unordered_set<std::string> EXCLUDED_CODES = {
       "EU", "OC", "YU", "AN", "SU", // 聚合码 / 历史码
   };
   
   if (EXCLUDED_CODES.count(cc_str)) continue;
   auto norm_it = ISO3_TO_ISO2.find(cc_str);
   if (norm_it != ISO3_TO_ISO2.end()) cc_str = norm_it->second;
   ```

2. 加一个 degree 阈值过滤：构造完 agg 后，drop 掉度数 < 10 的国家（微国家噪音）
3. 重新跑 4 window sweep，对比 before/after 的 community 划分是否改善

**预计工作量**：1 天

---

## 4.2 P1 · Paper 主章节支撑（**4 周内**）

### P1-1 · Robustness：Leiden 算法对比

在同一投影图上跑 Leiden，检查社区划分是否与 Louvain 一致（Normalized Mutual Information > 0.8 即可接受）。

### P1-2 · Consensus clustering：多 seed 共识

每个 window 跑 100 次 Louvain（不同随机种子），用 co-occurrence matrix 构建 consensus community。对比单次 Louvain 的结果，给出稳定性置信度。

### P1-3 · 社区语义化标签对齐

在 `make_figures.py` 的 `semantic_label()` 基础上系统化：跨 window 匹配每个社区到语义标签 {WEST_EU, EAST_BLOC, EAST_ASIA, US_PACIFIC, IBERO, GLOBAL_SOUTH}，输出对齐后的 pivot CSV。这样可以直接贴到 paper Table 1。

### P1-4 · 添加 2015-2017 前基线（如覆盖率允许）

跑 `CALL LOUVAIN_COAUTHOR(2015, 2017)` 作 Case A 的"深基线"，验证"RU 一直在 EU 社区"是长期稳态还是近期才形成。OpenAIRE 2015 之前的覆盖率需要先检查。

### P1-5 · 画三张 paper-ready 图的论文级别版本

当前 `make_figures.py` 产出的图是工作图。paper 级别需要：
- 一致的字体（LaTeX Computer Modern）
- 一致的配色（ColorBrewer qualitative）
- 矢量格式（PDF / SVG）
- bilingual labels（for journal with international audience）

---

## 4.3 P2 · 扩展深度（**8 周内**，可选）

### P2-1 · 子领域切分

限定到 AI / 半导体 / 生命科学 / 材料等**政治敏感度高**的领域，看 Case B 中美脱钩是否在这些领域**更剧烈**（预期更高 Q 值 + 更快时间响应）。需要在投影前 `filter Publication.FOS CONTAINS 'X'`。

### P2-2 · 并行化 `BuildCountryCoauthorProjection` 主循环

当前是单线程 1-2 秒，不是瓶颈。但做单年 window + 子领域扫描后会增加 20 倍计算量，并行化值得做。

### P2-3 · 从国家级下钻到机构级

取 RU 社区里 top 100 机构，看它们的邻居在 2020-2021 vs 2022-2023 是怎么变化的。预期：俄科院这样的头部机构变化最大，小地方学院变化最小。

### P2-4 · Case B 专项 paper（如 A+E 投稿成功后）

中美脱钩单独成篇，聚焦 Pacific Rim 的三裂过程和各子领域差异。

---

## 4.4 P3 · 长期方向（**3-6 月**）

### P3-1 · 扩展到 funding 网络

同样的投影方法，但在 `Project -[fundedBy]-> Funding` 和 `Project -[hasOrganization]-> Organization` 上跑。测试：基金流动的阵营结构和 publication 合作阵营是否一致？

### P3-2 · 动态社区检测算法

用 Tiles / DyNMoG 等**原生**支持时序的社区检测算法，替代当前"多 window + Jaccard 对齐"的拼接方法。可以更平滑地追踪社区演化。

### P3-3 · 和政策事件时间轴做因果推断

目前是描述性分析。下一步：对每个政策事件（CHIPS Act、JCPOA、Brexit），用 interrupted time series 或 synthetic control 方法估计"反事实科研结构"，给出因果量化。

---

# Part 5 · 里程碑与时间线

```
Week 1  ━━━━ P0 所有项目（ES/PT 异常 + 单年 window + 敏感度 + 清洗）
Week 2  ━━━━ P0 完结 + Paper outline 定稿
Week 3  ━━━━ P1-1 Leiden + P1-2 Consensus
Week 4  ━━━━ P1-3 语义标签对齐 + P1-5 论文图出稿
Week 5-6 ━━━ Paper 主稿 (Case A §4, Case E §6)
Week 7  ━━━━ Paper 主稿 (Case B §5, limitations §10)
Week 8  ━━━━ 合作者内部 review
Week 9-10 ━━ 修订稿
Week 11 ━━━ 投稿 (首选 Nature Human Behaviour 或 PNAS)
Week 12 ━━━ arXiv preprint + 社交媒体传播
```

**关键 Gate**：
- End of Week 2：P0 全部完成，且 ES/PT 异常有明确结论（真实效应 or 数据问题）
- End of Week 4：Paper 主章节的数据、图、表全部就绪
- End of Week 8：内部 review 通过，没有 claim 被推翻

---

# Part 6 · 风险与应对

| 风险 | 概率 | 影响 | 应对 |
|---|---|---|---|
| ES/PT 2022-2023 异常是 OpenAIRE 覆盖率问题 | 中 | 高 —— 影响 Iberoamérica 所有结论 | P0-1 必须先判断；若属实，paper 改口径 "exclude Iberoamérica" |
| max_coauthors 敏感度显示结论不稳定 | 低 | 极高 —— 核心 finding 被推翻 | P0-3 必做；若不稳定，改用机构级分析（P2-3）|
| Leiden / Louvain 结果差异大 | 低 | 中 —— 算法 claim 需改措辞 | P1-1 做；若大差异，paper 改成 "using Louvain, we find..." 的条件式表述 |
| 审稿人质疑 Q 值低 | 高 | 中 —— 需花篇幅解释 | `case_community_findings.md` §3.1a 已准备好应答 |
| 2024 数据不完整引起争议 | 中 | 低 —— 主结论不依赖 2024 | 在 paper 明确标注 "2024 partial data, used only for trend confirmation" |
| OpenAIRE 数据源的 bias / missing countries | 中 | 中 —— 有些小国可能系统性缺失 | P0-4 数据清洗时列出所有 edge case；paper 的 data section 说明 coverage limits |
| 合作者对 "西欧脱离俄罗斯" 这个 counter-intuitive 叙事犹豫 | 中 | 低 —— 叙事调整 | 准备两个备选 headline：strong claim 版和 hedged 版 |

---

# Part 7 · 产出物与投稿规划

## 7.1 核心产出物

| 类型 | 标题候选 | 目标期刊 |
|---|---|---|
| **主 paper** | *Sanctions Didn't Isolate — They Reroute, Unevenly: Two Sanctioned Nations, Two Topologies* | Nature Human Behaviour / PNAS / Science Advances |
| **Data paper** | *Country-level Co-authorship Louvain Communities from 30M OpenAIRE Publications, 2015-2024* | Scientific Data |
| **方法论工具 paper** | *NeuG-Louvain: Scalable Community Detection on Heterogeneous Graph Databases* | Software Impacts / SoftwareX |

## 7.2 长尾产出

- arXiv preprint (同时发)
- 一份 executive summary (面向政策圈 / 媒体)
- 一份中文摘要 (面向国内学术圈)
- 公开 Louvain pivot dataset + 分析脚本 (Zenodo / OSF)
- Blog post / Twitter thread (传播)

## 7.3 投稿前 checklist

- [ ] 所有 claim 都能追溯到具体数据（无 hand-waving）
- [ ] Modularity 解读部分已按 `case_community_findings.md` §3.1a 写入正文
- [ ] ES/PT 异常已解决或明确标注
- [ ] 敏感度分析已在 supplementary
- [ ] 代码 + 数据公开链接已准备
- [ ] 伦理声明（OpenAIRE 数据已公开，无 PII 问题）
- [ ] 作者贡献声明

---

**文档版本**：2026-04-24  
**下一次更新触发**：P0 任何一项完成后更新对应章节
