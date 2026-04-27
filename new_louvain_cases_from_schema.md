# 基于 schema.json 的新 Louvain Case（强故事性）

> **前置**：读完 `schema.json`（7 个 vertex label + 50+ edge label）后挖出的**之前遗漏的节点/边属性**所支撑的新案例。  
> **约束**：全部 Louvain 原生 + schema 已就绪 + 故事性优先。  
> **关系**：这是 `community_detection_cases.md` 的补充，不替代它 —— 那里的 case 偏"同口径下换变量"，这里的 case 偏"换完全不同的图结构"。

---

## 之前遗漏的关键 schema 信息

| 字段 / 边 | 价值 | 用它做什么 |
|---|---|---|
| `Funding.jurisdiction` (STRING) | **每个资助机构都有司法管辖区** —— NSF→US, ERC→EU, NSFC→CN, RSF→RU | Case Ω: 资助机构阵营 vs jurisdiction 标签 |
| `Project.startDate / endDate` | Project 有精确时间范围 | Case Π: H2020 vs Horizon Europe 时间切片 |
| `organization_IsChildOf_organization` | 机构层级关系（CNRS/CAS/MPG 等学院派系） | Case Σ: 按"学术王朝"聚合后的 Louvain |
| `publication_IsRelatedTo_community` | OpenAIRE 人工 curated 的主题社区 | Case Φ: 人工策展 vs Louvain 涌现对比 |
| `publication_IsAmongTopNSimilarDocuments_publication` | **内容相似度预计算边** | Case Ψ: 语义图 Louvain |
| `Author.country` + `Author.organization` | 作者级别的国家归属 | Case Θ: 作者级 Louvain + 流散追踪 |
| `publication_Reviews_publication` | 同行评议式引用 | （疑虑：语义模糊，先审计）|
| `datasource_hosts_publication` | 出版物的**实际 host 仓库** | Case Λ: 开放获取仓库的阵营化 |

---

## 🎯 Case Ω · 资助机构的隐形阵营（Funder Bloc Map）

### 故事钩子

> **"ERC 其实和 NSF 是一伙的，法国 ANR 反而像孤岛"** —— 政治上 ERC（欧盟）和 ANR（法国）是同一阵营，但在**论文级共同资助**上，我们可能发现 ERC 和 NSF 的重叠度远高于 ERC 和任何单一欧洲国家基金。**这是一个 invisible transatlantic elite funding network**，政治语言掩盖、数据揭示。

### 图构造

```
节点：Funding（所有资助机构，带 jurisdiction 属性）
边：Funding A ↔ Funding B 如果它们共同资助过同一个 Project
边权：shared projects 数
```

**Cypher 草图**：
```cypher
MATCH (f1:Funding)-[:fundedBy|funding_for_project_edges]->(p:Project)<-[:fundedBy|funding_for_project_edges]-(f2:Funding)
WHERE f1.id < f2.id
RETURN f1.shortName, f2.shortName, f1.jurisdiction, f2.jurisdiction, count(p) AS w
```

### Louvain 问题

Louvain 社区 **是否与 jurisdiction 对齐**？三种可能结果，每种都有故事：

1. **完全对齐**（社区 = 区域联盟）→ "funders cluster by politics" —— 符合直觉，但首次系统量化
2. **部分交叉**（如 ERC + NSF 同社区，NSFC + DFG 同社区）→ **"The Invisible Transatlantic Elite Network"** —— 反直觉 headline
3. **2022 前后变化**（RSF 在 2022 年从某个社区被挤出）→ Case A 在 funding 层面的镜像

### 预期 headline

> **"The Hidden Geography of Science Funding: Where the Money Really Flows"**

### 数据验证

`Funding.jurisdiction` 字段在 schema 里确实存在。需要先跑一条 audit：
```cypher
MATCH (f:Funding)
RETURN f.jurisdiction, count(*) AS n
ORDER BY n DESC
```

### 工作量

3-5 天。节点规模应该很小（~1000 个 funder），Louvain 毫秒级。

### 故事强度

★★★★★

---

## 🎯 Case Π · Horizon Europe 的真实断代（Brexit 案的正面执行）

### 故事钩子

> **"2020 年 12 月 31 日英国离开 H2020，但英国在欧洲科研 consortium 图上的位置真正发生位移是在 2023 年"** —— 用 `Project.startDate` 精确划分 H2020（2014-2020）和 Horizon Europe（2021-2027）两波项目，分别跑 Louvain，定位英国在每张图中的社区归属变化。

这是**原 case_community.md Case C（Brexit）用 FUNDING 而非 coauthorship 的正面执行**。coauthor 数据里 Brexit 几乎看不到（我们已经验证过），但 **project participation** 数据里 Brexit 是行政性的"一刀切"，应该信号极强。

### 图构造

```
节点：Organization (带 country_code)
边：Org A ↔ Org B 如果它们共同参与同一个 Project
边权：shared projects 数
时间切片：
  - H2020:  Project.startDate IN [2014-01-01, 2020-12-31]
  - HE:     Project.startDate IN [2021-01-01, 2027-12-31]
```

**Cypher 草图**：
```cypher
MATCH (p:Project)-[:hasParticipant]->(o1:Organization),
      (p)-[:hasParticipant]->(o2:Organization)
WHERE o1.id < o2.id
  AND p.startDate >= '2014-01-01' AND p.startDate <= '2020-12-31'
RETURN o1.id, o2.id, count(p) AS w
```

### Louvain 问题

- H2020 window：UK 机构是否和 DE/FR/NL 同社区？**答案几乎肯定是：是**
- HE window：UK 机构是否被挤出 EU 核心社区、形成孤立的 "Anglosphere residual" 小簇？
- 哪些 UK 机构**在 HE 下仍保持跨 EU 合作**（通过 associate member 身份）？哪些完全被切断？

### 预期 headline

> **"The Surgical Decoupling: How Horizon Europe's Exclusion List Rewrote the Map of European Research Consortia"**

### 故事层次

可以做**机构级具象化** —— 列出具体的 UK 大学名单：
- "牛津大学在 H2020 连接 127 个 EU 机构，在 HE 只连接 18 个"
- "Imperial College 在 H2020 是核心 hub，在 HE 被降级为边缘节点"

### 数据验证

`Project.startDate` 是 STRING，需先检查日期格式（ISO 8601? 还是 YYYY-MM-DD?）。

### 工作量

1-1.5 周。Organization 节点规模大（~50 万），但参与 EU project 的只占小部分，实际投影图可能几万规模。

### 故事强度

★★★★★（**原 Case C 的正确执行路径**）

---

## 🎯 Case Σ · 学术王朝的合作版图（Institutional Dynasty Louvain）

### 故事钩子

> **"中国科学院已经取代美国国家实验室系统，成为全球科研最中心的'学术帝国'"** —— 或者相反：**"即使在制裁下，俄科院仍通过 50+ 下属研究所维持了与 CNRS 的合作"** —— `IsChildOf` 边给我们**机构层级**，可以把 MPG 下 85 个研究所聚合成"MPG 帝国"、把 CAS 下数十个分院聚合成"CAS 帝国"，然后在这个**大约 5 万级的"学术王朝"节点**上跑 Louvain。

这和我们当前的"235 国家级" Louvain 是**互补视角**：国家级太粗（CNRS 和法国小学院都变成"FR"），机构级太细（每个实验室都是独立节点）。**"王朝级" 50k 节点的粒度是 sweet spot**。

### 图构造

```
第一步：识别"学术王朝"
  用 organization_IsParentOf_organization 递归聚合，每个 top-level parent 代表一个王朝
  即：把 "CNRS" 下所有被 parent 关联的 org 都吸收进 "CNRS umbrella"
  一个 Organization 归属到"离它最高的祖先 org"

第二步：构造 Dynasty × Dynasty 共作图
  每篇 Publication：
    取它所有 author orgs
    映射到各自的 dynasty
    对所有 dynasty pair 累加 1

第三步：在约 5 万 dynasty 节点上跑 Louvain
```

### Louvain 问题

- 中美两大"王朝联盟"（US NIH system vs Chinese CAS system）的跨 window 演化
- 俄罗斯 RAS（Russian Academy of Sciences）在 2022 前后的王朝归属
- 是否涌现出**新王朝联盟**（如 Belt-and-Road academic network）？

### 可能的震撼发现

- 即使在国家级 Louvain 看到 "US + DE 同社区"，王朝级可能揭示 "哈佛 + 中科院某所 > 哈佛 + 德国 DFG 某所"
- **个别研究所敢"出格"合作**的模式可能只在王朝级可见

### 预期 headline

> **"The Academic Empires: Mapping the Invisible Hierarchies of Global Science"**

### 数据验证

```cypher
MATCH (parent:Organization)<-[:IsChildOf]-(child:Organization)
RETURN parent.legalName, count(child) AS subsidiaries
ORDER BY subsidiaries DESC
LIMIT 30
```

预期看到 CNRS、MPG、CAS、RAS、UC system、NIH、Helmholtz 等。

### 工作量

2-3 周（主要是 dynasty 识别的递归聚合 + 边投影）。

### 故事强度

★★★★★

---

## 🎯 Case Φ · 人工策展 vs Louvain 涌现（OpenAIRE Curated Community 的盲区）

### 故事钩子

> **"我们找到了 12 个 OpenAIRE 人工策展遗漏的真正涌现学科"** —— OpenAIRE 维护了人工标注的 "Community"（如 "COVID-19"、"SDG research"、"European Marine Science"），这些是**自上而下**的策展。Louvain 在 publication-publication 相似度图上跑出的社区是**自下而上**涌现的。两者的**差集**就是**正在出现但还没被人工识别的新兴学科**。

这是一个**元科学级的发现**，完美展示 Louvain 作为"新兴领域探测器"的价值。

### 图构造

```
节点：Publication
边：publication_IsAmongTopNSimilarDocuments_publication （OpenAIRE 预计算的相似度边）
  或：publication_Cites_publication
边权：1.0（或 top-N 的排名反向权重）
```

### Louvain 问题

1. 跑 Louvain 得到涌现社区 C_L
2. 取出 OpenAIRE 人工社区 C_H（通过 `publication_IsRelatedTo_community`）
3. 对每个 Louvain 社区 c_l：计算与每个 OpenAIRE 社区 c_h 的 Jaccard 相似度
4. **高 Louvain 社区内聚性 + 低与任何 OpenAIRE 社区 Jaccard** → **新兴领域候选**

### 可能的产出

**"一张 Louvain 找到、OpenAIRE 没命名的新学科清单"**：
- "Climate × AI Foundation Models" —— 2023 涌现
- "LLM Alignment + Cognitive Science" —— 2024 涌现
- "Vaccine + mRNA Therapeutics" —— 2020 涌现（COVID 副产品）

### 预期 headline

> **"What the Taxonomists Missed: Emerging Sciences Detected by Louvain Community Detection"**

### 附加价值

可以直接**反馈给 OpenAIRE** 作为策展建议。这给 paper 增加**实际应用价值**。

### 工作量

1-2 周。主要成本在 Publication 级 Louvain 的规模（3000 万节点）—— 可能需要限定到**近 3 年**的 pub 做初步分析。

### 故事强度

★★★★★（方法论 + 实用价值双赢）

---

## 🎯 Case Ω2 · COVID 遗产：大流行锻造的合作活下来了吗？

### 故事钩子

> **"2020-2022 年间因为 COVID 临时绑定的那些跨国合作 —— 战后有多少活了下来？有多少解散了？"** —— 这是**一个自然实验**：全球科研网络中是否存在"COVID-forged alliances"，它们的生命周期有多长？

利用 OpenAIRE 已经标注的 COVID-19 Community：

### 图构造

```
步骤 1：定位 COVID 相关 pub
  MATCH (p:Publication)-[:IsRelatedTo]->(c:Community)
  WHERE c.name CONTAINS 'COVID' OR c.acronym = 'COVID-19'

步骤 2：对每个 window，构造 COVID pubs 的 co-author projection
  按 2020-2021 / 2022-2023 / 2024 切片

步骤 3：跑 Louvain
```

### Louvain 问题

- 2020-2021（COVID 高峰）：谁和谁同社区？
- 2022-2023（COVID 退潮）：这些社区是否依然存在？
- **"消失的 COVID 合作对"**：在 2020-2021 同社区，2024 已完全分开的机构/国家对列表

### 可能的震撼发现

- **"The 50 Alliances That Only Existed for the Pandemic"** —— 一份具体名单
- 或反面：**"Five Unexpected Alliances That Outlived COVID"** —— 如某些中美机构意外留存了合作

### 时间精度优势

COVID 有极清晰的时间轴（WHO 宣布大流行 2020-03-11，宣布结束 2023-05-05）。社区出生/死亡事件的时间定位可以精确到季度。

### 预期 headline

> **"Pandemic Collaboratories: The Short Life of Crisis-Forged Science"**

### 工作量

1-1.5 周（复用当前 co-author projection 代码 + 添加 community 过滤）。

### 故事强度

★★★★★

---

## 🥈 Tier 2 · 次优但仍值得做

### **Case Λ · 开放科学仓库的阵营化（Datasource Louvain）**

- 节点：Datasource（arXiv, bioRxiv, Zenodo, HAL, elibrary.ru, CNKI 等）
- 边：两个 datasource 共享的 publication 数（通过 datasource_hosts_publication）
- 故事：**"开放科学的基础设施是否也在地缘阵营化？"** 特别关注：俄国内 datasource（elibrary.ru）在 2022 前后的边际连接是否断裂；中国 CNKI 与国际仓库的关系

### **Case Θ · 作者级 Louvain 与流散追踪**

- 节点：Author（带 country/organization 属性）
- 边：共作图
- 规模挑战：千万级作者；需要先过滤到**高产作者**（如发表 ≥10 pub）
- 故事：**"The Scientists Who Hold the World Together"** —— 识别**跨 Louvain 社区的桥节点作者**，这些人是全球科研不碎裂的"人肉胶水"
- 附加：用 `Author.country` vs 实际发表机构分布，检测"流散学者"

### **Case Ψ · 语义图 Louvain（纯内容驱动）**

- 节点：Publication
- 边：`IsAmongTopNSimilarDocuments` 相似度边（不是 citation、不是 coauthor）
- 故事：**"The Content Map of Science"** —— 完全基于内容相似度的社区划分，和 citation-based / coauthor-based 对比
- 独特发现：内容相似但从不引用的"平行研究" —— 可能是领域隔离的反面证据

---

## 快照对照

| Case | 核心 schema 利用 | 图规模 | 故事 | 工作量 |
|---|---|---|---|---|
| **Ω · Funder Bloc** | `Funding.jurisdiction` + `funding_for_project_edges` | ~1000 节点 | ★★★★★ | 3-5 天 |
| **Π · Horizon Europe** | `Project.startDate` + `project_hasParticipant_organization` | ~数万 Org | ★★★★★ | 1-1.5 周 |
| **Σ · Academic Dynasties** | `organization_IsChildOf_organization` + hasAuthorInstitution | ~5 万 dynasty | ★★★★★ | 2-3 周 |
| **Φ · Curated vs Emergent** | `publication_IsRelatedTo_community` + similar docs | ~3000 万 Pub | ★★★★★ | 1-2 周（需子集）|
| **Ω2 · COVID Aftermath** | COVID Community + coauthor by year | ~百万 Pub | ★★★★★ | 1-1.5 周 |
| Λ · Datasource Louvain | `datasource_hosts_publication` | ~数千 Datasource | ★★★★ | 1 周 |
| Θ · Author Louvain | `publication_author_edges` + `Author.country` | ~千万 Author | ★★★★ | 2-3 周 |
| Ψ · Content Similarity | `IsAmongTopNSimilarDocuments` | ~3000 万 Pub | ★★★ | 1-2 周 |

---

## 推荐优先级

### 🚀 立刻做（下周 2 周）

**Case Ω · Funder Bloc Map**。理由：
- 节点规模最小（~1000），开发成本最低（3-5 天）
- `Funding.jurisdiction` 是非常漂亮的 ground truth
- 故事钩子极清晰（"The Hidden Geography of Science Funding"）
- 可以作为 Case A 的 **funding-level 镜像**，加入当前主 paper 的 §5 附论

### 🎯 2-4 周做

**Case Π · Horizon Europe Consortia**。理由：
- 完美解决原 case_community.md Case C（Brexit）的执行空白
- `Project.startDate` 给了干净的时间切片
- 可以**与 Case A（俄乌）配对**成一篇新 paper： *"Two Surgical Decouplings: UK's Horizon Exclusion vs Russia's Academic Self-Expulsion"*

### 🌟 1-2 月做

**Case Σ · Academic Dynasties**。理由：
- 实现成本较高（需要 dynasty 聚合）但 **payoff 极大**
- 基础设施投资：一旦实现 dynasty 聚合，可以**复用**于未来所有"机构级 Louvain"需求
- 故事钩子"学术帝国"具有极强传播力

### 🔬 长期

**Case Φ + Ω2** 合并成**元科学 paper**。COVID 作为自然实验 + 涌现学科检测，共同构成"Louvain 作为科学发现工具"的方法论 paper。

---

## 三个我现在就能确认有数据的审计 Cypher

跑完下面三条就能决定上面哪个 case 最先实施：

```cypher
// Audit 1: Funding.jurisdiction 的取值分布
MATCH (f:Funding)
RETURN f.jurisdiction, count(*) AS n
ORDER BY n DESC LIMIT 30;

// Audit 2: Project.startDate 的年份分布（验证 Horizon 时间切片可行）
MATCH (p:Project)
WHERE p.startDate IS NOT NULL
RETURN substring(p.startDate, 0, 4) AS year, count(*) AS n
ORDER BY year;

// Audit 3: Organization IsChildOf 的树深度和 top parents
MATCH (parent:Organization)<-[:IsChildOf]-(child:Organization)
RETURN parent.legalShortName, parent.country_code, count(child) AS children
ORDER BY children DESC LIMIT 30;
```

三条查询在 NeuG 上跑应该不到 10 秒。跑完我们立刻知道：
- Funder 有多少种 jurisdiction —— 决定 Case Ω 可行性
- Project 的时间分布 —— 决定 Case Π 的 window 选择
- 存在哪些学术王朝 —— 决定 Case Σ 的节点数量

---

**文档版本**：2026-04-24  
**与其他文档的关系**：
- `case_community.md` · 原始 5 个 case（本文档的 Case Π/Ω2 分别对应原 Case C/B 的补强）
- `case_community_findings.md` · 当前 Louvain 结果（国家级 co-author）
- `community_detection_cases.md` · 同口径的算法变体
- `hyperedge_algorithms_for_cases.md` · 算法家族上位抽象
- **本文档** · **基于 schema.json 实际字段的新投影目标**
