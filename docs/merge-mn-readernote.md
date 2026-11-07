# 合并记录：AdaptNote × MN × ReaderNote

> 目标：把 AdaptNote 与资料包里的 MN（`03-mn-源码/mn`，Node + Electron）合并成一个软件，
> 功能取并集，UI 以 ReaderNote 为主、参考 macOS 版 MarginNote。
> 资料包里的 `01-ReaderNote便携版-已解锁.zip` 是绕过许可的破解版，**全程未运行、未复用**；
> ReaderNote 只作为公开界面的静态参考（`02-ReaderNote-源码分析.tar.gz`）。

## 1. 为什么以 AdaptNote 为底座

| 维度 | AdaptNote（C++/Qt6/MuPDF） | MN（Node/Electron） |
|---|---|---|
| 渲染 | MuPDF：PDF/EPUB/XPS/CBZ/FB2 | pdf.js：只有 PDF |
| 标注 | 五类 + 手写压感 + 图片，坐标存页面空间 | 高亮/下划线，归一化矩形 |
| 存储 | SQLite 唯一真相源 + FTS5 中文子串 | SQLite + better-sqlite3 |
| 同步 | 版本向量 + 墓碑 + 冲突保留 | 快照 + updated_at 新者胜 |
| 交付 | Windows 便携 exe 已验证 | better-sqlite3 ABI 卡住，打不出 exe |

MN 的 P0 就是“打包成 exe”，所以把 MN 的功能搬进 AdaptNote，
而不是反过来在 Electron 里重写 MuPDF 渲染、写回 PDF 和冲突模型。

## 2. 复用了什么

**直接复用（未重写）**

- `src/core/*`：SQLite 存储、FTS5 搜索、镜像 JSON、文档身份（内容 SHA-1）
- `src/render/mupdf_engine.*`：PDF/EPUB 渲染、文本层、标注写回、DPR 渲染
- `src/ui/pageview.*`：选区、标注层、手写、缩放/旋转/双页
- `src/mind/*`：脑图树、布局、拖拽、连线、导出
- `src/export/*`：MD / HTML / OPML / XMind / Anki
- `src/sync/*`：版本向量合并、墓碑、冲突副本
- `src/ai/*`：OpenAI 兼容流式客户端与角色提示词

**从 MN 移植的逻辑**

| MN 位置 | 合并后 |
|---|---|
| `server/index.js` 生词 CRUD / review | `src/vocab/vocab.*` + `Store::words/upsertWord/reviewWord` |
| `server/index.js` TSV / Anki 导出 | `vocab::exportWords()` |
| `server/ai.js` `lookupWord()` | `vocab::lookupLocal()`（ECDICT）+ AI 兜底 |
| `server/ai.js` `ocrImage()` | `src/ocr/ocr.*`（tesseract，`ADAPT_TESSERACT` 可指定路径） |
| `server/ai.js` vision 兜底 | `AiClient::sendVision()`（非流式视觉请求） |
| `public/index.html` 右侧四标签 | 右侧 `摘录 / 笔记 / 生词 / 脑图 / AI` |
| `public/app.js` 选区工具条 | 阅读区浮出 pill：高亮/下划线/批注/查词/翻译/AI/OCR |
| `server/sync.js` 本地目录同步 | `WebDavClient::setLocalRoot()`，与 WebDAV 共用合并逻辑 |
| 五盒间隔重复 `[0,1,2,4,7,15]` 天 | `vocab::kReviewGapsDays`，答错回盒 0 |

**ReaderNote 参考的部分（只用公开界面与静态分析）**

- 三栏信息架构：左书架/大纲，中原文，右摘录/笔记/脑图
- 顶部文档标签、右侧卡片列表、浮出式选区工具条
- macOS 观感：暖灰 chrome、白色卡片、蓝色强调色、圆角与细分隔线

## 3. 功能并集

| 能力 | 合并前 AdaptNote | MN | 合并后 |
|---|---|---|---|
| PDF/EPUB/XPS/CBZ/FB2 | ✓ | 仅 PDF | ✓ |
| 高亮/下划线/波浪线/批注/图片摘录/手写 | ✓ | 部分 | ✓ |
| 标注写回 PDF、读回内嵌标注 | ✓ | ✗ | ✓ |
| 中文全文搜索（FTS5 trigram） | ✓ | LIKE | ✓ |
| 笔记本 / 阅读主题 / 多文档 | ✓ | 笔记本 | ✓ |
| 脑图树 / 拖拽 / 连线 / PNG / XMind | ✓ | SVG 树 | ✓ |
| 导出 MD/HTML/OPML/XMind/Anki | ✓ | 生词 TSV/Anki | ✓ |
| 同步 | WebDAV + 版本向量 | WebDAV / S3 / 本地目录 | WebDAV + **本地目录**（S3 未合并） |
| 生词本（五盒 SRS、发音、TSV/Anki） | ✗ | ✓ | **✓ 新增** |
| 查词（本地词典优先 + AI 兜底） | ✗ | ✓ | **✓ 新增** |
| OCR（tesseract + 视觉模型兜底） | ✗ | ✓ | **✓ 新增** |
| 自由提问式 AI（不限于选中文字） | ✗ | ✓ | **✓ 新增** |
| 浮出选区工具条 | 仅翻译 | 7 个动作 | **✓ 新增** |
| 生词随同步走 | — | ✓ 快照 | **✓ 新增**（`words.json`，墓碑传播） |
| 白板 / Excalidraw | ✗ | ✗ | 未合并（ReaderNote 有，成本高，见 §6） |

## 4. 界面

- 全部 QSS 重写为浅色 macOS 风格（`resources/app.qss`）。
- 左：书架 / 笔记本 / 目录；中：纸白原文 + 文档标签；右：摘录 / 笔记 / 生词 / 脑图 / AI。
- 顶栏：打开、撤销重做、翻页、缩放、标注工具、阅读/整理/脑图分段、导出、更多。
- 选中文字后浮出 pill，动作不再依赖顶部工具栏。
- AI 从独立 QDockWidget 合并进右侧标签，避免“打开 AI 就把笔记挤没”的旧问题。
- 右侧标签会记住上次选择（`ui.sideTab`，用 objectName 而不是会变的标题）。
- 脑图首次进入自动适配视口，之后保留用户平移/缩放。

## 5. 生词本怎么用

1. 选中一个词 → 浮出条「查词」，或在右侧「生词」标签直接输入。
2. 查找顺序：`<数据目录>/ecdict.db` → AI（OpenAI 兼容）。
   - ECDICT 是可选文件，放到数据目录即可（表 `stardict(word, phonetic, translation, definition)`）。
   - 没有词典也没配 Key 时，词仍会进生词本，释义留待补全，不丢词。
3. 「开始复习」进入专注复习：空格显示答案，`1` 不认识、`2` 认识，Esc 关闭；
   答对进下一盒，答错回第 0 盒。
4. 「发音」走 Qt TextToSpeech（没有引擎时明确提示）。
5. 「导出」支持 TSV、Anki 导入文本，以及 **Anki 卡包（.apkg）**（正面单词/音标，
   背面释义/例句/出处，可直接双击导入 Anki）。
6. 没有词典例句时，会自动截取原文中包含该词的句子作为例句。
7. 同步：`words.json` 与书库快照一起上传；删除是墓碑，另一台设备同步后也会消失。
   新词 id 由“小写拼写”的 SHA-1 派生，两端同时添加同一个词会收敛到同一张卡；
   同一拼写但 id 不同的旧数据在合并时归并到本地 id，内容按 `updated` 新者胜。
   原文回链只传 `docKey`（内容 SHA-1），对端按本地同名内容解析成自己的路径，
   因此换机器后「回原文」仍能跳到同一页。

## 6. OCR 怎么用

- 选中一块区域 → 浮出条「OCR」；没有选区时使用剪贴板图片，或选择图片文件。
- 「更多 → 整页 OCR」识别当前页；「OCR 设置…」可改语言包（如 `eng+chi_sim`）。
- 优先本机 `tesseract`（PATH、`C:/Program Files/Tesseract-OCR/`，或 `ADAPT_TESSERACT`）。
- 没装 tesseract 且配置了 AI Key → 自动走视觉模型（`AiClient::sendVision`）。
- 识别结果可复制，或「插入为摘录」成为一条笔记。

## 6.1 第二轮 UI 改进

- 左栏新增「页面」缩略图（分批渲染，500 页上限），当前页高亮，点击跳页。
- 右侧「摘录」从纯文本行改为卡片：颜色条 + 类型/页码 + 原文 + 批注/标签。
- 笔记大纲拖动时显示落点：上半区是蓝色插入线，下半区是“成为子笔记”的蓝色块。
- 左栏标签、右栏标签都会记住上次选择。
- 生词面板的主按钮恢复蓝色实心样式（修掉父级 QSS 选择器压过 `#primaryButton` 的问题）。

## 7. 明确没合并的

| 项 | 原因 |
|---|---|
| S3 / MinIO 同步 | 需要 SigV4 签名 + 真机联调；WebDAV 与本地目录已覆盖自托管场景 |
| ReaderNote 白板 / Excalidraw | 体量大，且 AdaptNote 的手写层 + 脑图已覆盖“画布整理”主路径 |
| ReaderNote 许可绕过 / 遥测屏蔽 | 不采用、不分发；只参考公开界面 |
| Electron 打包 | MN 的 `better-sqlite3` ABI 问题不再是问题：底座改为原生 Qt |

## 8. 测试

新增 `vocabtest`，覆盖：

- 五盒算法边界、大小写唯一、删除墓碑与复活、编辑不覆盖已有释义/出处
- ECDICT 查询（构造临时库）、AI 答案解析、TSV/Anki 导出（BOM、表头）
- 本地目录传输（put/get/If-Match/list/404）与两个库之间的完整同步（含生词墓碑）
- OCR 空图与 PNG 编码

合并后共 **12 套**自动化测试，全部通过。UI 相关的人工验收项仍见 `HANDOVER.md` 第 5 节。
