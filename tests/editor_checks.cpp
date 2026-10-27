// Exercise the real editor helpers and message handlers with a hidden window.
#include "../src/main.cpp"
#include <iostream>
#include <stdexcept>

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void LoadText(const std::wstring& text, std::size_t position = 0) {
    textBuffer = text;
    cursorPosition = selectionAnchor = position;
    firstVisibleRow = 0;
    hasPreferredCaretX = caretAtRowEnd = false;
    pendingHighSurrogate = 0;
    wheelRemainder = 0;
}

int main() {
    HWND window = nullptr;
    HDC dc = nullptr;
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    int result = 0;
    try {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProcedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Octavo.Checks";
        Require(RegisterClassW(&wc) != 0, "Register test window");
        window = CreateWindowW(wc.lpszClassName, L"Hidden editor checks",
            WS_OVERLAPPEDWINDOW, 0, 0, 760, 440, nullptr, nullptr, wc.hInstance, nullptr);
        Require(window != nullptr, "Create hidden test window");
        dc = GetDC(window);
        Require(dc != nullptr, "Get test DC");
        font = CreateCharacterFont();
        Require(font != nullptr, "Create editor font");
        oldFont = SelectObject(dc, font);

        LoadText(L"0123456789");
        for (std::size_t gap = 0; gap <= textBuffer.size(); ++gap) {
            const int x = 24 + MeasureText(dc, 0, gap);
            Require(FindCursorPositionInLine(dc, {0, textBuffer.size()}, 24, x) == gap,
                "Measured character gaps must round-trip to offsets");
        }
        LoadText(L"A\xD83D\xDE00Z");
        for (int x = 0; x < 200; ++x) {
            Require(FindCursorPositionInLine(dc, {0, textBuffer.size()}, 24, x) != 2,
                "Hit testing must never split a surrogate pair");
        }
        LoadText(std::wstring(20000, L'x'));
        Require(FindCursorPositionInLine(dc, {0, textBuffer.size()}, 24,
            24 + MeasureText(dc, 0, textBuffer.size())) == textBuffer.size(),
            "Long line end lookup");
        std::cout << "PASS: measured gaps, surrogate boundaries, 20,000-character line\n";

        LoadText(L"0123456789\nx\n\n0123456789", 8);
        MoveCursorVertically(window, true, false);
        Require(cursorPosition == 12, "Short row clamps at end");
        MoveCursorVertically(window, true, false);
        Require(cursorPosition == 13, "Empty row is navigable");
        MoveCursorVertically(window, true, false);
        Require(cursorPosition == 22, "Horizontal target survives short/empty rows");
        const std::size_t anchor = selectionAnchor;
        MoveCursorVertically(window, false, true);
        Require(selectionAnchor == anchor && cursorPosition == 13, "Shift extends selection");
        MoveCursorWithKey(false, false);
        Require(!hasPreferredCaretX && !HasSelection(), "Horizontal move resets target");
        std::cout << "PASS: vertical target, empty rows, selection\n";

        LoadText(L"a\nb", 1);
        WindowProcedure(window, WM_CHAR, L'\r', 0);
        Require(textBuffer == L"a\n\nb" && cursorPosition == 2, "Enter inserts LF");
        WindowProcedure(window, WM_CHAR, L'\b', 0);
        Require(textBuffer == L"a\nb", "Backspace joins rows");
        WindowProcedure(window, WM_KEYDOWN, VK_DELETE, 0);
        Require(textBuffer == L"ab", "Delete joins rows");
        std::cout << "PASS: Enter and newline deletion\n";

        std::wstring rows;
        for (int row = 0; row < 30; ++row) rows += L"abc\n";
        LoadText(rows);
        const TextLayout layout = CreateTextLayout(dc);
        const int visibleRows = (EditorBottom(window) - layout.top) / layout.lineHeight;
        Require(visibleRows > 0, "Test viewport has rows");
        const int partialY = layout.top + visibleRows * layout.lineHeight;
        Require(partialY < EditorBottom(window), "Test viewport has partial bottom row");
        const LPARAM partialPoint = MAKELPARAM(layout.left, partialY);
        WindowProcedure(window, WM_LBUTTONDOWN, MK_LBUTTON, partialPoint);
        const std::size_t clickedPosition = cursorPosition;
        Require(clickedPosition == static_cast<std::size_t>(visibleRows) * 4,
            "Click targets partial row");
        WindowProcedure(window, WM_LBUTTONUP, 0, partialPoint);
        Require(cursorPosition == clickedPosition && !HasSelection() && firstVisibleRow == 0,
            "Stationary click must not scroll or create selection");
        WindowProcedure(window, WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(layout.left, EditorBottom(window) + 10));
        Require(cursorPosition == clickedPosition && GetCapture() != window,
            "Footer click must not edit or capture mouse");

        UpdateViewport(window, false, 3);
        Require(FindCursorPositionAtMouse(window, layout.left, layout.top) == 12,
            "Scrolled mouse mapping targets document row");
        WindowProcedure(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2), 0);
        Require(firstVisibleRow == 3, "Partial wheel delta accumulates");
        WindowProcedure(window, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA / 2), 0);
        Require(firstVisibleRow == 3 && wheelRemainder == 0, "Opposite partial wheel cancels");
        UpdateViewport(window, false, 1000);
        Require(firstVisibleRow == layout.lines.size() - static_cast<std::size_t>(visibleRows),
            "Scrolling clamps at document bottom");
        cursorPosition = selectionAnchor = 0;
        WindowProcedure(window, WM_KEYDOWN, VK_LEFT, 0);
        Require(firstVisibleRow == 0, "Boundary arrow reveals offscreen caret");
        cursorPosition = selectionAnchor = textBuffer.size();
        UpdateViewport(window, true);
        Require(firstVisibleRow + static_cast<std::size_t>(visibleRows) == layout.lines.size(),
            "Caret reveals final row");
        std::cout << "PASS: click regression, footer, scroll mapping, wheel, caret visibility\n";

        // Drag autoscroll: a held button resting below the editor keeps scrolling and extending.
        firstVisibleRow = 0;
        cursorPosition = selectionAnchor = 0;
        WindowProcedure(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(layout.left, layout.top));
        Require(GetCapture() == window, "Drag captures the mouse");
        WindowProcedure(window, WM_MOUSEMOVE, MK_LBUTTON,
            MAKELPARAM(layout.left, EditorBottom(window) + 5));
        Require(autoscrollTimerRunning, "Dragging outside starts the autoscroll timer");
        WindowProcedure(window, WM_TIMER, kAutoscrollTimer, 0);
        Require(firstVisibleRow > 0, "Autoscroll tick scrolls down");
        const std::size_t firstTickAnchor = selectionAnchor;
        const std::size_t firstTickCursor = cursorPosition;
        Require(firstTickAnchor == 0 && firstTickCursor > 0, "Autoscroll extends the selection");
        WindowProcedure(window, WM_TIMER, kAutoscrollTimer, 0);
        Require(cursorPosition > firstTickCursor, "Resting outside keeps extending");
        WindowProcedure(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(layout.left, layout.top + 5));
        Require(!autoscrollTimerRunning, "Returning inside stops the timer");
        WindowProcedure(window, WM_MOUSEMOVE, MK_LBUTTON,
            MAKELPARAM(layout.left, layout.top - 5));
        Require(autoscrollTimerRunning, "Dragging above starts the timer");
        const std::size_t rowBeforeUp = firstVisibleRow;
        WindowProcedure(window, WM_TIMER, kAutoscrollTimer, 0);
        Require(firstVisibleRow < rowBeforeUp, "Autoscroll tick scrolls up");
        WindowProcedure(window, WM_LBUTTONUP, 0, MAKELPARAM(layout.left, layout.top - 5));
        Require(!autoscrollTimerRunning && GetCapture() != window, "Release stops autoscroll");
        const std::size_t rowAfterRelease = firstVisibleRow;
        WindowProcedure(window, WM_TIMER, kAutoscrollTimer, 0);
        Require(firstVisibleRow == rowAfterRelease, "Stray tick after release does nothing");
        std::cout << "PASS: drag autoscroll\n";

        LoadText(L"ab\n\tcd\nA\xD83D\xDE00Z", 0);
        Require(CaretStatusText() == L"Ln 1, Col 1", "Status at document start");
        cursorPosition = selectionAnchor = 4;
        Require(CaretStatusText() == L"Ln 2, Col 2", "Tab counts as one column");
        cursorPosition = selectionAnchor = textBuffer.size();
        Require(CaretStatusText() == L"Ln 3, Col 4", "Surrogate pair counts as one column");
        selectionAnchor = 3;
        Require(CaretStatusText() == L"Ln 3, Col 4  (7 selected)", "Selection counts characters");
        LoadText(L"", 0);
        Require(CaretStatusText() == L"Ln 1, Col 1", "Status on empty document");
        std::cout << "PASS: caret status readout\n";

        // Word wrap. Rows must tile the text, fit the width, and never split surrogate pairs.
        const auto wrapWidth = [&]() {
            RECT client{};
            GetClientRect(window, &client);
            return client.right - 2 * 24 - 2;
        };
        const auto requireTiling = [&](const TextLayout& rowsLayout, const char* message) {
            Require(!rowsLayout.lines.empty() && rowsLayout.lines.front().start == 0 &&
                rowsLayout.lines.back().end == textBuffer.size() && !rowsLayout.lines.back().soft, message);
            for (std::size_t row = 0; row + 1 < rowsLayout.lines.size(); ++row) {
                const TextLine& line = rowsLayout.lines[row];
                const TextLine& next = rowsLayout.lines[row + 1];
                Require(line.end > line.start || !line.soft, message);
                Require(next.start == (line.soft ? line.end : line.end + 1), message);
                if (line.soft) {
                    Require(!(IsHighSurrogate(textBuffer[line.end - 1]) && IsLowSurrogate(textBuffer[line.end])),
                        "A wrap must not split a surrogate pair");
                }
            }
        };
        Require(wordWrap, "Word wrap defaults on");
        std::wstring prose;
        for (int word = 0; word < 60; ++word) prose += L"alpha beta gamma delta ";
        LoadText(prose);
        TextLayout wrapped = CreateTextLayout(dc);
        Require(wrapped.lines.size() > 1, "Long prose wraps into rows");
        requireTiling(wrapped, "Wrapped rows tile the text");
        for (std::size_t row = 0; row < wrapped.lines.size(); ++row) {
            const TextLine& line = wrapped.lines[row];
            std::size_t trimmed = line.end;
            while (trimmed > line.start && IsBlank(textBuffer[trimmed - 1])) --trimmed;
            Require(MeasureText(dc, line.start, trimmed) <= wrapWidth(), "Wrapped row fits the width");
            Require(row == 0 || !IsBlank(textBuffer[line.start]), "A wrapped row does not start with a blank");
            Require(row + 1 == wrapped.lines.size() || line.soft, "Every row but the last is soft");
        }
        LoadText(L"one\ntwo three\n\nfour");
        wrapped = CreateTextLayout(dc);
        Require(wrapped.lines.size() == 4 && !wrapped.lines[0].soft && !wrapped.lines[2].soft &&
            wrapped.lines[2].start == wrapped.lines[2].end, "Short and empty lines keep one hard row each");
        LoadText(std::wstring(500, L'x'));
        wrapped = CreateTextLayout(dc);
        Require(wrapped.lines.size() > 2, "A word wider than the window breaks by character");
        requireTiling(wrapped, "Character-wrapped rows tile the text");
        std::wstring emoji;
        for (int index = 0; index < 300; ++index) emoji += L"\xD83D\xDE00";
        LoadText(emoji);
        requireTiling(CreateTextLayout(dc), "Emoji wrapping tiles without splitting pairs");
        LoadText(L"a\tb\tc\td\te\tf\tg\th\ti\tj\tk\tl\tm\tn\to\tp\tq\tr\ts\tt\tu\tv\tw\tx\ty\tz\t0\t1\t2\t3\t4\t5");
        requireTiling(CreateTextLayout(dc), "Tabs wrap and tile");
        std::cout << "PASS: wrap rows tile, fit, and never split pairs\n";

        // Caret at a wrap boundary, Home/End, vertical movement, and mouse hits.
        LoadText(prose);
        wrapped = CreateTextLayout(dc);
        const std::size_t boundary = wrapped.lines[0].end;
        cursorPosition = selectionAnchor = boundary;
        Require(RowOfOffset(wrapped, boundary) == 1, "A boundary offset defaults to the next row");
        cursorPosition = selectionAnchor = 2;
        WindowProcedure(window, WM_KEYDOWN, VK_END, 0);
        Require(cursorPosition == boundary && caretAtRowEnd && RowOfOffset(wrapped, cursorPosition) == 0,
            "End stays on the wrapped row");
        WindowProcedure(window, WM_KEYDOWN, VK_DOWN, 0);
        Require(RowOfOffset(wrapped, cursorPosition) == 1 && cursorPosition > boundary - 1,
            "Down from the row end moves to the next row");
        WindowProcedure(window, WM_KEYDOWN, VK_UP, 0);
        Require(RowOfOffset(wrapped, cursorPosition) == 0, "Up returns to the first row");
        cursorPosition = selectionAnchor = boundary + 3;
        caretAtRowEnd = false;
        WindowProcedure(window, WM_KEYDOWN, VK_HOME, 0);
        Require(cursorPosition == boundary && !caretAtRowEnd && RowOfOffset(wrapped, cursorPosition) == 1,
            "Home goes to the start of the visual row");
        Require(FindCursorPositionAtMouse(window, 24, wrapped.top + wrapped.lineHeight) == wrapped.lines[1].start,
            "Clicking the start of a wrapped row targets its first character");
        RECT wrapClient{};
        GetClientRect(window, &wrapClient);
        const std::size_t farRight = FindCursorPositionAtMouse(window, wrapClient.right - 24, wrapped.top);
        Require(RowOfOffset(wrapped, farRight) == 0, "Clicking past a wrapped row's end keeps the caret on it");
        cursorPosition = selectionAnchor = boundary;
        caretAtRowEnd = false;
        WindowProcedure(window, WM_KEYDOWN, VK_LEFT, 0);
        Require(cursorPosition == boundary - 1 && RowOfOffset(wrapped, cursorPosition) == 0,
            "Left from a row start returns to the previous row");

        // Toggling, the sideways view, and reflow on resize.
        LoadText(std::wstring(20000, L'x'), 20000);
        UpdateViewport(window, true);
        Require(horizontalOffset == 0, "Wrapped text never scrolls sideways");
        const TextLayout longWrapped = CreateTextLayout(dc);
        Require(RowOfOffset(longWrapped, cursorPosition) < firstVisibleRow + 40 &&
            RowOfOffset(longWrapped, cursorPosition) >= firstVisibleRow, "Caret row is revealed in wrapped text");
        WindowProcedure(window, WM_COMMAND, kViewWordWrap, 0);
        Require(!wordWrap, "View > Word Wrap turns wrapping off");
        UpdateViewport(window, true);
        Require(CreateTextLayout(dc).lines.size() == 1 && horizontalOffset > 0,
            "Without wrapping a long line is one row that scrolls sideways");
        WindowProcedure(window, WM_COMMAND, kViewWordWrap, 0);
        Require(wordWrap && horizontalOffset == 0, "Turning wrapping back on resets the sideways view");
        LoadText(prose);
        UpdateViewport(window, false, 4);
        const std::size_t topOffset = topRowOffset;
        cursorPosition = selectionAnchor = topOffset;
        SetWindowPos(window, nullptr, 0, 0, 420, 440, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        const TextLayout narrow = CreateTextLayout(dc);
        const TextLine& topRow = narrow.lines[firstVisibleRow];
        Require(topRow.start <= topOffset && (firstVisibleRow + 1 == narrow.lines.size() ||
            topOffset < narrow.lines[firstVisibleRow + 1].start), "Resize keeps the same text at the top");
        SetWindowPos(window, nullptr, 0, 0, 120, 440, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        requireTiling(CreateTextLayout(dc), "A very narrow window still wraps and tiles");
        SetWindowPos(window, nullptr, 0, 0, 760, 440, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        std::cout << "PASS: wrap caret affinity, navigation, toggle, resize\n";

        std::wstring big;
        for (int line = 0; line < 2000; ++line) big += L"The quick brown fox jumps over the lazy dog, again and again, until the line wraps.\n";
        LoadText(big);
        const ULONGLONG layoutStart = GetTickCount64();
        const TextLayout bigLayout = CreateTextLayout(dc);
        const ULONGLONG layoutMs = GetTickCount64() - layoutStart;
        std::cout << "INFO: wrapped layout of " << textBuffer.size() << " characters into "
            << bigLayout.lines.size() << " rows took " << layoutMs << " ms\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    if (GetCapture() == window) ReleaseCapture();
    if (oldFont != nullptr) SelectObject(dc, oldFont);
    if (font != nullptr) DeleteObject(font);
    if (dc != nullptr) ReleaseDC(window, dc);
    if (window != nullptr) DestroyWindow(window);
    return result;
}
