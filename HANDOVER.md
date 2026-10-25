# HANDOVER — AdaptNote（BookxNote 同类自用实现）

> 这份文档是"交接"用的：它记录**当前状态、为什么这么设计、踩过哪些坑、还没做什么**。
> 阅读顺序建议：①快速开始 → ②现状 → ③架构 → ④坑 → ⑤后续。

---

## 1. 一句话定位

对标 **BookxNote Pro**（已用 REA 反编译过其发布件）的**本地优先**阅读与标注工具：
PDF/EPUB 阅读 → 标注/手写 → 笔记/标签/搜索 → 导出（MD/HTML/OPML/XMind/Anki）→ WebDAV 同步 →
脑图/朗读/AI 助手。C++20 + Qt6 Widgets + MuPDF，约 **9.7k 行**，自用、不分发。

## 2. 快速开始

```bash
# 依赖（Debian 12 实测）
apt install cmake ninja-build qt6-base-dev qt6-sql-sqlite qt6-multimedia-dev \
            qt6-declarative-dev libmupdf-dev libmujs-dev libfreetype-dev \
            libharfbuzz-dev libgumbo-dev libopenjp2-7-dev libjbig2dec0-dev \
            libjpeg62-turbo-dev zlib1g-dev fonts-noto-cjk xvfb

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
bash scripts/make_testdata.sh build      # 生成 build/test.pdf + test.epub
./build/adaptnote --doc build/test.pdf   # 运行
bash scripts/demo.sh                     # 构建+造档+7 套测试+截图（一条命令）
```

单跑测试（都需要 `QT_QPA_PLATFORM=offscreen`，GUI 类用 offscreen 即可）：

```bash
cd build
QT_QPA_PLATFORM=offscreen XDG_DATA_HOME=$(mktemp -d) ./selftest test.pdf test.epub
QT_QPA_PLATFORM=offscreen XDG_DATA_HOME=$(mktemp -d) ./visualtest test.pdf
QT_QPA_PLATFORM=offscreen XDG_DATA_HOME=$(mktemp -d) ./featuretest test.pdf
QT_QPA_PLATFORM=offscreen XDG_DATA_HOME=$(mktemp -d) ./exporttest test.pdf
XDG_DATA_HOME=$(mktemp -d) ./synctest test.pdf ../scripts/dav_server.py
XDG_DATA_HOME=$(mktemp -d) ./aitest ../scripts/fake_openai_server.py
QT_QPA_PLATFORM=offscreen XDG_DATA_HOME=$(mktemp -d) ./p5test test.pdf
```

> 每个用例单独 `XDG_DATA_HOME`：早期共用目录导致用例互相污染（前一个用例的笔记串进后一个），
> 是一个真实踩过的坑，见 §7.4。

## 3. 现状

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 工程骨架、主题、引擎抽象、测试脚手架 | ✅ |
| P1 | PDF/EPUB 打开、单页/连续/双页、缩放/旋转、文本选择、高亮/下划线/波浪线/批注、写回 PDF、大纲/笔记面板 | ✅ |
| P2 | FTS5 全文搜索、标签、颜色筛选、图片摘录、撤销/重做、阅读位置记忆、旧库自动迁移 | ✅ |
| P3 | 导出 MD / HTML / OPML / XMind(8) / Anki `.apkg`（自研 ZIP 读写） | ✅ |
| P4 | WebDAV 双向同步 + 版本向量合并 + 墓碑 + 冲突保留 + If-Match 条件写 | ✅ |
| P5 | 手写笔迹（RDP+贝塞尔）、脑图（QGraphicsScene）、逐句朗读、AI 面板（OpenAI 兼容流式） | ✅ |
| 完善轮 | 墨迹速度调宽 + 橡皮、图片摘录同步、脑图点击跳转/导出 PNG、AI 角色编辑器、朗读语速菜单、只导出筛选结果 | ✅ |
| 完善轮 2 | **手写局部擦除**（按轨迹切断笔迹）、**整个书库合并导出**（按书分组，五种格式） | ✅ |
| 完善轮 3 | **书库视图**（文档表 + 跨库笔记搜索/标签筛选 + 双击打开/跳转） | ✅ |
| 完善轮 4 | **脑图可编辑**：笔记带 `mindGroup` / `mindOrder`，拖动改分组（可撤销），未分组的仍按页自动分组 | ✅ |
| 完善轮 5 | **书库文档管理**：改名、多选、从书库移除（笔记转墓碑以便同步传播、不动文件）、选中文档批量导出五种格式 | ✅ |
| 完善轮 6 | **笔迹颜色独立**（含预览与持久化）、**橡皮切点插值**（缺口 ≈ 2×半径） | ✅ |
| 完善轮 25 | **Windows 构建通道**：Actions workflow（MuPDF + 本项目 + 11 测试 + 打包上传）、`windows-vm.sh`（QEMU/KVM 小 VM）、`make_test_epub.py`（无 bash 也能造 fixture）、2c4g 实测内存预算 | ✅ 脚本就绪，实际产物需在 Windows 上跑出 |
| 完善轮 24 | **Windows 支持**：`M_PI`→`kPi`、MuPDF 路径改 UTF-8、测试解释器按平台切换、CMake 加 `WIN32/MSVC` 分支（MuPDF 查找 / `NOMINMAX` / `/utf-8` / `windeployqt` 目标）、`scripts/make_portable.ps1`；**未在真机编译验证** | ⚠️ 代码就绪，待 Windows 实机验证 |
| 完善轮 23 | **便携版打包**：`scripts/make_portable.sh` 收集二进制/Qt/插件/依赖闭包，`RPATH=$ORIGIN/lib` 重定位，`AdaptNote.sh` 把 `XDG_DATA_HOME` 指向包内 `data/`；`env -i` + 移目录 + xvfb 三重验证（0 个系统 Qt 依赖） | ✅ |
| 完善轮 22 | **数位板倾斜/旋转笔刷**：`InkPoint::tilt/angle` + `ink::tiltFromDegrees()` + 椭圆形且带方向的盖章渲染（倾斜越平越宽），倾斜/角度随笔画存档与同步 | ✅ |
| 完善轮 21 | **按脑图分组导出**：`ExportContext::byGroups` + Markdown/HTML 复用 `mindRoots()` 递归渲染（分组→子分组→笔记，未分组落页桶），导出菜单加勾选项 | ✅ |
| 完善轮 20 | **冲突可视化合并**：`conflicts::find()/plan()` 纯逻辑 + `ui/conflictdialog`左右对照面板，保留左/右/都保留三种处理，工具栏「冲突」入口 | ✅ |
| 完善轮 19 | **打开即提示内嵌标注**：`pendingEmbeddedAnnotations()`（去重后的待导入数，扫描上限 50 页）+ 每个文件版本只问一次（settings 里按 sha1(路径:大小:mtime) 记忆）；`setEmbeddedPromptEnabled()` 可关掉弹窗便于脚本/测试 | ✅ |
| 完善轮 18 | **读回 PDF 内嵌标注**：`IDocumentEngine::readAnnotations()` 把 Highlight/Underline/Squiggly/FreeText/Text/Ink 读成笔记（颜色/批注/作者/几何），`imports::fromPdfAnnotations()` 去重后入库，工具栏「读标注」 | ✅ |
| 完善轮 17 | **导入**：`imports::fromMarkdown()`（解析 BookxNote 形状的 `## 第 N 页` / `- **高亮** · #tag` / `> 摘录` / `- 批注：`）+ JSON 镜像无损往返 + `commit()` 批量入库 + 工具栏「导入」 | ✅ |
| 完善轮 16 | **PDF 换版本迁移**：文档记录文件大小/mtime，变化时按摘录文字全篇重定位（`migrate::migrateByText`，同页优先 → 全篇打分），一次事务写回 + 报告 + 可撤销 | ✅ |
| 完善轮 15 | **重排版锚点**：`Markup::anchorChapter/anchorOffset` + 引擎 `chapterOfPage/chapterOffsetOf/pageForChapterOffset` + `relocateAnchors()`（开档/换版式自动重定位） | ✅ |
| 完善轮 14 | **写库再优化**：FTS 语句预编译、改写路径复用插入批、可选 `--fast-import`（实测收益在噪声内，默认关）、bench 支持重复测量与全新 id | ✅ |
| 完善轮 13 | **导出跟随分组树**：`mind/tree.*` 抽成无 Widgets 依赖的纯树模块，XMind/OPML 直接复用 `buildFromNotes`（嵌套分组 + 页桶回落），OPML 叶子保留 type/tags/_note | ✅ |
| 完善轮 12 | **数位板压感**：`QTabletEvent` 接管笔压（+旋转/切向压力回退），鼠标仍用速度模拟；输入路径抽成 `inkInputBegin/Move/End` 便于无硬件测试 | ✅ |
| 完善轮 11 | **组内手动排序**：`mind::planReorder()` 纯函数（10/20/30 重排）+ 拖拽落点细化（插到某条之前/之后）+ `Alt+↑/↓` + 多笔记一步撤销 | ✅ |
| 完善轮 10 | **脑图多级分组**：`MindGroup::parentId` + 循环保护 + 删父组时子组上提；`buildFromNotes` 递归建树；右键「新建子分组 / 上移一级」 | ✅ |
| 完善轮 9 | **书库智能视图**：最近阅读 / 未读 / 读了一部分 / 有笔记 / 有冲突 / 按标签（`DocFilter` + `documentPathsWithTag`） | ✅ |
| 完善轮 8 | **性能**：渲染缓存 LRU（字节预算）、批量单事务写入、FTS 变更才重建、FTS id 集合缓存、quads 快序列化；新增 `tools/bench.cpp` + `scripts/bench.sh` 与 README 实测表 | ✅ |
| 完善轮 7 | **脑图分组实体化**：`mind_groups` 表 + 旧数据自动迁移（文本名→实体 id）、新��/重命名/删除分组、分组随同步合并（并发改名取新名） | ✅ |

**测试总量：299 项断言 + 31 项 Python 独立校验，全绿。**
`selftest 48 · visualtest 34 · featuretest 37 · exporttest 51 · synctest 53 · aitest 39 · p5test 37`

截图：`shots/{main,find,double,rotated,epub,mindmap}.png`；导出样例：`shots/exports/`。

## 4. 目录与模块职责

```
src/
  main.cpp                  CLI：--doc/--demo/--page/--mode/--zoom/--rotate/--find/
                            --export-dir/--mindmap/--screenshot
  core/model.{h,cpp}        Markup / Quad / InkPoint / Selection / TextHit / OutlineItem + JSON
  core/textselect.{h,cpp}   点命中、区间→按行合并 quad、文本提取（NFKC 归一化）
  core/store.{h,cpp}        SQLite（documents/markups/settings + FTS5）+ 每文档 JSON 镜像 + 迁移
                            + 书库管理（documents()/renameDocument()/forgetDocument()）
  render/engine.h           引擎抽象（11 个方法）
  render/mupdf_engine.cpp   MuPDF 后端：渲染/字符 quad/目录/PDF annot 增量写回
  ui/pageview.{h,cpp}       版式、缩放、旋转、渲染缓存、选区、叠加层、墨迹、朗读高亮、坐标映射
  ui/mainwindow.{h,cpp}     工具栏/面板/撤销栈/搜索/筛选/导出/同步/AI/脑图/朗读/书库入口
  ui/libraryview.{h,cpp}    书库：文档表（笔记数/冲突数/上次读到）+ 跨库笔记浏览
  export/zip.{h,cpp}        自研 ZIP 读写（deflate + CRC32），供 xmind/apkg 用
  export/exporter.{h,cpp}   5 种导出格式
  sync/vv.{h,cpp}           版本向量：dominates / concurrentWith / join
  sync/webdav_client.{h,cpp}PROPFIND/GET/PUT(If-Match)/MKCOL/DELETE（阻塞 + 超时）
  sync/sync_manager.{h,cpp} 合并引擎 + 按书推拉
  ai/ai_client.{h,cpp}      OpenAI 兼容流式（含 reasoning_content / 取消 / 错误体解析）
  ai/prompts.{h,cpp}        提示词模板 + 角色（JSON 持久化，可编辑）
  ink/ink.{h,cpp}           RDP 简化 + Catmull-Rom 平滑 + 压力线宽 + 包围盒
  mind/mindmap.{h,cpp}      笔记→树（显式分组优先，否则按页）、树形布局、缩放、拖拽重分组、渲染成图
  read/reader.{h,cpp}       断句 + 逐句推进（TTS 可选，无引擎则按字速）
tools/mkpdf.cpp             生成测试 PDF（A4/72dpi，含双栏页与层级页）
tests/*.cpp                 7 个测试目标（见上）
scripts/
  make_testdata.sh          造 test.pdf + test.epub（EPUB2 + NCX）
  demo.sh                   一键构建/测试/截图
  dav_server.py             最小 WebDAV 服务器（stdlib，PROPFIND/GET/PUT+If-Match/MKCOL）
  fake_openai_server.py     假 OpenAI SSE 端点（含 reasoning_content、慢速、401）
  validate_exports.py       Python 独立复核导出物（zipfile/sqlite3/xml.etree）
resources/app.qss           浅色主题
```

## 5. 数据模型

`Markup`（笔记/标注）核心字段：`id, docPath, page, type, color, quads[], text, comment,
tags[], imagePath, ink[], created, updated, writtenToPdf, vv, deviceId, deleted, conflictedFrom`

- `type`：`highlight | underline | squiggly | note | image | ink`
- **坐标**：`quads` / `ink[].pos` 一律是 **PDF 页面坐标（point，未缩放未旋转）**；显示时才乘 CTM。
- `vv`：版本向量 `device:ms,device:ms`；`deleted` 是墓碑；`conflictedFrom` 指向冲突源。

存储位置（`QStandardPaths::AppDataLocation`，Linux 为 `~/.local/share/AdaptNote/AdaptNote/`）：

```
library.db             documents / markups / settings / notes_fts(FTS5 trigram)  + WAL
mirror/<书名>.json     人类可读镜像（每次写笔记后重导，可 diff / 可 git）
images/<uuid>.png     图片摘录原始截图（2× 渲染后裁剪）
ai_roles.json         AI 角色（首次用内置 4 个播种，可编辑）
```

`settings` 表键：`deviceId`（同步用）、`sync.url/user/pass/path`、`ai.url/key/model/role`。
`documents` 表记录 `path/title/pages/lastPage/lastOffset`（阅读位置 = 页码 + 页内比例）。

## 6. 关键设计决策（以及为什么）

1. **坐标与视图解耦**：标注只存页面坐标；`PageView::pageToView/viewToPage` 是唯一双向映射。
   → 缩放/旋转/双页不错位的根因。`visualtest` 用像素断言"叠加层必须压在字上"。
2. **单一真相源**：SQLite 是唯一真源，JSON 镜像是给人看/给 git 看的副产物，PDF annots 是导出。
   → 参考产品是"PDF annots + markups.json 双写"，一致性靠自觉。
3. **引擎可替换**：`IDocumentEngine` 11 个方法，MuPDF 只是第一个后端（AGPL，自用不分发无义务；
   将来发布换 PDFium）。
4. **同步用版本向量而非"最后写入胜"**：每条笔记一个 VV；并发时**保留双方**（副本 + `#conflict`），
   删除是墓碑；合并后 VV 取 join 再 bump → **幂等**（第二轮 0 冲突）。
   删除 vs 编辑的语义：**内容优先**（删除不生效，改过的留下并打标签）。
5. **朗读降级**：`QTextToSpeech` 编译期可选（`ADAPT_HAVE_TTS`）；运行时无引擎就按字速逐句推进。
6. **AI 走 OpenAI 兼容协议**：`baseUrl + /v1/chat/completions`，`stream:true`，解析
   `choices[].delta.content` 与 `delta.reasoning_content`（思维链单列一个折叠区）。

## 7. 踩过的坑（症状 → 根因 → 修复 → 覆盖它的测试）

1. **`notes.tags` 触发 NOT NULL**：无标签时返回 `QString()`（null）→ Qt 绑成 SQL NULL。
   修复：非空使用 `QString::fromLatin1("")`。→ exporttest
2. **`.apkg` 里笔记挂错文档**：同步/导出载荷带了本机绝对路径 → 别的设备上 docPath 对不上。
   修复：线格式剥离 `docPath`，落库时挂回本地路径。→ synctest（"B has 3 notes"）
3. **`syncAll()` 拉不到新书**：只遍历"有笔记的文档"→ 新设备打开一本书永远空。
   修复：`documentPath = documents ∪ markups`。→ synctest
4. **测试互相污染**：demo.sh 共用 `XDG_DATA_HOME`，前一个用例的笔记串进后一个 → 假失败。
   修复：每个用例 `XDG_DATA_HOME=$(mktemp -d)`。
5. **墨迹没落库**：`ink` 字段没加数据库列 → 重开后点数 0、画面上看不见。
   修复：加列 + 迁移 + 读写 JSON。→ featuretest / visualtest
6. **`QRectF::united` 吞掉零尺寸矩形**：Qt 把零尺寸当 null，`united` 返回另一个零尺寸矩形
   → 笔迹包围盒退化成 4×4，命中框/可视区都错。修复：手算 min/max。→ p5test（bounds 断言）
7. **401 错误体读不到**：`readyRead` 已把响应体读进缓冲，`finished` 里再 `readAll()` 得到空。
   修复：优先解析缓冲。→ aitest（"api error surfaced"）
8. **412 前先读完请求体**：假 WebDAV 服务器在条件失败分支没读完 body → keep-alive 串包，
   导致"正确的 If-Match"反而 412。修复：先读 body 再判断。→ synctest（If-Match 两项）
9. **旋转后阅读位置丢失**：`setRotation` 只重排不修正滚动位置。修复：记页内比例 → 重排 → 回位。
   → visualtest（"rotated overlay stays inside the viewport"）
10. **CJK 康熙部首**：某些 PDF 字体无 ToUnicode，提取出 `⻚`/`⼆`。修复：NFKC 归一化（
    Kangxi 区有效，Radicals Supplement 仍残留，见 §8）。
11. **`const_cast` 调非 const 成员**：早期 `findMarkup` 只有非 const 版 → 改成 const/non-const 重载。
12. **null QString 绑成 SQL NULL**：`searchMarkups(query, QString())` 里 `? = ''` 的守卫
    在 NULL 下为假 → 跨库搜索与标签聚合全部返回空。修复：所有 scope 参数入口用 `nonNull()`
    归一化，并把「空 scope」显式解释为「整个书库」（`tagCounts` 改走 `allMarkups`）。→ librarytest
13. **分组节点排序把「无分组」排到最前**：早期排序直接按 `mindGroup` 字符串比较，空串排最前，导致页面桶盖在用户分组之上。修复：显式分组优先、页面桶在后。→ p5test
14. **分组迁移的方向性**：v0.6 把组名直接写在 `markups.mindGroup` 里，实体化后该字段必须是 id；迁移时用 `sha1(docPath|name)` 生成确定性 id，两台设备各自迁移同一份数据会得到同一个 id，避免了「同步后出现两个同名组」。→ selftest（legacy 库迁移）、synctest
15. **重构时用「区间替换」删掉了整段实现**：把 `upsertMarkupRow` 与 `markups()` 之间的代码整体替换，连带删掉了 `removeMarkup` / `deviceId` / `documents` / 分组 CRUD 等十几��函数 —— 编译能过是因为这些函数的**头文件声明还在**，直到链接测试目标才炸。教训：改这种大文件要么小步 edit，要么改完立刻用「头文件声明 vs cpp 实现」的脚本对账（我最后就是这么补回来的）。
16. **批量插入慢的元凶是每行一次 FTS 查询**：`indexMarkup` 每次都在 trigram 虚拟表上 SELECT 一次，3000 条新笔记 = 3000 次全表探测。改成 open 时一次性载入已索引 id 集合后，首次插入从 451 条/秒回到 1218 条/秒。→ featuretest（缓存）/ bench
18. **分组树的两个易错点**：① 环路——把 A 挂到自己的子孙下会让建树无限递归，`setMindGroupParent` 沿 `parentId` 向上走一遍做检查；② 叶子节点的 groupKey——页面桶里的笔记原本是 null groupKey，导致 `groupKeyAt` 命中叶子后返回空串，拖拽目标判断失效；现在叶子继承所属桶/组的 key。→ selftest / p5test
20. **一次重排会改多条笔记**：普通 `MarkupCommand` 只能撤销一条，所以加了 `MultiMarkupCommand`（保存 before/after 列表，一步撤销）；重排算法抽成 `mind::planReorder()` 纯函数单独测试，顺带能验证「原位丢弃 = 无变化」。→ p5test / featuretest
22. **Qt 会同时发 tablet 与 mouse 事件**：不处理就会一笔画两遍（一次压感、一次速度）。现在用 `stylusStroke_` 标记「这一笔归触控笔所有」，鼠标事件在笔画进行中被忽略。→ featuretest
23. **测试里保存了指向临时容器的指针**：`const Markup*` 指向 `visibleMarkups()` 返回的临时 QVector，`settle()` 之后内容已失效，导致「鼠标笔迹压感 0.20」这种诡异断言失败。改成保存值拷贝 + 用 id 识别。
25. **核心库不能依赖 Widgets**：导出器在 `adapt_core` 里，却想复用 `mind::buildFromNotes`，而 `mindmap.h` 会拉进 `QGraphicsView` → 编译失败。把纯树逻辑拆到 `mind/tree.{h,cpp}` 后，视图与导出共用同一套树构建，也顺带保证了「导出与界面看到的结构一致」。
26. **OPML 叶子变自闭合标签后丢了元数据**：树形重写时笔记叶子不再带 `type/tags/_note`，独立校验脚本立刻抓到。修复：导出时按 markupId 回查笔记补属性（`mind::Node` 保持精简）。
32. **写入 TEXT 标注时不能设 QuadPoints**：MuPDF 会让整条标注的写入抛异常，`/Contents` 因此丢失——读回来只有颜色没有批注。修法：Note 类型只设 Rect。
34. **`$ORIGIN` 在 `set -u` 的 bash 里会被展开**：`echo "rpath ($ORIGIN/lib)"` 直接报 `ORIGIN: unbound variable`。用单引号或转义。
36. **MuPDF 的路径必须是 UTF-8**：`QFile::encodeName()` 在 Windows 返回 ANSI 代码页，中文书名直接打不开（Linux 上测不出来）。改成 `path.toUtf8()`，并用带空格的中文路径在 Linux 上做了回归。→ selftest / 手工验证
35. **打包 GTK 平台主题会拖进整套桌面栈**：`libqgtk3.so` → gtk3/gio/gdk ≈15 MB；便携版只保留 Qt 原生主题即可（另外记得 strip 库）。68 MB → 61 MB zip。
34. **`mutool insert` 在 Debian 的 mupdf-tools 里不存在**：想用它造「多一页」的修订版做回归，结果子进程失败、拿到的 PDF 打不开，测试直接段错误（`revised.textPage()` 返回空）。改用自家 `mkpdf --blank-first` 生成两版，稳定可控。教训：外部工具依赖要**先探测能力**，并且测试里对「生成失败」要有明确的短路，而不是继续往下解引用。→ migratetest
31. **锚点在 CJK 文本上要按字符数而不是字节**：`anchorOffset` 用的是 `TextPage::chars` 的下标，和 `quadsForRange()` 同一套坐标，重建 quad 时用「原摘录的字符数」做跨度；若在两端用了不同口径，中文笔记会错位几个字。→ featuretest（重排后仍落在同一段文字上）
29. **基准测量的三个陷阱**（本轮全踩了一遍）：① 重复测量写同一批 id → 测的其实是 UPDATE，跑出 14k 条/秒的假数字；② 重复插入不清库 → 表越写越大，后几轮更慢，混在一起取平均没有意义；③ 插入 A 批却在 B 批上测改写 → 行数翻倍，还把 INSERT 当成 UPDATE 计时。正确做法：每轮全新 id、改写测量复用刚插入的那批、报告单轮数字 + 中位数，并注明宿主机噪声。
28. **无 TTS 引擎插件**：`qt6-speech-dev` 装了但 `/plugins/texttospeech` 为空 →
    `availableEngines()` 空 → 走定时推进（测试显式断言该降级路径）。

## 8. 已验证 vs 未验证（诚实清单）

**已验证（自动化）**：渲染尺寸/旋转、字符级选择、双栏不越界、SQLite 往返 + 迁移、
PDF annot 写入/重开/渲染可见、EPUB 重排版与目录、FTS5 中文子串、标签聚合、图片摘录、
撤销重做、阅读位置跨进程恢复、5 种导出格式回读、真实 HTTP 双设备同步（含冲突/删除/幂等/412）、
AI 流式（含鉴权头/请求体/取消/错误）、手写几何、脑图布局与渲染、朗读断句与推进。

**未验证**：
- **真实 Anki 客户端**导入 `.apkg`（只做了 schema/JSON/csum/integrity 校验）
- **XMind 2020+/Zen** 打开（输出的是 XMind 8 `content.xml`）
- **真实 DeepSeek/SiliconFlow** 回答质量（只用假端点验证协议层）
- **真机双端同步**（只有本地两库 + 真 HTTP）
- **真实语音朗读**（本机无引擎插件）
- 带 `/Rotate` 的 PDF 标注贴字（做了 `pdf_page_transform` 换算，未逐页验证）
- 大书性能（千页 PDF、上万条笔记未压测）

**已知限制**：手写的压感仍是速度模拟（真实数位板压感未接）；笔迹擦除是「切成多段」而不是「删掉一段线段内部的点」，因此极短碎段会被丢弃（<2 pt）。
脑图分组寄存在笔记上（无独立分组对象，删除最后一条笔记组即消失）；
手写无真实压感（用速度模拟）、橡皮按轨迹切断（切点附近的采样点会一起消失）；**手写墨迹不写回 PDF**（MuPDF 1.21 能写合法 `/Subtype /Ink` + `/InkList`，
但不生成外观流，任何 MuPDF 渲染下都不可见 → 因此刻意拒绝写入，墨迹只存在库里并参与同步/导出）；
`docKey = 文件名:大小`（换版本=新书）；
远端整本一个 JSON（无增量）；密码/Key 明文存库；冲突只做到"保留两份+标签"，无图形化合并；
EPUB 各端页码可能偏移（字符级 quad 不受影响）；脑图只读；手写无橡皮/压感。

## 9. 参考产品（BookxNote）反编译结论摘要

- Qt5.15 Widgets + QSS，**无 QML 业务 UI**（Qml/Quick 只为 QtWebEngine 服务）
- **静态链接 MuPDF**（PDF/EPUB 渲染、真 PDF annots：Highlight/Underline/Squiggly/FreeText/InkList）
- **libgit2 仅 21 个 API**：本地 commit 快照（msg=时间戳），**没有 merge/fetch/push** →
  跨设备不是 git，而是 WebDAV 文件级 + 应用层对象合并信号（`objMerged`）
- 同步：PROPFIND（getetag/getlastmodified/getcontentlength）+ 自维护 `synccache` 表，
  **无 If-Match**，出错靠"清理缓存/重新下载"按钮兜底
- 内嵌 `webdav/GoWebDAV.exe`（Go 1.19，gogf/gf，端口 9999，默认口令 MD5("123456")）
- AI：`api.deepseek.com` + `api.siliconflow.cn`，角色模板 JSON（用户可编辑）
- OCR：百度 / 腾讯云；翻译：百度/Google/DeepL/iciba；TTS：Qt TextToSpeech + lame 出 MP3
- 内嵌 markdown-it + highlight.js + MathJax2 + CodeMirror 主题，用 QWebChannel 驱动
  `channel.objects.content`（只有 `text`/`baseUrl` 属性和 `textChanged` 信号，桥面极小）

## 10. 后续优先级建议

**已识别、值得做的（按性价比）**
1. ~~墨迹写回 PDF~~ → **已试并放弃**（见 §8：MuPDF 1.21 不生成 ink 外观流）。
   要恢复这条路，需要换引擎（PDFium 能写 ink appearance）或自己往内容流里画。
2. ~~图片摘录参与同步~~ → **已完成**：线格式只带文件名，PNG 走 `<remote>/images/<name>`，双向补齐。
3. ~~手写橡皮 + 速度调宽~~ → **已完成**（`E` 橡皮、速度→压感→笔锋、笔宽预设、可撤销）
4. ~~AI 角色编辑器~~ → **已完成**（增删改角色，写回 `ai_roles.json`）
5. ~~脑图节点点击跳回原文 / 导出 PNG~~ → **已完成**
6. ~~朗读语速、跳过/重复本句~~ → **已完成**（朗读按钮右键菜单）
7. ~~导出只导筛选结果~~ → **已完成**；~~多文档合并导出~~ → **已完成**（导出菜单 → 导出整个书库）
8. ~~手写局部擦除~~ → **已完成**（`ink::splitStroke`，切点用二分插值落在橡皮边缘）；
   ~~笔迹颜色独立~~ → **已完成**（`ink.color` 持久化，橡皮半径跟笔宽联动）
9. 性能：千页 PDF 的渲染缓存上限、FTS 增量索引
10. ~~脑图拖拽改结构~~ → **已完成**；~~新建/重命名/删除分组~~ → **已完成**（`mind_groups` 实体 + 迁移 + 同步）；仍待做：组内手动排序、多级嵌套
11. ~~书库视图~~ → **已完成**；~~改名/删除文档/批量导出~~ → **已完成**；仍待做：拖拽批量整理、按阅读进度/标签的智能视图

**不建议现在做**：可视化冲突合并 UI（成本高、自用频率低）、跨平台打包/签名、
云端服务端（自用不需要）。

## 11. 常用命令速查

```bash
# 只看某一阶段是否还活着
cd build && ctest -R 'selftest|visualtest' --output-on-failure

# 生成一张界面截图（无头）
QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1500x1000x24" \
  ./build/adaptnote --doc build/test.pdf --demo --screenshot /tmp/shot.png

# 只导出不打开界面
./build/adaptnote --doc build/test.pdf --demo --export-dir /tmp/exports

# 渲染脑图
./build/adaptnote --doc build/test.pdf --demo --mindmap /tmp/map.png

# 检查导出的 APKG 结构
python3 scripts/validate_exports.py /tmp/exports
```
