# PDF 清晰度：根因与修复（阶段 C-1）

## 1. 现象

在 Windows 上，同一份矢量化 PDF 与成熟阅读器相比文字发虚、边缘有毛刺，放大后尤其明显。

## 2. 取证过程

### 2.1 参考产品

`assets/engines.js` 中 PDF.js 的 `OutputScale`：

```js
class Lr{constructor(){var t=window.devicePixelRatio||1;this.sx=t;this.sy=t}
```

文本层参数：`scale: n.scale * (globalThis.devicePixelRatio||1)`。

→ 参考产品**按设备像素比出图**。

### 2.2 本项目

```text
$ grep -rn "devicePixelRatio" src/
（无结果）
```

`src/render/mupdf_engine.cpp`：

```cpp
fz_matrix ctm = fz_scale(scale, scale);          // 只有逻辑缩放
pix = fz_new_pixmap_from_page(ctx_, pg, ctm, …); // 出图尺寸 = 页面点 × scale
```

`src/ui/pageview.cpp`：

```cpp
const QImage img = engine_->renderPage(page, scale_, rotation_);
…
p.drawImage(g.rect, img);   // 把 1x 位图拉伸到逻辑矩形
```

**结论**：Windows 显示缩放 150% 时，逻辑 600 px 宽的页面只渲染了 600 px 位图，却被绘制到 900 物理像素宽的区域 → 放大约 1.5×，必然模糊。这与 MuPDF 的渲染质量无关。

## 3. 修复

| 位置 | 改动 |
|---|---|
| `PageView::viewDpr()` | 新增：取 `viewport()->devicePixelRatioF()`，钳制到 `[1.0, 4.0]`；支持测试用覆盖值 |
| `PageView::imageFor()` | 渲染倍率改为 `scale_ * dpr`；缓存键加入 DPR 维度 |
| 大页面保护 | 目标像素超过 4000 万时按比例降采样，避免超大内存分配 |
| `PageView::paintEvent()` | 开启 `SmoothPixmapTransform`（下采样质量）；检测 DPR 变化时清空缓存 |
| `renderPageToImage()` | 导出路径保持 1× 语义不变，避免导出图片尺寸被动改变 |

### 缓存键

```text
page(32) | scaleBucket(16) | dprBucket(14) | rotation/90(2)
```

DPR 变化后旧图不会被误用；同时 `lastDpr_` 变化会主动清缓存。

## 4. 代价与边界

- 内存：2× DPR → 单页位图约 4× 字节。缓存仍受 96 MB 预算与 64 条上限约束。
- 渲染耗时：单页渲染成本随像素数上升。当前只渲染可见页，属于可接受范围。
- 极端情况：400% 缩放的 A0 图纸会被 4000 万像素上限降采样，属于刻意的保护。

## 5. 验收

### 自动化

`visualtest` 新增断言：

- `dpr=1 / 2 / 3` 时出图宽度比例分别约为 1 / 2 / 3（容差 6%）；
- 2× 位图宽度不小于逻辑页面宽度的 1.9 倍。

### 人工（必须做）

在 Windows 100% / 125% / 150% / 200% 缩放下对照：

- 同一 PDF、同一缩放倍率；
- 看正文小字号笔画、公式上下标、图表细线；
- 与修复前截图对比。

### 尚未覆盖

- 跨屏拖动（不同 DPI）后的重绘：已清缓存，但需要真机确认；
- 滚动过程中的预览/精绘调度：尚未实现，属后续性能项。
