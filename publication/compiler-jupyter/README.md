# Compiler and Jupyter article

Canonical publication: https://valetov.com/articles/gw-basic-2026/

`article.md` is the article's source. `build_site.py` renders it with the shared
stylesheet and produces a small home page, sitemap, robots.txt and downloadable evidence.
The existing site's favicons are preserved separately. No browser JavaScript,
database, external fonts or build service is required.

From the repository root:

```sh
python3 -m pip install -r publication/compiler-jupyter/requirements.txt
python3 publication/compiler-jupyter/build_site.py --output build/article-site
```

The generated directory is a static site rooted at `build/article-site`. Copy
its contents to the existing document root to publish. Back up any files it
will replace first; the build script does not deploy or modify Apache.

To regenerate the evidence, prepare a separate clean checkout of v0.18.2 at
`12131e52fff5b7bc1afbbc50e90dd2ac3bb79ec7`, build it with the article's CMake
commands, and install `gwbasickernel==0.1.0`, `jupyterlab` and `nbclient` in a
virtual environment. Run `gwbasickernel-install --sys-prefix` in that environment,
then use its Python executable:

```sh
python publication/compiler-jupyter/reproduce.py \
  --checkout /path/to/v0.18.2-checkout \
  --output /path/to/new-evidence
```

The runner compares interpreter/native stdout with each other and an independent
squares-table oracle. It executes two state-sharing notebook cells, the graphics
listing and a final text cell, then records PNG dimensions, versions and hashes.
Review regenerated files before replacing the checked-in evidence. Package
dependencies other than the kernel are recorded, not locked transitively.

The recorded example run is Linux x86-64. It is not a new release, a benchmark
or a claim of full compatibility. Source links in the article are pinned to the
release, while general project and documentation links follow ongoing work.
