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

Before publishing, back up the files that will be replaced and check for
concurrent updates. Copy the generated `gwbasic/`, `articles/`, `index.html`,
`robots.txt` and `sitemap.xml` to the existing document root, preserving the
site's favicons and any unrelated content. Stage complete directories before
switching them into place, and verify public pages, search and downloads after
publication. The builder does not modify Apache or deploy files itself.

The earlier `publication/compiler-jupyter/build_site.py` command forwards to
this builder, so article updates also retain the project subsite. Article source
and evidence regeneration instructions are in [compiler-jupyter](compiler-jupyter/README.md).
