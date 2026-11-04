"""MkDocs hooks for the H7ModEngine documentation site (see mkdocs.yml).

- Stamps the release the site was built from (H7_DOCS_REF, e.g. "v0.2.0") in the site name and footer.
- Rewrites relative links that leave docs/ (e.g. ``../src/lua/bindings.cpp``) into GitHub links at that ref,
  so the guides can point at source files and the links work both on GitHub and on the site.
- Generates the "Game classes" section (sdk/...) from tools/docs/sdk/*.json, see sdk_pages.py. The nav entry
  ``Game classes: sdk/index.md`` in mkdocs.yml is expanded into the overview and one page per package; the
  ~3000 class pages are reachable from those and from search, but not listed in the sidebar.
"""

import importlib.util
import os
import posixpath
import re

from mkdocs.structure.files import File

_spec = importlib.util.spec_from_file_location(
    "h7_sdk_pages", os.path.join(os.path.dirname(os.path.abspath(__file__)), "sdk_pages.py"))
sdk_pages = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sdk_pages)
_sdk = None

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
    global _sdk
    _sdk = sdk_pages.Sdk()
    if _sdk:
        config["nav"] = [_expand_sdk_nav(item) for item in config["nav"]]
    return config


def _expand_sdk_nav(item):
    if isinstance(item, dict):
        (title, value), = item.items()
        if value == "sdk/index.md":
            return {title: sdk_pages.nav_section(_sdk)}
        if isinstance(value, list):
            return {title: [_expand_sdk_nav(i) for i in value]}
    return item


def on_files(files, config):
    if _sdk:
        for src_uri, content in sdk_pages.pages(_sdk):
            files.append(File.generated(config, src_uri, content=content))
    return files


def on_page_markdown(markdown, page, config, files):
    if page.file.src_uri.startswith("sdk/"):
        return markdown
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
