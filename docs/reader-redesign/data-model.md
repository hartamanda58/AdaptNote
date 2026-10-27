# 数据模型：参考实现与本项目改造方案

来源：`resources/database/migrations/*.js`（24 个 Sequelize 迁移，可读 JS）
—— 这是参考实现**真实存在的库结构**，不是官网宣传。

## 1. 参考实现的 21 张表

```
Users  Profiles  Workspaces  Folders
Notebooks        ← 阅读主题
Resources        ← 资料库（文件 + 书目元数据）
References       ← 主题 ↔ 资料
Notes            ← 脑图节点（同时也是标注本体）
NoteComments
NoteRelations    ← 自由关联线（非树关系）
Notifications  Stats  Storages  Syncs  Versions
Manifests  Documents  ResourceFolders  ResourceStates
NotebookUserstates  ReferenceUserstates
```

## 2. 核心：`Notes` 一张表同时承担"标注"和"脑图节点"

| 字段 | 含义 |
|---|---|
| `parent_guid` | 树父节点 |
| `notebook_guid` | 所属阅读主题 |
| `resource_hash` | 关联资料（**用 md5 哈希，不是路径**） |
| `section` / `section_count` | 章节序号 / 跨章节数 |
| `position` | 章节内位置 |
| `path` / `toc` | 位置地址 / 大纲目录项 |
| `type` | `note` 文本圈选 / `bookmark` / `path` 笔迹 / `picture` / `snapshot` |
| `mark` | `underline` / `wavy` / `background` / `dotted` |
| `abstract` | 原文文本或 OCR 结果 |
| `raw` | 原始数据（base64 图 / 笔迹路径） |
| `html` / `content` | 标题富文本 / 笔记内容 |
| `track` | `main` 主树 / `subtree` 自由树 / `generalize` 概要节点；空且无 parent 则不渲染 |
| `minder_meta` | 脑图渲染偏移 |
| `s_guid` | 复制源（跨主题复用时复制节点） |
| `sequence` / `lock` / `tags` | 排序 / 锁定 / 标签 |

**要点**

1. 节点和标注是同一个实体 —— 不做"标注实体 + 节点引用实体"两层。
2. 资料用 **hash** 关联，不是绝对路径 → 换机器、改文件名不影响。
3. `track` 允许同一棵树里同时存在主树节点和自由节点。
4. 跨主题复用靠 **复制节点**（`s_guid` 记录来源），不是多对多引用。

## 3. `NoteRelations`：树之外的关联线

`start_guid` / `end_guid` / `points`（贝塞尔控制点）/ `arrow`（start/end/both）/ `title`。
**树结构与自由连线分离**，避免关联线被误当成父子关系（也避免成环）。

## 4. `Resources`：资料库

`hash` + `smd5`（取样哈希）+ `converted_hash`；书目元数据齐全（`title`、`authors`、`doi`、`isbn`、`publisher`、`keywords`、`abstract`、`source=user/zotero/calibre`）。
`ResourceStates` 分开存放本地/远端状态。

## 5. 与本项目现状的差距

| 能力 | 参考实现 | AdaptNote 现状 |
|---|---|---|
| 组织单元 | Notebook（跨资料主题） | 无；`mind_groups` 按 `docPath` |
| 笔记 ↔ 资料 | `resource_hash` + `References` | `markups.docPath`（绝对路径） |
| 树节点 | `Notes.parent_guid` + `track` | `mind_groups.parentId` |
| 自由连线 | `NoteRelations` | 无 |
| 资料元数据 | 完整书目字段 | 无 |
| 文件身份 | md5 | 路径 + size/mtime |

## 6. AdaptNote 改造方案（最小侵入）

保留现有 `markups`（标注）与 `mind_groups`（树节点）分离的既有结构，新增主题层：

```sql
-- 1) 阅读主题
create table topics (
  id text primary key,
  name text not null,
  description text,
  updated integer, deleted integer default 0,
  vv text, deviceId text
);

-- 2) 主题覆盖的资料（等价于 References）
create table topic_docs (
  topicId text not null,
  docPath text not null,
  [order] integer default 0,
  updated integer, deleted integer default 0,
  vv text, deviceId text,
  primary key (topicId, docPath)
);

-- 3) mind_groups 增加 topicId（保留 docPath 以兼容旧数据）
alter table mind_groups add column topicId text default '';

-- 4) 自由连线
create table mind_links (
  id text primary key,
  topicId text not null,
  fromGroup text, toGroup text,
  points text,            -- JSON: 贝塞尔控制点
  arrow text,             -- start/end/both
  title text,
  updated integer, deleted integer default 0,
  vv text, deviceId text
);
```

### 兼容与迁移

1. 旧数据：每个有笔记的文档自动生成一个主题（`name = 文档名`），其 `mind_groups.docPath` 保持不变，`topicId` 填新主题 id。
2. `mind_groups.docPath` 继续作为"从哪个文档来"的提示，但**访问权限改由 topicId 决定**。
3. 全部迁移在事务内，失败回滚；迁移前做一致性备份。
4. 同步：新表同样带 `vv`/`deviceId`/`deleted`，沿用现有版本向量机制。

### 有意不照搬的地方

- 不把标注和节点合成一张表：我们已有大量标注数据与同步逻辑，合并的迁移风险远大于收益。
- 不引入 md5 作为主身份：先用 `docPath` 为主、`size+mtime` 做修订判断，后续再加内容哈希做跨机迁移。

## 7. 待办

- [ ] 读取 `create-manifest.js` / `create-storage.js` / `create-sync.js` 的同步协议字段
- [ ] 定位脑图渲染库（`assets/board-renders.js`）确认布局算法
- [ ] 运行观察：跨主题添加同一资料时的实际行为
