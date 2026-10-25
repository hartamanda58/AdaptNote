#!/usr/bin/env python3
"""Independent cross-check of AdaptNote exports.

Uses only the Python standard library (zipfile / sqlite3 / xml.etree / json), so it
validates the files with parsers that share no code with the C++ writers.
"""
import json
import os
import sqlite3
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET

fails = []
passes = 0


def check(ok, what):
    global passes
    if ok:
        passes += 1
        print("PASS  " + what)
    else:
        fails.append(what)
        print("FAIL  " + what)


def main(outdir):
    md = os.path.join(outdir, "notes.md")
    html = os.path.join(outdir, "notes.html")
    opml = os.path.join(outdir, "notes.opml")
    xmind = os.path.join(outdir, "notes.xmind")
    apkg = os.path.join(outdir, "notes.apkg")

    # ---------------- markdown
    text = open(md, encoding="utf-8").read()
    check(len(text) > 200, "markdown has content (%d bytes)" % len(text))
    check(text.startswith("# "), "markdown starts with a title heading")
    check("## 第" in text, "markdown groups notes by page")
    assets = os.path.join(outdir, "notes.assets")
    check(os.path.isdir(assets) and len(os.listdir(assets)) >= 1,
          "markdown asset folder contains the excerpt image")

    # ---------------- html
    h = open(html, encoding="utf-8").read()
    check("<!DOCTYPE html>" in h, "html has a doctype")
    check("data:image/png;base64," in h, "html inlines the excerpt image")
    check("<script>alert" not in h, "html does not leak raw script tags")

    # ---------------- opml
    root = ET.parse(opml).getroot()
    outlines = root.findall(".//outline")
    check(root.tag == "opml", "opml root element")
    check(len(outlines) >= 2, "opml has %d outlines" % len(outlines))
    check(any(o.get("_note") for o in outlines), "opml carries notes in _note attributes")
    check(any(o.get("page") for o in outlines), "opml keeps the page number")

    # ---------------- xmind
    with zipfile.ZipFile(xmind) as z:
        check(z.testzip() is None, "xmind zip CRCs are valid")
        names = z.namelist()
        check("content.xml" in names and "META-INF/manifest.xml" in names,
              "xmind contains content.xml + manifest (%d entries)" % len(names))
        content = z.read("content.xml").decode("utf-8")
    xroot = ET.fromstring(content)
    ns = "{urn:xmind:xmap:xmlns:content:2.0}"
    check(xroot.tag == ns + "xmap-content", "xmind root is xmap-content")
    topics = xroot.findall(".//" + ns + "topic")
    check(len(topics) >= 3, "xmind has %d topics" % len(topics))
    titles = [t.findtext(ns + "title") or "" for t in topics]
    check(any(titles), "xmind topics have titles")
    check(any("第" in t for t in titles), "xmind has page grouping topics")

    # ---------------- anki apkg
    with zipfile.ZipFile(apkg) as z:
        check(z.testzip() is None, "apkg zip CRCs are valid")
        names = z.namelist()
        check("collection.anki2" in names, "apkg contains collection.anki2")
        check("media" in names, "apkg contains media map")
        media = json.loads(z.read("media").decode("utf-8"))
        check(isinstance(media, dict), "media map is a JSON object (%d entries)" % len(media))
        for idx, fname in media.items():
            check(idx in names, "media entry %s present in archive as %s" % (idx, fname))
        db_bytes = z.read("collection.anki2")

    tmp = tempfile.NamedTemporaryFile(suffix=".anki2", delete=False)
    tmp.write(db_bytes)
    tmp.close()
    try:
        con = sqlite3.connect(tmp.name)
        cur = con.cursor()
        check(cur.execute("pragma integrity_check").fetchone()[0] == "ok",
              "anki sqlite integrity_check ok")
        notes = cur.execute("select count(*) from notes").fetchone()[0]
        cards = cur.execute("select count(*) from cards").fetchone()[0]
        check(notes >= 3 and cards == notes, "anki notes=%d cards=%d" % (notes, cards))
        ver, models, decks = cur.execute("select ver, models, decks from col").fetchone()
        check(ver == 11, "anki collection version = %d" % ver)
        models = json.loads(models)
        decks = json.loads(decks)
        check(len(models) == 1, "anki models json parses (%d model)" % len(models))
        check(len(decks) == 1, "anki decks json parses (%d deck)" % len(decks))
        model = list(models.values())[0]
        check(len(model["flds"]) == 2 and len(model["tmpls"]) == 1,
              "anki model has 2 fields / 1 template")
        rows = cur.execute("select flds, tags, csum, sfld from notes").fetchall()
        check(all("\x1f" in r[0] for r in rows), "anki fields are 0x1f separated")
        check(all(r[1] == "" or (r[1].startswith(" ") and r[1].endswith(" "))
                  for r in rows),
              "anki tag strings are empty or space padded (%d rows)" % len(rows))
        check(any(r[1].strip() for r in rows), "at least one note carries tags")
        con.close()
    finally:
        os.unlink(tmp.name)

    print()
    if fails:
        print("EXPORT VALIDATION FAILED (%d failure(s))" % len(fails))
        return 1
    print("EXPORT VALIDATION PASSED (%d checks)" % passes)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "exports"))
