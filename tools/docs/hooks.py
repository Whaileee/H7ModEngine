"""MkDocs hooks for the H7ModEngine documentation site (see mkdocs.yml).

- Stamps the release the site was built from (H7_DOCS_REF, e.g. "v0.2.0") in the site name and footer.
- Rewrites relative links that leave docs/ (e.g. ``../src/lua/bindings.cpp``) into GitHub links at that ref,
  so the guides can point at source files and the links work both on GitHub and on the site.
"""

import os
import posixpath
import re

REPO = "https://github.com/Whaileee/H7ModEngine"
REF = os.environ.get("H7_DOCS_REF", "").strip()

_LINK = re.compile(r"(\]\()([^)\s#]+)(#[^)\s]*)?(\))")


def on_config(config):
    label = REF or "development build"
    config["site_name"] = f"H7ModEngine {REF}" if REF else "H7ModEngine (dev)"
    config["copyright"] = (
        f"Documentation for H7ModEngine {label}. "
        "Unofficial fan project, not affiliated with or endorsed by Ubisoft."
    )
    return config


def on_page_markdown(markdown, page, config, files):
    src_dir = posixpath.dirname(page.file.src_uri)
    ref = REF or "main"

    def fix(m):
        target = m.group(2)
        if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I) or target.startswith("/"):
            return m.group(0)
        resolved = posixpath.normpath(posixpath.join(src_dir, target))
        if not resolved.startswith("../"):
            return m.group(0)  # inside docs/: MkDocs resolves it
        repo_path = resolved[3:]
        kind = "tree" if repo_path.endswith("/") or "." not in posixpath.basename(repo_path) else "blob"
        return f"{m.group(1)}{REPO}/{kind}/{ref}/{repo_path}{m.group(3) or ''}{m.group(4)}"

    return _LINK.sub(fix, markdown)
