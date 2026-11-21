# Compiler and Jupyter article

Canonical publication: https://retroprogramming.org/articles/gw-basic-2026/

`article.md` is the article's source. The [site builder](../README.md) renders it
with the shared stylesheet and builds the home page, GW-BASIC documentation,
sitemap, robots.txt and downloadable evidence. The existing site's favicons are
preserved separately. The article needs no browser JavaScript; documentation
search uses Sphinx's local scripts and search index.

From the repository root:

```sh
python3 -m pip install -r publication/requirements.txt
python3 publication/build_site.py --output build/retroprogramming-site
```

Use a new or empty output directory. The original article-build command forwards
to the complete site builder. Follow the [publication instructions](../README.md)
to preserve unrelated files when deploying; the builder does not deploy or
modify Apache.

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
