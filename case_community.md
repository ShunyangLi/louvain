# Case Community · 用社区发现讲地缘/事件驱动的科研故事

**核心 pivot**：之前俄乌 case 用 subgraph matching 回答的是**计数问题**（论文/funding 掉了多少）。Community detection 的杀手级用途是回答下一层的**归属问题** —— "剩下的科研跑去哪个阵营了？谁接盘了空缺？出现了新的'科研小集团'吗？"

这是 subgraph matching 做不到的事：它需要**自下而上涌现出阵营**，而不是预设标签。

---

## 叙事框架（所有 case 复用）

```
事件时间点 (t0)
   │
   ├── t0 之前：目标 org/country 所在 Louvain 社区的邻居构成
   │
   ├── t0 之后：社区迁移轨迹
   │     ├── A 搬进另一个已有阵营？(e.g. 西→东)
   │     ├── B 形成孤立新社区？("XX 孤岛")
   │     └── C 表面消失但通过中转国保留连接？("制裁逃逸路径")
   │
   └── 结构性结论：事件不是"消灭"了科研，而是"重分配"了科研阵营
```

**三类可能的 finding**（每个 case 都适用这三分类，只是具体内容不同）：
1. **阵营迁移** —— 目标从阵营 X 搬到阵营 Y
2. **孤岛形成** —— 目标脱离所有阵营，自成孤立社区
3. **中转逃逸** —— 表面脱钩，实际通过第三国桥节点维持

---

## Case A · 俄罗斯的学术再阵营化（俄乌 case 的直接续集）

**问题**：2022 年俄乌战争后，俄罗斯 org 的 Louvain 社区邻居构成发生了什么？

**切入点**：延续你们已有的俄乌数据分析 —— 在"论文数下降 30%"之后接一句"**那剩下的 70% 在和谁合作？**"

**做法**：
- 边：`publication_hasAuthorInstitution_organization` + `publication_Cites_publication` 双重视角
- 按 year window (2018-2021 vs 2022-2024) 切片跑 Louvain
- 追踪俄罗斯头部 org（莫斯科大学 / 俄科院 / Skoltech 等）的社区 ID 迁移
- 计算社区邻居国家分布的"重心移动"

**预期三种 finding**：
| 类型 | 表现 | 故事张力 |
|---|---|---|
| 阵营迁移 | 从 "DE-UK-US 主导社区" 迁到 "CN-IN-IR 主导社区" | ★★★★ 直接印证"东转" |
| 孤岛形成 | 俄罗斯 org 自己聚成一个孤立社区 | ★★★★★ 最戏剧：科研孤立 |
| 中转逃逸 | 通过 TR/AE/KZ 这些"中立国" org 保留西方连接 | ★★★★★ 最反直觉：制裁失效 |

**Headline 候选**：*"Sanctions Did Not Isolate Russian Science — They Rerouted It"*

---

## Case B · 中美学术脱钩的社区裂变

**问题**：2018 贸易战 / 2020 实体清单 / 2022 CHIPS Act 前后，AI / 半导体 / 量子 领域的 Louvain 社区是否从"一个全球社区"裂变成"两个阵营"？

**做法**：
- 子图限定：`FOS CONTAINS 'computer science'` OR `'physics'` OR `'materials'`
- 时间切片：2015 / 2018 / 2020 / 2022 / 2024
- 度量"跨社区合作/引用比率"随时间的单调变化
- **关键产出：消失的桥节点名单** —— 曾经是中美纽带、后来连接断掉的机构（华为、中兴、MIT 某些 lab、哈佛陈曾熙公共卫生学院等）

**独特卖点**：故事有明确的"敌对阵营形成"节奏感，且每个时间点对应一条具体政策 —— 结构变化与政策事件能对齐成时间轴。

**风险**：需要先验证子图在 2015-2024 的中美 publication 覆盖是否足够。MVP 前先跑一次覆盖率检查。

**Headline 候选**：*"The Great Decoupling: Structural Evidence of a Bifurcating Global Science"*

---

## Case C · Brexit 的学术离婚

**问题**：英国 org 在 Louvain 社区中真的从 EU 主导社区"搬出去了"吗？Horizon Europe 暂停期间（2020-2024）损失有多大？

**做法**：
- 时间切片更细：2014 / 2016(脱欧公投) / 2020(正式脱欧) / 2022 / 2024
- 追踪英国头部 org（牛津 / 剑桥 / UCL / ICL）的社区邻居变化
- 量化 UK-EU 合作强度的时间曲线；对比 UK-US / UK-Commonwealth 是否补位
- 叠加 funding 维度：`funding_for_project_edges` 里英国 org 拿欧盟基金的占比变化

**独特卖点**：英国 case 是**长期缓变过程**，不像俄乌急性冲击 —— 适合展示"time-series Louvain"的**动态分析能力**。一系列小动作累积成结构位移，是一种更细腻的故事类型。

**Headline 候选**：*"A Slow Divorce: Tracking Eight Years of British Science Drifting from Europe"*

---

## Case D · 土耳其 2016 学术清洗的结构后果

**问题**：2016 年政变未遂后土耳其解雇了 4000+ 学者，他们去哪了？土耳其本土科研社区结构怎么变化的？

**做法**：
- 个人层面：定位"2016 后发表在 TR org 显著减少的 Author" → 查其后续 affiliation 变到哪些国家 → 追踪这些 author 在 Louvain 中的社区迁移
- 社区层面：TR 整体在 Louvain 中的社区邻居是否从 EU 迁移到 MENA
- 追踪流亡学者的**目的地国家分布** —— DE / US / UK / NL 哪个接收最多？

**独特卖点**：**个人层面的离散 + 社区层面的重组** 双重叙事。比国家级制裁更有人物感 —— 具体的学者被具体地清洗、具体地流亡。可以选 5-10 个典型流亡学者做 case-within-case。

**风险**：author 级别的追踪需要 `publication_author_edges` 的时间属性（publication.year）足够准确；需要先验证 author 跨机构迁移在数据中是否可追踪。

**Headline 候选**：*"Where Did 4,000 Purged Academics Go? Tracing an Academic Diaspora Through Community Structure"*

---

## Case E · 伊朗核协议崩溃（2018 JCPOA 退出）的科研孤立

**问题**：2018 年特朗普退出伊核协议 + 二级制裁后，伊朗 org 的 Louvain 邻居是否从"欧洲"切换到"中俄"？

**做法**：与 Case A 同构，只是换主角和时间点。
- 时间切片：2015(JCPOA 签署) / 2018(退出) / 2020 / 2022 / 2024
- 伊朗头部 org（Sharif / Tehran / IPM）的社区迁移
- 对比"JCPOA 期间"vs"退出后"的西方合作强度

**独特卖点**：**作为 Case A 的对照组** —— 同样是"被制裁国家的学术 isolation 路径"，但：
- 伊朗：缓慢 / 长期 / 持续多轮制裁
- 俄罗斯：急性 / 战时 / 一次性全面制裁

两者打包能拼出一张**"制裁学术后果谱系图"**：从缓性慢性孤立到急性休克的连续谱。

**Headline 候选**：*"Two Sanctioned Nations, Two Isolation Trajectories: Iran and Russia in the Global Science Network"*

---

## 推荐组合与下一步

### 组合 1 · 延续俄乌资产（推荐优先）
**Case A + Case E** 打包成一篇 paper。
- 论点：*制裁不是消灭科研，而是重新分配科研阵营*
- 你们已有的俄乌数据分析可以直接当第一章
- Case E 作为对照组增强 claim 的一般性
- 预计工作量：中等（Case A 可复用大量已有代码）

### 组合 2 · 追求最大新闻价值
**Case B（中美脱钩）** 单独成篇。
- 最具政策影响力和传播潜力
- 风险：需要更大的图 / 更广的年份覆盖 / 更细致的子领域切分
- 预计工作量：大（需要先做覆盖率验证）

### 组合 3 · 方法论突破
**Case C（Brexit） + Case A（俄乌）** 打包。
- 论点：*两种截然不同的结构脱钩范式 —— 急性休克 vs 慢性漂移*
- 方法论卖点：time-series Louvain 的两种应用模式
- 预计工作量：大（Brexit 需要 10 年时间跨度的细粒度切片）

---

## 待决策的前置问题

1. **时间分辨率**：Louvain 按 year 切片还是按 2-year / 3-year window？（window 更稳但牺牲时间精度）
2. **社区稳定性**：Louvain 多次运行结果有抖动，需要 consensus clustering 或固定随机种子？
3. **图范围**：全图跑 Louvain 还是只在相关子领域（如 Case B 限定 CS+physics）？
4. **目标 org 的选取**：俄罗斯头部 org 如何定义？按论文数 Top-K 还是按 OpenAIRE 覆盖完整度？
5. **数据覆盖验证**：2018-2024 年各目标国家的 publication / org / funding 覆盖率是否支撑结论？MVP 前需要先跑一份覆盖率 audit。

**下次讨论优先级**：选定 1-2 个 case 深入，按 case1.md 的格式写完整执行 plan（包括 Cypher 查询、采样策略、判读阈值、预期 finding、paper narrative 骨架）。
