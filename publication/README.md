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

The Docs workflow builds both copies and uploads `valetov-static-site`.
Successful `main` builds deploy GitHub Pages. The publication service on
asteroid01 checks every fifteen minutes for a successful Docs push run at the
current GitHub `main` commit. It downloads that run's static artifact and sends
it to the existing valetov.com Apache host, then compares every public file
with the artifact's SHA-256 manifest. It also synchronises `main`, `v*` release
tags, `refs/bugs/*` and `refs/identities/*` from GitHub to SDF. It never pushes
developer branches, forces divergent refs or deletes remote refs.

The service uses the owner's existing `gh` login and SSH configuration on
asteroid01. It requires that host, the sf01 coordination authority and the
destination hosts to be online. A persistent systemd user timer catches up
after downtime. No account credentials are copied to GitHub Actions. SSH host
keys are pinned in `~/.local/state/gwbasic-publication/known_hosts`; a changed
key stops the operation until the host identity has been checked.

## Operating the service

The installed scripts are reviewed copies in `~/.local/lib/gwbasic-publication/`;
they do not update themselves from the repository. The systemd units are
`gwbasic-publication.service` and `gwbasic-publication.timer`. Check or run them:

```sh
systemctl --user status gwbasic-publication.timer gwbasic-publication.service
systemctl --user start gwbasic-publication.service
journalctl --user -u gwbasic-publication.service -n 50 --no-pager
cat ~/.local/state/gwbasic-publication/status.json
```

Each run writes its stage results and failures to `status.json` and exits
nonzero on failure. A mirror failure does not suppress a website check.
The last verified website manifest remains in `website.json`; unchanged source
has four main pages checked over HTTPS without requiring an unexpired CI artifact. Pending Docs
runs defer publication; a failed Docs run is reported as a failure. No email
or chat notification is sent. Stop the timer for planned maintenance:

```sh
systemctl --user stop gwbasic-publication.timer
```

`sync_publication.py` takes short central resource leases on sf01 for
`website/valetov.com` and `repository/git.sdf.org/evv/gw-basic-2026`. Manual
publishers must take the same lease. Both asteroid01 and the web host also take local file locks.
The private bare source cache never reads or changes a developer checkout.

## Host installation and recovery

The server helper `deploy_site.py` is installed as root-owned
`/usr/local/libexec/gwbasic-deploy` on `zurichaws`, and runs as the existing
`apache` owner of the site through `sudo -u apache`. It needs no root write
access. It reads a bounded gzip tar from stdin and accepts only regular static
files under the managed paths; configuration files and executable suffixes
are rejected. Its state
and five most recent backups are in `/data/var/www/gwbasic-publication/`,
outside the document root. Installation records the reviewed existing site
once with `sudo -u apache /usr/local/libexec/gwbasic-deploy --initialise`. This refuses
to overwrite an existing baseline. The caller uploads static files only;
updating the installed helper or timer requires a separate reviewed install.

Deployment checks the saved live manifest before making changes, exchanges the
whole `gwbasic/` directory atomically, then replaces the article page and root
navigation files. It applies the home page's SELinux label and sets readable
file modes.
Favicons and sibling articles are outside its write set. The five historical
article downloads must retain their existing hashes. Stale generated subsite
files disappear with the directory exchange.

A failed installation rolls back from the backup and checks the restored
manifest. If the process is interrupted, `pending.json` identifies the backup;
the next invocation restores that transaction before considering a new one.
Do not edit the managed paths while recovery is pending. A failed rollback
keeps the journal and backup and requires inspection. Power-loss recovery is
not guaranteed; inspect the saved state and live files after a host or disk
failure. Out-of-band edits to
managed files stop automatic publication; reconcile the live files and saved
baseline under the central lease before resuming. After reviewing a deliberate
manual update, archive `current.json` outside the active state path, then run
`--initialise` as apache to record the approved files. Keep the archived record;
do not reset a baseline merely to bypass an unexplained mismatch.

For a manual rollback to an earlier successful publication, stop the timer,
take the website lease, preserve the current managed files, and restore the
chosen backup's `gwbasic/` directory and four pages. Verify their hashes against
its `baseline.json`, then restore that record as `current.json` under the same
lease. Keep the timer stopped while retaining an older publication: the local
`website.json` cache otherwise reports HTTPS mismatches for the same main SHA.
To deliberately republish the current build, move that local cache aside under
the lease and restart the service; its CI artifact must still be available. A
new successful main build also permits a fresh deployment.

An HTTPS verification failure is reported after installation and leaves the
new files in place for inspection. It does not automatically roll them back.
Inspect the service log and host receipt before retrying or restoring a backup.

The builder does not modify Apache or deploy files itself. Run the deployment
recovery, archive-validation and real-Git mirror tests locally with
`python3 tests/test_publication.py`; the Docs workflow runs the same checks.

The earlier `publication/compiler-jupyter/build_site.py` command forwards to
this builder, so article updates also retain the project subsite. Article source
and evidence regeneration instructions are in [compiler-jupyter](compiler-jupyter/README.md).
