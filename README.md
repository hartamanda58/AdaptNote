# AdaptNote — 本地优先的 PDF / EPUB 阅读与标注工具（自用版）

对标 BookxNote Pro 的自用实现。**P0–P5 全部完成并通过自动化验收**：
能打开 PDF/EPUB、连续/双页/单页、缩放旋转、拖选文本、五类标注（含图片摘录）、写入原 PDF、
全文搜索（FTS5）、标签与颜色筛选、撤销/重做、精细阅读位置记忆、导出 MD/HTML/OPML/XMind/Anki、
**WebDAV 双向同步（版本向量合并 + 冲突保留）**、手写笔迹、脑图、逐句朗读、AI 助手。

设计上刻意做了三处不同（相对参考产品）：

| 方面 | BookxNote Pro（反编译观察） | 本项目 |
|---|---|---|
| 标注坐标 | PDF annots + `markups.json` 双写，一致性靠代码自觉 | **JSON/SQLite 是唯一真相源**，PDF annots 是导出产物 |
| 搜索 | 未确认（未反编译该模块） | **SQLite FTS5 + trigram**，中文子串可搜 |
| 同步 | etag 比对 + 最后写入胜 + 手动重同步兜底 | **版本向量（per-note）+ 墓碑 + 冲突保留**，见下 |
| 引擎 | 静态链接 MuPDF，无替换路径 | **`IDocumentEngine` 抽象**，MuPDF 只是第一个后端 |

## 构建 / 运行

依赖（Debian 12 已在本机安装并验证）：

```bash
apt install cmake ninja-build qt6-base-dev qt6-sql-sqlite libmupdf-dev \
            libmujs-dev libfreetype-dev libharfbuzz-dev libgumbo-dev \
            libopenjp2-7-dev libjbig2dec0-dev libjpeg62-turbo-dev zlib1g-dev \
            fonts-noto-cjk xvfb
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 生成测试文档（PDF + EPUB）
bash scripts/make_testdata.sh build

# 跑程序
./build/adaptnote --doc build/test.pdf
```

一键跑完"构建 → 造测试文档 → 两套测试 → 截图"：

```bash
bash scripts/demo.sh        # 截图输出到 shots/
```

命令行参数：

```
--doc <file>          打开文档（pdf/epub/xps/cbz/fb2）
--demo                注入 4 条演示标注（高亮/下划线/波浪线/批注）
--mode single|continuous|double
--zoom fitwidth|fitpage|actual|<百分比>
--rotate 0|90|180|270
--page N              跳到第 N 页
--screenshot out.png  截图后退出（配合 xvfb-run 可无头运行）
```

## 快捷键

| 键 | 作用 |
|---|---|
| `Ctrl+O` | 打开文件 |
| `Ctrl+←/→` | 上一页 / 下一页 |
| `Ctrl+=` `Ctrl+-` / `Ctrl+滚轮` | 缩放 |
| `Ctrl+R` | 旋转 90° |
| `H` / `U` / `S` / `N` | 高亮 / 下划线 / 波浪线 / 批注（对当前选区） |
| `X` | 框选摘录图片（Esc 取消） |
| `Ctrl+F` | 本书内查找；`Enter` / `Shift+Enter` 下一个 / 上一个 |
| `Ctrl+Z` / `Ctrl+Shift+Z` | 撤销 / 重做（增删标注、改批注、改标签） |
| `Del` | 删除笔记面板中选中的笔记 |
| `Ctrl+S` | 把未写入的标注写回 PDF（增量保存） |
| 工具栏「同步」 | 与 WebDAV 双向同步（按钮右键 → 同步设置…） |
| `P` | 手写笔迹模式（拖拽书写，支持数位板压感；再按一次退出；按钮右键：笔宽 细/中/粗、笔迹颜色） |
| `E` | 橡皮：拖过手写笔迹即擦除；从中间擦会**切成两段**（可 Ctrl+Z 撤销） |
| `M` | 脑图：拖动笔记换分组或**插到某条之前**（上半格=插前，下半格=插后）；`Alt+↑/↓` 组内微调；右键分组可新建子分组/重命名/上移一级/删除；点击节点跳回原文 |
| `L` | 打开书库：智能视图（最近/未读/读了一部分/有笔记/有冲突/按标签）+ 跨库搜索；双击打开/跳转，可改名、移除、批量导出 |
| `R` | 逐句朗读（有语音引擎就出声，否则逐句高亮推进；按钮右键：语速/重读本句/跳过本句） |
| AI 面板 | 选角色 + 动作（解释/翻译/总结/出题/自定义）→ 对选区提问；「角色…」可增删改 |
| 工具栏「导出」 | Markdown / HTML / OPML / XMind / Anki `.apkg`；菜单可勾选「只导出筛选结果」与「按脑图分组输出（MD/HTML）」，子菜单「导出整个书库」按书分组 |

左上 dock 两个页签：

- **大纲**：原生书签优先，缺失时用「字号大于正文」的启发式生成（EPUB3 也适用）。
- **笔记**：顶部筛选条 —— 全文搜索框（FTS5，中文可搜子串）、标签下拉（带计数）、5 个颜色开关、
  「全部文档」开关（跨书库搜索）；下面是笔记列表（点击跳回原文、双击改批注、右键改标签/删除）。
  跨文档结果点击会自动切换并打开对应文档。

在页面上右键：对已有标注可编辑批注/标签/删除，对选区可直接加标注，或框选摘录图片。

## 架构

```
src/
  core/model.{h,cpp}        标注/选区/目录数据结构 + JSON 序列化
  core/textselect.{h,cpp}   点 → 字符命中、区间选择、按行合并 quad、文本提取（NFKC 规范化）
  core/store.{h,cpp}        SQLite（documents/markups + FTS5 notes_fts）+ 每文档 JSON 镜像
  export/zip.{h,cpp}        自研 ZIP 读写（deflate/store + CRC32），供 .xmind / .apkg 使用
  export/exporter.{h,cpp}   5 种导出格式（md/html/opml/xmind/anki）
  sync/vv.{h,cpp}           版本向量（支配 / 并发判定 / join）
  sync/webdav_client.{h,cpp} PROPFIND/GET/PUT(If-Match)/MKCOL/DELETE 阻塞式客户端
  sync/sync_manager.{h,cpp} 合并引擎 + 按书的推拉流程
  sync/conflicts.{h,cpp}    冲突配对与三种处理方案（纯逻辑）
  ui/conflictdialog.{h,cpp} 冲突合并面板（左右对照 + 三个动作）
  migrate/migrate.{h,cpp}   换版本迁移：按摘录文字在新版 PDF 里重新定位（纯逻辑，可测）
  import/import.{h,cpp}     导入：Markdown（BookxNote 形状）/ 本程序 JSON 镜像
  ai/ai_client.{h,cpp}      OpenAI 兼容流式客户端（含 reasoning_content / 取消 / 错误解析）
  ai/prompts.{h,cpp}        提示词模板 + 可编辑角色（JSON 持久化）
  ink/ink.{h,cpp}           手写笔迹：RDP 简化 + Catmull-Rom 平滑 + 压力线宽
  mind/tree.{h,cpp}         纯树逻辑（分组→节点、排序计划），供视图与导出共用、不依赖 Widgets
  mind/mindmap.{h,cpp}      脑图视图：布局、缩放、拖拽重分组/排序、渲染成图
  read/reader.{h,cpp}       朗读：断句 + 逐句推进（TTS 可选）+ 高亮驱动
  render/engine.h           引擎抽象（open/render/textPage/outline/writeAnnotations）
  render/mupdf_engine.cpp   MuPDF 后端：渲染、字符 quad、目录、PDF annot 写入（增量保存）
  ui/pageview.{h,cpp}       翻页/缩放/旋转/版式、渲染缓存、选区、标注绘制、坐标映射
  ui/mainwindow.{h,cpp}     工具栏、大纲/笔记面板、标注增删改、写回 PDF
  ui/libraryview.{h,cpp}    书库窗口：智能视图 + 文档表（笔记数/冲突数/上次读到）+ 跨库笔记浏览
tools/mkpdf.cpp             生成测试 PDF（A4/72dpi，含双栏页与层级页）
tests/selftest.cpp          引擎/选择/存储/FTS/标签/迁移/写回/EPUB 共 48 项断言
tests/visualtest.cpp        叠加层几何回归（标注与查找命中都必须压在字上）共 29 项
tests/featuretest.cpp       P2 功能回归（图片摘录/标签/查找/撤销重做/阅读位置/导出入口）共 32 项
tests/exporttest.cpp        导出回读校验（自研 ZipReader / XML 解析 / SQLite）共 51 项
tests/synctest.cpp          同步（版本向量 + 真实 WebDAV 双设备）共 53 项
tests/aitest.cpp            AI（提示词/流式/鉴权/取消，假端点）共 37 项
tests/p5test.cpp            手写几何 / 脑图布局 / 朗读断句
tests/migratetest.cpp       换版本迁移（真实生成修订版 PDF）
tests/importtest.cpp        导入闭环（导出→导入、外来 Markdown、JSON 镜像）
tests/conflicttest.cpp      冲突：合并产生 → 配对 → 保留左/右/都保留
scripts/validate_exports.py 独立交叉验证（Python stdlib：zipfile/sqlite3/xml.etree）
resources/app.qss           浅色主题
```

### 三个关键设计

1. **坐标与视图解耦**（这是"缩放/旋转后标注不错位"的根因）
   标注一律存 PDF 页面坐标（point，未旋转、未缩放）。显示时：
   `view = pageRect.topLeft + R(rotation) · p · zoom`。
   `PageView::pageToView / viewToPage` 是唯一的双向映射，绘制、命中、选择全部走它。

2. **一份真相源 + 可选的 PDF 落地**
   标注先写 SQLite（并同步导出人类可读的 `…markups.json` 镜像），PDF annots 只在按 `Ctrl+S`
   且未写过时才写入（`writtenToPdf` 标记），避免重复标注。

3. **引擎可替换**
   `IDocumentEngine` 只有 11 个方法。若将来要发布（AGPL 义务触发），补一个 PDFium 后端
   即可，UI/存储/导出代码不动。

## 重排版文档的定位（EPUB）

EPUB 的页码会随版式（字号、页宽、设备）变化，所以笔记除了页码/quad 之外还存一个稳定锚点：
`anchorChapter`（MuPDF chapter 索引）+ `anchorOffset`（该章内的字符偏移）。

- 新建笔记时由引擎把 `(page, charIndex)` 换算成 `(chapter, offset)`（`supportsAnchors()` 为真的文档才记）
- 打开文件或用 `setReflowLayout()` 改变版式后，`relocateAnchors()` 反查 `pageForChapterOffset()`，
  页码变了就重建 quad 并批量写回（一次事务）
- 测试断言的不变量是「锚点仍指向同一段文字」，而不是「页码不变」

## 从其他阅读器导入

工具栏「导入」接受两类文件，导入的笔记走和原生笔记完全一样的存储/搜索/同步路径：

- **Markdown**（BookxNote 等导出物的形状）：`## 第 N 页` 分节、`- **高亮** · #标签`
  条目、`> 摘录`、`- 批注：…`；类型（高亮/下划线/波浪线/批注）与标签都会解析
- **本程序的 JSON 镜像**（`mirror/<书>.markups.json`）：**无损往返**，连 quad 几何都在
- **PDF 里已有的标注**：打开文件时若发现内嵌标注会**问一次**是否读进来（同一文件版本只问一次，
  按"路径+大小+mtime"记忆）；也可随时点工具栏「读标注」。支持 Highlight / Underline / Squiggly /
  FreeText / Text / Ink，颜色、批注内容、作者、几何都还原，已存在的按"页码+类型+几何"去重

导入时可以选择挂到当前打开的书，或按文件里的书名另建文档记录。测试 `importtest`
把「导出 → 导入」跑成闭环：3 条笔记的类型、标签、批注、页码、正文全部一致，
JSON 镜像还能保住几何。

## 冲突怎么处理（可视化合并）

同步遇到"两端都改了同一条"时会保留两份并打上 `#conflict`。工具栏「冲突」打开合并面板：
左侧原始版本、右侧冲突副本并排显示（类型/页码/标签/时间/摘录/批注），每个冲突有三种处理：

| 动作 | 结果 |
|---|---|
| **保留左侧** | 删掉副本，保留原始笔记（并去掉 `#conflict` 标签） |
| **保留右侧** | 用副本的内容（摘录/批注/标签/颜色/几何/页码）覆盖原始笔记，再删掉副本 |
| **都保留** | 两条独立的笔记，都去掉 `#conflict` 与 `conflictedFrom` |

配对逻辑在 `sync/conflicts.{h,cpp}`（纯函数，`find()` + `plan()`），面板只负责展示与执行，
所以 `conflicttest` 能同时验证"合并产生冲突 → 配对 → 三种处理后的库状态"。

## 换了一版 PDF 怎么办（文本迁移）

页码会因为换文件而全部错位。打开文件时如果发现 **文件大小或 mtime 变了**，程序会自动做一次
「按文字迁移」：

- 先看原页：摘录文字还在就只刷新 quad（最常见）
- 不在原页：全篇扫描，用摘录前 24 个非空白字符定位，按字符数取回跨度，用相似度打分
- 移动结果 **一次事务写回**，弹窗报告「移动 N 条 / 未找到 M 条」，可 `Ctrl+Z` 撤销
- 找不到文字（那一版删掉了这段）的笔记保持原样并列入「未找到」，不会乱跳

`migrateByText()`（`src/migrate/`）是不依赖界面的纯逻辑，`migratetest` 用
`mkpdf --blank-first` 生成「前面多一页」的修订版做真实回归：3 条笔记全部 1→2、3→4 且
相似度 1.00，quad 落在新页的字符上；原文被删除的笔记被正确报为 lost。

## 同步与冲突（P4）

远端布局：`<base>/adaptnote/<docKey>.json`，一本书一个文件。
`docKey = sha1(小写文件名:字节数)` 前 16 位（同一本书在两台机器上得到同一个 key）。

载荷是该书**全部笔记（含墓碑）**的 JSON，并且**剥离本机绝对路径**（路径是设备私有的，
拉取后再挂回本地路径）。所有合并都在"每条笔记一个版本向量（device→毫秒时间戳）"上做：

| 两端情况 | 处理 |
|---|---|
| A 的 VV 支配 B（逐设备 ≥ 且至少一处 >） | 取 A |
| VV 完全相同 | 取 updated 较新者（并列取本地） |
| 并发 · 两边都改同一条 | **两份都留**：较新者保留原 id，另一份变成新 id 副本 + `#conflict` 标签 + `conflictedFrom` |
| 并发 · 一边删、一边改 | **内容优先**：删除不生效，改过的笔记保留原 id 并打 `#conflict` |
| 两边都删 | 只留一个墓碑，不算冲突 |

- **幂等**：合并后把版本向量取 join 再 bump，所以第二轮同步 `0 冲突 / 0 推送`（测试断言）。
- **删除可传播**：删除 = 墓碑（`deleted=1` + VV 自增），会同步到其他设备；本地查询默认过滤墓碑。
- **条件写**：`PUT` 带 `If-Match: <etag>`，服务端返回 `412` 时重新 `GET → 合并 → PUT` 一次。
- **冲突 UI**：冲突副本自动带 `#conflict` 标签，笔记面板的标签下拉会列出 `conflict (n)`，
  点开逐条处理（改批注/删除任一份即完成合并）。

`synctest` 用**真实 HTTP** 验证：脚本 `scripts/dav_server.py`（仅标准库）实现
PROPFIND / GET / PUT(If-Match) / MKCOL，两台"设备"各自独立的 SQLite，覆盖：
推送→拉取、并发编辑、删除 vs 编辑、纯删除传播、幂等、412 前置条件、墓碑不进 UI 查询。

## 测试

```bash
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure
```

`selftest` 覆盖：打开 PDF、页数、A4 尺寸（595×842pt）、1.5× 渲染尺寸、旋转 90° 后尺寸交换、
文本提取、行选区 quad、双栏不越界、SQLite 往返、JSON 镜像生成、PDF 标注写入 + 重开校验 +
渲染可见性、EPUB 可重排版/页数/目录/渲染。

`visualtest` 覆盖：视口背景色、页面矩形位置、每条标注的叠加层位置（必须既"有色"又"压着字"）、
旋转后叠加层跟随页面、双页模式两页并排且顶部对齐。

当前结果：**selftest 48/48、visualtest 29/29、featuretest 32/32、exporttest 51/51、synctest 53/53 全 PASS**，
外加 Python 独立校验 **31/31 PASS**（用 zipfile 校验 CRC、sqlite3 校验 Anki 库、xml.etree 校验 opml/xmind）。

## 便携版打包（Linux x64）

```bash
bash scripts/make_portable.sh          # → dist/AdaptNote-<ver>-linux-x64.zip
```

产出结构（全部自包含，解压即用）：

```
AdaptNote-<ver>-linux-x64/
  AdaptNote.sh      启动器：设 QT_PLUGIN_PATH / LD_LIBRARY_PATH / XDG_DATA_HOME=./data
  bin/adaptnote     主程序（strip 过，RPATH=$ORIGIN/../lib）
  lib/              Qt 6.4 + 依赖闭包（不含 glibc / ld.so）
  plugins/          platforms(xcb,offscreen,minimal) · sqldrivers(sqlite) · imageformats
                    · iconengines · styles · tls · platformthemes(仅 Qt 原生，不打包 GTK 主题)
  data/             运行后生成：library.db / mirror / images / ai_roles.json（删掉=恢复出厂）
  samples/          test.pdf（3 页测试书）+ test.epub
  README.txt · BUILD-INFO.txt
```

**验证过的点**（不是"应该能跑"）：

| 检查 | 结果 |
|---|---|
| 解压后直接运行（`env -i`，无系统 Qt） | ✅ 截图正常，`ldd` 里 **0 个** `/usr/lib/.../libQt6*` |
| 真实 X11 平台（xvfb + 打包的 `libqxcb.so`） | ✅ 正常出图 |
| 整个目录移到带空格的路径（`$ORIGIN` 重定位） | ✅ 正常 |
| 数据落在包内 | ✅ `data/AdaptNote/AdaptNote/library.db`，删 `data/` 后重建为空库 |
| 从包里导出五种格式 | ✅ md/html/opml/xmind/apkg 全 ok |
| 体积 | 122 MB 展开 / **61 MB zip**（143 个文件） |

踩过的两个打包坑：**①`$ORIGIN` 在 `set -u` 的 bash 里会被当变量展开**（要用单引号）；**②不要打包 GTK 平台主题** —— `libqgtk3.so` 会拖进整套 gtk3/gio/gdk（约 15 MB），去掉后 Qt 原生主题照样工作，顺手 strip 掉库里的符号，68 MB → 61 MB。

注意：目标是 x86_64 Linux，且 **glibc 不打包**，因此需要与构建机兼容的 glibc（Debian 12 = 2.36 及以上）。

## Windows 便携版

代码本身已经是跨平台的（Qt6 + 标准 C++，无 POSIX 依赖），Windows 版需要**在一台 Windows
机器上编译**——交叉编译 Qt+MuPDF 不现实。

> **状态：已在真 Windows 上跑通。** GitHub Actions `windows-latest` 上完整流程成功
> （run `40225724484`）：编译 MuPDF → 编译本项目 → **11 套测试全绿** → 产出
> `AdaptNote-<ver>-windows-x64.zip`。下面的步骤就是 CI 里实际验证过的那一套。

### 1. 装构建环境（一次性）

| 组件 | 说明 |
|---|---|
| Visual Studio 2019/2022（含 C++ 工具） | MSVC x86_64 工具链（CI 上是 VS18 / MSVC 14.51） |
| Qt 6.8+ MSVC 套件 | 例如 `C:\Qt\6.8.3\msvc2022_64`。**不要用 6.5.x**：它在 VS18 上会因
`qvarlengtharray.h` 里的 `stdext::checked_array_iterator` 被移除而编译失败 |
| CMake ≥ 3.21 + Ninja | 或直接用 VS 生成器 |
| MuPDF 1.21.1（静态库） | 见下 |

### 2. 编译 MuPDF（静态、无 GUI 依赖）

MuPDF **1.21.1 没有 `Makefile.nmake`**（那是老版本的），Windows 上走 VS 解决方案：

```bat
git clone --recursive https://github.com/ArtifexSoftware/mupdf.git
cd mupdf && git checkout 1.21.1

REM MuPDF 默认用静态 CRT（/MT），Qt 用动态 CRT（/MD）——必须先统一，否则链接期报
REM LNK2038: mismatch detected for 'RuntimeLibrary'
powershell -Command "Get-ChildItem platform\win32\*.vcxproj | ForEach-Object {
  (Get-Content $_ -Raw) -replace '<RuntimeLibrary>MultiThreaded</RuntimeLibrary>',
    '<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>' | Set-Content $_ -Encoding UTF8 }"

msbuild platform\win32\mupdf.sln /m /t:libmupdf /p:Configuration=Release /p:Platform=x64
```

产物在 `platform\win32\x64\Release\`：`libmupdf.lib` + `libthirdparty.lib` +
`libmuthreads.lib` + `libresources.lib`，四个都要链。项目 CMake 会把它们全部 glob 进来，
另外再链接系统库 `user32 gdi32 advapi32 ole32 shell32 ws2_32 comdlg32`。

> zlib 头文件用 MuPDF 自带的 `thirdparty/zlib`（`MUPDF_ZLIB_INCLUDE`），不要指望系统里有一份。

### 3. 编译与打包

```bat
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64 ^
      -DMUPDF_ROOT=C:\src\mupdf
cmake --build build-win --config Release --parallel

pwsh -File scripts\make_portable.ps1 -QtDir C:\Qt\6.8.3\msvc2022_64 -MuPdfRoot C:\src\mupdf
```

产出 `dist\AdaptNote-<ver>-windows-x64.zip`：

```
AdaptNote-<ver>-windows-x64/
  AdaptNote.bat        启动器（把 XDG_DATA_HOME 指到 .\data，数据全在包内）
  bin\adaptnote.exe
  bin\*.dll           windeployqt 收集的 Qt 运行库 + 插件 + MSVC 运行库
  data\               首次运行后生成（library.db / mirror / images / ai_roles.json）
  samples\            test.pdf / test.epub
```

也可以只让 CMake 收集依赖：`cmake --build build-win --target deploy-windows`。

### 4. 已经做过的 Windows 兼容处理（全部在 CI 上验证过）

- `M_PI` 换成 `an::kPi`（MSVC 默认不定义 `M_PI`）
- 传给 MuPDF 的路径改为 **UTF-8**（`QFile::encodeName` 在中文路径下会按 ANSI 编码，必然打不开）
- **CJK 字面量**：`QLatin1Char('”')` 这类写法在 MSVC 下会截断成 0x?? 并报 C4309，
  导致中文右引号/括号永远匹配不上 → 一律改成 `QChar(0x201D)`、`QChar(0xFF09)` 这种码点写法
- 测试里的 `python3` 按平台切换成 `python`
- CMake 增加 `WIN32` / `MSVC` 分支：MuPDF 查找顺序、`NOMINMAX`、`/utf-8`、CRT 一致、`windeployqt` 目标
- 便携脚本改为 PowerShell：`windeployqt --compiler-runtime` + `Compress-Archive`
- **无头测试**：CI 上设 `QT_QPA_PLATFORM=offscreen` + `ADAPT_HEADLESS=1`，所有
  `QMessageBox` / `QInputDialog` 走包装函数直接返回默认值，否则 offscreen 下弹窗会永久阻塞
  （第一次 CI 就是这么挂 13 分钟的）

> ⚠️ 踩过的坑：**Windows 上 Qt 的 `QPdfWriter` 生成出来的 PDF 没有文本层**（同一次运行里
> 找不到可嵌入字体，文字被画成了路径）。所以测试**不能**依赖它生成“带文字”的 fixture ——
> `migratetest` 现在自己手写 PDF（标准 Helvetica、无嵌入字体），跨平台行为完全一致。

### 附：三种拿到 Windows 构建的途径

| 途径 | 需要什么 | 适用 |
|---|---|---|
| **GitHub Actions**（已跑通） | 一个 GitHub 账号 | `.github/workflows/windows-portable.yml`：手动触发 → 真 Windows runner 编译 MuPDF + 本项目 → 跑 11 个测试 → 上传 `AdaptNote-windows-x64` 产物 zip。免费、约 45–55 分钟（MuPDF 约 10 分钟，应用约 30 分钟） |
| **本机 QEMU/KVM** | Windows ISO | `scripts/windows-vm.sh <iso>`：2 vCPU / 3 GB / 40 GB qcow2、KVM 加速、SMB 共享目录把 zip 取回。建议装 **Server Core** 或 **LTSC**（无桌面也够用，测试跑 offscreen） |
| 云 Windows 主机 | 按小时计费 | 2c4g 可用但紧（见下），4c8g 更省心 |

**2c4g 的实测预算**（本项目在 Linux 上用 `-j2` 模拟 2 核测得）：

| 环节 | 内存峰值 | 说明 |
|---|---|---|
| 本项目 clean build（11 个测试目标） | 464 MB | 138 秒，`-j2` |
| 单个最重编译单元 | 337–464 MB | MSVC 通常再高 20–40% |
| **MuPDF 源码编译** | **1.6–2.0 GB** | ⚠️ 唯一容易 OOM 的一步 |

所以要：`-j2` 并行、给构建目录加 Defender 排除、开页面文件、别同时开 VS IDE、先编 `adaptnote` 再编测试；磁盘预留 ≥12–15 GB。

### 5. 尚未验证 / 需要你在 Windows 上确认

编译、链接、11 套自动化测试、打包都已经在真 Windows runner 上跑通。仍然**没有**验证的是
需要人眼/硬件的部分：

- **Windows 上的实际界面手感**（字体渲染、高分屏缩放、深浅色主题）——CI 只跑 offscreen
- Windows 上**墨迹压感**依赖 Qt 的 `QTabletEvent`（Wacom/WinTab 驱动），已实现但未在真机试过
- 桌面快捷方式/文件关联（本项目刻意不做注册表改动，保持绿色便携）
- 同步的 WebDAV 依赖 Qt Network + 打包的 TLS 插件；自签证书的服务器可能需要额外信任配置

## 性能（实测，`bash scripts/bench.sh` 可复现）

机器：AMD Ryzen 9 5950X（本机容器，Release 构建），PDF 由 `tools/bench.cpp` 现场生成。

### 300 页 / 3,000 条笔记

| 指标 | 数值 | 备注 |
|---|---|---|
| 打开文档 | 0.8 ms | 300 页索引 |
| 渲染单页 @1.5×（冷） | 11.7 ms | 893×1263 |
| 渲染 20 个不同页 | 115 ms | 5.7 ms/页（含光栅化） |
| 页面文本提取 | 1.4 ms | 1764 字符 |
| 文本选择 ×200 | 53 ms | 0.27 ms/次 |
| **批量写入 3000 条笔记** | **≈2.2 s** | **≈1.3k 条/秒**（单事务；三次测量中位数，本机为共享容器，单次波动 1.7–2.3 s） |
| 重复写同样 3000 条 | 224 ms | 内容未变 → 跳过 FTS 重建（约快 10×） |
| 读出全部笔记 | 39 ms | 3000 行 |
| FTS 搜索（标签/正文） | 22 / 11 ms | 1000 / 539 命中 |
| 导出 Markdown | 11 ms | 272 KB |
| 导出 XMind | 57 ms | 92 KB |
| 生成同步载荷（JSON） | 89 ms | 1.4 MB |
| 合并 2×3000 条 | 12 ms | 版本向量合并 |

### 1000 页 / 6,000 条笔记

| 指标 | 数值 |
|---|---|
| 打开文档 | 0.8 ms |
| 渲染单页 @1.5×（冷） | 7.7 ms |
| 渲染 20 个不同页 | 58 ms（2.9 ms/页） |
| 批量写入 6000 条笔记 | 8.1 s（741 条/秒） |
| 读出全部笔记 | 101 ms |
| FTS 搜索（标签） | 45 ms（2000 命中） |
| 导出 Markdown | 38 ms（552 KB） |
| 生成同步载荷 | 128 ms（2.8 MB） |
| 合并 2×6000 条 | 28 ms |

**这一轮做的优化**

1. **渲染缓存改成真正的 LRU**：原来超过 32 张就整体清空；现在按**字节预算**（默认 96 MB）+ 条目上限（64）淘汰最久未用的页面，并有回归测试守住"预算内 / 最旧页被淘汰 / 当前页仍在"。高倍缩放时不再抖动。
2. **批量写入走单事务**：`Store::upsertMarkups()`，同步路径改用它；镜像文件每次批量只导一次。
3. **FTS 只在内容变化时重建**：写入前比较 `text|comment|tags` 指纹（内存 + 表），颜色/位置/同步触碰不再触发 rebuild → 重复写快约 9×。
4. **FTS 已索引 id 集合一次载入**：批量插入新笔记时不再每行做一次 trigram 虚拟表查询（这是首次插入最大的开销）。
5. **quads 序列化走手写快路径**：每行写库不再构造 `QJsonDocument`。
6. **FTS 语句预编译**：`delete`/`insert into notes_fts` 复用同一条预编译语句（原来每行重新 prepare）。
7. **可选「快速导入」**（`--fast-import`）：导入期间 `synchronous=off`。实测只快 ~10–15%，**落在本机噪声内**，故默认关闭；代价是断电可能丢最近的笔记。

## 已知问题（P1 范围内）

- 若 PDF 字体没有 `ToUnicode`，提取文本可能出现康熙部首/兼容字（如 `⻚`）。已对 Kangxi 区做 NFKC
  归一化，但 **CJK Radicals Supplement（U+2E80–U+2EFF）不在 NFKC 覆盖内**，仍可能残留。
- 页面带 `/Rotate` 的 PDF：标注写入时用 `pdf_page_transform` 做了转换，但旋转页的精确贴字
  未逐页验证（当前测试文档为 0 旋转）。
- 图片摘录以 PNG 存在应用数据目录（images/<id>.png），不嵌入 PDF；备份时要连库一起备份。
- 跨文档的笔记只能跳转/删除，编辑批注与标签需要先打开对应文档。
- 撤销栈只在内存里，关闭程序后清空（数据库里的结果不回滚）。
- **Anki 卡包**按 Anki schema 11 生成（`collection.anki2` + `media`），已用 SQLite 与 JSON 回读验证，
  但本机没有 Anki 客户端，**未做真实导入验证**；模型固定为 Basic（Front=摘录，Back=批注+标签+出处）。
- **XMind** 输出的是 XMind 8 的 `content.xml` 结构（BookxNote 也是这条路）；XMind 2020+/Zen 更偏好
  `content.json`，可能需要「文件→导入」而不是双击打开。
- 导出是按「当前文档的全部笔记」进行，暂不支持只导出筛选结果或多文档合并导出。
- 手写笔迹：无真实压感（用指针速度模拟粗细）；橡皮按擦除轨迹切断，切点在擦除边缘按采样线段**插值**生成（缺口宽度 ≈ 2×橡皮半径，不会多丢采样点）。
- **手写墨迹不写回 PDF**：MuPDF 1.21 能写出合法的 `/Subtype /Ink` + `/InkList`，但不生成
  外观流（appearance），任何 MuPDF 渲染下都不可见；因此程序**刻意拒绝**把墨迹写进 PDF
  （存档、同步、导出照常，只是 PDF 里没有它）。要真正落地需换 PDFium 之类能生成 ink 外观的引擎。
- **同步（P4）的边界**：图片摘录的 PNG **会同步**（线格式只带文件名，二进制走 `<remote>/images/`）；
  `docKey` 依赖「同名同大小」，换一版 PDF 会被当作另一本书；远端整本一个 JSON（无增量传输）；
  密码明文存在本地库里；同步是手动按钮，没有后台定时；冲突只做到「保留两份 + 标签」，
  **没有可视化合并界面**；EPUB 因各端排版不同，页码提示可能偏移（字符级定位不受影响）。
- EPUB 的标注无法写回原文件（EPUB 是打包 XHTML），只存在库里——与参考产品行为一致。
- 单页模式的"翻页动画"和阅读位置记忆（只记了页码，不记页内偏移）。

## 路线图

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 工程骨架、主题、引擎抽象、测试脚手架 | ✅ |
| P1 | PDF/EPUB 打开、版式/缩放/旋转、文本选择、四类标注、SQLite+JSON、写回 PDF、大纲/笔记面板 | ✅ |
| P2 | 全文搜索（FTS5）、标签、颜色过滤、摘录图片、撤销/重做、阅读位置精细记忆 | ✅ |
| P3 | 导出 MD / HTML / OPML / XMind / Anki `.apkg` | ✅ |
| P4 | WebDAV 同步 + 版本向量合并 + 冲突 UI | ✅ |
| P5 | 脑图（QGraphicsScene）、手写笔迹、TTS 朗读、AI 面板（OpenAI 兼容 + 自带 Key） | ✅ |

## 授权

自用、不分发：MuPDF（AGPLv3）的义务不触发。**若要发布**：换成 PDFium 后端（BSD），
或购买 Artifex 商业授权。Qt6 为 LGPL（动态链接，符合要求）。本项目未复用任何第三方产品的
代码、图标、样式或文案。
