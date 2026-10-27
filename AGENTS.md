# Project guidance

The project's name is Octavo (chosen over Skrimshander; both from Moby-Dick). The executable (`build\octavo.exe`) and the Notion page name are already Octavo; the folder and a few internal strings (window title, window class names) still say TextEditor until the user asks for the rename.

This is an incremental C++/Win32 learning project. Implement one agreed capability at a time and explain input, state, and drawing at a practical conceptual level. Do not jump ahead to a full editor.

Docs and interactive verification are batched to save compute: update STEPS.md, docs/STATUS.md, README.md, and docs/ARCHITECTURE.md, and launch and exercise the running app, every 5 steps (last done at Step 33 for the Octavo v0.1 milestone; next at Step 35, together with the Notion update), or sooner when the user says so. Between batches, build and run `build.bat check` but leave docs and interactive checks alone; note the pending steps in the final message. At a batch, STEPS.md gets one short entry per milestone (the shareable build sequence); detailed state and verification go in docs/STATUS.md.

Read README.md and docs/ARCHITECTURE.md; use docs/STATUS.md for current scope and evidence. Build with `build.bat`; close the running executable before rebuilding. Keep drawing and mouse hit testing on the same layout calculation.

When verification is requested, `build.bat check` runs focused native regressions in `tests/editor_checks.cpp`. Hidden-window checks are behavioral evidence, not visual verification.

Preserve the existing layout; do not migrate it solely to satisfy newer workspace conventions. No parallel Python implementation yet. New documents default to `.lemur` (plain UTF-8); “Save as hylic” is the ordinary-text Save As action and adopts its destination on success. Keep the deeper mythos implicit. docs/VISION.md holds the iceberg rule and the approved hint sites; read it before adding any personal or quirky touch, and never explain the mythos in README or user-facing text.
Maintain factual documentation when behavior or architecture materially changes. Distinguish compilation from interactive verification. Update TextEditor Notion every 10 milestones unless the user requests otherwise; last update Step 22, next routine update Step 35 (delayed by the user to sync with the docs batch). Preserve child pages. Skip screenshots when requested.
