# Architecture

`WinMain` registers a custom window and runs the Windows message loop. `WindowProcedure` receives input and repaint messages.

## State and input

`textBuffer` is one UTF-16 `std::wstring`. `cursorPosition` and `selectionAnchor` are offsets in it. Their range describes selection; there are no separate strings for selected text or each row. Character movement/deletion avoids splitting UTF-16 surrogate pairs, but does not implement full grapheme navigation.

`WM_CHAR` handles typing, Backspace, and Return. Return becomes an internal LF (`\n`). `WM_KEYDOWN` handles all four arrows, Delete, and clipboard shortcuts. Removing an LF joins its surrounding rows. Typing replaces any selection.

## Shared layout

`CreateTextLayout` finds each row's start/end offsets, excluding its LF. It always retains the final row, even when empty. GDI font metrics determine row spacing; text starts at a fixed margin. Each `TextLine` also has a `soft` flag: true when the row was cut by word wrap and flows into the next row, false when a real line break (or the end of the text) follows. `RowOfOffset(layout, offset)` is the single place that maps a caret offset to its row; painting, viewport reveal, vertical movement, and Home/End all use it.

## Word wrap

Word wrap is on by default and toggled from View > Word Wrap or Alt+Z (`wordWrap`, `ToggleWordWrap`). `CreateTextLayout` gets the wrap width from the client area of the window behind its DC (`WindowFromDC`): client width minus the two 24-pixel margins and 2 pixels for the caret bar. With wrapping off the layout is exactly one row per line, as before.

`WrapLine` cuts one logical line into rows. It probes outward by doubling and then bisects, using the tab-aware `MeasureText` from the row start, so a very long line is not measured whole for every row. A row breaks after a run of blanks, and the blanks hang past the right edge. If no blank fits, a long word breaks at the last character that fits, never inside a surrogate pair, and every row holds at least one character so a tiny window cannot loop. Tab stops restart at each row's left edge, because measuring, drawing, and hit testing all measure from the row start. A 168,000-character document wraps in about 15 ms, so layouts are rebuilt on demand and not cached.

A wrap boundary offset is both the end of one row and the start of the next. By default it belongs to the next row. `caretAtRowEnd` plus `caretAffinityOffset` put the caret at the end of the upper row instead; End, clicking or dragging past the end of a wrapped row, and Up/Down landing there set it (`FindCursorPositionInLine` sets it whenever it returns a soft row's end). The flag only counts while the caret is still at that offset, and every code path that already clears `hasPreferredCaretX` also clears it. A soft row end draws no selected-line-break block; only real LFs do. Status-bar `Ln` still counts real lines.

While wrapping, `UpdateViewport` forces `horizontalOffset` to zero and skips horizontal reveal, Shift+wheel does nothing, and drag autoscroll scrolls only vertically. `topRowOffset` remembers the text offset at the top of the view; `ReflowView` (called on resize and on toggling) looks up the row now holding that offset so the same text stays at the top, then reveals the caret if needed.

`PaintDocument` draws each visible row, the selection's intersection with that row, and the caret at its row and measured prefix width. Selected line breaks get a small highlight block at the row end. Drawing clips to the editor area above the instructions.

`FindCursorPositionAtPoint` uses mouse Y to choose a row, then measured character gaps to choose an offset from mouse X. Click and drag therefore use the same geometry as painting.

Character gap lookup gets cumulative widths in one `GetTextExtentExPointW` call, rather than repeatedly measuring increasingly long prefixes. Surrogate-pair interiors are excluded from valid gaps.

## Clipboard

Windows supplies `CF_UNICODETEXT`. Copy exports internal LF as CRLF; paste normalizes CRLF, CR, and LF to LF. Tabs are kept. Cut removes text only after copying succeeds.

Windows supplies window management, input messages, fonts, drawing primitives, and clipboard storage. Our code supplies editing behavior and layout. Every state change requests a repaint; no native text-entry control is involved.

## Vertical movement

Up/Down finds the caret's current row, remembers its measured X position, and finds the nearest character gap on the adjacent row using the same helper as mouse hit testing. Consecutive vertical moves retain that target across short or empty rows. Horizontal movement, mouse placement, or editing resets it. Shift retains the selection anchor; movement without Shift collapses selection at the destination.


## Vertical scrolling

`firstVisibleRow` is view state, separate from text and caret offsets. Painting subtracts it from row indices; mouse hit testing adds it to the clicked screen row. `UpdateViewport` clamps scrolling to document bounds and reveals the caret after editing, navigation, or resizing. Wheel input browses without moving the caret and respects the Windows wheel-lines/page preference, accumulating partial wheel deltas. The viewport uses complete rows; very small windows may clip a row. There is no visible scrollbar. Horizontal scrolling is described below.

Mouse placement and dragging repaint without revealing the caret, so a click on a partial bottom row does not change the coordinate mapping between mouse-down and mouse-up. Clicks in the instruction footer are ignored; captured drags below the editor clamp to its visible lower edge for hit testing.

## Drag autoscroll and Page Up/Down

`DragSelectTo(window, x, y, scroll)` extends the selection to a mouse point using `FindCursorPositionAtMouse` and stores the point. A point above the editor top, below `EditorBottom`, or past the side margins gives a scroll delta of one row (or 16 pixels) plus the distance out, capped at 10 rows or 160 pixels. `WM_MOUSEMOVE` while captured calls it without scrolling and starts a 50 ms timer (`kAutoscrollTimer`) when the point is outside; `WM_TIMER` calls it again with scrolling from the stored point, so a resting mouse keeps moving. The timer stops when the point returns inside, on button release, and on `WM_CAPTURECHANGED` (which also covers New). Scrolling goes through `UpdateViewport` with row/pixel deltas, never revealing the caret.

Page Up/Down reuses `MoveCursorVertically` with a `byPage` flag: the step is the number of fully visible rows, clamped to the first and last row, keeping the preferred X. Shift extends the selection; `UpdateViewport` then reveals the caret.

## Caret readout

`CaretStatusText` scans the buffer up to `cursorPosition` counting LFs for the line and counting characters since the last LF for the column (a surrogate pair counts once, a tab once). With a selection it appends the selected character count. `WM_PAINT` splits the footer strip into two lines: the shortcut hint, then this text right-aligned with a 24 pixel margin. Every viewport update invalidates the window, so the readout follows caret moves.

## Word motion and tab stops

Characters fall into three classes: blank (space, tab), word (alphanumeric, underscore, surrogate halves), and other. Line breaks are their own stops. `NextWordStart` and `PreviousWordStart` find destinations; `MoveCursorByWord` (Shift keeps the anchor) and `DeleteWord` (selection first, otherwise to the word boundary, including trailing blanks going forward) use them. Deleting records one undo snapshot.

Tabs advance to the next multiple of four space widths measured from the row's left edge. `MeasureText(dc, start, end)` expects a row start and expands tabs segment by segment; `FindCursorPositionInLine` builds cumulative widths the same way; `DrawTabbedText` draws tab-free segments at those positions for both plain and selected text. `GetTabbedTextExtentW` is avoided because its 16-bit widths would overflow on very long lines.

## Saving

Ctrl+S or File > Save calls `SaveDocument`. With no successful destination yet, the Win32 common Save dialog chooses a path, defaults to `.lemur`, and confirms overwriting an existing file. Cancel leaves the document alone. Later saves reuse `savedPath`, including an opened ordinary-text file's path/extension; this is updated only after success, and the window title shows it. `.lemur` is the same UTF-8 text encoding as the other extensions, with no header, metadata, or system file association.

`EncodeDocument` converts internal UTF-16 to strict UTF-8 without a BOM, translating LF to Windows CRLF. `WriteDocumentToPath` creates a unique sibling temporary file, writes and flushes it, closes it, and uses `MoveFileExW` to replace the destination. Write/replacement failures report Windows errors and leave the buffer untouched; failure cleanup attempts to remove the owned temporary file. `comdlg32.lib` supplies the Save dialog.

Replacing the destination does not preserve all original file metadata; this is a plain text-file baseline rather than a complete document persistence system.

## Unsaved changes and closing

`lastSavedText` holds the text snapshot from the last successful save (initially empty). Comparing it with `textBuffer` determines unsaved changes and the title's asterisk. Reverting text to the snapshot clears the marker; caret movement and selection do not change it.

`SaveDocument` now returns success/failure. Only success updates both `savedPath` and the snapshot. `WM_CLOSE` calls `ConfirmDocumentChange`: unchanged text closes directly; changed text uses a native Task Dialog with Save, Discard, and Cancel. Save closes only on success, including completing a first-save filename dialog. Discard closes without writing; Cancel, Escape, a canceled Save dialog, a failed save, or a failed prompt keep the editor open. `comctl32.lib` and a Common Controls v6 manifest dependency supply the labeled dialog. This covers normal window closing, not crash recovery or operating-system session shutdown.

## Opening

Ctrl+O calls `OpenDocument`. The native picker chooses an existing path, then `ConfirmDocumentChange(window, DocumentChange::Open)` protects unsaved text. Save must succeed to continue; Cancel leaves the current document alone. Discard permits replacement but does not clear the current buffer first.

`ReadDocumentFromPath` reads into temporary bytes, strips an optional UTF-8 BOM, decodes strictly as UTF-8, rejects embedded NUL, and normalizes CRLF/CR to internal LF. Read/decode failure leaves the existing text/path/caret/selection/view untouched (a preceding user-selected Save may update its saved snapshot). Reading occurs after the prompt so saving the selected file itself cannot load a stale copy.

Only a complete successful load replaces text, sets the path and saved snapshot, and resets caret, anchor, scroll, preferred horizontal target, wheel remainder, and pending surrogate input. Other encodings are not auto-detected; tabs are retained and laid out on 4-space stops. File size is bounded by the Win32 UTF-8 decoder's signed-int length; practical size remains limited by memory and the simple layout.

## Save As

`SaveDocument(window, true)` opens the native Save dialog titled “Save as hylic.” Ctrl+Shift+S or the File menu requests this mode; Ctrl+S and the unsaved-change prompt use normal save mode. The suggested current path replaces the last extension (only after the last path separator) with `.txt`. Filters offer `.txt`, `.md`, `.log`, and All files; the native picker updates its suggested extension when the filter changes. Explicit filenames can override the suggestion. These are plain-text filename choices, not conversion or syntax generation. Both modes use the same UTF-8 encoder and temporary-file replacement routine. `savedPath` and `lastSavedText` change only after successful writing, so canceled or failed hylic saving retains the previous document identity and unsaved state. A different destination leaves the original file untouched; choosing the original path is a confirmed overwrite. This is Save As, not an export that retains the old current path.

## File menu

`CreateEditorMenu` (originally `CreateFileMenu`) constructs a native menu bar and File popup, passes the bar to `CreateWindowExW`, and reports creation failure rather than silently omitting commands. Native Windows menu drawing and mnemonic handling send `WM_COMMAND` IDs to the window procedure. New/Open/Save/hylic call the existing document helpers; Exit sends `WM_CLOSE` and therefore uses normal unsaved-change protection. Shortcut labels are descriptive; the existing key handler performs the same operations. Windows owns the attached menu for the lifetime of the window. No custom menu drawing or file associations are added. The menu reduces the client area; layout continues using client coordinates shared by painting, caret revelation, and mouse mapping.

## Edit menu

`CreateEditorMenu` builds the File, Edit, and View popups (View holds only Word Wrap, shown checked via `WM_INITMENUPOPUP`; Alt+Z is handled in `WM_SYSKEYDOWN`). Edit commands arrive as `WM_COMMAND` IDs (`EditCommand`) and call the same helpers as the Ctrl-key handler: `UndoOrRedo`, `CutSelection`, `PasteClipboard`, `SelectAllText`, `FindNextOrOpenDialog`, plus the existing copy and Find/Replace dialog routines. `WM_INITMENUPOPUP` grays items that would do nothing: Undo and Redo by history, Cut and Copy by selection, Paste by clipboard text, Select All by an empty buffer. Find Next/Previous stay enabled because they open Find without a term. Commands that still have nothing to do return without changing text or history.

## Undo and redo

`EditSnapshot` contains the complete text, caret offset, selection anchor, and first visible row. Typing (a complete surrogate pair is one action), Enter, Backspace/Delete, cut, and paste capture the previous state and call `RecordEdit` after the complete action. Only changed text adds history; replacing a selection is one action. Navigation, selection, copying, and failed/empty edits do not add entries.

Typed characters (including Tab and surrogate pairs) go through `RecordTypedEdit`, which merges them into the previous entry while the run is structurally continuous: the history is non-empty, there was no selection, the caret and text length equal those recorded after the last typed character, and at most 2000 ms have passed. Any other edit calls `RecordEdit`, which closes the run; Enter, Backspace/Delete, word deletion, cut, paste, replace, undo/redo, New, and Open all do. Caret moves are not hooked: they break the caret match instead. Moving away and returning to the exact same offset with nothing in between would still count as one run.

Ctrl+Z moves the current snapshot to redo history and restores the latest undo snapshot; Ctrl+Y does the reverse. Restoring resets pending surrogate input, the preferred horizontal target, and partial wheel state, then clamps the viewport and reveals the caret. A new text change clears redo. Each history stack retains at most 100 snapshots; full-text copies make this a small-document baseline, with no byte budget.

Save/Save As retain history and do not roll back disk writes during undo. The current text is still compared to `lastSavedText`, so undo/redo can change the unsaved marker. Successful Open clears both histories; canceled/failed Open preserves them. File path and saved snapshot are not part of edit history.

## New document

Ctrl+N calls `NewDocument`. `ConfirmDocumentChange` takes a `DocumentChange` enum (Close, Open, New) to provide action-specific prompt wording. Unchanged text proceeds directly; changed text must be saved successfully or explicitly discarded. Cancel, canceled/failed Save, and prompt failure return before any reset.

New clears text, file path, saved snapshot, and both history stacks; resets caret, selection, viewport, preferred horizontal target, wheel remainder, and pending surrogate input; releases any mouse capture; updates the title and repaints. The window remains open. Empty text equals the empty saved snapshot, so the new document is clean. Its first Save asks for a filename. New is a document boundary and cannot be undone into the previous document.

## Horizontal scrolling

`horizontalOffset` is a pixel distance shared by all rows. Drawing subtracts it from text, selection, and caret X coordinates; mouse hit testing uses the same shifted origin. Drawing clips to fixed 24-pixel side margins and captured mouse input clamps to those edges. Up/Down's preferred X remains a document coordinate, independent of scrolling.

`UpdateViewport` measures the widest document row (including space for a newline highlight), clamps horizontal scrolling, and reveals the caret after editing/navigation/resizing. Shift+vertical wheel scrolls sideways in 96-pixel increments per notch; down moves right. Native horizontal wheel input also works, positive moving right. Horizontal and vertical wheels keep separate partial-notch remainders. Browsing and mouse selection do not reveal the caret or move it automatically.

Undo snapshots include horizontal offset; restoring clamps it and reveals the restored caret. New and successful Open reset it and the horizontal wheel remainder. This section describes the non-wrapping view; with Word Wrap on, `horizontalOffset` is held at zero. There is no visible horizontal scrollbar. Widths are remeasured on viewport updates rather than cached, so this remains a small-document implementation.

## Home and End

`MoveCursorToRowBoundary` finds the caret's visual row with `RowOfOffset` and goes to its `start` (Home) or `end` (End); without wrapping a row is a whole line, so this is the line boundary. Blank rows have coincident start/end positions. End on a wrapped row sets `caretAtRowEnd` so the caret stays on that row (see Word wrap). Shift preserves the selection anchor; otherwise the anchor follows the caret. The move resets pending surrogate input and the preferred vertical X target, then `UpdateViewport` reveals the destination. No text changes or history entries are created. Alt-modified Home/End are not handled.

## Document boundaries and Select All

The Ctrl shortcut handler places the caret at offset zero for Ctrl+Home or `textBuffer.size()` for Ctrl+End. Shift preserves the existing anchor; without Shift, the anchor follows the caret. Ctrl+A assigns anchor zero and caret buffer-end, selecting all text including line breaks. These operations reset pending surrogate input and preferred vertical X and reveal the destination through the shared viewport routine. They do not change text, saved state, or undo history. For an empty buffer both boundary offsets are zero.

## Find

`ReplaceAllMatches` scans the original `textBuffer` from offset zero, appending each untouched span and its replacement to a separate result. The source position advances past each original match by the nonempty search length; replacement text is never fed back into that scan, and overlapping candidates are skipped. After appending the remaining suffix, a changed result commits once using the existing snapshot/history routine. Caret and anchor move after the last inserted replacement; transient input/preferred X reset and the shared viewport routine reveals it. Zero matches or an unchanged result do not mutate document state or history. A native message box reports the number of matches, including identical replacements; this is a count of matched occurrences rather than changed characters. Empty search returns immediately. Full-document result and history copies retain the existing small-document memory constraints.

Step 23 adds `ReplaceTextW` through `ShowFindDialog(window, true)`. Find and Replace share the persistent request and buffers, with one modeless dialog at a time. Switching sends `WM_CLOSE` to the existing dialog before reinitializing its request; submitted search/replacement terms are retained, unsubmitted field edits are not. The message loop routes Ctrl+F/H before dialog keyboard handling, alongside F3. The native Replace All button is available; the owner handles `FR_FINDNEXT`, `FR_REPLACE`, and `FR_REPLACEALL`.

`ReplaceMatch` requires a nonempty search and a selection of exactly the same length/content. Otherwise it calls forward Find and returns without editing. On a match it captures the existing edit snapshot, replaces that range, collapses anchor/caret after the replacement, clears transient input/preferred X, records one changed-text action, and reveals the caret. Empty replacement deletes the match; identical replacement creates no history entry. It does not automatically select another match after editing. Both dialog fields accept 1,023 UTF-16 units and use single-line native controls. The editor suppresses Ctrl-modified ASCII Backspace because Ctrl+H otherwise translates to that character. Native dialog behavior is described in [Microsoft's Find and Replace documentation](https://learn.microsoft.com/en-us/windows/win32/dlgbox/find-and-replace-dialog-boxes).

Ctrl+F uses the Win32 `FindTextW` modeless common dialog. Windows supplies the text field, buttons, and dialog keyboard behavior; our editor supplies search and selection. The persistent `FINDREPLACEW` structure and 1,024-unit input buffer outlive the dialog. Direction and whole-word controls are hidden; Find Next searches forward and Shift+F3 provides backward, literal substring search. The Match case checkbox is shown and defaults on; the global `matchCase` follows the dialog flags. When off, `FoldCase` (`CharLowerBuffW`) lowercases haystack and needle for Find, Replace, and Replace All, and `SelectionMatchesSearch` makes Replace's selection check use the same rule.

`RegisterWindowMessageW(FINDMSGSTRINGW)` identifies dialog notifications. Find Next stores the submitted term and calls `FindMatch` in forward mode; dialog termination clears its handle and restores editor focus. The message loop uses `IsDialogMessageW` for Enter/Tab/Escape and intercepts F3 while the dialog is open, reading Shift to choose direction. F3 and Shift+F3 also work in the editor after closure; without a submitted term either opens Find. Reopening prefills the last submitted term. A term typed but not submitted does not replace the remembered search.

`FindMatch` forward mode searches from the selection's upper offset, or the caret when unselected, then wraps to the buffer beginning. Backward mode uses the selection's lower offset or caret as its boundary and `rfind` to locate the nearest match ending at or before that boundary. It guards the unsigned subtraction of term length; if no earlier match exists, it wraps to the last match in the buffer. Success assigns match boundaries to anchor/caret and reveals the caret with the shared viewport routine. Failure shows a message and leaves selection, text, and view alone. Search creates no undo entry or dirty change. Terms persist across New/Open within the process. The dialog accepts up to 1,023 UTF-16 units; no Find match-count display, whole-word, regex, or multi-line query UI exists yet. Successive matches are non-overlapping; a lone match can select itself again after wrapping.
