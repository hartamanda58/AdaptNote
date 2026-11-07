# HANDOVER — AdaptNote

> 交接文档。新会话先读这份，再按需读 `docs/reader-redesign/`。
> 旧版本（2026-10-27）的内容重写在文末「附录：历史交接」，仅在需要考古时看。

---

## 0. 当前状态（2026-11，MN/ReaderNote 合并后）

- 分支 `main`；合并前基线 `ea38004`，本次合并已提交 `b369160` → `19bc53e` → `79e6e23`
- 本地 **12 套测试全绿**（新增 `vocabtest`）；Windows CI 测试列表已同步到 12 套
- 应用：C++20 + Qt6 Widgets + MuPDF 1.21，约 1.8 万行，自用、不分发
- 目标产品：BookxNote Pro / 阅析笔记（ReaderNote）风格的「阅读 → 标注 → 摘录 → 脑图 → 主题整理」
- **本次合并说明见 `docs/merge-mn-readernote.md`**：以 AdaptNote 为底座，吸收 MN 的
  生词本（复习/查词/.apkg）/ OCR / 自由 AI 提问 / 本地目录 + S3 同步，
  UI 改为浅色 MarginNote/macOS 三栏，并补页面缩略图、摘录卡片、拖动落点提示

> ReaderNote 只参考公开界面与静态分析；资料包里的破解版全程未运行、未复用。

### 最近一次验收（2026-11-07，合并版）

```text
run 42353005512  Windows 构建（main @ 900e60a）  success
12/12 套测试在 Windows runner 上全部 PASSED（test-diagnostics 产物已核对，
含新增 vocabtest：SigV4 官方向量 + 假 S3 双库推拉）
```

便携包运行验收（inspect-win-artifact）本轮未触发；上一次全绿记录见 `ci-report` 的
`ci/windows-report.txt`。12 套测试的 `.out.txt` 在 run 的 test-diagnostics artifact 里。

### 最新可下载包

```text
https://github.com/hartamanda58/AdaptNote/releases/download/merged-mn-readernote-900e60a/AdaptNote-windows-x64-portable.zip
sha256: 988a68df5b2269f77b4c7772767579118056df3590c89e78be65e214208f85da
```

包含合并版全部功能（生词本/OCR/S3 同步/新 UI）。包内直接运行 `bin\adaptnote.exe` 即可；
`portable.flag` 让它把数据写到包根 `data\`；`AdaptNote.bat` 仍然兼容。
`ecdict.db`（可选查词）放包根 `data\`；tesseract 可装系统或用 `ADAPT_TESSERACT` 指定。

注意：这版 CI 产出的 zip 内层文件名曾被 `make_portable.ps1` 的 `$name` 变量覆盖成
`test.epub.zip`（已修复并提交）；发布包是解层重命名后的产物。

---

## 1. 环境与路径

| 项 | 值 |
| --- | --- |
| 工作副本 | `/root/adaptnote`（唯一改代码的地方） |
| 远程 | `https://github.com/hartamanda58/AdaptNote.git`（public） |
| 分支 | `main` |
| Token | `/root/.gh_token`（600），**用完应吊销** |
| CI 脚本 | `/root/bxn/ci.py`、`push_dispatch.py` |
| 参考压缩包 | `/root/Px72.tar`（270MB，**不在仓库内**） |

Push：

```bash
cd /root/adaptnote
export GH_TOKEN=$(< /root/.gh_token)
git -c credential.helper='!f() { echo username=x-access-token; echo password=$GH_TOKEN; }; f' push origin main
```

> 历史 clone `/root/pi work/AdaptNote-clone` 的 origin 曾经指错（指向本地目录，push 不进 GitHub）。
> 不要在那里改代码。

### 参考包内容（`/root/Px72.tar`）

- `03-mn-源码/mn/` —— **自研 MN**（Node + Web + Electron 壳），三栏深色 UI 的参考对象；
  有 `STATE.md`、`HANDOFF.txt`、182 项测试，但未打包成 Windows exe（better-sqlite3 ABI 问题）。
- `02-ReaderNote-源码分析.tar.gz` —— ReaderNote 的静态逆向产物（decompiled JS + 报告）。
- `01-ReaderNote便携版-已解锁.zip` —— **绕过许可的破解版**。
  **不要运行、不要利用、不要分发**；需要行为参考时只看 `02` 的静态分析与公开界面。

---

## 2. 构建与测试

```bash
cd /root/adaptnote
python3 scripts/syntax_check.py <changed files>   # 用 compile_commands.json 做 -fsyntax-only
python3 scripts/check_dialogs.py                  # 禁止 headless 下裸模态框
cmake --build build -j2
```

12 套测试（**每个测试必须独立数据目录**，共用会互相污染）：

```bash
cd build
run(){ n=$1; shift; QT_QPA_PLATFORM=offscreen ADAPT_HEADLESS=1 \
  ADAPT_DATA_DIR=$(mktemp -d) XDG_DATA_HOME=$(mktemp -d) ./$n "$@"; }
run selftest     test.pdf test.epub
run visualtest   test.pdf
run featuretest  test.pdf test.epub
run exporttest   test.pdf
run synctest     test.pdf ../scripts/dav_server.py
run aitest       ../scripts/fake_openai_server.py
run p5test       test.pdf
run librarytest
run migratetest  test.pdf
run importtest   test.pdf
run conflicttest
run vocabtest   ../scripts/fake_s3_server.py
```

截图（固定尺寸，便于比较）：

```bash
QT_QPA_PLATFORM=offscreen ADAPT_HEADLESS=1 ADAPT_DATA_DIR=$(mktemp -d) \
  ./adaptnote --doc test.pdf --demo --width 1360 --height 900 --screenshot out.png
```

### CI 流程

1. 触发构建：
   `POST /repos/hartamanda58/AdaptNote/actions/workflows/windows-portable.yml/dispatches {"ref":"main"}`
2. 构建成功后触发打包检查：
   `POST .../inspect-win-artifact.yml/dispatches {"ref":"main","inputs":{"run_id":"<build run id>"}}`
3. 读报告：
   ```bash
   git fetch -f origin ci-report:ci-report
   git show ci-report:ci/windows-report.txt
   ```
4. 产物：Actions artifact `AdaptNote-windows-x64`（约 42MB zip），再嵌套一层 zip；
   发布时用 Releases 上传，别放仓库。

---

## 3. 已完成（功能层面）

### 3.1 阅读与标注

- PDF / EPUB / XPS / CBZ / FB2 打开；连续/单页/双页、缩放、旋转、目录、页码跳转
- 高亮 / 下划线 / 波浪线 / 批注 / 图片摘录 / 手写（压感、倾斜）/ 局部橡皮
- **标注坐标存 PDF 页面坐标**，显示时乘 CTM；放大、旋转、双页都不错位
- 标注写回 PDF（增量保存）；读取 PDF 内已有标注
- 朗读（逐句高亮；有 TTS 引擎就发声）
- 查找（页内、可切到主题笔记范围）

### 3.2 PDF 清晰度（重要修复）

历史 bug：全库从未读 `devicePixelRatio`，`fz_scale(scale, scale)` 后在 150% 缩放下把 1× 位图拉大。
现在：`PageView::viewDpr()` / `imageFor()` 按 `scale * dpr` 渲染，缓存键含 DPR，
开启 `SmoothPixmapTransform`，单页 40 Mpx 上限。打包验收固定 `--zoom 100 --render-probe` 断言 DPR2=2×。

### 3.3 存储与同步

- **SQLite 是唯一真相源**；墨迹不回写 PDF（MuPDF 1.21 不为 Ink 生成外观流）
- FTS5 + trigram tokenizer，支持中文子串搜索
- 派生 JSON 镜像（每本书一个）
- WebDAV 同步：版本向量 + 墓碑；并发保留双方并打 `conflict` 标签
- 文档身份 = **内容 SHA-1**（早期用「文件名+大小」会串库）
- 主题同步：远端 `topics.json`（topics / topicDocs / mindLinks），wire 不含绝对路径

### 3.4 导出

MD / HTML / OPML / XMind / Anki(apkg)；可按脑图分组输出；可只导出筛选结果。
笔记标题、层级（含笔记套笔记）会带进 OPML/XMind/MD。

### 3.5 笔记本工作区

- 应用级笔记本标签（关闭标签不删数据）
- 新建空笔记本、添加多份资料、资料标签切换
- 跨资料点击笔记跳回原文，保留整个笔记本大纲
- 摘录编辑器：**我的标题 / 只读原文 / 个人批注** 分开存；保存可撤销
- 自由笔记（无原文来源）
- 大纲拖拽：上半区排序、下半区建父子；拒绝跨资料与成环
- 分组层级拖动、全量撤销（删除分组撤销会恢复组内笔记归属与子分组）
- 独立脑图窗口：连线、删除笔记、导出 PNG、分组右键菜单

### 3.6 阅读主题（跨文档）

- 表：`topics` / `topic_docs` / `mind_links`；`mind_groups.topicId`
- 主题 CRUD、加/移资料、主题脑图（每本书一个分支）、主题内笔记搜索
- 旧库自动迁移：确定性 topic id、事务、幂等（只覆盖「有笔记的文档」）

### 3.7 脑图交互（近期反复修的区域）

| 操作 | 行为 |
| --- | --- |
| 左键单击卡片 | 选中 + 定位原文（`markupActivated`） |
| 双击卡片 | 编辑（`renameRequested`） |
| 右键笔记 | 编辑 / 定位 / 删除 |
| 右键页分支 | 在此页新建自由笔记 |
| 右键分组 | 重命名 / 新建子分组 / 上移 / 删除 |
| 左键拖卡片 | 换分组 / 排序 / 改父级 |
| 中键拖、空白左拖 | 平移画布 |
| 连线模式 | 依次点两张卡 |

关键：卡片文字/装饰是**独立 scene item**，不是卡片子项。命中要沿 `data(4)`（nodeKey）回查，
再映射到 tree 节点。`keyAt` / `groupKeyAt` / `dropTargetAt` 都按这个规则。

### 3.8 UI（合并后）

UI 以 ReaderNote 为主、参考 macOS 版 MarginNote：**浅色三栏**，QSS 已整体重写。

```text
左：书架 / 笔记本 / 目录
中：原始纸白 PDF/EPUB（不染色）+ 文档标签
右：摘录 / 笔记 / 生词 / 脑图 / AI（五标签，记住上次选择）
```

- 主题 QSS 资源别名已修（`QT_RESOURCE_ALIAS app.qss`）
- 大纲行是自定义 delegate：标题 + 摘录/批注预览 + 页码
- 顶部工具栏保留高频动作；模式按钮带文字（阅读/整理/脑图）
- 选中文字后浮出 pill：高亮 / 下划线 / 批注 / 查词 / 翻译 / AI / OCR
- AI 从独立 dock 合并进右侧标签，不再挤掉笔记面板

### 3.10 合并新增（来自 MN）

- 生词本：本地 ECDICT 优先、AI 兜底、五盒间隔重复、发音、TSV/Anki 文本/`.apkg` 卡包、
  `words.json` 随同步走（墓碑传播、确定性 id、docKey 原文回链）
- 生词复习模式：空格显示答案、1 不认识、2 认识；答对进盒、答错回 0
- 划词浮出条：高亮 / 下划线 / 批注 / 查词 / 翻译 / AI / OCR
- OCR：选中区域 / 剪贴板 / 整页 → tesseract（语言包可设）；没有 tesseract 时走视觉模型
- 本地目录同步（U 盘/NAS）与 **S3 / MinIO（SigV4）**，三种方式共用合并逻辑
- 左栏「页面」缩略图（分批渲染、当前页高亮）、右栏摘录卡片化
- 笔记大纲拖动落点提示：上半区插入线 / 下半区“成为子笔记”块
- 左/右侧标签记住上次位置（`ui.shelfTab` / `ui.sideTab`）

### 3.9 划词翻译

选中文字 → 浮出「翻译」→ 目标语言（简中/英/日/韩/繁中）→ 流式显示、可复制。
复用 AI 设置的 OpenAI 兼容接口；未配 Key 时明确提示。
顺带修了 `buildUserPrompt` 里 `%s` 不生效的旧 bug。

---

## 4. 未完成 / 已知问题

1. **ReaderNote 白板 / Excalidraw 未合并**：手写层 + 脑图覆盖主路径，白板另算一个大工程
2. OCR 依赖外部 `tesseract`，没有随包分发；没有 tesseract 时必须配视觉模型
3. 生词的原文回链用 `docKey`（内容 SHA-1）解析；对端没打开过同一本书时无法定位
4. 大纲/脑图拖拽已有落点提示，但 **跨资料拖动** 仍只支持同一本书内
5. 主题 WebDAV **未做真实双机验证**（只有本地 Python DAV 测试）；本地目录/S3 也只在测试里验过
6. 旧库迁移只覆盖「有笔记的文档」；多主题旧库未覆盖
7. 大纲/脑图 **多选、批量操作、折叠按钮** 未做
8. 真实数位板压感/倾斜、真实 Anki/XMind 客户端、真实 WebDAV/S3 未人工验收
9. 双页对照、章节自动归类未做；缩略图最多渲染 500 页

---

## 5. 已验证 vs 未验证（诚实清单）

**已验证（自动化）**：12 套本地测试（新增 `vocabtest`）；Windows 构建与打包运行（导出、数据目录、截图、DPI）。

**未验证（需要真人/真机）**：

- 新的浅色 UI 在 125%/150%/200% 缩放下的观感（只有 100% 截图）
- 浮出 pill 在触控板/触屏上的手感
- 真实 tesseract 安装后的识别质量（本机无 tesseract；已用假 exe 验证进程管线与失败分支，视觉模型只验了请求体）
- 本地目录同步接真实 NAS/U 盘，S3 接真实 AWS/MinIO（测试用临时目录与假服务器）

- 高分屏下的人眼观感（125%/150%/200%、多显示器）
- 数位板真实压感与笔迹偏移
- 两台真实设备之间的 WebDAV 同步（含断网恢复、ETag 变化）
- Anki 桌面版导入 `.apkg`；XMind 桌面版打开 `.xmind`
- 打字输入时的真实手感（大纲重命名、编辑器）

> CI 全绿 ≠ 体验达标。之前多次把「自动化通过」当成「用户可用」，导致来回返工。

---

## 6. 关键代码坑（踩过的）

- `QTreeWidget` 内部拖动会绕过 SQLite 与撤销栈 → 必须自定义 `startDrag`/`dropEvent` + mime
- `QGraphicsView` 的鼠标事件必须发给 **viewport**，发给 view 无效
- 脑图卡片文字是独立 item，不能假设 `parentItem()` 是卡片
- `ensureSchema()` 里调用 Store 公开方法时 `open_` 还是 false
- `repairSearchIndex` 操作的表是 `markups`；`notes_fts` 没有 `docName` 列（由 `docPath` 派生）
- `setMindGroupTopic` 必须 `coalesce(topicId,'') <> ?`，否则 NULL 行匹配 0 行
- headless 下不能 `exec()` 模态框；菜单也要 `if (!kHeadless) menu.exec(...)`
- 编辑批注在 `kHeadless` 下直接返回，否则 CI 卡死
- `mainwindow.cpp` 必须 `#include "core/paths.h"`
- 页分组的 `mindOrder` 排序要用 `QMap`，Windows 上 `QHash` 迭代顺序不稳
- `mkdir` 生成的多层 zip 会嵌套；发布前要解一层

---

## 7. 建议的下一步

1. **白板/Excalidraw**：合并后仍缺的一块；S3 已补
2. 脑图多选/批量操作、折叠按钮
3. 真实设备验收清单（Windows 高分屏、数位板、双机 WebDAV/本地目录、Anki/XMind、tesseract）
4. 主题同步补冲突/墓碑的集成测试（远端并发改名、远端删资料）
5. 生词“回原文”跨设备定位（现在只传书名）；右侧面板在窄窗口下的最小宽度

---

## 附录：历史交接（2026-10-27，已过期）

以下内容记录当时的设计与坑，部分细节已被后续改动覆盖；仅在考古时参考。

- 当时定位：对标 BookxNote Pro，约 9.7k 行，P0–P5 分阶段推进
- 当时已有：阅读/标注/手写/朗读/AI、FTS5、五类导出、WebDAV 同步
- 当时未做：主题、笔记本工作区、深色三栏 UI、划词翻译

> 具体旧文见 git 历史：`git show ea38004^:HANDOVER.md`
