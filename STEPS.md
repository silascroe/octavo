# How we built the text editor

One capability at a time, from a window to a working text editor.

1. **First window:** A native Windows window that could display one typed character.
2. **Text buffer:** Multiple characters stored together and drawn as text.
3. **Caret movement:** A caret and Left/Right movement through the text.
4. **Click to place:** Clicking between characters to position the caret.
5. **Forward Delete:** Delete removes the character ahead of the caret, alongside Backspace.
6. **Keyboard selection:** Shift+Left/Right selects text, with highlighting and replacement when typing.
7. **Mouse selection:** Dragging to select text.
8. **Clipboard:** Copy, cut, and paste through the Windows clipboard.
9. **Multiple lines:** Enter creates rows; drawing, caret placement, selection, and mouse input work across them.
10. **Vertical movement:** Up/Down moves between rows, remembering the horizontal target through shorter rows.
11. **Vertical scrolling:** Mouse-wheel scrolling and automatically keeping the caret visible.
12. **Saving:** Ctrl+S saves UTF-8 text files and updates the chosen file on later saves.
13. **Unsaved changes:** Track changes and offer Save / Discard / Cancel before closing.
14. **Opening:** Ctrl+O opens UTF-8 files while protecting the current document.
15. **Save As:** Ctrl+Shift+S saves under another name or location and adopts that path after success.
16. **Undo and redo:** Ctrl+Z/Ctrl+Y restore text, caret, and selection across edits; opening a file starts fresh history.
17. **New document:** Ctrl+N starts blank with a fresh path and history, protecting unsaved text with Save / Discard / Cancel.
18. **Horizontal scrolling:** Long lines follow the caret sideways; Shift+wheel browses across text, with matching drawing and mouse placement.
19. **Line boundaries:** Home/End jumps to the current line's beginning/end; Shift selects to that boundary and scrolling follows.
20. **Document boundaries:** Ctrl+Home/End jumps to the whole document's beginning/end; Shift extends selection, and Ctrl+A selects everything.
21. **Find:** Ctrl+F opens a search box; exact case-sensitive matches become selections, and F3 finds the next match with wraparound.
22. **Find Previous:** Shift+F3 searches backward through exact matches, wrapping to the last match when it reaches the beginning.
23. **Replace:** Ctrl+H opens replacement fields; Replace changes one exact selected match as a single undoable edit.
24. **Replace All:** Change all exact matches across the original document in one undoable batch, with a match count.
25. **File menu and identity:** New/Open/Save/Exit are visible; new files default to plain-text `.lemur`, with “Save as hylic” for ordinary text extensions.
26. **Edit menu:** Undo, Redo, Cut, Copy, Paste, Select All, Find, Find Next/Previous, and Replace are menu commands that share code with their keyboard shortcuts.
27. **Word motion, tab stops, graying:** Ctrl+Arrow/Backspace/Delete work by word, tabs advance to 4-space stops, Edit items gray out when idle, and Find can ignore case.
28. **Typing undo and paging:** A run of typed characters undoes as one step, and Page Up/Down moves a screenful.
29. **Drag autoscroll:** Holding a drag outside the window keeps scrolling and extending the selection.
30. **Caret readout:** The footer shows line, column, and the selected character count.
31. **Row-based layout:** Layout rows can now be "soft" (cut by wrapping) and one helper maps a caret offset to its row; Home/End use visual rows. No visible change.
32. **Word wrap:** Long lines wrap at the window edge at word boundaries, with the caret, selection, mouse, and Up/Down all following the wrapped rows.
33. **Wrap toggle and Octavo v0.1:** View > Word Wrap (Alt+Z), reflow on resize that keeps the same text on top, and the window is now titled Octavo v0.1.
