# Hyperedge 视角下的 Case 发现算法全景

> **核心观念**：在 hypergraph 语义里，一个 **hyperedge** 就是一组顶点 `{v₁, v₂, ..., vₖ}`。  
> Community detection 只是其中一种产生 hyperedge 的方式（通过 modularity 优化）。  
> **任何能在图上产出"有意义的顶点集合"的算法，都能喂进我们的 case-finding 框架。**  
> 这个视角把我们的工具箱从"1 个 Louvain"扩展到**十几个家族的算法**，每个家族对"什么是一组相关节点"有不同的定义。

---

## 目录

1. [为什么要广义化](#1-为什么要广义化)
2. [算法家族盘点 —— 按 hyperedge 语义分类](#2-算法家族盘点--按-hyperedge-语义分类)
3. [同一 case，不同算法 lens 看到的不同东西](#3-同一-case不同算法-lens-看到的不同东西)
4. [只有 non-Louvain 算法才能做的新 case](#4-只有-non-louvain-算法才能做的新-case)
5. [算法 × Case 对照矩阵](#5-算法--case-对照矩阵)
6. [推荐的 Multi-Lens 组合策略](#6-推荐的-multi-lens-组合策略)

---

## 1. 为什么要广义化

### Louvain 的盲区

Louvain 给每个节点**分配恰好一个社区**（partition），并优化 modularity。这在三种情况下**会误导**：

1. **桥节点本质上同时属于多个社区** —— Louvain 强制二选一，信息丢失  
   例：土耳其在 MENA + Central Asia 之间，Louvain 逼它选一个
2. **重要结构不是"社区"而是"特定模式"** —— Louvain 找不到"RU-DE-UK 三角"这种具象结构  
   Motif / subgraph matching 更合适
3. **高阶相互作用被拉平** —— 一篇 10 国联合论文对 Louvain 变成 45 条二元边，丧失"10 国同时坐一张桌"的信息  
   Hypergraph Louvain / tensor methods 保留

### 广义框架

```
输入：图 G = (V, E)（可以是 hypergraph、temporal graph、labeled graph...）
算法 A：G → 2^V 的子集族 H = {h₁, h₂, ...}，其中每个 hᵢ ⊆ V
解读：每个 hᵢ 是一个"hyperedge"——一组在某种意义上"相关"的节点

Case-finding：
  - 对比 H_{window_1} 和 H_{window_2}：哪些 hyperedge 消失了？哪些新出现？
  - 追踪特定关注节点（RU、CN、IR）在 hyperedge 集合中的 membership 变化
```

**Community detection 对应的特例**：H 是 V 的一个**严格划分**（每个节点恰好在一个 hᵢ 里）。  
**放宽这个约束**，就打开了大量新算法。

---

## 2. 算法家族盘点 —— 按 hyperedge 语义分类

### 🔵 Family 1 · Partition-based（严格划分）

每个节点恰好属于一个 hyperedge（社区）。

| 算法 | hyperedge 语义 | 特征 | 适合 |
|---|---|---|---|
| **Louvain** | modularity 最优划分 | 快、层级、已用 | 当前 case |
| **Leiden** | 改进的 Louvain（修复 disconnected） | 比 Louvain 严格 | 同 Louvain 但做 robustness |
| **Label Propagation** | 标签传播收敛态 | 快、不稳定 | 不推荐主用，可作 baseline |
| **Infomap** | 最小描述长度（信息流） | 对有向图友好 | 用于 **citation 图**（directed） |
| **SBM (Stochastic Block Model)** | 生成式概率社区 | 给出**不确定性估计** | 论文强度低时的置信度分析 |
| **Spectral Clustering** | 图拉普拉斯特征向量 | 需预设 k | 已知要分 N 簇时 |

### 🟢 Family 2 · Overlapping（重叠社区）

一个节点可以属于多个 hyperedge。

| 算法 | hyperedge 语义 | 适合 |
|---|---|---|
| **k-clique percolation (CFinder)** | 共享 k-1 节点的 k-团链 | 找**桥节点** = 多社区成员 |
| **BigCLAM** | 节点 → 多社区亲和度矩阵 | 大规模可扩展 |
| **SLPA** | 每个节点维护标签包 | 可调重叠度 |
| **Link Community** | 先给**边**聚类，再解析 node membership | 自然产生重叠 |

**对我们的价值**：**土耳其同时在 MENA 和 Central Asia**、**乌克兰同时在 Europe 和 Post-Soviet**、**英国同时在 Europe 和 Anglosphere** —— 这些信号 Louvain 看不到，overlapping 算法能看到。

### 🟡 Family 3 · Dense Subgraph / Clique-based

找局部**超密集**的节点集合。

| 算法 | hyperedge 语义 | 适合 |
|---|---|---|
| **k-clique enumeration** | 完全子图 K_k | 找"所有人都合作过"的小团 |
| **k-truss decomposition** | 每条边至少在 (k-2) 个三角形里 | 比 k-clique 快，语义近似 |
| **k-core decomposition** | 每个节点度 ≥ k | 找图的"核心"与"外围" |
| **Quasi-clique / k-plex** | 允许少量缺失的准团 | 论文工厂、小圈子 |
| **Dense subgraph discovery** | 密度最大的子集 | 异常检测 |

**对我们的价值**：
- **论文工厂检测**（Case T）的正解 —— paper mill 本质上是 quasi-clique
- **CERN 俱乐部**（Case L）—— 大协作就是 k-clique
- **消失的三角/四团**：战前存在战后消失的特定 small clique

### 🟣 Family 4 · Motif / Subgraph Pattern

找**具体形状**的节点组合。

| 算法 | hyperedge 语义 | 适合 |
|---|---|---|
| **Motif counting（三角、方、星）** | 特定 3-4 节点模式 | 小规模、具象 |
| **Graphlet decomposition** | 所有 k-节点 isomorphism 类计数 | 结构指纹 |
| **Temporal motif** | 按时序排列的模式 | "A 先 B 后 C 的合作链" |
| **Subgraph matching（Cypher 查询）** | 用户定义的具体 pattern | **你们已经用过** |

**对我们的价值**：**Case A 的"消失的桥梁"可以用具体 subgraph 表达** —— `MATCH (ru:Country {code:'RU'})-[:cooperate]-(de:Country {code:'DE'})-[:cooperate]-(uk:Country {code:'UK'})-[:cooperate]-(ru)`，战前战后数一下这个三角形的 instance 数。**Louvain 看"谁和谁一组"，motif 看"哪些具体合作消失"**。这是两种互补 lens。

### 🔴 Family 5 · Hypergraph-Native（高阶相互作用）

直接在 hypergraph 上算，每个超边是一篇论文的**全部合作者**。

| 算法 | hyperedge 语义 | 适合 |
|---|---|---|
| **Hypergraph Louvain / Leiden** | 原生多方合作的社区 | 不丢失多元合作信息 |
| **Tensor decomposition (NNTF)** | 张量分解出潜在成分 | 找"合作模式类型" |
| **Simplicial complex analysis** | 高阶 homology（2-simplex、3-simplex...） | 拓扑分析 |
| **Persistent homology** | 跨 scale 的拓扑特征 | 找"持久的合作结构" |

**对我们的价值**：当前 projection 把一篇 10 国论文变成 45 条边。Hypergraph 原生方法保留"10 国同时坐一桌"的一阶信息。**如果 hypergraph Louvain 给出的社区和 pairwise Louvain 不同，说明 pairwise 遗漏了重要的高阶信息**。

### 🟠 Family 6 · Temporal / Dynamic

跨时间追踪演化的 hyperedge。

| 算法 | hyperedge 语义 | 适合 |
|---|---|---|
| **Multilayer Modularity (Mucha 2010)** | 跨时间共同优化的社区 | 避免每 window 独立跑的抖动 |
| **Dynamic Community Detection (DCD)** | 社区的 birth/death/merge/split 事件 | Case O（Sticky Teams）正解 |
| **Tiles / Trace / DyCPM** | 流式社区更新 | 未来实时分析 |
| **Event-based community discovery** | 按触发事件切窗 | 2022 Feb 作为"外部切点" |

**对我们的价值**：当前我们用"独立 window + Jaccard 对齐"拼凑时序分析。动态社区检测可以**原生产出社区生命周期事件**："Community X 在 window 2 分裂为 X1 和 X2"。这种语义比 Jaccard 匹配更干净。

### 🟤 Family 7 · Embedding + Clustering

先把节点嵌入到向量空间，再用传统聚类。

| 算法 | hyperedge 语义 | 适合 |
|---|---|---|
| **DeepWalk / Node2Vec + K-means** | embedding 空间邻近 | 快、可扩展 |
| **GraphSAGE + 聚类** | 归纳式 embedding | 支持新节点 |
| **GCN / GAT + 聚类** | 监督/半监督聚类 | 有标签时更强 |
| **Graph Transformer + 聚类** | 全局 attention | 大模型方向 |

**对我们的价值**：Embedding 捕获的是**结构等价性**（两节点在图中的角色相似）而非**邻接性**（两节点直接连接）。美国和中国可能结构等价（都是超级枢纽），但 Louvain 把它们分到不同社区；embedding + 聚类会把它们放一起。**这是回答"哪些国家扮演相似角色"的正确 lens**。

### ⚫ Family 8 · Role / Structural Equivalence

不关心"谁和谁是朋友"，关心"谁扮演相似的角色"。

| 算法 | hyperedge 语义 | 适合 |
|---|---|---|
| **RolX** | 非负矩阵分解出的角色类别 | 节点分类（hub / bridge / periphery） |
| **GraphWave** | 基于扩散模式的角色 embedding | 更细粒度 |
| **Structural equivalence blockmodeling** | 等价类划分 | 经典社会网络分析 |

**对我们的价值**：**俄罗斯可能在 2022 前是"欧洲 hub"，2022 后变成"东欧 hub"** —— 角色变化比社区变化更微妙也更有故事性。Role-based analysis 是 Louvain 不能替代的 lens。

### ⚪ Family 9 · Frequent Pattern Mining

挖掘高频出现的子图模式。

| 算法 | hyperedge 语义 | 适合 |
|---|---|---|
| **gSpan** | 频繁子图 | 找"典型合作形状" |
| **CloseGraph** | 闭合频繁子图 | 去冗余 |
| **FFSM** | 快速频繁子图 | 大规模 |

**对我们的价值**：可以发现"**不同区域的典型合作结构形状**" —— 欧洲是 clique-heavy（所有人互相合作），东亚是 star-heavy（一个 hub + 多 spokes）。这是 Louvain 看不到的**形态学**发现。

### 🟥 Family 10 · Link Prediction / Counter-factual

不是找"存在的"hyperedge，而是找"应该存在但不存在"的。

| 算法 | hyperedge 语义 | 适合 |
|---|---|---|
| **Neighbor-based LP** | 共邻预测 | 简单快速 |
| **GNN-based LP** | embedding 相似度预测 | 精度更高 |
| **SVD-based** | 低秩补全 | 可解释 |

**对我们的价值**：**"战争假如没发生，俄德本该多发多少论文？"** 用 2020-2021 预测 2022-2023，对比真实 2022-2023，差值就是战争的**反事实效应**。这是因果量化的正解。

---

## 3. 同一 case，不同算法 lens 看到的不同东西

拿我们最熟悉的 **Case A（Russia）** 过一遍，看每种算法 lens 各自能挖到什么：

| 算法 lens | 对 RU 能给出的 finding |
|---|---|
| **Louvain (partition)** | "RU 从 EU 社区迁到 East-Bloc 社区"（当前已有） |
| **Leiden** | 同上，但社区划分更严格，可作 robustness |
| **Infomap (directed)** | "信息从 EU 流向 RU 的路径在 2022 后减弱 40%"（用 citation 方向） |
| **SBM** | "RU 在 2022-2023 属于 East-Bloc 社区的概率 = 0.87（有不确定性）" |
| **CFinder (overlapping)** | "RU 在 2020 同时属于 {EU, Slavic} 两个社区；2022 后只属于 Slavic" |
| **k-clique (k=4)** | "2020 存在 127 个包含 RU 的 4-clique，2022 后只剩 23 个" |
| **k-truss** | "RU 从 trussness=8 降到 trussness=3" —— 从**深度嵌入**到**边缘 loose 连接** |
| **Motif counting** | "RU-DE-UK 三角消失了 84 次，RU-PL-CZ 三角几乎保留"（具体 pattern 粒度） |
| **Subgraph matching** | 用户定义 pattern，直接查个数（最具象、故事强） |
| **Hypergraph Louvain** | 用 publication 作 hyperedge：RU 从"多元西欧协作"掉到"少元东欧协作" |
| **Multilayer modularity** | RU 的社区演化事件：**split**（EU → {West-EU, East-Bloc} at t=2022） |
| **RolX (role)** | "RU 从'欧洲核心 hub'变成'东欧次级 hub'" —— 角色降级 |
| **Node2Vec + 聚类** | RU 在 embedding 空间从 DE/FR 的邻近 0.3 距离，挪到 PL/CZ 的邻近 0.4 |
| **Link prediction** | "按 2020 trend，2022-2023 该有 RU-DE 联合论文 1400 篇，实际 420 篇 → 缺口 70%" |

**每一行都是一个独立可引用的 finding**。同一个事件，10+ 种算法 lens 给出 10+ 种视角。

**这就是方法论论文的 selling point**：不是"我们用 Louvain 做了 X"，而是"**我们用多个 hyperedge-producing 算法构建了制裁结构冲击的多棱镜画像**"。

---

## 4. 只有 non-Louvain 算法才能做的新 case

下面这些 case 是 Louvain 根本回答不了的 —— 必须用其他算法。

### **Case α · 多成员轨迹（Overlapping）**

**问题**：乌克兰 2018 同时属于 {Europe, Post-Soviet} 两个社区。战后它失去了 Europe 成员资格，还是失去了 Post-Soviet 成员资格，还是两个都失去了，自成新社区？

**算法**：CFinder (k-clique percolation) 或 SLPA

**故事**：战争**打破了多元归属**本身。Louvain 二选一的 partition 掩盖了这个更细腻的故事。

**Schema 利用**：现有国家共作图，无需新边

**工作量**：1 周（主要是 CFinder 算法实现/调库）

---

### **Case β · 消失的三角（Motif）**

**问题**：在俄乌战争前，有多少个 3 国合作三角形（e.g. RU-DE-UK、CN-US-KR）是"活跃的"（比如 100+ 篇联合论文）？战后消失了多少？具体是哪些？

**算法**：Triangle counting（motif 最简形式）+ subgraph matching

**故事**：**一份具体三角形列表** —— "This collaboration triangle died in 2022: Russia-Germany-UK physics partnership (847 papers in 2018-2021 → 62 in 2022-2023)"。极具象。

**Schema 利用**：现有国家共作图

**工作量**：3-5 天（三角枚举有成熟算法）

---

### **Case γ · 角色退化（Structural Role）**

**问题**：俄罗斯在欧洲科研网络中的"角色"从 2020 的 hub 降级到 2022 的 periphery？还是其他角色变化？

**算法**：RolX 或 GraphWave（role-based embedding）

**故事**：不是"俄罗斯去哪了"，而是"**俄罗斯在科学生态中的功能变了**"。功能变化比位置变化更本质。

**Schema 利用**：机构级或国家级图

**工作量**：1-2 周

---

### **Case δ · 反事实损失（Link Prediction）**

**问题**：如果没有俄乌战争，2022-2023 应该有多少篇 RU-DE / RU-UA / RU-FR 联合论文？实际的"缺口"有多大？

**算法**：GNN-based link prediction（用 2018-2021 数据训练，预测 2022-2023 边权）

**故事**：**因果性量化**。不再是"RU 的社区变了"这种描述，而是"**战争导致了 N 篇本该存在的合作论文永远没有发生**"。

**Schema 利用**：现有图，但需要 time-aware LP 框架

**工作量**：2-3 周（需 PyTorch Geometric 或 DGL）

---

### **Case ε · 高阶 vs 二阶的差异（Hypergraph）**

**问题**：如果用**publication 作为 hyperedge** 跑 hypergraph Louvain，得到的社区和我们当前 pairwise Louvain 相同吗？不同的话，差异在哪？

**算法**：Hypergraph Louvain（论文作者集合作 hyperedge）

**故事**：如果显著不同 —— 说明 pairwise projection **系统性偏见**多元合作少、双边合作多的国家对。如果相似 —— 说明我们当前结果稳健。两种结果都有价值。

**Schema 利用**：Publication → Organization → country_code，聚合成 hyperedge 集合

**工作量**：1-2 周（需要 hypergraph Louvain 实现）

---

### **Case ζ · 论文工厂的准团结构（Quasi-clique）**

**问题**：Paper mill 的签名是**几乎完全的相互合作** —— 不完全是 k-clique，但 80-90% 的可能合作都发生了。Louvain 会把他们当成紧密社区，但**k-plex / quasi-clique** 直接识别这个语义。

**算法**：k-plex enumeration 或 (k, l)-quasi-clique

**故事**：**比 Louvain 更精确的论文工厂指纹**。Louvain 可能把正常的紧密研究团队和 paper mill 混在一起；quasi-clique 算法能通过"完全性阈值"区分。

**Schema 利用**：Author 共作图

**工作量**：2-3 周

---

### **Case η · 合作形状的地理差异（Frequent Subgraph）**

**问题**：欧洲、北美、东亚、Global South 的**典型合作子图形状**是什么？是 star（一个 PI + 许多 collaborators）还是 clique（所有人互相合作）还是 chain（接力式合作）？

**算法**：gSpan（频繁子图挖掘）

**故事**：**合作**不只是"多少"和"谁"的问题，还有**形状**问题。不同地区的合作形状差异反映不同的**学术组织文化**。

**Schema 利用**：Publication 级子图，按作者 country 打 label

**工作量**：2-3 周

---

### **Case θ · 动态社区事件（Dynamic Community Detection）**

**问题**：我们当前的"多 window + Jaccard"是拼凑时序分析。用原生 DCD 算法（如 Tiles 或 Mucha's multilayer modularity），能否直接产出**社区演化事件列表**（birth、death、merge、split、resurgence）？

**算法**：Multilayer Modularity (Mucha 2010) 或 Tiles

**故事**：**一张"2018-2024 全球科研社区演化时间线"**，每个事件一行："t=2022.Q1, SPLIT: Europe-community → {West-EU, East-Bloc}, modularity gain +0.03"。极其清晰可读。

**Schema 利用**：现有数据，但需要 multi-layer 组织

**工作量**：3-4 周（算法复杂，但输出优雅）

---

## 5. 算法 × Case 对照矩阵

| Case / Algorithm | Louvain | Leiden | SBM | CFinder | k-clique | Motif | Subgraph Match | Hyper-Louvain | Multilayer | RolX | Node2Vec | Link Pred |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A · Russia 迁移 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| B · 中美脱钩 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| E · 伊朗沉没 | ✅ | ✅ | ✅ | ✅ |  |  | ✅ |  | ✅ | ✅ | ✅ | ✅ |
| G · Citation 图 | ✅ | ✅ | ✅ |  |  |  |  | ✅ |  |  |  |  |
| H · AI 脱钩 | ✅ | ✅ |  |  | ✅ | ✅ | ✅ |  |  |  |  |  |
| I · Funding 网络 | ✅ | ✅ | ✅ | ✅ |  |  |  | ✅ | ✅ |  |  |  |
| M · 机构级桥梁 | ✅ | ✅ |  |  | ✅ | ✅ | ✅ |  |  | ✅ | ✅ |  |
| T · Paper Mill | ✅ | ✅ |  |  | **⭐** | ✅ |  |  |  |  |  |  |
| α · 多归属轨迹 |  |  |  | **⭐** | ✅ |  |  |  |  |  |  |  |
| β · 消失三角 |  |  |  |  |  | **⭐** | **⭐** |  |  |  |  |  |
| γ · 角色退化 |  |  |  |  |  |  |  |  |  | **⭐** | ✅ |  |
| δ · 反事实损失 |  |  |  |  |  |  |  |  |  |  |  | **⭐** |
| ε · 高阶差异 |  |  |  |  |  |  |  | **⭐** |  |  |  |  |
| ζ · 准团签名 | ✅ |  |  |  | ✅ |  |  |  |  |  |  |  |
| η · 形状地理 |  |  |  |  |  | ✅ | ✅ |  |  |  |  |  |
| θ · 社区事件 | ✅ | ✅ |  |  |  |  |  |  | **⭐** |  |  |  |

✅ = 可以用  
**⭐** = 最佳 / 唯一适合

---

## 6. 推荐的 Multi-Lens 组合策略

### 🎯 策略 A · 核心 case 的三棱镜

主 paper（Case A/B/E）加 **3 个补充 lens**：

1. **Leiden**（robustness）—— 证明 Louvain 结果在更严格算法下仍成立
2. **CFinder 或 SLPA**（overlapping）—— 揭示 UK、UA、TR 这类多归属国家的真实 membership 变化
3. **Motif counting**（具象）—— 给出"消失的三角/四团"的**具体清单**

预期：paper 的证据厚度从"1 种 lens 的 3 个 case"提升到"**3 种 lens 的 3 个 case** = 9 个独立证据"。

### 🎯 策略 B · 方法论 paper：Multi-Lens Framework

直接把"multi-lens case finding"本身作为主题写一篇 methodology paper：

- Title: ***"A Hyperedge Multi-Lens Framework for Detecting Structural Ruptures in Scientific Collaboration Networks"***
- 贡献：不是"我们发现了 X"，而是"**我们提出了一个统一框架**，可以用任何产出 vertex-set 的算法分析结构冲击"
- 实证：把同一事件（俄乌）在 5-8 个 lens 下分析，展示不同 lens 产生互补 insight

这比纯 finding paper 的理论贡献更大，适合投 Nature Communications / PNAS / PLOS Complex Systems。

### 🎯 策略 C · 深入一个 non-Louvain lens

挑 1 个 non-Louvain lens **做透**：

**推荐 Case δ · 反事实损失**。理由：
- 这是 **因果推断** 方向，方法论新颖
- 故事极强："如果没有战争会怎样？"
- GNN link prediction 是热门技术，有实现工具
- 可以独立成 AI for Science 领域的 paper

### 🎯 策略 D · 低投入高收益的捷径

**Case β · 消失的三角** 是最快的选项：

- 算法简单（三角枚举有成熟实现）
- 数据已就绪（现有共作图）
- 输出极具象（具体三角形列表）
- 可以直接拼到当前 paper 作为 §5 "A List of Broken Collaborations"

**工作量 3-5 天，故事强度 ★★★★★**。是我最推荐**立即做**的补强。

---

## 基础设施：统一的 Hyperedge 输出接口

为支持多 lens 分析，建议定义统一的 data structure：

```python
@dataclass
class Hyperedge:
    nodes: FrozenSet[str]      # 该 hyperedge 包含的节点
    algorithm: str              # "louvain" / "cfinder" / "triangle" / ...
    window: str                 # "2020-2021"
    weight: float = 1.0         # hyperedge 的强度（可选）
    metadata: Dict = field(default_factory=dict)  # 算法特定信息（如 modularity score）

@dataclass
class HyperedgeSet:
    algorithm: str
    window: str
    edges: List[Hyperedge]
```

然后所有分析函数接受 `HyperedgeSet` 而不是算法特定的数据结构。**这样任何新算法加入只需实现 `G → HyperedgeSet` 的接口**，下游分析代码（Jaccard 对齐、迁移检测、可视化）完全复用。

这是 1-2 周的基础设施投入，之后**每增加一个新 lens 的成本从 2-3 周降到 3-5 天**。

---

## 和之前文档的衔接

| 文档 | 定位 | 关系 |
|---|---|---|
| `case_community.md` | 原始问题设想（5 个 case） | 问题出处 |
| `case_community_findings.md` | Case A/B/E 分析报告 | 已执行（用 Louvain 一种 lens） |
| `plan.md` | 总执行计划 | Part 4 待更新，加入 multi-lens 策略 |
| `community_detection_cases.md` | 社区检测 case 清单 | **本文档是它的上位抽象** |
| `hyperedge_algorithms_for_cases.md` (**本文档**) | 算法家族全景 + 新 case | 全景图 |

建议下一步：**更新 `plan.md` 的 Part 4** 加入 "Phase 2.5 · Multi-Lens Augmentation"，把 Case β（消失三角）作为最先做的 non-Louvain 补强。

---

**文档版本**：2026-04-24  
**下一次扩展触发**：任选一个 non-Louvain lens 实际跑一轮后，更新其实际效果 vs 预期
