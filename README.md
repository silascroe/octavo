# Octavo

Octavo is a small Windows text editor built from scratch in C++20. It uses a custom Win32 window and draws its own text, caret, and selections with GDI.

It started as a way to learn how a text editor works, one capability at a time. It can now open, edit, and save text files, with word wrap, undo and redo, search and replace, and the usual keyboard and mouse controls.

## Build and run

On Windows, install Visual Studio or Visual Studio Build Tools with **Desktop development with C++**. From a PowerShell window in the project directory:

```powershell
.\build.bat
.\build\octavo.exe
```

Close Octavo before rebuilding so the linker can replace the executable.

To run the focused native regression checks:

```powershell
.\build.bat check
```

These checks use a hidden window and exercise editor behavior; they do not check the app's visual appearance.

## Using Octavo

Word wrap is on by default. Toggle it with **View > Word Wrap** or `Alt+Z`. When wrap is off, use `Shift` + mouse wheel or a horizontal scroll wheel to move sideways through long lines.

| Action | Shortcut |
| --- | --- |
| New / Open / Save | `Ctrl+N` / `Ctrl+O` / `Ctrl+S` |
| Save as hylic | `Ctrl+Shift+S` |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Y` |
| Find / Replace | `Ctrl+F` / `Ctrl+H` |
| Find next / previous | `F3` / `Shift+F3` |
| Select all | `Ctrl+A` |
| Move or delete by word | `Ctrl+Left/Right` / `Ctrl+Backspace/Delete` |

Find and Replace match case by default; uncheck **Match case** to search without it. Replace All applies to the whole document as one undoable edit.

New documents default to the `.lemur` extension. A `.lemur` file is ordinary UTF-8 text. **Save as hylic** lets you choose a `.txt`, `.md`, or `.log` name, among others; after saving, that destination becomes the current file. Octavo opens UTF-8 files, including files with a BOM, and saves UTF-8 without a BOM using Windows CRLF line endings. It does not register file associations with Windows.

## Current limits

Octavo is an early, small-document editor. It does not yet have visible scrollbars, a blinking caret, full grapheme-cluster navigation, or crash recovery. Layout is recalculated as the view changes, and undo keeps full-document snapshots, so very large files can be slow or memory-intensive. Only UTF-8 text can be opened; other encodings and files containing embedded NUL characters are rejected.

## Project notes

[STEPS.md](STEPS.md) records the build sequence. [Architecture](docs/ARCHITECTURE.md) explains the implementation, and [status](docs/STATUS.md) records known limits and past verification.
