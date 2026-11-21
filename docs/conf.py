project = "GW-BASIC 2026"
author = "Eremey Valetov"
copyright = "2026, Eremey Valetov"

extensions = ["myst_parser"]
source_suffix = {".md": "markdown"}
myst_heading_anchors = 3
exclude_patterns = ["fortran-debug-media-handoff.md"]

html_theme = "furo"
html_title = "GW-BASIC 2026"
html_baseurl = "https://valetov.com/gwbasic/"

html_theme_options = {
    "source_repository": "https://github.com/evvaletov/gw-basic-2026",
    "source_branch": "main",
    "source_directory": "docs/",
}
