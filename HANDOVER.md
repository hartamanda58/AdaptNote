# HANDOVER — AdaptNote

> 交接文档。新会话先读这份，再按需读 `docs/reader-redesign/`。
> 旧版本（2026-10-27）的内容重写在文末「附录：历史交接」，仅在需要考古时看。

---

## 0. 当前状态（2026-11，MN/ReaderNote 合并后）

- 分支 `main`；合并前基线 `ea38004`，已提交 `b369160` → `19bc53e` → `79e6e23` → `91cdec8` → `58efc35` → `01c238b` → `818dcfb` → `7ccc5d1` → `edb6c40` → `1ca897c` → `1bfc57c` → `dfa3933` → `cd43fb9` → `d6a3867` → `40403e3` → `08b8e87` → `828fc77`
- 本地 **12 套测试全绿**（新增 `vocabtest`）；Windows CI 测试列表已同步到 12 套
- 应用：C++20 + Qt6 Widgets + MuPDF 1.21，约 1.8 万行，自用、不分发
- 目标产品：BookxNote Pro / 阅析笔记（ReaderNote）风格的「阅读 → 标注 → 摘录 → 脑图 → 主题整理」
- **本次合并说明见 `docs/merge-mn-readernote.md`**：以 AdaptNote 为底座，吸收 MN 的
  生词本（复习/查词/.apkg）/ OCR / 自由 AI 提问 / 本地目录 + S3 同步，
  UI 改为浅色 MarginNote/macOS 三栏，并补页面缩略图、摘录卡片、拖动落点提示

> ReaderNote 只参考公开界面与静态分析；资料包里的破解版全程未运行、未复用。

### 最近一次验收（2026-11-10，GUI 子系统版）

```text
run 42804067519  Windows 构建（main @ 828fc77）  success
12/12 套测试在 Windows runner 上全部 PASSED
打包检查：adaptnote.exe subsystem = 2 (Windows GUI)，不会弹终端
包内已移除 AdaptNote.bat，保留 portable.flag
自带 tesseract + ECDICT，zip 76.0 MB
```

之前全绿：42760150369（08b8e87）、42669136585（40403e3）、42652722172（d6a3867）、
42644688948（cd43fb9）、42578862213（dfa3933）、42457738576（1bfc57c）、
42447994594（1ca897c）、42444699623（edb6c40）、42433464805（7ccc5d1）、
42393475215（818dcfb）、42387890013（01c238b）、42373627839（58efc35）、
42353005512（900e60a）。
inspect-win-artifact 运行验收本轮未触发；旧记录在 `ci-report` 的 `ci/windows-report.txt`。

### 最新可下载包

```text
https://github.com/hartamanda58/AdaptNote/releases/download/gui-828fc77/AdaptNote-windows-x64-gui.zip
sha256: a621f427c6dfd8e4484bca5a134912259c3ab0ce06af98ccb4ebeae66e63ccf2
```

**GUI 子系统版**（压缩 76 MB / 解压 ~150 MB）：双击 `bin\adaptnote.exe` 直接进界面，
不再弹黑色终端；OCR 也不再闪控制台；`AdaptNote.bat` 已移除。翻译仍含
「DeepL 免费（内置）」以及 Google/Bing/DeepL 官方/DeepLX/AI。

- 整理模式截图：`shots/merged-ui-main.png`
- 阅读模式截图：`shots/merged-ui-read.png`
- 划词翻译截图：`shots/merged-translate.png`
- 38 秒整体演示：`releases/download/gui-828fc77/AdaptNote-demo.mp4`
  （sha256 48f7ae6f…）
- 10 秒卡片拖拽：`releases/download/gui-828fc77/AdaptNote-map-drag.mp4`
  （sha256 5405b3c2…）
- 6 秒分组拖拽：`releases/download/gui-828fc77/AdaptNote-group-drag.mp4`
  （sha256 89419ada…）
- 体验包：`releases/download/gui-828fc77/AdaptNote-demo-pack-828fc77.zip`
  （sha256 c4620f55…）
包内直接运行 `bin\adaptnote.exe`；`portable.flag` 让它把数据写到包根 `data\`。

- `ocr\tesseract.exe`（UB-Mannheim 5.4）+ `ocr\tessdata\{eng,chi_sim}`（tessdata_fast）；
  `ocr.cpp` 优先找包内引擎并自动加 `--tessdata-dir`（`make_portable.ps1 -WithExtras` 打包）
- `data\ecdict.db`：`scripts/make_ecdict.py` 从 ecdict-sqlite-28 裁出 43,823 个常用词条
  （词频 / Collins / Oxford 评级，8 MB）；替换同名文件可换全量词典
- `scripts/strip_coff_debug.py` 剥掉 libtesseract-5.dll 里 97 MB COFF 调试符号（108→76 MB），
  打包时对自带引擎跑 `--list-langs` 冒烟；任一下载/校验失败则警告并退回视觉模型
- 换全量词典：下载 ecdict-sqlite-28.zip，解压出的 stardict.db 改名为 ecdict.db 放入 `data\`

上一个版本（08b8e87，内置免费 DeepL）：
`releases/download/deepl-free-08b8e87/AdaptNote-windows-x64-deeplfree.zip`
更早：40403e3（界面刷新）、d6a3867（DeepLX）、cd43fb9（清晰渲染 + 翻译源）。
更早：7ccc5d1（折叠/排序/参考记忆）、818dcfb（多选/拖拽/双文档）、
01c238b（就地编辑）、58efc35（自带 OCR/词典）。
更早的不带 OCR/词典版（43 MB）：
`releases/download/merged-mn-readernote-900e60a/AdaptNote-windows-x64-portable.zip`

注意：早期 CI 产出的 zip 内层文件名曾被 `make_portable.ps1` 的 `$name` 变量覆盖成
`test.epub.zip`（已修复）；发布包是解层重命名后的产物。

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
- **右侧摘录就地编辑**：双击卡片在面板底部直接写标题/批注/标签，自动保存，单步撤销；
  新建标注后光标直接落在编辑器里（不再弹 `captureEditor`）
- **脑图卡片双击就地改名**（Enter 提交 / Esc 取消），F2 同样；分组也适用
- 笔记大纲拖动落点提示：上半区插入线 / 下半区“成为子笔记”块
- 阅读区 `Ctrl+滚轮` 光标锚定缩放、触控板捏合、中键平移
- 视觉刷新：暖纸色外壳、统一线性图标、圆角脑图卡片（色条/阴影/渐变根节点）、
  白色摘录卡片、圆角页面与柔和投影
- Windows 包改为 GUI 子系统：双击 `bin\adaptnote.exe` 不再弹黑色终端；
  OCR 调用 tesseract 时用 CREATE_NO_WINDOW，不再闪控制台；`AdaptNote.bat` 已移除
- PDF 页至少按 144dpi 渲染（高分屏再乘 DPR），解决 100% 下的矢量字发虚
- 划词翻译支持 **DeepL 免费（内置，无需终端）** / Google / Bing / DeepL / DeepLX / AI；
  “自动”Bing→Google，中文原文自动改译英文
- DeepL 免费走匿名 oneshot 接口（iOS 客户端），>1500 字符按句切分；可能限流
- DeepLX 可填自建或公开实例 URL + 可选 Token；兼容 `{code,data}`/`{translations}`/纯文本响应
- 回到有笔记的书默认「整理」布局（文档+脑图），新模式会记住
- 右侧 dock 的「脑图」标签接通数据（之前一直是空白）
- **脑图多选与批量**：Ctrl 点选 / Shift 框选 / Ctrl+A / Esc；批量删除、移组、改色；
  拖动多选中的一张即整组移动；批量操作合成单步撤销
- **原文拖进脑图**：按住已有选区拖到分组卡片，生成摘录并归组；参考窗格同样可拖
- **双文档并排**：文档栏「参考 ▾」在右侧打开另一份资料；参考窗格可直接标注
  （高亮/下划线/批注，写入该参考文档），也能「摘录选中」或拖到脑图；
  跨文档摘录要求处于笔记本/主题，这样会合并到同一张主题脑图；文档与宽度会记住并恢复
- 脑图容器卡片右下角 `−`/`+` 直接折叠分支（带子节点数），⋯ 菜单可「全部折叠/展开全部」；
  选中多张拖到“当前所在分组”会按相对顺序整体排序
- 脑图卡片拖拽在「整理」主脑图、右侧 dock「脑图」标签、独立脑图窗口三处都接通：
  拖到分组卡片 = 归组；拖到组内卡片上方 = 排序；拖多选 = 整组移动；
  拖到页/资料节点 = 移动笔记的出处（主题脑图内可跨书）；原文选区也能直接拖进来
- **分组卡片也支持拖动排序**：拖到同级分组前 / 空白处移到末尾（写 `mind_groups.order`，
  单步撤销）；跨资料拖动会落回自己文档末尾。分组双击/F2 改名走对话框，
  右键可新建子分组、删除；组卡暂不支持拖成子分组（用右键新建子分组）
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
7. 分组只能拖到**同级**位置，不能拖成另一分组的子分组；暂未做“全部折叠到某一层”
8. 参考窗格可标注但右栏摘录列表仍只列主文档；Google 在部分网络需代理，
   Bing 的 Edge 接口与 DeepL 也可能被网络拦截，Auto 会 Bing→Google 回退
9. 内置 DeepL 免费走第三方逆向的匿名 oneshot，可能被限流/失效（429 会提示）；
   要长期稳定用官方 API 或自建 DeepLX；真实数位板/Anki/XMind/WebDAV 仍未人工验收
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

1. **暗色主题**（外壳；当前只有阅读区“夜间纸”）
2. **手写跟手度**（压感/倾斜已有数据，但笔迹延迟与打磨未做）
3. **白板 / Excalidraw**（大工程，ReaderNote 有、我们没合并）
4. 真实设备验收清单（Windows 高分屏、数位板、双机 WebDAV/本地目录+S3、Anki/XMind、tesseract）
5. 主题同步补冲突/墓碑的集成测试（远端并发改名、远端删资料）
6. 右侧摘录列表增加“参考文档”筛选（现在只列主文档）

---

## 附录：历史交接（2026-10-27，已过期）

以下内容记录当时的设计与坑，部分细节已被后续改动覆盖；仅在考古时参考。

- 当时定位：对标 BookxNote Pro，约 9.7k 行，P0–P5 分阶段推进
- 当时已有：阅读/标注/手写/朗读/AI、FTS5、五类导出、WebDAV 同步
- 当时未做：主题、笔记本工作区、深色三栏 UI、划词翻译

> 具体旧文见 git 历史：`git show ea38004^:HANDOVER.md`
