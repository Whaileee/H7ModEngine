# docs/ — documentation site source

MkDocs Material site (`mkdocs.yml` at the repo root, `docs_dir: docs`), published to
https://whaileee.github.io/H7ModEngine/ **from release tags only**. PRs touching docs build with `--strict`.
This file is excluded from the site (`exclude_docs` in `mkdocs.yml`).

| File | Audience |
|---|---|
| `index.md` | Site home |
| `API.md` | Modders: guide + summary reference tables |
| `reference/*.md` | Modders: full API reference (`h7`, objects, types, mod/manifest/settings, events) |
| `DEVELOPMENT.md` | Engine developers |
| `assets/sdk.css` | Styling for the generated "Game classes" pages |
| `sdk/` | Does not exist in git: generated at build time by `tools/docs/` |

## Rules

- API changes update **both** `API.md` and the matching `reference/*.md` page in the same change.
- The first ```lua block under each heading of `reference/{h7,mod,objects}.md` is the **typed signature**
  (`h7.first(class: string | Class) → Object?`; `name?:` = optional, `→` then results). The debug console's
  suggestions are generated from these lines: after changing one, run `python tools/docs/console_api.py`
  (CI checks it).
- Describe behaviour that was verified in game; mark anything unverified.
- New pages must be added to `nav` in `mkdocs.yml`. Links and anchors must resolve (strict build).
- Links that leave `docs/` (e.g. `../src/lua/bindings.cpp`) are fine: `tools/docs/hooks.py` turns them into
  GitHub links at the release tag.
- Preview: `pip install -r tools/docs/requirements.txt` then `mkdocs serve` (or `mkdocs build --strict` to check).
