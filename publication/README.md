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
stay outside the published directory. Search runs in the browser with Sphinx's JavaScript assets and index.

The Docs workflow builds the assembled site once and uses its `gwbasic/`
subtree for GitHub Pages. Successful `main` builds deploy Pages and upload
`valetov-static-site`. The publication service checks every fifteen minutes
for a successful Docs push run at the current GitHub `main` commit. It sends
that artifact to the existing Apache host, then compares every public file
with its SHA-256 manifest. It also synchronises `main`, `v*` release tags,
`refs/bugs/*` and `refs/identities/*` from GitHub to SDF. Pushes are atomic and
reject divergent refs. Existing remote refs are retained.

The service uses the owner's existing `gh` login and SSH configuration on the
publishing host. It requires that host and the destinations to be online;
changes also require the coordination host. A persistent systemd user timer
catches up after downtime. Credentials stay on the publishing host. SSH host
keys are pinned in `~/.local/state/gwbasic-publication/known_hosts`; check host
identity before replacing a changed key.

## Installation and operation

Install reviewed copies of `sync_publication.py` and `deploy_site.py` in
`~/.local/lib/gwbasic-publication/`, and the two systemd units in
`~/.config/systemd/user/`. They do not update themselves from the repository.
Configure the existing host aliases and coordination helper in the private
`~/.config/gwbasic-publication.env` file:

```ini
GWBASIC_WEB_HOST=web-host
GWBASIC_LEASE_HOST=coordination-host
GWBASIC_LEASE_SCRIPT=/path/to/resource_lease.py
```

The coordinator runs the shared resource-lease helper with `--resource`,
`--owner` and `--expected-seconds` arguments. Its JSON `state` is `ACQUIRED` on
connection and `HELD` after `renew 300` or `status`; it releases on `release` or
EOF. Publishers take `website/valetov.com`; mirror writers take
`repository/git.sdf.org/evv/gw-basic-2026`. Manual changes use the same leases.
Both the publishing host and web host also take local file locks. The private
bare source cache never reads or changes a developer checkout.

After installing host keys and checking authentication from the user manager,
enable the timer. The user manager needs lingering enabled for unattended runs.

```sh
systemctl --user daemon-reload
systemctl --user enable --now gwbasic-publication.timer
systemctl --user start gwbasic-publication.service
systemctl --user status gwbasic-publication.timer gwbasic-publication.service
journalctl --user -u gwbasic-publication.service -n 50 --no-pager
cat ~/.local/state/gwbasic-publication/status.json
```

Each run records stage results and exits nonzero on failure. A mirror failure
does not suppress a website check. `website.json` retains the last verified
manifest; `last-install.json` records the host receipt before HTTPS verification
and retains its backup path across unchanged retries.
The host records that path with the committed manifest, so a retry also returns
it if the original SSH reply was lost.
For unchanged source, four main pages are checked without downloading a CI
artifact. A matching SDF mirror is checked without taking a transaction lease
or pushing. Pending Docs runs defer publication; a failed run reports failure.
No email or chat notification is sent. If a Docs push run is cancelled, rerun
that push run; a manual workflow dispatch does not qualify for publication.

## Web host setup and recovery

Install `deploy_site.py` as root-owned `/usr/local/libexec/gwbasic-deploy` and
make it executable. The SSH login must be able to run
`sudo -n -u apache /usr/local/libexec/gwbasic-deploy` non-interactively. The
helper runs as the existing `apache` site owner, with no root write access. A root-owned
`/etc/gwbasic-publication.json` selects two separate absolute paths on the same
filesystem: `root` is the existing document root; `state` is a private directory
outside it. Record the installed helper's hash, and review any future helper
update alongside the CI archive validator.

The helper bounds gzip expansion before reading tar metadata. It accepts
regular static files only, rejecting configuration files and executable
suffixes. It preserves favicons, sibling articles and the hashes of the five
historical article downloads. A fresh subsite directory replaces the old one
atomically, removing stale generated pages. Four navigation/article pages are
replaced individually. Files receive readable modes and the existing home
page's SELinux label. Check the exchange operation, modes and labels on the
actual destination filesystem before the first live run.

After checking the existing site's evidence hashes, initialise its baseline:

```sh
sudo -u apache /usr/local/libexec/gwbasic-deploy --initialise
```

This refuses an existing baseline or pending recovery. A failed installation
rolls back and verifies the restored manifest. An interrupted transaction is
restored on the next invocation before a new payload is considered. Private
staging remnants and unfinished backups are then removed. The five most recent
completed deployment backups remain in the configured state directory.

A failed rollback retains `pending.json` and its backup for inspection. Do not
edit managed files while recovery is pending. Power-loss recovery is not
guaranteed; inspect saved state and live files after a host or disk failure.
Out-of-band edits stop publication. After reviewing a deliberate manual update,
archive `current.json` outside the active state path and initialise again under
the website lease. Keep the old record; do not reset a baseline to bypass an
unexplained mismatch.

For manual rollback, stop the timer and take the website lease. Preserve the
current files, restore a completed backup's `gwbasic/` directory and four pages,
verify their hashes against its `baseline.json`, and restore that record as
`current.json`. Keep the timer stopped while retaining an older publication:
the local `website.json` cache otherwise reports HTTPS mismatches at the same
main SHA. To deliberately republish current main, move that local cache aside
under the lease and restart the service; its CI artifact must still be
available. A new successful main build also permits a fresh deployment.

An HTTPS verification failure leaves the newly installed files in place and
reports failure. The timer retries automatically; stop it to hold the site for
inspection. Read `last-install.json` and the service log before manually
retrying or restoring a backup. Stop the timer for planned maintenance with
`systemctl --user stop gwbasic-publication.timer`.

The builder does not modify Apache or deploy files itself. Run
`python3 tests/test_publication.py` for recovery, validation and real-Git mirror
checks, and `python3 publication/check_site_archive.py build/valetov-site` to
validate a real build. The Docs workflow runs both.

The earlier `publication/compiler-jupyter/build_site.py` command forwards to
this builder, so article updates also retain the project subsite. Article source
and evidence regeneration instructions are in [compiler-jupyter](compiler-jupyter/README.md).
