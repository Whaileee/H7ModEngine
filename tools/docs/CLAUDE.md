# tools/docs/ — docs site generators (Python)

| File | Role |
|---|---|
| `hooks.py` | MkDocs hook: stamps the release tag (`H7_DOCS_REF`) on the site, rewrites links leaving `docs/` to GitHub links at that tag, expands the "Game classes" nav |
| `sdk_pages.py` | Generates `sdk/` pages (one per game class) from `sdk/*.json` at build time |
| `extract_sdk.py` | Builds `sdk/*.json` from the decompiled UnrealScript + the engine's `dumps/objects.txt` |
| `sdk/*.json` | Committed declarations of every class the game loads (no function bodies). Regenerate, don't hand-edit |
| `requirements.txt` | Pinned mkdocs / mkdocs-material versions used by CI |

Regenerate the JSON only when the targeted game version changes (or the extractor changes), then commit it:

```bash
python tools/docs/extract_sdk.py --src N:/UDK/Custom/Development/Src --dump "<Game>/H7ModEngine/dumps/objects.txt"
```

The dump (`DumpSDK=1`) is the ground truth for what exists in game; the `.uc` sources supply declarations.
Release CI builds without the game or sources, so everything the site needs must be in `sdk/*.json`.
Only declarations may go into the JSON — never function bodies (they are Ubisoft's code).
