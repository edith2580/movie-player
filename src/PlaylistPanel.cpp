#include "PlaylistPanel.h"

#include <commctrl.h>
#include <imm.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace {

constexpr wchar_t kPanelClassName[] = L"MoviePlayer.PlaylistPanel";
constexpr UINT_PTR kSlideTimerId = 1;
constexpr UINT kSlideTimerMs = 15;
constexpr double kSlideDurationMs = 180.0;
constexpr UINT_PTR kControlSubclassId = 1;

constexpr int kIdClose = 101;
constexpr int kIdAutoAdvance = 102;
constexpr int kIdRepeatOne = 103;
constexpr int kIdRepeatAll = 104;
constexpr int kIdList = 105;

constexpr int kDefaultPanelWidthDip = 300;
constexpr int kMinPanelWidthDip = 200;
constexpr int kPaddingDip = 12;
constexpr int kHeaderHeightDip = 38;
constexpr int kCloseButtonSizeDip = 24;
constexpr int kOptionRowHeightDip = 20;
constexpr int kOptionRowGapDip = 4;
constexpr int kOptionVerticalPadDip = 8;
constexpr int kIndicatorColumnDip = 24;

void RegisterPanelClass(HINSTANCE instance) {
    WNDCLASSEXW existing{};
    if (GetClassInfoExW(instance, kPanelClassName, &existing)) {
        return;
    }
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &PlaylistPanel::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kPanelClassName;
    if (!RegisterClassExW(&windowClass)) {
        throw std::runtime_error("RegisterClassEx failed for PlaylistPanel");
    }
}

void DrainQueuedChars(HWND window) {
    MSG character{};
    while (PeekMessageW(&character, window, WM_CHAR, WM_DEADCHAR, PM_REMOVE) ||
           PeekMessageW(&character, window, WM_SYSCHAR, WM_SYSDEADCHAR, PM_REMOVE)) {
    }
}

}  // namespace

PlaylistPanel::PlaylistPanel(HWND mainWindow, HINSTANCE instance, Callbacks callbacks)
    : mainWindow_(mainWindow), instance_(instance), callbacks_(std::move(callbacks)) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    RegisterPanelClass(instance_);
    dpi_ = GetDpiForWindow(mainWindow_);

    window_ = CreateWindowExW(
        0, kPanelClassName, nullptr,
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        mainWindow_, nullptr, instance_, this);
    if (!window_) {
        throw std::runtime_error("CreateWindowEx failed for PlaylistPanel");
    }

    ImmAssociateContextEx(window_, nullptr, 0);
    DragAcceptFiles(window_, TRUE);

    CreateControls();
    UpdateFonts();
    RefreshHeaderCount();
    RefreshOptionChecks();
    RefreshList();
}

PlaylistPanel::~PlaylistPanel() {
    if (window_ && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    window_ = nullptr;
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (boldFont_) {
        DeleteObject(boldFont_);
        boldFont_ = nullptr;
    }
}

int PlaylistPanel::Dip(int value) const {
    return MulDiv(value, static_cast<int>(dpi_), 96);
}

int PlaylistPanel::TargetPanelWidth(int clientWidth, UINT dpi) const {
    const int preferred = MulDiv(kDefaultPanelWidthDip, static_cast<int>(dpi), 96);
    const int minimum = MulDiv(kMinPanelWidthDip, static_cast<int>(dpi), 96);
    const int capped = std::max(minimum, clientWidth * 45 / 100);
    return std::min(clientWidth, std::min(preferred, capped));
}

void PlaylistPanel::Toggle() {
    SetOpen(!targetOpen_);
}

void PlaylistPanel::SetOpen(bool open) {
    if (open == targetOpen_ && !animating_) {
        if (open) {
            SetFocus(list_);
        }
        return;
    }
    targetOpen_ = open;
    animFrom_ = progress_;
    animTo_ = open ? 1.0 : 0.0;
    animStart_ = GetTickCount64();
    animating_ = true;

    if (open) {
        UpdatePanelBounds();
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        SetFocus(files_.empty() ? autoAdvanceCheck_ : list_);
    }
    SetTimer(window_, kSlideTimerId, kSlideTimerMs, nullptr);
}

bool PlaylistPanel::HasFocus() const {
    const HWND focused = GetFocus();
    return focused != nullptr && (focused == window_ || IsChild(window_, focused));
}

void PlaylistPanel::SetPlaylist(const std::vector<std::wstring>& files, size_t currentIndex) {
    const bool sameFiles = files == files_;
    const size_t previousIndex = currentIndex_;
    files_ = files;
    currentIndex_ = files_.empty() ? 0 : std::min(currentIndex, files_.size() - 1);

    RefreshHeaderCount();

    if (sameFiles && !files_.empty()) {
        if (previousIndex != currentIndex_) {
            wchar_t emptyText[] = L"";
            wchar_t activeMark[] = L"▶";
            if (previousIndex < files_.size()) {
                ListView_SetItemText(list_, static_cast<int>(previousIndex), 0, emptyText);
                ListView_RedrawItems(list_, static_cast<int>(previousIndex), static_cast<int>(previousIndex));
            }
            ListView_SetItemText(list_, static_cast<int>(currentIndex_), 0, activeMark);
            ListView_RedrawItems(list_, static_cast<int>(currentIndex_), static_cast<int>(currentIndex_));
        }
        ListView_SetItemState(list_, static_cast<int>(currentIndex_), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, static_cast<int>(currentIndex_), FALSE);
        return;
    }

    RefreshList();
}

void PlaylistPanel::SetOptions(bool autoAdvance, RepeatMode repeat) {
    autoAdvance_ = autoAdvance;
    repeat_ = repeat;
    RefreshOptionChecks();
}

int PlaylistPanel::Layout(int clientWidth, int clientHeight, UINT dpi) {
    const bool dpiChanged = dpi != dpi_;
    clientWidth_ = clientWidth;
    clientHeight_ = clientHeight;
    dpi_ = dpi;
    panelWidth_ = TargetPanelWidth(clientWidth, dpi);

    if (dpiChanged) {
        UpdateFonts();
    }

    visibleWidth_ = static_cast<int>(std::lround(static_cast<double>(panelWidth_) * progress_));
    if (visibleWidth_ > 0 || animating_) {
        UpdatePanelBounds();
        LayoutControls();
    }
    return visibleWidth_;
}

void PlaylistPanel::UpdatePanelBounds() {
    if (!window_ || clientHeight_ <= 0 || panelWidth_ <= 0) {
        return;
    }
    const int x = clientWidth_ - visibleWidth_;
    SetWindowPos(window_, HWND_TOP, x, 0, panelWidth_, clientHeight_, SWP_NOACTIVATE);
}

void PlaylistPanel::AdvanceAnimation() {
    const double elapsed = static_cast<double>(GetTickCount64() - animStart_);
    const double t = std::clamp(elapsed / kSlideDurationMs, 0.0, 1.0);
    // Cubic ease-out for both opening and closing keeps reversals responsive.
    const double eased = 1.0 - std::pow(1.0 - t, 3.0);
    progress_ = animFrom_ + (animTo_ - animFrom_) * eased;

    if (t >= 1.0) {
        progress_ = animTo_;
        animating_ = false;
        KillTimer(window_, kSlideTimerId);
        if (!targetOpen_) {
            if (HasFocus()) {
                SetFocus(mainWindow_);
            }
            ShowWindow(window_, SW_HIDE);
        }
    }

    if (callbacks_.onAnimationStep) {
        callbacks_.onAnimationStep();
    }
}

void PlaylistPanel::CreateControls() {
    const auto subclass = [this](HWND control) {
        SetWindowSubclass(control, &PlaylistPanel::ControlSubclassProc, kControlSubclassId, reinterpret_cast<DWORD_PTR>(this));
    };

    titleLabel_ = CreateWindowExW(
        0, L"STATIC", L"재생 목록",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
        0, 0, 0, 0, window_, nullptr, instance_, nullptr);

    countLabel_ = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX,
        0, 0, 0, 0, window_, nullptr, instance_, nullptr);

    closeButton_ = CreateWindowExW(
        0, L"BUTTON", L"×",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdClose)), instance_, nullptr);
    subclass(closeButton_);

    autoAdvanceCheck_ = CreateWindowExW(
        0, L"BUTTON", L"다음 파일 자동 재생",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdAutoAdvance)), instance_, nullptr);
    subclass(autoAdvanceCheck_);

    repeatOneCheck_ = CreateWindowExW(
        0, L"BUTTON", L"한 개 반복",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdRepeatOne)), instance_, nullptr);
    subclass(repeatOneCheck_);

    repeatAllCheck_ = CreateWindowExW(
        0, L"BUTTON", L"전체 목록 반복",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdRepeatAll)), instance_, nullptr);
    subclass(repeatAllCheck_);

    list_ = CreateWindowExW(
        0, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdList)), instance_, nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ImmAssociateContextEx(list_, nullptr, 0);
    subclass(list_);

    LVCOLUMNW col{};
    col.mask = LVCF_FMT | LVCF_TEXT;
    col.fmt = LVCFMT_CENTER;
    wchar_t col0[] = L"";
    col.pszText = col0;
    ListView_InsertColumn(list_, 0, &col);
    col.fmt = LVCFMT_LEFT;
    wchar_t col1[] = L"파일";
    col.pszText = col1;
    ListView_InsertColumn(list_, 1, &col);

    emptyLabel_ = CreateWindowExW(
        0, L"STATIC", L"재생 목록이 비어 있습니다",
        WS_CHILD | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX,
        0, 0, 0, 0, window_, nullptr, instance_, nullptr);
}

void PlaylistPanel::UpdateFonts() {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    LOGFONTW baseFont{};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
        baseFont = metrics.lfMessageFont;
    } else {
        baseFont.lfHeight = -MulDiv(9, static_cast<int>(dpi_), 72);
        baseFont.lfCharSet = DEFAULT_CHARSET;
        baseFont.lfWeight = FW_NORMAL;
        wcscpy_s(baseFont.lfFaceName, L"Segoe UI");
    }

    HFONT nextRegular = CreateFontIndirectW(&baseFont);
    LOGFONTW bold = baseFont;
    bold.lfWeight = FW_SEMIBOLD;
    HFONT nextBold = CreateFontIndirectW(&bold);

    const std::array<HWND, 7> regularControls{
        countLabel_, closeButton_, autoAdvanceCheck_, repeatOneCheck_, repeatAllCheck_, list_, emptyLabel_};
    for (HWND control : regularControls) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(nextRegular), TRUE);
    }
    SendMessageW(titleLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(nextBold), TRUE);

    if (font_) {
        DeleteObject(font_);
    }
    if (boldFont_) {
        DeleteObject(boldFont_);
    }
    font_ = nextRegular;
    boldFont_ = nextBold;
}

void PlaylistPanel::LayoutControls() {
    if (panelWidth_ <= 0 || clientHeight_ <= 0) {
        return;
    }
    const int pad = Dip(kPaddingDip);
    const int leftEdge = 1;  // Leave room for the 1px vertical separator on the left.
    const int headerHeight = Dip(kHeaderHeightDip);
    const int closeSize = Dip(kCloseButtonSizeDip);
    const int innerLeft = leftEdge + pad;
    const int innerWidth = std::max(0, panelWidth_ - leftEdge - pad * 2);

    const int closeX = std::max(innerLeft, panelWidth_ - pad - closeSize);
    const int closeY = (headerHeight - closeSize) / 2;
    MoveWindow(closeButton_, closeX, closeY, closeSize, closeSize, TRUE);

    const int countWidth = Dip(72);
    const int countX = std::max(innerLeft, closeX - Dip(6) - countWidth);
    const int titleWidth = std::max(0, countX - innerLeft - Dip(6));
    MoveWindow(titleLabel_, innerLeft, 0, titleWidth, headerHeight - 1, TRUE);
    MoveWindow(countLabel_, countX, 0, std::max(0, closeX - Dip(6) - countX), headerHeight - 1, TRUE);

    int y = headerHeight + Dip(kOptionVerticalPadDip);
    const int rowHeight = Dip(kOptionRowHeightDip);
    const int rowGap = Dip(kOptionRowGapDip);
    MoveWindow(autoAdvanceCheck_, innerLeft, y, innerWidth, rowHeight, TRUE);
    y += rowHeight + rowGap;
    MoveWindow(repeatOneCheck_, innerLeft, y, innerWidth, rowHeight, TRUE);
    y += rowHeight + rowGap;
    MoveWindow(repeatAllCheck_, innerLeft, y, innerWidth, rowHeight, TRUE);
    y += rowHeight + Dip(kOptionVerticalPadDip);

    const int listTop = y + 1;  // 1px horizontal divider above the list.
    const int listHeight = std::max(0, clientHeight_ - listTop);
    const int listWidth = std::max(0, panelWidth_ - leftEdge);
    MoveWindow(list_, leftEdge, listTop, listWidth, listHeight, TRUE);
    MoveWindow(emptyLabel_, leftEdge, listTop, listWidth, listHeight, TRUE);
    FitColumns();
    InvalidateRect(window_, nullptr, FALSE);
}

void PlaylistPanel::FitColumns() {
    RECT rect{};
    GetClientRect(list_, &rect);
    const int available = std::max(0, static_cast<int>(rect.right));
    const int indicatorWidth = std::min(available, Dip(kIndicatorColumnDip));
    ListView_SetColumnWidth(list_, 0, indicatorWidth);
    ListView_SetColumnWidth(list_, 1, std::max(0, available - indicatorWidth));
}

void PlaylistPanel::RefreshHeaderCount() {
    if (files_.empty()) {
        SetWindowTextW(countLabel_, L"0개");
    } else {
        const std::wstring text = std::to_wstring(currentIndex_ + 1) + L" / " + std::to_wstring(files_.size());
        SetWindowTextW(countLabel_, text.c_str());
    }
}

void PlaylistPanel::RefreshOptionChecks() {
    updatingChecks_ = true;
    SendMessageW(autoAdvanceCheck_, BM_SETCHECK, autoAdvance_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(repeatOneCheck_, BM_SETCHECK, repeat_ == RepeatMode::One ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(repeatAllCheck_, BM_SETCHECK, repeat_ == RepeatMode::All ? BST_CHECKED : BST_UNCHECKED, 0);
    updatingChecks_ = false;
}

void PlaylistPanel::RefreshList() {
    const bool empty = files_.empty();
    ShowWindow(list_, empty ? SW_HIDE : SW_SHOW);
    ShowWindow(emptyLabel_, empty ? SW_SHOW : SW_HIDE);

    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);

    for (size_t i = 0; i < files_.size(); ++i) {
        std::wstring mark = (i == currentIndex_) ? L"▶" : L"";
        std::wstring title = std::to_wstring(i + 1) + L". " + std::filesystem::path(files_[i]).filename().wstring();

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = mark.data();
        ListView_InsertItem(list_, &item);
        ListView_SetItemText(list_, static_cast<int>(i), 1, title.data());
    }

    if (!empty) {
        ListView_SetItemState(list_, static_cast<int>(currentIndex_), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, static_cast<int>(currentIndex_), FALSE);
    }

    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    FitColumns();
    InvalidateRect(list_, nullptr, TRUE);
}

void PlaylistPanel::MoveFocusCycle(HWND current, bool backwards) {
    std::vector<HWND> stops{autoAdvanceCheck_, repeatOneCheck_, repeatAllCheck_};
    if (!files_.empty()) {
        stops.push_back(list_);
    }
    stops.push_back(closeButton_);

    const auto it = std::find(stops.begin(), stops.end(), current);
    if (it == stops.end()) {
        SetFocus(stops.front());
        return;
    }
    const size_t index = static_cast<size_t>(it - stops.begin());
    const size_t next = backwards ? (index + stops.size() - 1) % stops.size() : (index + 1) % stops.size();
    SetFocus(stops[next]);
}

bool PlaylistPanel::HandleControlKeyDown(HWND control, WPARAM key) {
    const KeyChord chord = ChordFromKey(key);
    if ((toggleShortcut_ != kNoKey && chord == toggleShortcut_) || chord == VK_ESCAPE) {
        DrainQueuedChars(control);
        SetOpen(false);
        return true;
    }
    if (key == VK_TAB && (GetKeyState(VK_CONTROL) >= 0) && (GetKeyState(VK_MENU) >= 0)) {
        DrainQueuedChars(control);
        MoveFocusCycle(control, GetKeyState(VK_SHIFT) < 0);
        return true;
    }
    if (control == list_) {
        if (key == VK_RETURN) {
            DrainQueuedChars(control);
            const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
            if (selected >= 0 && static_cast<size_t>(selected) < files_.size() && callbacks_.onPlayItem) {
                callbacks_.onPlayItem(static_cast<size_t>(selected));
            }
            return true;
        }
        if (key == VK_SPACE && callbacks_.onTogglePause) {
            DrainQueuedChars(control);
            callbacks_.onTogglePause();
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK PlaylistPanel::ControlSubclassProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam,
                                                    UINT_PTR subclassId, DWORD_PTR refData) {
    auto* self = reinterpret_cast<PlaylistPanel*>(refData);
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && self && self->HandleControlKeyDown(control, wParam)) {
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, &PlaylistPanel::ControlSubclassProc, subclassId);
    }
    return DefSubclassProc(control, message, wParam, lParam);
}

LRESULT CALLBACK PlaylistPanel::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<PlaylistPanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    self->window_ = window;
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT PlaylistPanel::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        if (wParam == kSlideTimerId) {
            AdvanceAnimation();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC hdc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        FillRect(hdc, &client, GetSysColorBrush(COLOR_WINDOW));

        // 1px border on the left edge separating the drawer from the video.
        RECT leftBorder{0, 0, 1, client.bottom};
        FillRect(hdc, &leftBorder, GetSysColorBrush(COLOR_BTNSHADOW));

        // 1px dividers below the header and below the playback options.
        const int headerBottom = Dip(kHeaderHeightDip) - 1;
        RECT headerDivider{1, headerBottom, client.right, headerBottom + 1};
        FillRect(hdc, &headerDivider, GetSysColorBrush(COLOR_BTNFACE));

        const int optionsBottom = headerBottom + 1 + Dip(kOptionVerticalPadDip) * 2 +
                                  Dip(kOptionRowHeightDip) * 3 + Dip(kOptionRowGapDip) * 2;
        RECT optionsDivider{1, optionsBottom, client.right, optionsBottom + 1};
        FillRect(hdc, &optionsDivider, GetSysColorBrush(COLOR_BTNFACE));

        EndPaint(window_, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
        const bool secondary = control == countLabel_ || control == emptyLabel_;
        SetTextColor(hdc, GetSysColor(secondary ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED && !updatingChecks_) {
            switch (LOWORD(wParam)) {
            case kIdClose:
                SetOpen(false);
                break;
            case kIdAutoAdvance:
                if (callbacks_.onToggleAutoAdvance) {
                    callbacks_.onToggleAutoAdvance();
                }
                break;
            case kIdRepeatOne:
                if (callbacks_.onToggleRepeatOne) {
                    callbacks_.onToggleRepeatOne();
                }
                break;
            case kIdRepeatAll:
                if (callbacks_.onToggleRepeatAll) {
                    callbacks_.onToggleRepeatAll();
                }
                break;
            default:
                break;
            }
        }
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->hwndFrom == list_) {
            if (header->code == NM_DBLCLK) {
                const int item = reinterpret_cast<const NMITEMACTIVATE*>(lParam)->iItem;
                if (item >= 0 && static_cast<size_t>(item) < files_.size() && callbacks_.onPlayItem) {
                    callbacks_.onPlayItem(static_cast<size_t>(item));
                }
                return 0;
            }
            if (header->code == NM_CUSTOMDRAW) {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                    return CDRF_NOTIFYITEMDRAW;
                }
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    const size_t item = static_cast<size_t>(draw->nmcd.dwItemSpec);
                    if (item == currentIndex_ && boldFont_) {
                        SelectObject(draw->nmcd.hdc, boldFont_);
                        if ((draw->nmcd.uItemState & CDIS_SELECTED) == 0) {
                            draw->clrText = GetSysColor(COLOR_HOTLIGHT);
                        }
                        return CDRF_NEWFONT;
                    }
                    return CDRF_DODEFAULT;
                }
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (HandleControlKeyDown(window_, wParam)) {
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_LBUTTONDOWN:
        SetFocus(files_.empty() ? autoAdvanceCheck_ : list_);
        return 0;
    case WM_DROPFILES:
        return SendMessageW(mainWindow_, WM_DROPFILES, wParam, lParam);
    case WM_DESTROY:
        KillTimer(window_, kSlideTimerId);
        return 0;
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}
