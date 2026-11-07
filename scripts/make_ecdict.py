#!/usr/bin/env python3
"""Build the trimmed ECDICT sqlite that ships in the portable bundle.

Input: ecdict-sqlite-28.zip (full database, ~217 MB download, ~780 MB unpacked).
Output: a ~30 MB ecdict.db with just the columns the app queries:

    stardict(word, phonetic, translation, definition)

Rows are limited to the most frequent English headwords (stardict.frq rank),
which covers essentially everything a reader meets; rare/archaic entries stay
out so the bundle does not bloat. Run with -h for options.
"""
import argparse
import glob
import os
import sqlite3
import sys
import zipfile


def find_database(extracted_dir):
    for pattern in ("*.db", "*.sqlite", "*.sqlite3"):
        hits = glob.glob(os.path.join(extracted_dir, "**", pattern), recursive=True)
        if hits:
            return max(hits, key=os.path.getsize)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zip", help="path to ecdict-sqlite-28.zip")
    parser.add_argument("out", help="output ecdict.db path")
    parser.add_argument("--rank", type=int, default=60000,
                        help="keep headwords with a frequency rank <= this (default 60000)")
    parser.add_argument("--workdir", default=None, help="scratch dir for extraction")
    args = parser.parse_args()

    work = args.workdir or os.path.join(os.path.dirname(os.path.abspath(args.zip)),
                                        "ecdict-extract")
    os.makedirs(work, exist_ok=True)

    print("extracting %s -> %s" % (args.zip, work), flush=True)
    with zipfile.ZipFile(args.zip) as zf:
        zf.extractall(work)

    db = find_database(work)
    if not db:
        print("no sqlite database found inside the archive", file=sys.stderr)
        return 2

    src = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    tables = {r[0] for r in src.execute("select name from sqlite_master where type='table'")}
    if "stardict" not in tables:
        print("database has no stardict table: %s" % sorted(tables), file=sys.stderr)
        return 2

    columns = {r[1] for r in src.execute("pragma table_info(stardict)")}
    if not {"word", "phonetic", "translation", "definition"} <= columns:
        print("stardict table lacks the expected columns: %s" % sorted(columns),
              file=sys.stderr)
        return 2

    if os.path.exists(args.out):
        os.remove(args.out)
    if os.path.exists(args.out + "-journal"):
        os.remove(args.out + "-journal")

    dst = sqlite3.connect(args.out)
    dst.execute("pragma journal_mode=off")
    dst.execute("create table stardict(word text, phonetic text, translation text, "
                "definition text)")

    has_frq = "frq" in columns
    has_collins = "collins" in columns
    has_oxford = "oxford" in columns
    # "Curated" rows: a frequency rank, a Collins star rating or an Oxford tag. These
    # ~44k headwords are the working vocabulary; the other 3.3M rows are inflections,
    # names and archaisms that would bloat the bundle to ~400 MB.
    curated = []
    params = []
    if has_frq:
        curated.append("(frq is not null and frq <> '' and cast(frq as integer) > 0 "
                       "and cast(frq as integer) <= ?)")
        params.append(args.rank)
    if has_collins:
        curated.append("(collins is not null and collins <> '' and cast(collins as integer) > 0)")
    if has_oxford:
        curated.append("(oxford is not null and oxford <> '' and cast(oxford as integer) > 0)")
    if not curated:
        curated.append("1=1")

    select = ("select word, phonetic, translation, definition from stardict "
              "where word is not null and trim(word) <> '' "
              "and (" + " or ".join(curated) + ") "
              "and (translation is not null and trim(translation) <> '' "
              " or definition is not null and trim(definition) <> '')")
    params = tuple(params)

    total = 0
    chunk = []
    for row in src.execute(select, params):
        chunk.append(tuple((v if v is not None else "") for v in row))
        if len(chunk) >= 20000:
            dst.executemany("insert into stardict values(?,?,?,?)", chunk)
            total += len(chunk)
            chunk = []
    if chunk:
        dst.executemany("insert into stardict values(?,?,?,?)", chunk)
        total += len(chunk)

    dst.execute("create unique index if not exists idx_ecdict_word "
                "on stardict(word collate nocase)")
    dst.commit()
    dst.execute("vacuum")
    dst.close()
    src.close()

    size_mb = os.path.getsize(args.out) / 1e6
    print("ecdict.db: %d entries, %.1f MB (rank <= %d)" % (total, size_mb, args.rank), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
