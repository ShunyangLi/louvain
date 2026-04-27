# Community Detection 可执行案例清单

> **定位**：从所有 case 候选中，**只挑出原生属于 community detection 问题的**，给出可直接落地的执行规格。  
> **不在此清单**：pure time-series（Case N/S/U/V/W）、distribution/Gini（P）、centrality-only（K）—— 这些虽然有价值，但不是 Louvain 能解决的问题。

---

## 目录

- [筛选原则：什么样的问题属于 community detection](#筛选原则)
- [🟢 Group 1 · 变投影，不换图结构（改 <100 行代码）](#-group-1--变投影不换图结构)
  - Case G · Citation Country Graph
  - Case H · AI/Semiconductor Sub-Sector Louvain
  - Case J · Climate Science Louvain
  - Case L · 大协作论文反转 Louvain
- [🟡 Group 2 · 新投影目标（改 200-500 行）](#-group-2--新投影目标)
  - Case I · Funding Agency Communities
  - Case M · Institution-Level Louvain
  - Case T · Paper Mill Community Detection
  - Case Y · Research School-of-Thought Detection（新增）
- [🔴 Group 3 · 算法扩展（改 >500 行或新算法）](#-group-3--算法扩展)
  - Case O · Temporal Sticky-Team Detection
- [推荐实施顺序](#推荐实施顺序)

---

## 筛选原则

Community detection 适合回答三类问题：

1. **"哪些节点天然聚成一类？"** —— 无标签聚类（我们当前 Case A/B/E 属于此类）
2. **"某个预设节点属于哪个涌现社区？"** —— 节点归属追踪（Case A 的 RU 追踪）
3. **"图中是否存在异常紧密的子结构？"** —— 异常/欺诈检测（Case T）

不适合 community detection 的：
- "Top X% 有多集中" → Gini
- "X 随时间怎么变" → time series
- "X 是否是桥节点" → betweenness centrality
- "两个节点之间 k 跳内可达吗" → path query

下面所有 case 都满足上面三类之一。

---

## 🟢 Group 1 · 变投影，不换图结构

**共同特征**：当前 `BuildCountryCoauthorProjection` 的代码改动 < 100 行就能完成。图结构（239 国 × 国家对加权边）保持不变，只是改 **数据源或过滤条件**。每个 case 都能在 2-3 天内跑出第一轮结果。

---

### **Case G · Citation Country Graph —— "科学的两张地图"**

**图定义**：
- 节点：239 个 country_code
- 边：`weight(A, B) = # of (pub_P 引用 pub_Q，其中 pub_P 有 org 在 A，pub_Q 有 org 在 B)`
- 无向化（或作为 **directed** Louvain 跑，更能抓"谁引用谁"的非对称性）

**Community detection 问题**：citation 投影出的 Louvain 社区**是否和 co-authorship 投影一致**？如果不一致，哪里分化？

**Schema 利用**：
```
Publication ─cites─▶ Publication ─hasAuthorInstitution─▶ Organization(country_code)
```

**预期三种发现**（每种都值得写）：
1. **结构一致** → 强化主 paper 的 robustness
2. **Citation 社区滞后 coauthor 社区 2-3 年** → 新发现：引用比合作更"死板"，政治信号响应更慢
3. **Citation 比 coauthor 更分化**（Q 更高）→ **"嘴上全球引用，实际各引各的"**

**代码 delta**：
- 新函数 `BuildCountryCitationProjection()`（仿照 `BuildCountryCoauthorProjection`）
- 内层循环：`cites_view.get_edges(pub_vid)` 代替 `pub_org_view.get_edges(pub_vid)`
- 聚合：citing pub 的作者国 × cited pub 的作者国，每对 +1

**工作量**：3-5 天。

**故事强度**：★★★★★

---

### **Case H · AI / Semiconductor Sub-Sector Louvain —— "脱钩最早发生在哪"**

**图定义**：同国家图，**但只在 AI/半导体领域的 publication 上投影**。

**Community detection 问题**：在政治最敏感的领域里，**中美脱钩是否比整体领域提前 3-5 年发生**？

**Schema 利用**：
```
Publication.FOS / Publication.subjects 字段做过滤：
  'computer science' ∩ ('artificial intelligence' ∪ 'machine learning')
  OR 'semiconductor' / 'integrated circuits'
```

**预期发现**（可 pre-register）：
- 2018-2019 window 的 AI 领域 Louvain 里，**CN 和 US 已不同社区**（比整体图早 4 年）
- 2022-2023 AI 社区的 Q 值**显著高于**整体 Q（阵营化程度更强）
- 跨领域对比：AI 强分化 > 半导体强分化 > 物理无分化

**代码 delta**：
- `BuildCountryCoauthorProjection` 的 publication 外层循环加一行 `if (!fos_match(pub_vid, filter)) continue;`
- 新参数 `std::vector<std::string> fos_filter`

**工作量**：需先确认 FOS 字段覆盖率（P0 任务），如果字段可用，2-3 天。

**故事强度**：★★★★★（Case B 的完整版）

---

### **Case J · Climate Science Louvain —— "唯一没脱钩的领域"**

**图定义**：同 Case H，但过滤 `FOS CONTAINS 'climate' / 'atmospheric' / 'environmental science'`

**Community detection 问题**：气候科学领域是否**不存在**中美脱钩？如果不存在，Louvain 社区结构应**始终**保持 CN + US 同社区。

**预期两种发现**：
1. ✅ 气候领域 CN-US 4/4 window 同社区 → **"The Last Common Ground"** 叙事
2. ❌ 气候领域也脱钩 → **"Even Climate Couldn't Escape"** 反转叙事

两种都有价值。是 Case H 的天然对照组。

**代码 delta**：Case H 的代码改 FOS filter 即可，2 小时。

**工作量**：1-2 天（与 Case H 共享 FOS 覆盖率审计）。

**故事强度**：★★★★

---

### **Case L · 大协作论文反转 Louvain —— "CERN 俱乐部的地理"**

**图定义**：同国家图，**只保留** `max_coauthors > 30` 的 publication（即我们当前过滤掉的那部分）。

**Community detection 问题**：在**被强制全球合作**的大协作项目里，还有没有阵营？

**预期发现**：
- 大概率 Q 值极低 → 证实"大协作不受地缘影响"
- 这本身就是 paper 的一个**负面结果章节**：有些科研形式是政治免疫的

**代码 delta**：加个 bool flag `invert_coauthors_filter`，一行代码修改。

**工作量**：半天。

**故事强度**：★★★（主要作为 Case A/B/E 的 robustness / contrast 章节）

---

## 🟡 Group 2 · 新投影目标

**共同特征**：不再是"国家投影"，图的**节点类型**本身换了。需要写新的 `BuildXProjection()` 函数，代码 delta 200-500 行。但架构上完全兼容 `LouvainComputer::Compute()`，复用所有算法代码。

---

### **Case I · Funding Agency Communities —— "资助机构的隐形联盟"**

**图定义**：
- 节点：每个 Funding agency（NSF, ERC, NSFC, JSPS, DFG, RSF, ...）
- 边：`weight(F1, F2) = # of Projects co-funded by F1 and F2`
- 或等价地：`weight(F1, F2) = # of Publications whose supporting projects include both F1 and F2`

**Community detection 问题**：全球资助机构**是否已经按地缘聚合**？即 NSF/NIH/NSF-C 聚成北美簇，ERC/DFG/ANR 聚成欧洲簇，NSFC 自成一簇等？

**Schema 利用**：
```
Funding ◀─fundedBy─ Project ─fundedBy─▶ Funding   (共资助关系)
  或通过 Publication:
Project ─result─▶ Publication  &  Project ─fundedBy─▶ Funding
  → 聚合: 每篇 Publication 有 projects 集合 → 对应的 Fundings 集合 → pair-wise 边
```

**关键假设 & 验证价值**：**Funding 网络应该比 coauthor 网络更"快"**。政策变了，资助决策立即变（月级），而 publication 要 2-5 年后才反映。如果 funding Louvain 的俄乌/中美裂变时点比 coauthor 早 2 年，**就直接量化了"政策 → 资助 → 论文" 的 pipeline lag**。

**代码 delta**：新函数 `BuildFundingCoFundProjection()`：
1. 遍历 Project，取 `(proj) -[fundedBy]-> (funding)` 的所有 funding
2. 对 (f1, f2) 对累加权重
3. 双向填充 adj_

**工作量**：需先做 Funding 节点数据覆盖率审计（OpenAIRE 小基金收录可能不全）。如果数据 OK，1 周。

**故事强度**：★★★★★（**方法论贡献大**：量化 pipeline lag）

---

### **Case M · Institution-Level Louvain —— "消失的桥梁列表"**

**图定义**：
- 节点：~50 万 Organization
- 边：`weight(O1, O2) = # of Publications with both O1 and O2 as authoring institutions`
- 按 window 分别跑

**Community detection 问题**：在机构层级，哪些**具体机构**从 2020-2021 的"跨阵营桥节点"变成了 2022-2023 的"孤立节点"？

**Schema 利用**：同 `BuildCountryCoauthorProjection`，但**不聚合到国家**，直接在 Organization 级别建图。

**Louvain 的角色**：
1. 每 window 跑 Louvain，得到机构级社区划分
2. 对每个机构 O，计算其在 window W 的"社区边界度"（`edges_to_other_communities / total_edges`）
3. **"消失的桥" = 2020-2021 边界度高（即跨社区合作多） 但 2022-2023 边界度降到近 0 的机构**
4. 按地缘/机构类型（university / industry / institute / company）统计

**故事钩子**：
- 具象化：**可以列出真实机构名字**（"华为 X 实验室"、"哈佛 Y center"）
- 可视化：做成 interactive scrollytelling，每个机构一个卡片
- **"200 Lost Bridges: A Registry of Collaborations Severed by the 2022 Rupture"**

**代码 delta**：
- 去掉国家聚合步骤（`BuildCountryCoauthorProjection:642-658` 那段）
- 节点数从 239 → ~500,000，性能需评估（Louvain 机构级可能跑 10-30 分钟，但能跑）
- 新增 `compute_boundary_ratio(org, window)` 分析函数

**工作量**：2-3 周（主要是性能调优 + 实体解析的 edge cases）。

**故事强度**：★★★★★（具象 + 传播爆款）

---

### **Case T · Paper Mill Community Detection —— "论文工厂的拓扑指纹"**

**图定义**：
- 节点：Author
- 边：`weight(A1, A2) = # of co-authored publications`
- 全图，不按时间切

**Community detection 问题**：paper mill 的合作模式在图上表现为**异常紧密且内向的小社区** —— 一群作者**只和彼此合作**，很少与外界。Louvain 能否自动识别这种结构？

**操作流程**：
1. 在 Author 全图跑 Louvain
2. 对每个社区 C，计算：
   - 大小 |C|
   - 内部边密度 `density(C) = edges_internal / C(|C|, 2)`
   - 边界比 `boundary(C) = edges_external / edges_total`
   - 论文集中度：社区内作者总 publication 数 vs 他们共同发表的 publication 数
3. **可疑社区 = 高内部密度 + 低边界比 + 共同论文占比高**

**故事钩子**：
- **"We found 500 suspicious author cliques in OpenAIRE"**
- 按国家/机构/年份分布这些可疑社区
- 验证：对 top 10 可疑社区做人工审查（查期刊、查论文内容），看是否真的是 paper mill

**为什么 Louvain 适合**：paper mill 正好对应 Louvain 最擅长的**"内部密集 + 外部稀疏"** 定义。Louvain 的 Q 值本身就是这个特征的度量。

**代码 delta**：
- 新的 `BuildAuthorCoauthorGraph()` 函数（Author × Publication bipartite 投影到 Author × Author）
- Louvain 跑完后新增 `analyze_community_anomalies()` 分析函数
- 节点数：可能千万级，需要性能评估

**风险**：直接指控具体机构/作者有法律风险，发表需要严谨 hedged language。

**工作量**：2-3 周。

**故事强度**：★★★★★（timely + methodological + policy）

---

### **Case Y · Research School-of-Thought Detection —— "学术思想流派自动识别"** (新增)

**图定义**：
- 节点：每个 FOS（Field of Study）主题词 / 每个 subject keyword
- 边：`weight(FOS1, FOS2) = # of publications tagged with BOTH FOS1 and FOS2`

**Community detection 问题**：OpenAIRE 的 Publication 多标签 FOS 字段能否通过共现模式涌现出**"学术流派"**？

**预期发现**：
- 一个社区可能是 **{"deep learning", "transformer", "NLP", "attention mechanism"}** → 现代 AI 学派
- 一个社区可能是 **{"SVM", "kernel method", "structural risk", "VC dimension"}** → 经典统计学习学派
- 追踪随时间：不同学派的社区规模变化 → **"哪个流派在消亡，哪个在崛起"**

**Schema 利用**：
```
Publication ─hasSubject─▶ FOS/Subject(name)
```
两两共现 → 加权共现图

**故事钩子**：
- **"The Invisible Tribes of Science: How Deep Learning Killed Kernel Methods in 10 Years"**
- 可以做**流派兴衰的动画**：每个 window 的 FOS Louvain 社区，相邻 window 间的 Sankey

**代码 delta**：新函数 `BuildFOSCooccurrenceProjection()`，类似 co-author 但 projection 目标是 FOS term。

**工作量**：1-2 周。

**故事强度**：★★★★★（纯元科学，独立成篇）

---

## 🔴 Group 3 · 算法扩展

**共同特征**：当前 Louvain 是**静态**的（单一 window）。如果要追踪**社区的时序演化**，需要"动态社区检测"算法。

---

### **Case O · Temporal Sticky-Team Detection —— "科研团队的寿命"**

**图定义**（动态版）：
- 节点：Author
- 边：co-authorship（按 window 切，每 window 一张图）
- 研究单位：**同一社区在连续 window 中的持久性**

**Community detection 问题**：一个 2018-2019 的作者社区（"团队"），有多少在 2020-2021 依然存在（即大部分成员仍属于同一社区）？

**方法**：
1. 每个 window 独立跑 Louvain（同 Case A 做法）
2. 对相邻 window 的社区做 **Jaccard 对齐**（同 `analyze_pivot.py` 里对国家做的事，但对作者做）
3. 定义"持久团队" = Jaccard ≥ 0.7 连续出现在 3+ windows
4. 统计持久团队的生命周期分布

**预期发现**：
- **"研究团队的平均寿命 3.2 年"** —— 一个可引用的元科学数字
- 领域差异：实验物理学团队 > 计算机科学团队
- 国家差异：终身雇佣制国家（日/德）的团队寿命更长

**为什么 Louvain 适合**：团队结构**是**社区结构的本体定义 —— 一个团队就是一组"内部密集合作、外部稀疏合作"的人。所以 Louvain 是自然选择。

**可选进阶**：换用 **Leiden** 或 **动态 Modularity** 算法（Mucha et al. 2010 的 multilayer modularity）来**联合优化**多个 window，避免单 window 独立 Louvain 产生的不稳定。

**代码 delta**：
- Author 级 BuildCoauthorProjection (同 Case T)
- 按 window 切
- 新算法：`align_communities_across_windows(communities_by_window)` 用 Jaccard 匹配
- 新分析：`detect_persistent_teams(aligned_communities, min_persistence=3)`

**工作量**：3-4 周。

**故事强度**：★★★★（元科学深度）

---

## 推荐实施顺序

### 🚀 Phase 1（2 周内）· 走完 Case A/B/E 的 robustness + 新发现

**只做 Group 1 的 G/H/J**。代码改动小，与当前 paper 直接互补。  
预期产出：Case A/B/E 主 paper 的 §11 robustness chapter 和一个新子章节"Sectoral Decoupling Timing"。

### 🎯 Phase 2（4-6 周）· 做 Case I + Case M，独立成篇

**Case I (Funding) + Case M (Institution)** 打包。Funding lag + institutional bridges 合成一个**方法论 paper**：  
*"People Move Fast, Money Moves Faster, Papers Move Slowest: Temporal Lag in Scientific Realignment"*

### 🌟 Phase 3（3+ 月）· 做 Case T，独立 paper / 高关注度

**Case T (Paper Mill Detection)** 独立成篇。这是**最具传播力和政策影响的**单一 case，也是把 NeuG + Louvain 能力作为**方法论贡献**兜售给科研评价体系的好机会。

### 🔬 Phase 4（长期）· 元科学方向

**Case O (Sticky Teams) + Case Y (School of Thought)** 合并成一本**元科学 monograph**（三章书稿或 review paper）。

---

## 跨 case 共享的基础设施投资（一次建，多次用）

做这些 case 前先把以下 3 件事抽象出来，避免反复造轮子：

1. **通用的 `BuildProjection()` 接口**：让 projection 逻辑参数化（节点源类型 / 边投影类型 / 过滤条件），当前 `BuildCountryCoauthorProjection` 变成一个 instance
2. **跨 window Jaccard 对齐库**（Python）：抽出 `analyze_pivot.py` 里的对齐逻辑，通用化到 author / org / fos 层面
3. **Louvain 社区语义标签库**：当前 `make_figures.py` 里的 `semantic_label()` 需要每个 case 重写，抽象成配置驱动的通用标签器

这 3 块 infrastructure 投资大约 1-2 周，完成后**后续每个 case 的实现成本减半**。

---

## 快照矩阵

| Case | Graph 尺度 | 节点类型 | 新函数行数 | 工作量 | 故事 |
|---|---|---|---|---|---|
| G · Citation Country | 239 节点 | country | +50 | 3-5 天 | ★★★★★ |
| H · AI Sub-Sector | 239 节点 | country | +20 | 2-3 天 | ★★★★★ |
| J · Climate Exception | 239 节点 | country | +5（复用 H）| 1 天 | ★★★★ |
| L · Big-Collab Only | 239 节点 | country | +10 | 半天 | ★★★ |
| I · Funding Agencies | ~1000 | funder | +150 | 1 周 | ★★★★★ |
| M · Institution Bridges | 500K | org | +200 | 2-3 周 | ★★★★★ |
| T · Paper Mill | ~10M | author | +400 | 2-3 周 | ★★★★★ |
| Y · FOS School | ~10K | subject | +150 | 1-2 周 | ★★★★★ |
| O · Sticky Teams | ~10M × 4 | author × window | +500 + alg | 3-4 周 | ★★★★ |

---

**文档版本**：2026-04-24  
**衔接文档**：`plan.md`（总执行计划，Part 4 的 P2/P3 部分可以被这里的 Phase 2/3 替换）
