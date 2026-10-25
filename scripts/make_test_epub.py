#!/usr/bin/env python3
"""Builds the sample EPUB used by the tests and the portable bundle.

    python scripts/make_test_epub.py build/test.epub

Kept as a standalone script so the Windows CI (no bash) can generate the same fixture
that scripts/make_testdata.sh produces on Linux.
"""
import sys
import zipfile

CH1 = """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml"><head><meta charset="utf-8"/>
<title>第一章 阅读的姿势</title></head><body>
<h1>第一章 阅读的姿势</h1>
<p>阅读的本质不是把书看完，而是把书里的东西变成自己的。所以我们把工具的重点放在标注、
摘录、复述和连接上，而不是放在翻页动画上。</p>
<h2 id="s1">1.1 标注即思考</h2>
<p>每一条高亮都应该能回答一个问题：我当时为什么觉得这句重要？如果回答不出来，
这条高亮就只是装饰。</p>
<p>The same idea in English: a highlight you cannot explain is decoration, not knowledge.</p>
<h2 id="s2">1.2 结构与回顾</h2>
<p>把标注按章节收拢，再按自己的问题重新排序，才算真正读完一本书。</p>
</body></html>"""

CH2 = """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml"><head><meta charset="utf-8"/>
<title>第二章 笔记的最小结构</title></head><body>
<h1>第二章 笔记的最小结构</h1>
<p>最小可用的笔记结构只有三样：原文摘录、自己的批注、以及它所属的位置。
其余的标签、颜色、脑图，都是在这三样之上的投影。</p>
<h2>2.1 位置是索引</h2>
<p>位置让笔记可以被跳回，因此位置必须与视图解耦：页码 + 页面坐标，而不是像素坐标。</p>
<h2>2.2 版本与同步</h2>
<p>同步的难点从来不是传输，而是两处都改过同一段内容时，谁说了算。</p>
</body></html>"""

NAV = """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><meta charset="utf-8"/><title>目录</title></head><body>
<nav epub:type="toc" id="toc"><h1>目录</h1><ol>
<li><a href="ch1.xhtml">第一章 阅读的姿势</a>
  <ol>
   <li><a href="ch1.xhtml#s1">1.1 标注即思考</a></li>
   <li><a href="ch1.xhtml#s2">1.2 结构与回顾</a></li>
  </ol>
</li>
<li><a href="ch2.xhtml">第二章 笔记的最小结构</a></li>
</ol></nav></body></html>"""

NCX = """<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
 <head><meta name="dtb:uid" content="urn:uuid:adaptnote-test-0001"/></head>
 <docTitle><text>AdaptNote 测试书</text></docTitle>
 <navMap>
  <navPoint id="n1" playOrder="1"><navLabel><text>第一章 阅读的姿势</text></navLabel><content src="ch1.xhtml"/>
   <navPoint id="n1a" playOrder="2"><navLabel><text>1.1 标注即思考</text></navLabel><content src="ch1.xhtml#s1"/></navPoint>
   <navPoint id="n1b" playOrder="3"><navLabel><text>1.2 结构与回顾</text></navLabel><content src="ch1.xhtml#s2"/></navPoint>
  </navPoint>
  <navPoint id="n2" playOrder="4"><navLabel><text>第二章 笔记的最小结构</text></navLabel><content src="ch2.xhtml"/></navPoint>
 </navMap>
</ncx>"""

OPF = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="bookid">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:identifier id="bookid">urn:uuid:adaptnote-test-0001</dc:identifier>
  <dc:title>AdaptNote 测试书</dc:title>
  <dc:language>zh-CN</dc:language>
  <dc:creator>AdaptNote</dc:creator>
  <meta property="dcterms:modified">2025-01-01T00:00:00Z</meta>
 </metadata>
 <manifest>
  <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
  <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
  <item id="ch2" href="ch2.xhtml" media-type="application/xhtml+xml"/>
  <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
 </manifest>
 <spine toc="ncx">
  <itemref idref="ch1"/>
  <itemref idref="ch2"/>
 </spine>
</package>"""

CONTAINER = """<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
 <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>"""


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test.epub"
    with zipfile.ZipFile(out, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", OPF, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/nav.xhtml", NAV, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", NCX, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/ch1.xhtml", CH1, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/ch2.xhtml", CH2, zipfile.ZIP_DEFLATED)
    print("wrote", out)


if __name__ == "__main__":
    main()
