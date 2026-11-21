#!/usr/bin/env python3
"""Build valetov.com with the article and the shared GW-BASIC documentation."""
import argparse
from html import escape
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from markdown_it import MarkdownIt

HERE = Path(__file__).resolve().parent
ARTICLE = HERE / "compiler-jupyter"
DOCS = HERE.parent / "docs"
ORIGIN = "https://valetov.com"
ARTICLE_PATH = "/articles/gw-basic-2026/"
URL = ORIGIN + ARTICLE_PATH
DESCRIPTION = "Follow GW-BASIC 2026 from a BASIC listing through generated C, then from a persistent interpreter through Sixel to inline Jupyter graphics."
DATE = "2026-11-20"
DATE_TEXT = "20 November 2026"
FILES = ("examples.ipynb", "graphics.png", "squares-output.txt", "squares.bas.c", "verification.json")


def page(title, body, canonical, *, article=False):
    structured = ""
    if article:
        data = {"@context": "https://schema.org", "@type": "TechArticle",
                "headline": title, "description": DESCRIPTION, "url": URL,
                "datePublished": DATE, "image": URL + "graphics.png",
                "author": {"@type": "Person", "name": "Eremey Valetov", "url": ORIGIN + "/"}}
        structured = '<script type="application/ld+json">' + json.dumps(data).replace("<", "\\u003c") + '</script>'
    description = DESCRIPTION if article else "Software notes and experiments by Eremey Valetov."
    return f'''<!doctype html>
<html lang="en-GB">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{escape(title)} | Eremey Valetov</title>
<meta name="description" content="{escape(description, quote=True)}">
<meta name="author" content="Eremey Valetov">
<link rel="canonical" href="{canonical}">
<link rel="icon" href="/favicon.svg" type="image/svg+xml">
<meta property="og:type" content="{'article' if article else 'website'}">
<meta property="og:title" content="{escape(title, quote=True)}">
<meta property="og:description" content="{escape(description, quote=True)}">
<meta property="og:url" content="{canonical}">
<meta property="og:image" content="{URL}graphics.png">
<style>{(ARTICLE / 'style.css').read_text(encoding="utf-8")}</style>
{structured}
</head>
<body>
<a class="skip" href="#content">Skip to content</a>
<header><a href="/">Eremey Valetov</a><span><a href="/gwbasic/">GW-BASIC 2026</a></span></header>
<main id="content">{body}</main>
<footer><a href="https://github.com/evvaletov">GitHub</a> &middot; <a href="/">valetov.com</a></footer>
</body>
</html>
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.output.exists() and (not args.output.is_dir() or any(args.output.iterdir())):
        parser.error("Choose a new or empty output directory to avoid retaining stale pages")
    with tempfile.TemporaryDirectory(prefix="gwbasic-doctrees-") as doctrees:
        subprocess.run([sys.executable, "-m", "sphinx", "-W", "--keep-going", "-n",
                        "-d", doctrees, "-b", "html", str(DOCS),
                        str(args.output / "gwbasic")], check=True)
    heading, body = (ARTICLE / "article.md").read_text(encoding="utf-8").split("\n", 1)
    if not heading.startswith("# "):
        parser.error("article.md must begin with a level-one title")
    title = heading[2:]
    rendered = MarkdownIt("commonmark").render(body)
    body_html = f'<article><h1>{escape(title)}</h1><p class="meta">Eremey Valetov &middot; <time datetime="{DATE}">{DATE_TEXT}</time></p>{rendered}</article>'
    article_dir = args.output / ARTICLE_PATH.strip("/")
    article_dir.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        shutil.copyfile(ARTICLE / name, article_dir / name)
    (article_dir / "index.html").write_text(page(title, body_html, URL, article=True), encoding="utf-8")
    home = f'''<h1>Eremey Valetov</h1><p>Software notes and experiments.</p>
<section class="entry"><h2><a href="/gwbasic/">GW-BASIC 2026</a></h2>
<p>A C11 reimplementation of GW-BASIC with a native compiler, DOS builds and a Jupyter kernel.</p>
<p><a href="/gwbasic/install.html">Install and download</a> &middot;
<a href="/gwbasic/demos.html">Try the demos</a> &middot;
<a href="/gwbasic/language-reference.html">Language reference</a></p></section>
<section class="entry"><h2><a href="{ARTICLE_PATH}">{escape(title)}</a></h2>
<p class="meta"><time datetime="{DATE}">{DATE_TEXT}</time></p>
<p>{escape(DESCRIPTION)}</p></section>'''
    (args.output / "index.html").write_text(page("Software notes", home, ORIGIN + "/"), encoding="utf-8")
    urls = [ORIGIN + "/", URL] + [
        ORIGIN + "/gwbasic/" + path.relative_to(args.output / "gwbasic").as_posix()
        for path in sorted((args.output / "gwbasic").rglob("*.html"))
        if path.name not in ("search.html", "genindex.html")]
    (args.output / "sitemap.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">'
        + "".join(f"<url><loc>{escape(url)}</loc></url>" for url in urls)
        + '</urlset>\n', encoding="utf-8")
    (args.output / "robots.txt").write_text(f"User-agent: *\nAllow: /\nSitemap: {ORIGIN}/sitemap.xml\n", encoding="utf-8")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
