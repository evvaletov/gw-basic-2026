# Publishing valetov.com

The public GW-BASIC subsite is https://valetov.com/gwbasic/. Its documentation
comes from `docs/`, which also builds the GitHub Pages mirror. Both copies use
the valetov.com URLs as their canonical addresses.

From the repository root, in a Python virtual environment:

```sh
python -m pip install -r publication/requirements.txt
python publication/build_site.py --output build/valetov-site
```

Choose a new or empty output directory for each build. The builder runs Sphinx
with warnings treated as errors, then adds the home page, the compiler/Jupyter
article and its downloadable evidence, robots.txt and a sitemap. Build caches
stay outside the published directory. Search uses Sphinx's local JavaScript
assets and index; no database or external search service is needed.

The Docs workflow builds both copies and uploads `valetov-static-site` for
review. Successful `main` builds deploy GitHub Pages. Publishing valetov.com
uses the existing server; the workflow does not deploy there.
Republish the valetov.com build when documentation changes on `main` to keep
the canonical copy current.

Before publishing, back up the files that will be replaced and check for
concurrent updates. Stage the complete generated `gwbasic/` directory before
switching it into place. Copy generated article files individually into
`articles/gw-basic-2026/`, preserving sibling articles. For a documentation-only
update, verify that the five existing article downloads remain byte-for-byte
unchanged. Replace `index.html`, `robots.txt` and `sitemap.xml` individually,
preserving the site's favicons and unrelated content. Publish new pages before
the navigation that links to them, then verify public pages, search and downloads.
The builder does not modify Apache or deploy files itself.

The earlier `publication/compiler-jupyter/build_site.py` command forwards to
this builder, so article updates also retain the project subsite. Article source
and evidence regeneration instructions are in [compiler-jupyter](compiler-jupyter/README.md).
