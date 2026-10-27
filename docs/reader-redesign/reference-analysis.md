# ReaderNote v0.5.1 逆向分析（阶段 A 证据）

对象：`ReaderNote.Setup-v0.5.1-x64.exe`
SHA-256：`145617b60d5659ec3340ff70c7af13b71565d3dfc8fe68fa2846a56f059d154a`
大小：193,058,456 字节（约 184 MiB）
PE 版本资源：`ProductName=ReaderNote`、`FileVersion=0.5.1`、`CompanyName=Tianjin GauzyTech`

分析方式：离线静态解包（未运行安装器），7-Zip + 自写 ASAR 提取器。

> 结论分级：**【观察】** 为文件中直接可见；**【推断】** 为基于结构的合理推论；**【未确认】** 需要运行或进一步分析。

---

## 1. 打包结构

```
installer.exe                     NSIS 3 Unicode 安装器（PE32 i386，2025 构建）
└── $PLUGINSDIR/app-64.7z         192 MB 主体负载
    ├── ReaderNote.exe            157 MB   Electron 主程序
    ├── resources/app.asar        179 MB   Electron 应用资源
    ├── resources/app.asar.unpacked/…     原生模块
    ├── resources/ecdict.db       121 MB   离线英汉词典
    ├── d3dcompiler_47.dll / libEGL.dll / libGLESv2.dll / ffmpeg.dll
    ├── snapshot_blob.bin / v8_context_snapshot.bin / icudtl.dat
    └── resources/samples/…       article.pdf demo.epub demo.docx picture.png
```

**【观察】** 主体是 Electron 应用（Chromium 运行时文件齐全），不是原生 Qt。

## 2. 技术栈（来自 `app.asar` 的 `package.json` 与目录）

| 领域 | 证据 | 结论 |
|---|---|---|
| 应用框架 | `ReaderNote.exe` + `app.asar` + `snapshot_blob.bin` | Electron |
| 界面框架 | `umi.js`、`assets/@antd.js`、`pages/` | React + UmiJS + Ant Design |
| 富文本 | `lexical/lexical-editor.umd.js`（19.8 MB） | Lexical |
| Markdown | `vditor/`（含 mermaid、graphviz、MathJax、lute） | Vditor |
| 手绘/白板 | `excalidraw/` | Excalidraw |
| 公式 | `katex/` | KaTeX |
| **PDF 渲染** | `pdfjs/`（`pdf.worker.min.js`、`cmaps/`、`fonts/`）、`assets/engines.js` 内 `OutputScale` 以 `window.devicePixelRatio` 初始化 | **PDF.js** |
| 本地数据库 | `sqlite3 5.1.6`、`sequelize 6.19`、`umzug`、`sql.js` | SQLite + Sequelize 迁移 |
| 本地服务 | `koa`、`@koa/cors`、`koa-static`、`koa-range`、`detect-port`、`bonjour` | 进程内 HTTP 服务 |
| OCR | `onnxruntime-node 1.23`、`ppu-paddle-ocr`、`windows-media-ocr`、`DirectML.dll`、`dxcompiler.dll` | 双 OCR（Paddle-ONNX / Windows Media OCR） |
| 词典 | `js-mdict`、`resources/ecdict.db` | MDict + 离线词库 |
| 系统集成 | `node-powershell`、`node-wmic`、`systeminformation`、`winax`、`node-machine-id` | Windows 设备/许可相关 |
| 同步后端 | `pg`、`mysql2`、`mariadb`、`nedb` 类依赖 | 远端同步库适配 |

## 3. 主进程被 V8 字节码保护

```
dist/main/main.js                      17 KB  webpack 外壳（require 各 .jsc）
dist/main/main.compiled.jsc           1.2 MB  ← V8 字节码（bytenode）
dist/main/ocr.worker.compiled.jsc    10.7 MB
dist/main/syncer.worker.compiled.jsc  2.0 MB
dist/main/converter.worker.compiled.jsc
```

**【观察】** 核心业务逻辑编译为 V8 字节码，不是可读 JS。
**【结论】** 对主进程做纯静态语义还原成本高；可行路线是：preload 的 IPC 面 + 数据库迁移脚本 + 渲染层 bundle + 运行时行为观察。

## 4. 数据模型（`resources/database/migrations/*.js`，24 个）

迁移文件名本身已给出实体与演进顺序：

```
20220609 create-user / create-sync / create-storage
20240719 modify-note / modify-user
20250121 modify-sync / 20250127 create-document / 20250302 create-stat
20250303 create-resource-state / 20260410 create-resource-folder
```

**【推断】** 存在 user、sync、storage、note、document、stat、resource-state、resource-folder 等实体，且"笔记"在 2024-07 被改造过（很可能就是引入主题式卡片）。
**待办**：读取每个迁移的 `up()` 定义，落成字段级 ER 图。

## 5. IPC 接口面（`dist/main/preload.js`，混淆但命名空间完整）

```
app  win  db  assistant  vfs  dictionary  cache  license  setting  updater
repo  syncer  sysClipboard  llm  editor  ocr  converter  browser  backup
agentapi  textcache
```

值得注意的：

- `vfs`：`import / save / open / pickFile / pickFolder / writeTemp / saveBlob`
- **`textcache`**：`get / set / manifest / clear` → 文本抽取结果有独立缓存层
- `ocr` / `converter`：各自 worker
- `syncer`：`setProgress / start / pause / resume / reload / invoke`
- `backup`：`list / create / restore / delete`
- `browser`：`open / navigateTo / ...` → 内嵌浏览器（翻译/AI 面板）
- `dictionary`：`search / searchAll`
- `license`：`activate / deactivate / check`
- `llm`：`getConfig / summarize / translate / getPurchaseUrl` → AI 走自有额度

## 6. 对你三个问题的直接回答

### 6.1 PDF 清晰度

**【观察】** `assets/engines.js` 内 PDF.js 的 `OutputScale` 实现：

```js
class Lr{constructor(){var t=window.devicePixelRatio||1;this.sx=t;this.sy=t} …}
```

且文本层使用 `scale: n.scale * (globalThis.devicePixelRatio||1)`。

**【结论】** ReaderNote 的 PDF 是 canvas 渲染，并且**按设备像素比出图**。
这就是"同一个 PDF 它清晰、我们发虚"的技术差异点，而不是 MuPDF 本身不行。

### 6.2 UI

**【观察】** React + AntD + UmiJS，页面级路由（`pages/`），面板可拖拽（截图为三栏：主题/脑图 | 缩略图 | 正文）。
**【推断】** 布局用 React 组件 + 可调分栏实现，主题化依赖 AntD 设计令牌。

### 6.3 脑图

**【观察】** 渲染层含 `excalidraw`（白板）与自定义 `board-renders.js`（6.75 MB）。
**【未确认】** 脑图是否为独立引擎（如 simple-mind-map / mind-elixir）或自研；需在 `board-renders.js` 中定位节点/连线/布局实现。
**【观察-截图】** 卡片分三区：标题、原文摘录、自己的笔记，右下角有定位原文入口。

## 7. 与 AdaptNote 的关键对照结论

| 维度 | ReaderNote | AdaptNote（当前） | 行动 |
|---|---|---|---|
| PDF 渲染 | PDF.js canvas，按 DPR 出图 | MuPDF 出图，**未乘 DPR** | 立即修（已实施） |
| 文本缓存 | 独立 `textcache` 层 | 内存 TextPage 缓存 | 后续评估持久化 |
| 编辑器 | Lexical + Vditor | 自研 QTextEdit | 先不换，聚焦交互 |
| 手绘 | Excalidraw | 自研 ink | 保留自研 |
| 笔记模型 | 迁移中演进（user/sync/note/document/resource-folder） | Markup + mind_groups | 保持，按需扩展 |

## 8. 局限与未做

- 未运行安装器，未观察真实交互；所有 UI 行为结论来自截图与资源。
- 未反编译 `.jsc`，主进程逻辑（同步、许可、OCR 流程）未还原。
- 未读取 24 个迁移的字段定义（下一步）。
- 未验证其 OCR 实际精度、性能与离线程度。
- 未验证许可/激活机制，也不对此做绕过。
