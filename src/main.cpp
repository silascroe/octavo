#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"Octavo.Window";
enum FileCommand : UINT { kFileNew = 1001, kFileOpen, kFileSave, kFileHylic, kFileExit };
enum EditCommand : UINT {
    kEditUndo = 1101, kEditRedo, kEditCut, kEditCopy, kEditPaste, kEditSelectAll,
    kEditFind, kEditFindNext, kEditFindPrevious, kEditReplace
};
enum ViewCommand : UINT { kViewWordWrap = 1201 };
wchar_t pendingHighSurrogate{};

std::wstring textBuffer;
std::size_t cursorPosition{};
std::size_t selectionAnchor{};
int preferredCaretX{};
bool hasPreferredCaretX{};
// A caret at the exact offset where a wrapped row ends is also where the next row starts.
// By default it is drawn on the next row; when this flag is set (and the offset still
// matches) it is drawn at the end of the upper row instead.
bool caretAtRowEnd{};
std::size_t caretAffinityOffset{};
std::size_t firstVisibleRow{};
std::size_t topRowOffset{};  // text offset of the first visible row, to survive reflow
int horizontalOffset{};
int wheelRemainder{};
int horizontalWheelRemainder{};
std::wstring savedPath;
std::wstring lastSavedText;
std::wstring searchTerm;
HWND findDialog{};
UINT findMessage{};
FINDREPLACEW findRequest{};
wchar_t findText[1024]{};
std::wstring replacementTerm;
wchar_t replacementText[1024]{};
bool replaceDialogMode{};
bool matchCase = true;
// Word wrap (View > Word Wrap, Alt+Z). Off restores one row per line with sideways scrolling.
bool wordWrap = true;

struct EditSnapshot {
    std::wstring text;
    std::size_t cursor;
    std::size_t anchor;
    std::size_t firstRow;
    int horizontal;
};

constexpr std::size_t kHistoryLimit = 100;
std::vector<EditSnapshot> undoHistory;
std::vector<EditSnapshot> redoHistory;

EditSnapshot CaptureEditSnapshot() {
    return {textBuffer, cursorPosition, selectionAnchor, firstVisibleRow, horizontalOffset};
}

void PushHistory(std::vector<EditSnapshot>& history, EditSnapshot snapshot) {
    if (history.size() == kHistoryLimit) history.erase(history.begin());
    history.push_back(std::move(snapshot));
}

// A run of typed characters shares one undo step. The run continues only while the
// caret and text size are exactly where the last typed character left them, and
// the last keystroke was recent; anything else (movement, deletion, Enter, paste,
// undo, a pause) starts a new step.
bool typingGroupOpen{};
std::size_t typingGroupCursor{};
std::size_t typingGroupTextSize{};
ULONGLONG typingGroupTick{};
constexpr ULONGLONG kTypingGroupGapMs = 2000;

void RecordEdit(EditSnapshot before) {
    typingGroupOpen = false;
    if (before.text == textBuffer) return;
    PushHistory(undoHistory, std::move(before));
    redoHistory.clear();
}

void RecordTypedEdit(EditSnapshot before) {
    if (before.text == textBuffer) return;
    const ULONGLONG now = GetTickCount64();
    const bool continuing = typingGroupOpen && !undoHistory.empty() &&
        before.cursor == before.anchor &&
        before.cursor == typingGroupCursor &&
        before.text.size() == typingGroupTextSize &&
        now - typingGroupTick <= kTypingGroupGapMs;
    if (!continuing) RecordEdit(std::move(before));
    typingGroupOpen = true;
    typingGroupCursor = cursorPosition;
    typingGroupTextSize = textBuffer.size();
    typingGroupTick = now;
}

bool RestoreHistory(bool redo) {
    auto& source = redo ? redoHistory : undoHistory;
    auto& destination = redo ? undoHistory : redoHistory;
    pendingHighSurrogate = 0;
    typingGroupOpen = false;
    if (source.empty()) return false;
    PushHistory(destination, CaptureEditSnapshot());
    EditSnapshot snapshot = std::move(source.back());
    source.pop_back();
    textBuffer = std::move(snapshot.text);
    cursorPosition = snapshot.cursor;
    selectionAnchor = snapshot.anchor;
    firstVisibleRow = snapshot.firstRow;
    horizontalOffset = snapshot.horizontal;
    hasPreferredCaretX = caretAtRowEnd = false;
    wheelRemainder = 0;
    horizontalWheelRemainder = 0;
    return true;
}

bool HasUnsavedChanges() {
    return textBuffer != lastSavedText;
}

void UpdateWindowTitle(HWND window) {
    std::wstring title = HasUnsavedChanges() ? L"* Octavo v0.1" : L"Octavo v0.1";
    if (!savedPath.empty()) title += L" - " + savedPath;
    SetWindowTextW(window, title.c_str());
}

bool EncodeDocument(std::string& bytes, DWORD& error) {
    std::wstring diskText;
    for (wchar_t character : textBuffer) {
        if (character == L'\n') diskText += L'\r';
        diskText += character;
    }
    bytes.clear();
    if (diskText.empty()) return true;
    if (diskText.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        error = ERROR_FILE_TOO_LARGE;
        return false;
    }
    const int length = static_cast<int>(diskText.size());
    const int byteCount = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        diskText.data(), length, nullptr, 0, nullptr, nullptr);
    if (byteCount == 0) {
        error = GetLastError();
        return false;
    }
    bytes.resize(byteCount);
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, diskText.data(), length,
        bytes.data(), byteCount, nullptr, nullptr) == 0) {
        error = GetLastError();
        return false;
    }
    return true;
}

bool WriteDocumentToPath(const std::wstring& path, DWORD& error) {
    std::string bytes;
    if (!EncodeDocument(bytes, error)) return false;

    // Finish a sibling temporary file before replacing the destination.
    // Failed writes leave the previous saved file intact.
    static unsigned long sequence = 0;
    std::wstring temporaryPath;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 100; ++attempt) {
        temporaryPath = path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(++sequence);
        file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    bool written = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(bytes.size() - offset);
        DWORD count = 0;
        const bool writeSucceeded = WriteFile(file, bytes.data() + offset, chunk, &count, nullptr) != FALSE;
        if (!writeSucceeded || count == 0) {
            error = writeSucceeded ? ERROR_WRITE_FAULT : GetLastError();
            written = false;
            break;
        }
        offset += count;
    }
    if (written && !FlushFileBuffers(file)) {
        error = GetLastError();
        written = false;
    }
    if (!CloseHandle(file) && written) {
        error = GetLastError();
        written = false;
    }
    if (written && !MoveFileExW(temporaryPath.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        written = false;
    }
    if (!written) DeleteFileW(temporaryPath.c_str());
    return written;
}

bool SaveDocument(HWND window, bool saveAs = false) {
    std::wstring destination = savedPath;
    if (saveAs || destination.empty()) {
        if (saveAs && !destination.empty()) {
            const std::size_t separator = destination.find_last_of(L"/\\");
            const std::size_t dot = destination.find_last_of(L'.');
            if (dot != std::wstring::npos && (separator == std::wstring::npos || dot > separator)) {
                destination.erase(dot);
            }
            destination += L".txt";
        }
        std::vector<wchar_t> filename(32768);
        destination.copy(filename.data(), filename.size() - 1);
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window;
        dialog.lpstrFilter = saveAs ?
            L"Text files (*.txt)\0*.txt\0Markdown text (*.md)\0*.md\0Log text (*.log)\0*.log\0All files (*.*)\0*.*\0\0" :
            L"Lemur files (*.lemur)\0*.lemur\0\0";
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrDefExt = saveAs ? L"txt" : L"lemur";
        dialog.lpstrTitle = saveAs ? L"Save as hylic" : L"Save";
        dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) {
            const DWORD error = CommDlgExtendedError();
            if (error != 0) {
                const std::wstring message = L"Could not open the Save dialog. Error " +
                    std::to_wstring(error) + L". Your text is still in the editor.";
                MessageBoxW(window, message.c_str(), L"Save failed", MB_OK | MB_ICONERROR);
            }
            return false;
        }
        destination = filename.data();
    }
    DWORD error = ERROR_SUCCESS;
    if (!WriteDocumentToPath(destination, error)) {
        wchar_t explanation[1024]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, explanation, 1024, nullptr);
        const std::wstring message = L"Could not save:\n" + destination + L"\n\n" +
            explanation + L"\nError " + std::to_wstring(error) +
            L". Your text is still in the editor.";
        MessageBoxW(window, message.c_str(), L"Save failed", MB_OK | MB_ICONERROR);
        return false;
    }
    savedPath = destination;
    lastSavedText = textBuffer;
    UpdateWindowTitle(window);
    return true;
}

enum class DocumentChange { Close, Open, New };

bool ConfirmDocumentChange(HWND window, DocumentChange action) {
    if (!HasUnsavedChanges()) return true;
    const TASKDIALOG_BUTTON buttons[] = {
        {IDYES, L"Save"}, {IDNO, L"Discard"}, {IDCANCEL, L"Cancel"}
    };
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = window;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    dialog.pszWindowTitle = L"Unsaved changes";
    dialog.pszMainIcon = TD_WARNING_ICON;
    dialog.pszMainInstruction = action == DocumentChange::Close ? L"Save changes before closing?" :
        action == DocumentChange::Open ? L"Save changes before opening another document?" :
        L"Save changes before starting a new document?";
    dialog.pszContent = action == DocumentChange::Close ?
        L"Discard closes the editor without saving these changes. Cancel keeps it open." :
        action == DocumentChange::Open ?
        L"Discard replaces the current text if the file loads. Cancel keeps the current document." :
        L"Discard starts a blank document without saving these changes. Cancel keeps the current document.";
    dialog.cButtons = 3;
    dialog.pButtons = buttons;
    dialog.nDefaultButton = IDCANCEL;
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&dialog, &choice, nullptr, nullptr))) {
        MessageBoxW(window, L"Could not show the unsaved-change prompt. The current document will be kept.",
            L"Unsaved changes", MB_OK | MB_ICONERROR);
        return false;
    }
    if (choice == IDNO) return true;
    if (choice == IDYES) return SaveDocument(window);
    return false;
}

void NewDocument(HWND window) {
    if (!ConfirmDocumentChange(window, DocumentChange::New)) return;
    textBuffer.clear();
    savedPath.clear();
    lastSavedText.clear();
    undoHistory.clear();
    redoHistory.clear();
    typingGroupOpen = false;
    cursorPosition = selectionAnchor = firstVisibleRow = topRowOffset = 0;
    horizontalOffset = 0;
    preferredCaretX = 0;
    hasPreferredCaretX = caretAtRowEnd = false;
    wheelRemainder = 0;
    horizontalWheelRemainder = 0;
    pendingHighSurrogate = 0;
    if (GetCapture() == window) ReleaseCapture();
    UpdateWindowTitle(window);
    InvalidateRect(window, nullptr, TRUE);
}

bool ReadDocumentFromPath(const std::wstring& path, std::wstring& text, DWORD& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        error = GetLastError();
        CloseHandle(file);
        return false;
    }
    if (size.QuadPart > (std::numeric_limits<int>::max)()) {
        error = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return false;
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    bool read = true;
    while (offset < bytes.size()) {
        DWORD count = 0;
        const DWORD chunk = static_cast<DWORD>(bytes.size() - offset);
        if (!ReadFile(file, bytes.data() + offset, chunk, &count, nullptr)) {
            error = GetLastError();
            read = false;
            break;
        }
        if (count == 0) {
            error = ERROR_HANDLE_EOF;
            read = false;
            break;
        }
        offset += count;
    }
    if (!CloseHandle(file) && read) {
        error = GetLastError();
        read = false;
    }
    if (!read) return false;
    const std::size_t start = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    std::wstring decoded;
    if (bytes.size() > start) {
        const int length = static_cast<int>(bytes.size() - start);
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            bytes.data() + start, length, nullptr, 0);
        if (count == 0) {
            error = GetLastError();
            return false;
        }
        decoded.resize(count);
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + start,
            length, decoded.data(), count) == 0) {
            error = GetLastError();
            return false;
        }
    }
    // This plain-text editor does not support embedded NUL/binary data.
    if (decoded.find(L'\0') != std::wstring::npos) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    std::wstring normalized;
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        if (decoded[index] == L'\r') {
            normalized += L'\n';
            if (index + 1 < decoded.size() && decoded[index + 1] == L'\n') ++index;
        } else {
            normalized += decoded[index];
        }
    }
    text = std::move(normalized);
    return true;
}

void OpenDocument(HWND window) {
    std::vector<wchar_t> filename(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Lemur and text files\0*.lemur;*.txt;*.md;*.log\0Lemur files (*.lemur)\0*.lemur\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrTitle = L"Open UTF-8 text document";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        const DWORD error = CommDlgExtendedError();
        if (error != 0) {
            const std::wstring message = L"Could not open the file picker. Error " + std::to_wstring(error);
            MessageBoxW(window, message.c_str(), L"Open failed", MB_OK | MB_ICONERROR);
        }
        return;
    }
    if (!ConfirmDocumentChange(window, DocumentChange::Open)) return;
    // Read after the prompt: Save may have updated the selected file itself.
    std::wstring loadedText;
    DWORD error = ERROR_SUCCESS;
    if (!ReadDocumentFromPath(filename.data(), loadedText, error)) {
        wchar_t explanation[1024]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, explanation, 1024, nullptr);
        const std::wstring message = L"Could not open:\n" + std::wstring(filename.data()) +
            L"\n\n" + explanation + L"\nError " + std::to_wstring(error) +
            L". Choose a UTF-8 text file. The current document has not been replaced.";
        MessageBoxW(window, message.c_str(), L"Open failed", MB_OK | MB_ICONERROR);
        return;
    }
    textBuffer = std::move(loadedText);
    undoHistory.clear();
    redoHistory.clear();
    typingGroupOpen = false;
    savedPath = filename.data();
    lastSavedText = textBuffer;
    cursorPosition = selectionAnchor = firstVisibleRow = topRowOffset = 0;
    horizontalOffset = 0;
    hasPreferredCaretX = caretAtRowEnd = false;
    wheelRemainder = 0;
    horizontalWheelRemainder = 0;
    pendingHighSurrogate = 0;
    UpdateWindowTitle(window);
    InvalidateRect(window, nullptr, TRUE);
}

bool IsHighSurrogate(wchar_t character) {
    return character >= 0xD800 && character <= 0xDBFF;
}

bool IsLowSurrogate(wchar_t character) {
    return character >= 0xDC00 && character <= 0xDFFF;
}

bool HasSelection() {
    return selectionAnchor != cursorPosition;
}

std::size_t SelectionStart() {
    return selectionAnchor < cursorPosition ? selectionAnchor : cursorPosition;
}

std::size_t SelectionEnd() {
    return selectionAnchor > cursorPosition ? selectionAnchor : cursorPosition;
}

bool DeleteSelection() {
    hasPreferredCaretX = caretAtRowEnd = false;
    if (!HasSelection()) {
        return false;
    }

    const std::size_t selectionStart = SelectionStart();
    const std::size_t selectionEnd = SelectionEnd();
    textBuffer.erase(selectionStart, selectionEnd - selectionStart);
    cursorPosition = selectionStart;
    selectionAnchor = cursorPosition;
    return true;
}

bool CopySelectionToClipboard(HWND window) {
    if (!HasSelection()) {
        return false;
    }

    std::wstring selectedText;
    for (std::size_t index = SelectionStart(); index < SelectionEnd(); ++index) {
        if (textBuffer[index] == L'\n') {
            selectedText += L'\r';
        }
        selectedText += textBuffer[index];
    }
    const std::size_t byteCount = (selectedText.size() + 1) * sizeof(wchar_t);
    HGLOBAL clipboardMemory = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (clipboardMemory == nullptr) {
        return false;
    }

    void* clipboardData = GlobalLock(clipboardMemory);
    if (clipboardData == nullptr) {
        GlobalFree(clipboardMemory);
        return false;
    }
    std::memcpy(clipboardData, selectedText.c_str(), byteCount);
    GlobalUnlock(clipboardMemory);

    if (!OpenClipboard(window)) {
        GlobalFree(clipboardMemory);
        return false;
    }

    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, clipboardMemory) != nullptr;
    CloseClipboard();
    if (!copied) {
        GlobalFree(clipboardMemory);
    }
    return copied;
}

bool PasteFromClipboard(HWND window) {
    hasPreferredCaretX = caretAtRowEnd = false;
    if (!OpenClipboard(window)) {
        return false;
    }

    std::wstring clipboardText;
    HANDLE clipboardHandle = GetClipboardData(CF_UNICODETEXT);
    if (clipboardHandle != nullptr) {
        const wchar_t* characters = static_cast<const wchar_t*>(GlobalLock(clipboardHandle));
        if (characters != nullptr) {
            const std::size_t capacity = GlobalSize(clipboardHandle) / sizeof(wchar_t);
            std::size_t length = 0;
            while (length < capacity && characters[length] != L'\0') {
                ++length;
            }
            clipboardText.assign(characters, length);
            GlobalUnlock(clipboardHandle);
        }
    }
    CloseClipboard();

    std::wstring pastedText;
    for (std::size_t index = 0; index < clipboardText.size(); ++index) {
        const wchar_t character = clipboardText[index];
        if (character == L'\r') {
            pastedText += L'\n';
            if (index + 1 < clipboardText.size() && clipboardText[index + 1] == L'\n') {
                ++index;
            }
        } else if (character == L'\n') {
            pastedText += L'\n';
        } else if (character == L'\t' || (character >= L' ' && character != 0x7F)) {
            pastedText += character;
        }
    }

    if (pastedText.empty()) {
        return false;
    }

    EditSnapshot before = CaptureEditSnapshot();
    DeleteSelection();
    textBuffer.insert(cursorPosition, pastedText);
    cursorPosition += pastedText.size();
    selectionAnchor = cursorPosition;
    pendingHighSurrogate = L'\0';
    RecordEdit(std::move(before));
    return true;
}

bool DeleteCharacterBeforeCursor() {
    hasPreferredCaretX = caretAtRowEnd = false;
    if (cursorPosition == 0) {
        return false;
    }

    std::size_t firstUnit = cursorPosition - 1;
    if (IsLowSurrogate(textBuffer[firstUnit]) && firstUnit > 0 && IsHighSurrogate(textBuffer[firstUnit - 1])) {
        --firstUnit;
    }

    textBuffer.erase(firstUnit, cursorPosition - firstUnit);
    cursorPosition = firstUnit;
    return true;
}

bool DeleteCharacterAtCursor() {
    hasPreferredCaretX = caretAtRowEnd = false;
    if (cursorPosition >= textBuffer.size()) {
        return false;
    }

    std::size_t endPosition = cursorPosition + 1;
    if (IsHighSurrogate(textBuffer[cursorPosition]) &&
        endPosition < textBuffer.size() &&
        IsLowSurrogate(textBuffer[endPosition])) {
        ++endPosition;
    }

    textBuffer.erase(cursorPosition, endPosition - cursorPosition);
    return true;
}

bool MoveCursorLeft() {
    if (cursorPosition == 0) {
        return false;
    }

    --cursorPosition;
    if (cursorPosition > 0 && IsLowSurrogate(textBuffer[cursorPosition]) && IsHighSurrogate(textBuffer[cursorPosition - 1])) {
        --cursorPosition;
    }
    return true;
}

bool MoveCursorRight() {
    if (cursorPosition >= textBuffer.size()) {
        return false;
    }

    if (IsHighSurrogate(textBuffer[cursorPosition]) &&
        cursorPosition + 1 < textBuffer.size() &&
        IsLowSurrogate(textBuffer[cursorPosition + 1])) {
        cursorPosition += 2;
    } else {
        ++cursorPosition;
    }
    return true;
}

bool MoveCursorWithKey(bool moveRight, bool extendSelection) {
    hasPreferredCaretX = caretAtRowEnd = false;
    const std::size_t previousCursorPosition = cursorPosition;
    const std::size_t previousSelectionAnchor = selectionAnchor;

    if (!extendSelection && HasSelection()) {
        cursorPosition = moveRight ? SelectionEnd() : SelectionStart();
        selectionAnchor = cursorPosition;
    } else {
        if (!extendSelection) {
            selectionAnchor = cursorPosition;
        }

        if (moveRight) {
            MoveCursorRight();
        } else {
            MoveCursorLeft();
        }

        if (!extendSelection) {
            selectionAnchor = cursorPosition;
        }
    }

    return cursorPosition != previousCursorPosition || selectionAnchor != previousSelectionAnchor;
}

// Word classes for Ctrl+Arrow and Ctrl+Backspace/Delete: 0 blank, 1 word, 2 other.
// Surrogate halves count as word characters so a pair is never split.
int WordClass(wchar_t character) {
    if (character == L' ' || character == L'\t') return 0;
    if (character == L'_' || IsHighSurrogate(character) || IsLowSurrogate(character) ||
        IsCharAlphaNumericW(character)) return 1;
    return 2;
}

// Line breaks are their own stops, so word motion never jumps over one silently.
std::size_t NextWordStart(std::size_t position) {
    const std::size_t size = textBuffer.size();
    if (position >= size) return size;
    if (textBuffer[position] == L'\n') return position + 1;
    const int wordClass = WordClass(textBuffer[position]);
    if (wordClass != 0) {
        while (position < size && textBuffer[position] != L'\n' &&
            WordClass(textBuffer[position]) == wordClass) ++position;
    }
    while (position < size && WordClass(textBuffer[position]) == 0) ++position;
    return position;
}

std::size_t PreviousWordStart(std::size_t position) {
    if (position == 0) return 0;
    if (textBuffer[position - 1] == L'\n') return position - 1;
    while (position > 0 && WordClass(textBuffer[position - 1]) == 0) --position;
    if (position == 0 || textBuffer[position - 1] == L'\n') return position;
    const int wordClass = WordClass(textBuffer[position - 1]);
    while (position > 0 && textBuffer[position - 1] != L'\n' &&
        WordClass(textBuffer[position - 1]) == wordClass) --position;
    return position;
}

void MoveCursorByWord(bool moveRight, bool extendSelection) {
    cursorPosition = moveRight ? NextWordStart(cursorPosition) : PreviousWordStart(cursorPosition);
    if (!extendSelection) selectionAnchor = cursorPosition;
    hasPreferredCaretX = caretAtRowEnd = false;
    pendingHighSurrogate = 0;
}

// Delete the selection, else the word span between the caret and the neighboring stop.
bool DeleteWord(bool forward) {
    hasPreferredCaretX = caretAtRowEnd = false;
    pendingHighSurrogate = 0;
    if (DeleteSelection()) return true;
    const std::size_t from = forward ? cursorPosition : PreviousWordStart(cursorPosition);
    const std::size_t to = forward ? NextWordStart(cursorPosition) : cursorPosition;
    if (from == to) return false;
    textBuffer.erase(from, to - from);
    cursorPosition = selectionAnchor = from;
    return true;
}

HFONT CreateCharacterFont() {
    return CreateFontW(
        32,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH,
        L"Segoe UI");
}

// Offsets still refer to the original buffer; a row excludes its newline. A "soft" row
// was cut because the text did not fit and flows straight into the next row.
struct TextLine {
    std::size_t start;
    std::size_t end;
    bool soft = false;
};

struct TextLayout {
    std::vector<TextLine> lines;
    int left = 24;
    int top = 24;
    int lineHeight = 40;
    int newlineWidth = 8;
};

// Tabs advance to the next multiple of this many space widths, measured from the
// row's left edge. All measuring and drawing below go through these helpers.
constexpr int kTabColumns = 4;

int TabWidth(HDC deviceContext) {
    SIZE size{};
    GetTextExtentPoint32W(deviceContext, L" ", 1, &size);
    const int width = size.cx * kTabColumns;
    return width > 0 ? width : 1;
}

// First tab in [from, to), or `to` when there is none. Bounded so a row never scans the rest of the buffer.
std::size_t NextTabOrEnd(std::size_t from, std::size_t to) {
    while (from < to && textBuffer[from] != L'\t') ++from;
    return from;
}

int NextTabStop(int x, int tabWidth) {
    return (x / tabWidth + 1) * tabWidth;
}

// Width from `start` (always a row start) to `end`, expanding tabs.
int MeasureText(HDC deviceContext, std::size_t start, std::size_t end);

bool IsBlank(wchar_t character) {
    return character == L' ' || character == L'\t';
}

// Cuts the line [start, end) into rows no wider than `width`. A row breaks after a run of
// blanks, which hang past the edge; a word wider than the window breaks where it fits.
// Every row holds at least one character, so a tiny window cannot loop forever.
void WrapLine(HDC deviceContext, std::size_t start, std::size_t end, int width,
    std::vector<TextLine>& rows) {
    std::size_t rowStart = start;
    for (;;) {
        // Probe outward by doubling, then bisect, so a very long line is not measured
        // whole for every row it produces.
        std::size_t fits = rowStart;
        std::size_t over = end;
        std::size_t probe = rowStart + 64;
        for (;;) {
            if (probe > end) probe = end;
            if (MeasureText(deviceContext, rowStart, probe) <= width) {
                fits = probe;
                if (probe == end) break;
                probe = rowStart + 2 * (probe - rowStart);
            } else {
                over = probe;
                break;
            }
        }
        if (fits == end) {
            rows.push_back({rowStart, end, false});
            return;
        }
        while (over - fits > 1) {
            const std::size_t middle = fits + (over - fits) / 2;
            if (MeasureText(deviceContext, rowStart, middle) <= width) fits = middle;
            else over = middle;
        }
        std::size_t cut = fits > rowStart ? fits : rowStart + 1;
        if (cut < end && IsHighSurrogate(textBuffer[cut - 1]) && IsLowSurrogate(textBuffer[cut])) {
            if (cut - 1 > rowStart) --cut;
            else ++cut;
        }
        std::size_t breakAt = 0;
        if (cut < end && IsBlank(textBuffer[cut])) {
            // The text so far fits and a blank follows: hang the blanks and break after them.
            breakAt = cut;
            while (breakAt < end && IsBlank(textBuffer[breakAt])) ++breakAt;
        } else {
            for (std::size_t position = cut; position > rowStart; --position) {
                if (IsBlank(textBuffer[position - 1])) {
                    breakAt = position;
                    break;
                }
            }
            if (breakAt == 0) breakAt = cut;
        }
        if (breakAt >= end) {
            rows.push_back({rowStart, end, false});
            return;
        }
        rows.push_back({rowStart, breakAt, true});
        rowStart = breakAt;
    }
}

TextLayout CreateTextLayout(HDC deviceContext) {
    TextLayout layout;
    TEXTMETRICW metrics{};
    if (GetTextMetricsW(deviceContext, &metrics)) {
        layout.lineHeight = metrics.tmHeight + 8;
        layout.newlineWidth = metrics.tmAveCharWidth > 8 ? metrics.tmAveCharWidth : 8;
    }

    // Rows wrap at the text area's width, the same width the viewport scrolls within.
    int wrapWidth = 0;
    if (wordWrap) {
        RECT client{};
        HWND window = WindowFromDC(deviceContext);
        if (window != nullptr && GetClientRect(window, &client)) {
            const int available = client.right - 2 * layout.left - 2;
            wrapWidth = available > 0 ? available : 1;
        }
    }
    const auto addLine = [&](std::size_t lineStart, std::size_t lineEnd) {
        if (wrapWidth > 0) WrapLine(deviceContext, lineStart, lineEnd, wrapWidth, layout.lines);
        else layout.lines.push_back({lineStart, lineEnd});
    };

    std::size_t start = 0;
    for (std::size_t index = 0; index < textBuffer.size(); ++index) {
        if (textBuffer[index] == L'\n') {
            addLine(start, index);
            start = index + 1;
        }
    }
    // Always retain the final line, including an empty row after Enter.
    addLine(start, textBuffer.size());
    return layout;
}

// The row holding `offset`. At a soft wrap boundary the offset belongs to the next row
// unless the caret was explicitly placed at the end of the upper one.
std::size_t RowOfOffset(const TextLayout& layout, std::size_t offset) {
    std::size_t row = 0;
    while (row + 1 < layout.lines.size() && offset > layout.lines[row].end) ++row;
    if (row + 1 < layout.lines.size() && layout.lines[row].soft && offset == layout.lines[row].end &&
        !(caretAtRowEnd && caretAffinityOffset == offset)) {
        ++row;
    }
    return row;
}

int EditorBottom(HWND window) {
    RECT area{};
    GetClientRect(window, &area);
    return area.bottom > 64 ? area.bottom - 64 : 0;
}

// Scrolling changes the view, never the stored text or selection offsets.
void UpdateViewport(HWND window, bool revealCaret, int rowDelta = 0, int pixelDelta = 0) {
    UpdateWindowTitle(window);
    HDC deviceContext = GetDC(window);
    if (deviceContext == nullptr) {
        return;
    }
    HFONT font = CreateCharacterFont();
    HGDIOBJ originalFont = font != nullptr ? SelectObject(deviceContext, font) : nullptr;
    const TextLayout layout = CreateTextLayout(deviceContext);
    RECT client{};
    GetClientRect(window, &client);
    const int rawWidth = client.right - 2 * layout.left - 2;
    const int visibleWidth = rawWidth > 0 ? rawWidth : 1;
    if (wordWrap) {
        // Wrapped rows always fit the window, so there is nothing to scroll sideways.
        horizontalOffset = 0;
    } else {
        int widestLine = 0;
        for (const TextLine& line : layout.lines) {
            const int width = MeasureText(deviceContext, line.start, line.end);
            if (width > widestLine) widestLine = width;
        }
        // Include the selected-newline block at the right end of a row.
        const long long extent = static_cast<long long>(widestLine) + layout.newlineWidth;
        const long long maxOffset = extent > visibleWidth ? extent - visibleWidth : 0;
        const int maxHorizontal = maxOffset > (std::numeric_limits<int>::max)() ?
            (std::numeric_limits<int>::max)() : static_cast<int>(maxOffset);
        const long long requestedOffset = static_cast<long long>(horizontalOffset) + pixelDelta;
        horizontalOffset = requestedOffset < 0 ? 0 :
            requestedOffset > maxHorizontal ? maxHorizontal : static_cast<int>(requestedOffset);
    }
    const int availableHeight = EditorBottom(window) - layout.top;
    const std::size_t visibleRows = availableHeight >= layout.lineHeight ?
        static_cast<std::size_t>(availableHeight / layout.lineHeight) : 1;
    const std::size_t maxFirstRow = layout.lines.size() > visibleRows ?
        layout.lines.size() - visibleRows : 0;
    if (firstVisibleRow > maxFirstRow) {
        firstVisibleRow = maxFirstRow;
    }
    if (rowDelta < 0) {
        const std::size_t distance = static_cast<std::size_t>(-rowDelta);
        firstVisibleRow = distance > firstVisibleRow ? 0 : firstVisibleRow - distance;
    } else if (rowDelta > 0) {
        const std::size_t distance = static_cast<std::size_t>(rowDelta);
        firstVisibleRow += distance > maxFirstRow - firstVisibleRow ?
            maxFirstRow - firstVisibleRow : distance;
    }
    if (revealCaret) {
        const std::size_t caretRow = RowOfOffset(layout, cursorPosition);
        if (caretRow < firstVisibleRow) {
            firstVisibleRow = caretRow;
        } else if (caretRow - firstVisibleRow >= visibleRows) {
            firstVisibleRow = caretRow - visibleRows + 1;
        }
        if (!wordWrap) {
            const int caretX = MeasureText(deviceContext, layout.lines[caretRow].start, cursorPosition);
            if (caretX < horizontalOffset) horizontalOffset = caretX;
            else if (caretX - horizontalOffset > visibleWidth) horizontalOffset = caretX - visibleWidth;
        }
    }
    topRowOffset = layout.lines[firstVisibleRow].start;
    if (originalFont != nullptr) {
        SelectObject(deviceContext, originalFont);
    }
    if (font != nullptr) {
        DeleteObject(font);
    }
    ReleaseDC(window, deviceContext);
    InvalidateRect(window, nullptr, TRUE);
}

// After a reflow (resize or wrap toggle) rows are renumbered, so keep the same text at the
// top of the view by looking up the row that holds the remembered offset.
void ReflowView(HWND window) {
    HDC deviceContext = GetDC(window);
    if (deviceContext != nullptr) {
        HFONT font = CreateCharacterFont();
        HGDIOBJ originalFont = font != nullptr ? SelectObject(deviceContext, font) : nullptr;
        const TextLayout layout = CreateTextLayout(deviceContext);
        std::size_t row = 0;
        while (row + 1 < layout.lines.size() && topRowOffset >= layout.lines[row + 1].start) ++row;
        firstVisibleRow = row;
        if (originalFont != nullptr) SelectObject(deviceContext, originalFont);
        if (font != nullptr) DeleteObject(font);
        ReleaseDC(window, deviceContext);
    }
    UpdateViewport(window, true);
}

void ToggleWordWrap(HWND window) {
    wordWrap = !wordWrap;
    horizontalOffset = 0;
    hasPreferredCaretX = caretAtRowEnd = false;
    ReflowView(window);
}

int MeasureText(HDC deviceContext, std::size_t start, std::size_t end) {
    const int tabWidth = TabWidth(deviceContext);
    int x = 0;
    std::size_t position = start;
    while (position < end) {
        const std::size_t segmentEnd = NextTabOrEnd(position, end);
        if (segmentEnd > position) {
            SIZE size{};
            GetTextExtentPoint32W(deviceContext, textBuffer.c_str() + position,
                static_cast<int>(segmentEnd - position), &size);
            x += size.cx;
        }
        if (segmentEnd < end) x = NextTabStop(x, tabWidth);
        position = segmentEnd + 1;
    }
    return x;
}

// Draws [from, to) of a row so it lines up with MeasureText, skipping the tab characters themselves.
void DrawTabbedText(HDC deviceContext, int rowLeft, int y, std::size_t rowStart,
    std::size_t from, std::size_t to) {
    const int tabWidth = TabWidth(deviceContext);
    int x = MeasureText(deviceContext, rowStart, from);
    std::size_t position = from;
    while (position < to) {
        const std::size_t segmentEnd = NextTabOrEnd(position, to);
        if (segmentEnd > position) {
            const int count = static_cast<int>(segmentEnd - position);
            TextOutW(deviceContext, rowLeft + x, y, textBuffer.c_str() + position, count);
            SIZE size{};
            GetTextExtentPoint32W(deviceContext, textBuffer.c_str() + position, count, &size);
            x += size.cx;
        }
        if (segmentEnd < to) x = NextTabStop(x, tabWidth);
        position = segmentEnd + 1;
    }
}

std::size_t FindOffsetInRow(HDC deviceContext, const TextLine& line,
    int left, int targetX) {
    if (line.start == line.end || targetX <= left) {
        return line.start;
    }
    // Ask GDI for every cumulative width in one pass, rather than measuring
    // each successively longer prefix separately.
    // widths[i] is the x after character i; tabs jump to the next tab stop.
    std::vector<int> widths(line.end - line.start);
    const int tabWidth = TabWidth(deviceContext);
    int rowX = 0;
    for (std::size_t position = line.start; position < line.end;) {
        const std::size_t segmentEnd = NextTabOrEnd(position, line.end);
        if (segmentEnd > position) {
            const std::size_t count = segmentEnd - position;
            std::vector<int> segmentWidths(count);
            SIZE size{};
            if (!GetTextExtentExPointW(deviceContext, textBuffer.c_str() + position,
                static_cast<int>(count), 0, nullptr, segmentWidths.data(), &size)) {
                return line.start;
            }
            for (std::size_t index = 0; index < count; ++index) {
                widths[position - line.start + index] = rowX + segmentWidths[index];
            }
            rowX += segmentWidths[count - 1];
        }
        if (segmentEnd < line.end) {
            rowX = NextTabStop(rowX, tabWidth);
            widths[segmentEnd - line.start] = rowX;
        }
        position = segmentEnd + 1;
    }
    std::size_t previousPosition = line.start;
    int previousX = left;
    for (std::size_t boundary = line.start + 1; boundary <= line.end; ++boundary) {
        if (IsHighSurrogate(textBuffer[boundary - 1]) &&
            boundary < line.end && IsLowSurrogate(textBuffer[boundary])) {
            ++boundary;
        }
        const int boundaryX = left + widths[boundary - line.start - 1];
        if (targetX < previousX + (boundaryX - previousX) / 2) {
            return previousPosition;
        }
        previousPosition = boundary;
        previousX = boundaryX;
    }
    return line.end;
}

// Nearest character gap in a row. Landing on the end of a wrapped row keeps the caret
// visually on that row (otherwise it would jump to the start of the next one).
std::size_t FindCursorPositionInLine(HDC deviceContext, const TextLine& line,
    int left, int targetX) {
    const std::size_t position = FindOffsetInRow(deviceContext, line, left, targetX);
    caretAtRowEnd = line.soft && position == line.end;
    caretAffinityOffset = position;
    return position;
}

std::size_t FindCursorPositionAtPoint(HDC deviceContext, int mouseX, int mouseY) {
    const TextLayout layout = CreateTextLayout(deviceContext);
    std::size_t row = firstVisibleRow;
    if (mouseY > layout.top) {
        row += static_cast<std::size_t>((mouseY - layout.top) / layout.lineHeight);
    }
    if (row >= layout.lines.size()) {
        row = layout.lines.size() - 1;
    }
    return FindCursorPositionInLine(deviceContext, layout.lines[row], layout.left - horizontalOffset, mouseX);
}

// Home/End go to the start/end of the caret's visual row; without wrapping that is the whole line.
void MoveCursorToRowBoundary(HWND window, bool toEnd, bool extendSelection) {
    HDC deviceContext = GetDC(window);
    if (deviceContext == nullptr) return;
    HFONT font = CreateCharacterFont();
    HGDIOBJ originalFont = font != nullptr ? SelectObject(deviceContext, font) : nullptr;
    const TextLayout layout = CreateTextLayout(deviceContext);
    const TextLine& row = layout.lines[RowOfOffset(layout, cursorPosition)];
    cursorPosition = toEnd ? row.end : row.start;
    if (!extendSelection) selectionAnchor = cursorPosition;
    hasPreferredCaretX = false;
    // End on a wrapped row stays visually on that row even though the offset also starts the next one.
    caretAtRowEnd = toEnd && row.soft;
    caretAffinityOffset = cursorPosition;
    pendingHighSurrogate = 0;
    if (originalFont != nullptr) SelectObject(deviceContext, originalFont);
    if (font != nullptr) DeleteObject(font);
    ReleaseDC(window, deviceContext);
}

bool MoveCursorVertically(HWND window, bool moveDown, bool extendSelection, bool byPage = false) {
    HDC deviceContext = GetDC(window);
    if (deviceContext == nullptr) {
        return false;
    }
    HFONT font = CreateCharacterFont();
    HGDIOBJ originalFont = font != nullptr ? SelectObject(deviceContext, font) : nullptr;
    const TextLayout layout = CreateTextLayout(deviceContext);
    std::size_t row = RowOfOffset(layout, cursorPosition);
    if (!hasPreferredCaretX) {
        preferredCaretX = layout.left + MeasureText(deviceContext, layout.lines[row].start, cursorPosition);
        hasPreferredCaretX = true;
    }
    const std::size_t previousPosition = cursorPosition;
    const std::size_t previousAnchor = selectionAnchor;
    std::size_t step = 1;
    if (byPage) {
        const int pageRows = (EditorBottom(window) - layout.top) / layout.lineHeight;
        step = pageRows > 1 ? static_cast<std::size_t>(pageRows) : 1;
    }
    if (moveDown) {
        row = layout.lines.size() - 1 - row < step ? layout.lines.size() - 1 : row + step;
    } else {
        row = row < step ? 0 : row - step;
    }
    cursorPosition = FindCursorPositionInLine(deviceContext, layout.lines[row],
        layout.left, preferredCaretX);
    if (!extendSelection) {
        selectionAnchor = cursorPosition;
    }
    if (originalFont != nullptr) {
        SelectObject(deviceContext, originalFont);
    }
    if (font != nullptr) {
        DeleteObject(font);
    }
    ReleaseDC(window, deviceContext);
    pendingHighSurrogate = L'\0';
    return previousPosition != cursorPosition || previousAnchor != selectionAnchor;
}

std::size_t FindCursorPositionAtMouse(HWND window, int mouseX, int mouseY) {
    hasPreferredCaretX = caretAtRowEnd = false;
    HDC deviceContext = GetDC(window);
    if (deviceContext == nullptr) {
        return cursorPosition;
    }
    HFONT characterFont = CreateCharacterFont();
    HGDIOBJ originalFont = nullptr;
    if (characterFont != nullptr) {
        originalFont = SelectObject(deviceContext, characterFont);
    }
    // A captured drag can leave the editor; keep it on the visible edge.
    const int bottom = EditorBottom(window);
    if (mouseY >= bottom) {
        mouseY = bottom > 0 ? bottom - 1 : 0;
    }
    RECT client{};
    GetClientRect(window, &client);
    const int leftMargin = 24;
    const int rightEdge = client.right > 2 * leftMargin ? client.right - leftMargin : leftMargin;
    if (mouseX < leftMargin) mouseX = leftMargin;
    if (mouseX > rightEdge) mouseX = rightEdge;
    const std::size_t position = FindCursorPositionAtPoint(deviceContext, mouseX, mouseY);
    if (originalFont != nullptr) {
        SelectObject(deviceContext, originalFont);
    }
    if (characterFont != nullptr) {
        DeleteObject(characterFont);
    }
    ReleaseDC(window, deviceContext);
    return position;
}

// Characters in [from, to), counting a surrogate pair as one.
std::size_t CountCharacters(std::size_t from, std::size_t to) {
    std::size_t count = 0;
    for (std::size_t index = from; index < to; ++index) {
        if (!IsLowSurrogate(textBuffer[index])) ++count;
    }
    return count;
}

// Footer readout: 1-based line and column of the caret, plus the selection size.
std::wstring CaretStatusText() {
    std::size_t line = 1;
    std::size_t lineStart = 0;
    for (std::size_t index = 0; index < cursorPosition; ++index) {
        if (textBuffer[index] == L'\n') {
            ++line;
            lineStart = index + 1;
        }
    }
    std::wstring text = L"Ln " + std::to_wstring(line) + L", Col " +
        std::to_wstring(CountCharacters(lineStart, cursorPosition) + 1);
    if (HasSelection()) {
        const std::size_t from = cursorPosition < selectionAnchor ? cursorPosition : selectionAnchor;
        const std::size_t to = cursorPosition < selectionAnchor ? selectionAnchor : cursorPosition;
        text += L"  (" + std::to_wstring(CountCharacters(from, to)) + L" selected)";
    }
    return text;
}

// While the left button is held, a timer keeps scrolling and extending the selection
// from the last mouse point, so a drag that rests outside the editor still moves.
constexpr UINT_PTR kAutoscrollTimer = 1;
constexpr UINT kAutoscrollMs = 50;
constexpr int kAutoscrollMaxRows = 10;
constexpr int kAutoscrollMaxPixels = 160;
int dragMouseX{};
int dragMouseY{};
bool autoscrollTimerRunning{};

void StopAutoscroll(HWND window) {
    if (autoscrollTimerRunning) {
        KillTimer(window, kAutoscrollTimer);
        autoscrollTimerRunning = false;
    }
}

// Extends the selection to the mouse point. With `scroll`, a point outside the editor
// first scrolls the view, faster the farther out it is. Returns whether the point is outside.
bool DragSelectTo(HWND window, int mouseX, int mouseY, bool scroll) {
    dragMouseX = mouseX;
    dragMouseY = mouseY;
    int rowDelta = 0;
    int pixelDelta = 0;
    HDC deviceContext = GetDC(window);
    if (deviceContext != nullptr) {
        HFONT font = CreateCharacterFont();
        HGDIOBJ originalFont = font != nullptr ? SelectObject(deviceContext, font) : nullptr;
        const TextLayout layout = CreateTextLayout(deviceContext);
        RECT client{};
        GetClientRect(window, &client);
        const int bottom = EditorBottom(window);
        const int right = client.right - layout.left;
        const int row = layout.lineHeight > 0 ? layout.lineHeight : 1;
        if (mouseY < layout.top) rowDelta = -(1 + (layout.top - mouseY) / row);
        else if (mouseY >= bottom) rowDelta = 1 + (mouseY - bottom) / row;
        // Wrapped text never scrolls sideways.
        if (wordWrap) pixelDelta = 0;
        else if (mouseX < layout.left) pixelDelta = -(16 + (layout.left - mouseX) / 2);
        else if (mouseX > right) pixelDelta = 16 + (mouseX - right) / 2;
        if (rowDelta > kAutoscrollMaxRows) rowDelta = kAutoscrollMaxRows;
        if (rowDelta < -kAutoscrollMaxRows) rowDelta = -kAutoscrollMaxRows;
        if (pixelDelta > kAutoscrollMaxPixels) pixelDelta = kAutoscrollMaxPixels;
        if (pixelDelta < -kAutoscrollMaxPixels) pixelDelta = -kAutoscrollMaxPixels;
        if (originalFont != nullptr) SelectObject(deviceContext, originalFont);
        if (font != nullptr) DeleteObject(font);
        ReleaseDC(window, deviceContext);
    }
    if (scroll && (rowDelta != 0 || pixelDelta != 0)) {
        UpdateViewport(window, false, rowDelta, pixelDelta);
    }
    const std::size_t newPosition = FindCursorPositionAtMouse(window, mouseX, mouseY);
    if (newPosition != cursorPosition) {
        cursorPosition = newPosition;
        UpdateViewport(window, false);
    }
    return rowDelta != 0 || pixelDelta != 0;
}

void PaintDocument(HDC deviceContext, const RECT& editorArea) {
    const int savedContext = SaveDC(deviceContext);
    IntersectClipRect(deviceContext, editorArea.left, editorArea.top,
        editorArea.right, editorArea.bottom);
    SetBkMode(deviceContext, TRANSPARENT);
    const TextLayout layout = CreateTextLayout(deviceContext);
    const int drawingLeft = layout.left - horizontalOffset;
    const std::size_t caretRow = RowOfOffset(layout, cursorPosition);
    IntersectClipRect(deviceContext, layout.left, editorArea.top,
        editorArea.right > 2 * layout.left ? editorArea.right - layout.left : layout.left + 1,
        editorArea.bottom);
    for (std::size_t row = firstVisibleRow; row < layout.lines.size(); ++row) {
        const int y = layout.top + static_cast<int>(row - firstVisibleRow) * layout.lineHeight;
        if (y >= editorArea.bottom) {
            break;
        }
        const TextLine& line = layout.lines[row];
        SetTextColor(deviceContext, GetSysColor(COLOR_WINDOWTEXT));
        DrawTabbedText(deviceContext, drawingLeft, y, line.start, line.start, line.end);

        if (HasSelection()) {
            const std::size_t start = SelectionStart() > line.start ? SelectionStart() : line.start;
            const std::size_t end = SelectionEnd() < line.end ? SelectionEnd() : line.end;
            // Only a real line break gets the small highlight block; a soft wrap has none.
            const bool selectedNewline = !line.soft && line.end < textBuffer.size() &&
                SelectionStart() <= line.end && SelectionEnd() > line.end;
            if (start < end || selectedNewline) {
                const int left = drawingLeft + MeasureText(deviceContext, line.start, start);
                const int right = drawingLeft + MeasureText(deviceContext, line.start, end) +
                    (selectedNewline ? layout.newlineWidth : 0);
                RECT highlight{left, y, right, y + layout.lineHeight};
                FillRect(deviceContext, &highlight, GetSysColorBrush(COLOR_HIGHLIGHT));
                if (start < end) {
                    SetTextColor(deviceContext, GetSysColor(COLOR_HIGHLIGHTTEXT));
                    DrawTabbedText(deviceContext, drawingLeft, y, line.start, start, end);
                }
            }
        }

        if (row == caretRow) {
            const int x = drawingLeft + MeasureText(deviceContext, line.start, cursorPosition);
            RECT caret{x, y, x + 2, y + layout.lineHeight - 8};
            FillRect(deviceContext, &caret, GetSysColorBrush(COLOR_WINDOWTEXT));
        }
    }
    RestoreDC(deviceContext, savedContext);
}

// Per-unit lowercase; length-preserving, so offsets in the folded copy are valid in the buffer.
std::wstring FoldCase(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

bool SelectionMatchesSearch() {
    const std::size_t start = SelectionStart();
    const std::size_t length = SelectionEnd() - start;
    if (!HasSelection() || length != searchTerm.size()) return false;
    if (matchCase) return textBuffer.compare(start, length, searchTerm) == 0;
    return FoldCase(textBuffer.substr(start, length)) == FoldCase(searchTerm);
}

void FindMatch(HWND window, bool backwards = false) {
    if (searchTerm.empty()) return;
    const std::wstring foldedBuffer = matchCase ? std::wstring() : FoldCase(textBuffer);
    const std::wstring& haystack = matchCase ? textBuffer : foldedBuffer;
    const std::wstring needle = matchCase ? searchTerm : FoldCase(searchTerm);
    std::size_t match = std::wstring::npos;
    if (backwards) {
        const std::size_t boundary = HasSelection() ? SelectionStart() : cursorPosition;
        if (boundary >= needle.size()) {
            match = haystack.rfind(needle, boundary - needle.size());
        }
        if (match == std::wstring::npos) match = haystack.rfind(needle);
    } else {
        const std::size_t start = HasSelection() ? SelectionEnd() : cursorPosition;
        match = haystack.find(needle, start);
        if (match == std::wstring::npos) match = haystack.find(needle);
    }
    if (match == std::wstring::npos) {
        const std::wstring explanation = std::wstring(matchCase ? L"No exact match found for:\n" :
            L"No match found for:\n") + searchTerm;
        MessageBoxW(findDialog != nullptr ? findDialog : window, explanation.c_str(),
            L"Find", MB_OK | MB_ICONINFORMATION);
        return;
    }
    selectionAnchor = match;
    cursorPosition = match + searchTerm.size();
    hasPreferredCaretX = caretAtRowEnd = false;
    pendingHighSurrogate = 0;
    UpdateViewport(window, true);
}

void ReplaceMatch(HWND window) {
    if (searchTerm.empty()) return;
    const std::size_t start = SelectionStart();
    const std::size_t length = SelectionEnd() - start;
    if (!SelectionMatchesSearch()) {
        // First locate a match; a second Replace action explicitly changes it.
        FindMatch(window);
        return;
    }
    EditSnapshot before = CaptureEditSnapshot();
    textBuffer.replace(start, length, replacementTerm);
    cursorPosition = start + replacementTerm.size();
    selectionAnchor = cursorPosition;
    pendingHighSurrogate = 0;
    hasPreferredCaretX = caretAtRowEnd = false;
    RecordEdit(std::move(before));
    UpdateViewport(window, true);
}

void ReplaceAllMatches(HWND window) {
    if (searchTerm.empty()) return;
    std::wstring result;
    std::size_t sourcePosition = 0;
    std::size_t lastReplacementEnd = 0;
    std::size_t count = 0;
    const std::wstring foldedBuffer = matchCase ? std::wstring() : FoldCase(textBuffer);
    const std::wstring& haystack = matchCase ? textBuffer : foldedBuffer;
    const std::wstring needle = matchCase ? searchTerm : FoldCase(searchTerm);
    for (;;) {
        const std::size_t match = haystack.find(needle, sourcePosition);
        if (match == std::wstring::npos) break;
        result.append(textBuffer, sourcePosition, match - sourcePosition);
        result += replacementTerm;
        lastReplacementEnd = result.size();
        sourcePosition = match + searchTerm.size();
        ++count;
    }
    if (count != 0) {
        result.append(textBuffer, sourcePosition, std::wstring::npos);
        if (result != textBuffer) {
            EditSnapshot before = CaptureEditSnapshot();
            textBuffer = std::move(result);
            cursorPosition = lastReplacementEnd;
            selectionAnchor = cursorPosition;
            pendingHighSurrogate = 0;
            hasPreferredCaretX = caretAtRowEnd = false;
            RecordEdit(std::move(before));
            UpdateViewport(window, true);
        }
    }
    const std::wstring explanation = std::to_wstring(count) +
        (count == 1 ? L" occurrence matched." : L" occurrences matched.");
    MessageBoxW(findDialog != nullptr ? findDialog : window, explanation.c_str(),
        L"Replace All", MB_OK | MB_ICONINFORMATION);
}

void ShowFindDialog(HWND window, bool replace = false) {
    if (findDialog != nullptr) {
        if (replaceDialogMode == replace) {
            SetForegroundWindow(findDialog);
            return;
        }
        SendMessageW(findDialog, WM_CLOSE, 0, 0);
    }
    if (findMessage == 0) findMessage = RegisterWindowMessageW(FINDMSGSTRINGW);
    if (findMessage == 0) {
        MessageBoxW(window, L"Could not register the Find dialog message.", L"Find", MB_OK | MB_ICONERROR);
        return;
    }
    // Common-dialog state and its editable buffer must outlive this function.
    findRequest = {};
    findText[searchTerm.copy(findText, 1023)] = L'\0';
    findRequest.lStructSize = sizeof(findRequest);
    findRequest.hwndOwner = window;
    findRequest.Flags = FR_DOWN | (matchCase ? FR_MATCHCASE : 0) | FR_HIDEUPDOWN | FR_HIDEWHOLEWORD;
    findRequest.lpstrFindWhat = findText;
    findRequest.wFindWhatLen = static_cast<WORD>(sizeof(findText) / sizeof(findText[0]));
    replaceDialogMode = replace;
    if (replace) {
        replacementText[replacementTerm.copy(replacementText, 1023)] = L'\0';
        findRequest.lpstrReplaceWith = replacementText;
        findRequest.wReplaceWithLen = static_cast<WORD>(sizeof(replacementText) / sizeof(replacementText[0]));
    }
    findDialog = replace ? ReplaceTextW(&findRequest) : FindTextW(&findRequest);
    if (findDialog == nullptr) {
        MessageBoxW(window, L"Could not open the Find dialog.", L"Find", MB_OK | MB_ICONERROR);
    }
}

// Edit actions shared by keyboard shortcuts and the Edit menu.
void UndoOrRedo(HWND window, bool redo) {
    if (RestoreHistory(redo)) UpdateViewport(window, true);
}

void CutSelection(HWND window) {
    EditSnapshot before = CaptureEditSnapshot();
    if (CopySelectionToClipboard(window) && DeleteSelection()) {
        RecordEdit(std::move(before));
        UpdateViewport(window, true);
    }
}

void PasteClipboard(HWND window) {
    if (PasteFromClipboard(window)) UpdateViewport(window, true);
}

void SelectAllText(HWND window) {
    selectionAnchor = 0;
    cursorPosition = textBuffer.size();
    hasPreferredCaretX = caretAtRowEnd = false;
    pendingHighSurrogate = 0;
    UpdateViewport(window, true);
}

void FindNextOrOpenDialog(HWND window, bool backwards) {
    if (searchTerm.empty()) ShowFindDialog(window);
    else FindMatch(window, backwards);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (findMessage != 0 && message == findMessage) {
        const auto* request = reinterpret_cast<const FINDREPLACEW*>(lParam);
        if ((request->Flags & FR_DIALOGTERM) != 0) {
            findDialog = nullptr;
            SetFocus(window);
        } else if ((request->Flags & FR_FINDNEXT) != 0) {
            searchTerm = request->lpstrFindWhat;
            matchCase = (request->Flags & FR_MATCHCASE) != 0;
            FindMatch(window);
        } else if ((request->Flags & FR_REPLACE) != 0) {
            searchTerm = request->lpstrFindWhat;
            replacementTerm = request->lpstrReplaceWith;
            matchCase = (request->Flags & FR_MATCHCASE) != 0;
            ReplaceMatch(window);
        } else if ((request->Flags & FR_REPLACEALL) != 0) {
            searchTerm = request->lpstrFindWhat;
            replacementTerm = request->lpstrReplaceWith;
            matchCase = (request->Flags & FR_MATCHCASE) != 0;
            ReplaceAllMatches(window);
        }
        return 0;
    }
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kFileNew: NewDocument(window); return 0;
        case kFileOpen: OpenDocument(window); return 0;
        case kFileSave: SaveDocument(window); return 0;
        case kFileHylic: SaveDocument(window, true); return 0;
        case kFileExit: SendMessageW(window, WM_CLOSE, 0, 0); return 0;
        case kEditUndo: UndoOrRedo(window, false); return 0;
        case kEditRedo: UndoOrRedo(window, true); return 0;
        case kEditCut: CutSelection(window); return 0;
        case kEditCopy: CopySelectionToClipboard(window); return 0;
        case kEditPaste: PasteClipboard(window); return 0;
        case kEditSelectAll: SelectAllText(window); return 0;
        case kEditFind: ShowFindDialog(window); return 0;
        case kEditFindNext: FindNextOrOpenDialog(window, false); return 0;
        case kEditFindPrevious: FindNextOrOpenDialog(window, true); return 0;
        case kEditReplace: ShowFindDialog(window, true); return 0;
        case kViewWordWrap: ToggleWordWrap(window); return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) == 0) {
            ToggleWordWrap(window);
            return 0;
        }
        break;
    case WM_INITMENUPOPUP: {
        // Gray out Edit commands that would do nothing; File items are unaffected.
        HMENU popup = reinterpret_cast<HMENU>(wParam);
        const auto setEnabled = [popup](UINT command, bool enabled) {
            EnableMenuItem(popup, command, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
        };
        setEnabled(kEditUndo, !undoHistory.empty());
        setEnabled(kEditRedo, !redoHistory.empty());
        setEnabled(kEditCut, HasSelection());
        setEnabled(kEditCopy, HasSelection());
        setEnabled(kEditPaste, IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE);
        setEnabled(kEditSelectAll, !textBuffer.empty());
        CheckMenuItem(popup, kViewWordWrap, MF_BYCOMMAND | (wordWrap ? MF_CHECKED : MF_UNCHECKED));
        return 0;
    }
    case WM_SIZE:
        ReflowView(window);
        return 0;

    case WM_MOUSEHWHEEL:
    case WM_MOUSEWHEEL: {
        if (message == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0) {
            horizontalWheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam) *
                (message == WM_MOUSEHWHEEL ? 1 : -1);
            const int notches = horizontalWheelRemainder / WHEEL_DELTA;
            horizontalWheelRemainder %= WHEEL_DELTA;
            if (notches != 0) UpdateViewport(window, false, 0, notches * 96);
            return 0;
        }
        wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
        const int notches = wheelRemainder / WHEEL_DELTA;
        wheelRemainder %= WHEEL_DELTA;
        UINT rowsPerNotch = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &rowsPerNotch, 0);
        // Page-scroll preference uses one editor viewport per notch.
        if (rowsPerNotch == WHEEL_PAGESCROLL) {
            HDC dc = GetDC(window);
            if (dc != nullptr) {
                HFONT font = CreateCharacterFont();
                HGDIOBJ oldFont = font != nullptr ? SelectObject(dc, font) : nullptr;
                const TextLayout layout = CreateTextLayout(dc);
                const int rows = (EditorBottom(window) - layout.top) / layout.lineHeight;
                rowsPerNotch = rows > 0 ? static_cast<UINT>(rows) : 1;
                if (oldFont != nullptr) SelectObject(dc, oldFont);
                if (font != nullptr) DeleteObject(font);
                ReleaseDC(window, dc);
            } else {
                rowsPerNotch = 3;
            }
        }
        if (notches != 0 && rowsPerNotch != 0) {
            // Bound unusual system settings to avoid overflowing the signed delta.
            const int rows = rowsPerNotch > 1000 ? 1000 : static_cast<int>(rowsPerNotch);
            UpdateViewport(window, false, -notches * rows);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(window, &paint);

        RECT clientArea{};
        GetClientRect(window, &clientArea);

        RECT editorArea = clientArea;
        editorArea.bottom = EditorBottom(window);
        HFONT characterFont = CreateCharacterFont();
        HGDIOBJ originalFont = nullptr;
        if (characterFont != nullptr) {
            originalFont = SelectObject(deviceContext, characterFont);
        }
        PaintDocument(deviceContext, editorArea);
        if (originalFont != nullptr) {
            SelectObject(deviceContext, originalFont);
        }
        if (characterFont != nullptr) {
            DeleteObject(characterFont);
        }

        // The footer strip is two lines: shortcut hint, then the caret readout on the right.
        RECT instructionsArea = clientArea;
        instructionsArea.top = editorArea.bottom;
        instructionsArea.bottom = instructionsArea.top + (clientArea.bottom - instructionsArea.top) / 2;
        DrawTextW(deviceContext,
            L"Ctrl+F: Find. Ctrl+H: Replace. F3 / Shift+F3: next / previous. Ctrl+S: save. Ctrl+Z/Y: undo/redo.",
            -1, &instructionsArea, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT statusArea = clientArea;
        statusArea.top = instructionsArea.bottom;
        statusArea.right -= 24;
        const std::wstring status = CaretStatusText();
        DrawTextW(deviceContext, status.c_str(), -1, &statusArea,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        EndPaint(window, &paint);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (GET_Y_LPARAM(lParam) >= EditorBottom(window)) {
            return 0;
        }
        SetFocus(window);
        cursorPosition = FindCursorPositionAtMouse(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        selectionAnchor = cursorPosition;
        SetCapture(window);
        UpdateViewport(window, false);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (GetCapture() == window && (wParam & MK_LBUTTON) != 0) {
            const bool outside = DragSelectTo(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false);
            if (!outside) {
                StopAutoscroll(window);
            } else if (!autoscrollTimerRunning) {
                autoscrollTimerRunning = SetTimer(window, kAutoscrollTimer, kAutoscrollMs, nullptr) != 0;
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam != kAutoscrollTimer) break;
        if (GetCapture() != window) StopAutoscroll(window);
        else DragSelectTo(window, dragMouseX, dragMouseY, true);
        return 0;

    case WM_CAPTURECHANGED:
        StopAutoscroll(window);
        return 0;

    case WM_LBUTTONUP: {
        if (GetCapture() == window) {
            StopAutoscroll(window);
            DragSelectTo(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false);
            ReleaseCapture();
        }
        return 0;
    }

    case WM_CHAR: {
        const wchar_t character = static_cast<wchar_t>(wParam);

        if (character == L'\b') {
            // Ctrl+H also translates to ASCII Backspace; it is a dialog shortcut.
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) return 0;
            if (pendingHighSurrogate != L'\0') {
                pendingHighSurrogate = L'\0';
                return 0;
            }

            EditSnapshot before = CaptureEditSnapshot();
            if (DeleteSelection() || DeleteCharacterBeforeCursor()) {
                selectionAnchor = cursorPosition;
                RecordEdit(std::move(before));
                UpdateViewport(window, true);
            }
            return 0;
        }

        if (IsHighSurrogate(character)) {
            pendingHighSurrogate = character;
            return 0;
        }

        if (IsLowSurrogate(character)) {
            if (pendingHighSurrogate != L'\0') {
                EditSnapshot before = CaptureEditSnapshot();
                DeleteSelection();
                textBuffer.insert(cursorPosition, 1, pendingHighSurrogate);
                textBuffer.insert(cursorPosition + 1, 1, character);
                cursorPosition += 2;
                selectionAnchor = cursorPosition;
                pendingHighSurrogate = L'\0';
                RecordTypedEdit(std::move(before));
                UpdateViewport(window, true);
            }
            return 0;
        }

        pendingHighSurrogate = L'\0';
        if (character == L'\r' || character == L'\t' || (character >= L' ' && character != 0x7F)) {
            EditSnapshot before = CaptureEditSnapshot();
            DeleteSelection();
            textBuffer.insert(cursorPosition, 1, character == L'\r' ? L'\n' : character);
            ++cursorPosition;
            selectionAnchor = cursorPosition;
            // Enter always ends the run; other characters extend it.
            if (character == L'\r') RecordEdit(std::move(before));
            else RecordTypedEdit(std::move(before));
            UpdateViewport(window, true);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_F3) {
            FindNextOrOpenDialog(window, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
        if ((wParam == VK_HOME || wParam == VK_END) &&
            (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
            (GetKeyState(VK_MENU) & 0x8000) == 0) {
            MoveCursorToRowBoundary(window, wParam == VK_END, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            UpdateViewport(window, true);
            return 0;
        }
        if (wParam == VK_PRIOR || wParam == VK_NEXT) {
            const bool extendSelection = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            MoveCursorVertically(window, wParam == VK_NEXT, extendSelection, true);
            UpdateViewport(window, true);
            return 0;
        }
        if (wParam == VK_UP || wParam == VK_DOWN) {
            const bool extendSelection = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            MoveCursorVertically(window, wParam == VK_DOWN, extendSelection);
            UpdateViewport(window, true);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (GetKeyState(VK_MENU) & 0x8000) == 0) {
            if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                MoveCursorByWord(wParam == VK_RIGHT, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                UpdateViewport(window, true);
                return 0;
            }
            if (wParam == VK_BACK || wParam == VK_DELETE) {
                EditSnapshot before = CaptureEditSnapshot();
                if (DeleteWord(wParam == VK_DELETE)) {
                    RecordEdit(std::move(before));
                    UpdateViewport(window, true);
                }
                return 0;
            }
            if (wParam == 'F') {
                ShowFindDialog(window);
                return 0;
            }
            if (wParam == 'H') {
                ShowFindDialog(window, true);
                return 0;
            }
            if (wParam == 'A') {
                SelectAllText(window);
                return 0;
            }
            if (wParam == VK_HOME || wParam == VK_END) {
                cursorPosition = wParam == VK_HOME ? 0 : textBuffer.size();
                if ((GetKeyState(VK_SHIFT) & 0x8000) == 0) selectionAnchor = cursorPosition;
                hasPreferredCaretX = caretAtRowEnd = false;
                pendingHighSurrogate = 0;
                UpdateViewport(window, true);
                return 0;
            }
            if (wParam == 'N') {
                NewDocument(window);
                return 0;
            }
            if (wParam == 'Z' || wParam == 'Y') {
                UndoOrRedo(window, wParam == 'Y');
                return 0;
            }
            if (wParam == 'O') {
                OpenDocument(window);
                return 0;
            }
            if (wParam == 'S') {
                SaveDocument(window, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                return 0;
            }
            if (wParam == 'C') {
                CopySelectionToClipboard(window);
                return 0;
            }
            if (wParam == 'X') {
                CutSelection(window);
                return 0;
            }
            if (wParam == 'V') {
                PasteClipboard(window);
                return 0;
            }
        }
        if (wParam == VK_LEFT) {
            const bool extendSelection = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            MoveCursorWithKey(false, extendSelection);
            UpdateViewport(window, true);
            return 0;
        }
        if (wParam == VK_RIGHT) {
            const bool extendSelection = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            MoveCursorWithKey(true, extendSelection);
            UpdateViewport(window, true);
            return 0;
        }
        if (wParam == VK_DELETE) {
            EditSnapshot before = CaptureEditSnapshot();
            if (DeleteSelection() || DeleteCharacterAtCursor()) {
                selectionAnchor = cursorPosition;
                RecordEdit(std::move(before));
                UpdateViewport(window, true);
            }
            return 0;
        }
        break;

    case WM_CLOSE:
        if (ConfirmDocumentChange(window, DocumentChange::Close)) DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

HMENU CreateEditorMenu() {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU edit = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    if (menu == nullptr || file == nullptr || edit == nullptr || view == nullptr) {
        if (view != nullptr) DestroyMenu(view);
        if (edit != nullptr) DestroyMenu(edit);
        if (file != nullptr) DestroyMenu(file);
        if (menu != nullptr) DestroyMenu(menu);
        return nullptr;
    }
    const bool populated =
        AppendMenuW(file, MF_STRING, kFileNew, L"&New\tCtrl+N") &&
        AppendMenuW(file, MF_STRING, kFileOpen, L"&Open...\tCtrl+O") &&
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr) &&
        AppendMenuW(file, MF_STRING, kFileSave, L"&Save\tCtrl+S") &&
        AppendMenuW(file, MF_STRING, kFileHylic, L"Save as &hylic...\tCtrl+Shift+S") &&
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr) &&
        AppendMenuW(file, MF_STRING, kFileExit, L"E&xit\tAlt+F4");
    const bool editPopulated =
        AppendMenuW(edit, MF_STRING, kEditUndo, L"&Undo\tCtrl+Z") &&
        AppendMenuW(edit, MF_STRING, kEditRedo, L"&Redo\tCtrl+Y") &&
        AppendMenuW(edit, MF_SEPARATOR, 0, nullptr) &&
        AppendMenuW(edit, MF_STRING, kEditCut, L"Cu&t\tCtrl+X") &&
        AppendMenuW(edit, MF_STRING, kEditCopy, L"&Copy\tCtrl+C") &&
        AppendMenuW(edit, MF_STRING, kEditPaste, L"&Paste\tCtrl+V") &&
        AppendMenuW(edit, MF_SEPARATOR, 0, nullptr) &&
        AppendMenuW(edit, MF_STRING, kEditSelectAll, L"Select &All\tCtrl+A") &&
        AppendMenuW(edit, MF_SEPARATOR, 0, nullptr) &&
        AppendMenuW(edit, MF_STRING, kEditFind, L"&Find...\tCtrl+F") &&
        AppendMenuW(edit, MF_STRING, kEditFindNext, L"Find &Next\tF3") &&
        AppendMenuW(edit, MF_STRING, kEditFindPrevious, L"Find Pre&vious\tShift+F3") &&
        AppendMenuW(edit, MF_STRING, kEditReplace, L"R&eplace...\tCtrl+H");
    const bool viewPopulated =
        AppendMenuW(view, MF_STRING, kViewWordWrap, L"&Word Wrap\tAlt+Z");
    // On success the menu bar owns each popup; on failure destroy the unattached ones.
    if (!populated || !editPopulated || !viewPopulated ||
        !AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File")) {
        DestroyMenu(view);
        DestroyMenu(edit);
        DestroyMenu(file);
        DestroyMenu(menu);
        return nullptr;
    }
    if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"&Edit")) {
        // The File popup is already owned by the menu bar and is freed with it.
        DestroyMenu(view);
        DestroyMenu(edit);
        DestroyMenu(menu);
        return nullptr;
    }
    if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View")) {
        DestroyMenu(view);
        DestroyMenu(menu);
        return nullptr;
    }
    return menu;
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    windowClass.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&windowClass) == 0) {
        MessageBoxW(nullptr, L"Could not register the window class.", L"Octavo", MB_OK | MB_ICONERROR);
        return 1;
    }

    HMENU menu = CreateEditorMenu();
    if (menu == nullptr) {
        MessageBoxW(nullptr, L"Could not create the menus.", L"Octavo", MB_OK | MB_ICONERROR);
        return 2;
    }
    HWND window = CreateWindowExW(
        0,
        kWindowClassName,
        L"Octavo v0.1",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        760,
        440,
        nullptr,
        menu,
        instance,
        nullptr);

    if (window == nullptr) {
        DestroyMenu(menu);
        MessageBoxW(nullptr, L"Could not create the window.", L"Octavo", MB_OK | MB_ICONERROR);
        return 2;
    }

    ShowWindow(window, showCommand);

    MSG message{};
    for (;;) {
        BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1) {
            MessageBoxW(nullptr, L"The Windows message loop failed.", L"Octavo", MB_OK | MB_ICONERROR);
            return 3;
        }
        if (result == 0) {
            return static_cast<int>(message.wParam);
        }

        if (findDialog != nullptr && message.message == WM_KEYDOWN &&
            (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (GetKeyState(VK_MENU) & 0x8000) == 0 &&
            (message.wParam == 'F' || message.wParam == 'H')) {
            ShowFindDialog(window, message.wParam == 'H');
            continue;
        }
        if (findDialog != nullptr && message.message == WM_KEYDOWN && message.wParam == VK_F3) {
            FindMatch(window, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            continue;
        }
        if (findDialog != nullptr && IsDialogMessageW(findDialog, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}


