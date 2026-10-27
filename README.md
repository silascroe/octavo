# Octavo

Octavo (the working directory is still named `TextEditor`; the executable is `octavo.exe`) is a C++20 Windows text editor built one capability at a time to learn how input, stored text, and drawing connect. Uses a custom Win32 window and GDI rather than a Windows EDIT control.

## Build and run

Install Visual Studio Build Tools with Desktop development with C++. From this directory in PowerShell:

```powershell
.\build.bat
.\build\octavo.exe
```

Close the editor before rebuilding so the linker can replace its executable.

Run the focused native regression checks with `.\build.bat check`. They use a hidden window and exercise the real editor helpers and message handlers; they do not verify visual appearance.

## Current milestone: Octavo v0.1 (Step 33)

Octavo is a complete small text editor: it wraps long lines at the window edge (View > Word Wrap, Alt+Z, on by default; turn it off to scroll long lines sideways), opens and saves UTF-8 files, undoes and redoes, searches and replaces, and handles selection, clipboard, tabs, and Unicode. `build\octavo-v0.1.exe` is a copy of the v0.1 build.

Home/End go to the start/end of the visual row. The Edit menu offers Undo, Redo, Cut, Copy, Paste, Select All, Find, Find Next/Previous, and Replace, using the same code as their keyboard shortcuts; items gray out when they would do nothing. Ctrl+Left/Right move by word (Shift extends the selection), and Ctrl+Backspace/Delete delete by word. Tabs advance to 4-space stops, are kept when pasted, and work with the mouse. Page Up/Down moves a screenful (Shift extends). Dragging a selection past the window edge keeps scrolling while the button is held. The footer shows the caret as `Ln 2, Col 8` plus the selected character count. The Find dialog's Match case box is checked by default; unchecking it ignores case in Find, Replace, and Replace All.

The File menu exposes New, Open, Save, Save as hylic, and Exit, using the same operations as the keyboard shortcuts. New documents first save as `.lemur`, which is ordinary UTF-8 text. Save keeps an opened/saved file's existing path and extension. Save as hylic (Ctrl+Shift+S) offers `.txt`, `.md`, `.log`, and All files; these are filename choices, not document conversion or Markdown rendering. Its successful destination becomes the current path; the previous file remains intact when choosing a different path. Open lists Lemur and these text files together. No system file association is installed.

Ctrl+H opens Replace. Find Next selects a match (case-sensitive unless Match case is unchecked); Replace changes the selection only if it equals the search term under the same rule, otherwise it first locates a match. Replace All changes every non-overlapping match across the whole original document as one undo action, regardless of the caret/selection. Inserted replacements are not searched again within that batch. It reports the match count and places the caret after the last replacement. Empty replacement removes matches. Zero matches or identical replacement leave document state/history unchanged. Ctrl+F/H switches between Find and Replace even with a dialog focused; unsubmitted dialog edits are not retained when switching.

Ctrl+F opens Find; Enter selects the next substring match. F3 repeats forward; Shift+F3 searches backward. Both reuse the last submitted term and wrap at the document boundary, with Find open or closed. Escape closes Find; a missing term leaves the document and selection intact. Ctrl+N starts a blank document after Save / Discard / Cancel protection for unsaved text. It clears the old filename and undo history; its first Save chooses a new path. Enter inserts a line break. Type, click to place the caret, drag or Shift+arrows to select, Backspace/Delete to remove text, and Ctrl+C/X/V to copy/cut/paste. Home/End moves to the current row's beginning/end; Ctrl+Home/End moves to the document beginning/end. Shift selects to either boundary. Ctrl+A selects everything. All four arrows move the caret; Up/Down remembers its horizontal target across shorter rows. Left/Right crosses line breaks; deleting a break joins rows. Clipboard line breaks and tabs are preserved.

Ctrl+S chooses a `.lemur` filename on the first save, then updates that same file. Files are UTF-8 without a BOM, with Windows CRLF line endings. The title shows the successful save path; an asterisk marks text that differs from the last save. Failed saves report an error and keep the text in memory. Closing changed text offers Save, Discard, or Cancel. Canceling or failing a save keeps the editor open. Ctrl+O opens UTF-8 text files (with or without a BOM), normalizes line endings, and resets caret/scroll. Unsaved changes use the same Save / Discard / Cancel protection before replacement; failed loads keep the current document. Other encodings and embedded NUL/binary content are rejected. Ctrl+Shift+S chooses a destination with Save as hylic. Only success adopts the new path; Cancel or failure keeps the old path and unsaved state. Choosing a different path leaves the original intact. Ctrl+Z undoes and Ctrl+Y redoes edits, restoring caret and selection. History retains up to 100 edits; a run of typed characters is one step, ended by Enter, a deletion, paste, a caret move, undo/redo, or a pause over two seconds. New edits clear redo. Saving keeps history; opening a file clears it. Mouse-wheel scrolling browses rows; with wrapping off, Shift+wheel browses sideways (down moves right) and native horizontal wheel input also works. Editing and arrows keep the caret visible; clicks and selections use the shifted view. There are no visible scrollbars. The caret is a simple drawn bar.

## Entry points

- [Build steps](STEPS.md): the short, shareable sequence of how we made the editor.
- `src/main.cpp`: all editor code.
- `build.bat`: MSVC build; generated executable and milestone captures are in `build/`.
- [Architecture](docs/ARCHITECTURE.md): how messages, state, and layout connect.
- [Status](docs/STATUS.md): evidence, limits, and next milestone.






