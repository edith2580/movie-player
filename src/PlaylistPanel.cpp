#include "PlaylistPanel.h"

#include <commctrl.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace {

constexpr wchar_t kPanelClassName[] = L"MoviePlayer.PlaylistWindow";
constexpr wchar_t kPanelTitle[] = L"재생 목록";
constexpr int kAppIconId = 1;
constexpr UINT_PTR kControlSubclassId = 1;

constexpr int kIdAutoAdvance = 102;
constexpr int kIdRepeatOne = 103;
constexpr int kIdRepeatAll = 104;
constexpr int kIdList = 105;

constexpr int kMinWindowWidthDip = 240;
constexpr int kMinWindowHeightDip = 260;
constexpr int kPaddingDip = 10;
constexpr int kToolbarHeightDip = 40;
constexpr int kToggleWidthDip = 30;
constexpr int kToggleHeightDip = 28;
constexpr int kToggleGapDip = 6;
constexpr int kDockSnapDip = 24;

void RegisterPanelClass(HINSTANCE instance) {
    WNDCLASSEXW existing{};
    if (GetClassInfoExW(instance, kPanelClassName, &existing)) {
        return;
    }
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &PlaylistPanel::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kAppIconId));
    windowClass.hIconSm = windowClass.hIcon;
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

RECT VisibleWindowBounds(HWND window) {
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        GetWindowRect(window, &rect);
    }
    return rect;
}

D2D1_COLOR_F ColorFromSys(int sysColorIndex, float alpha = 1.0f) {
    const COLORREF color = GetSysColor(sysColorIndex);
    return D2D1::ColorF(static_cast<float>(GetRValue(color)) / 255.0f,
                        static_cast<float>(GetGValue(color)) / 255.0f,
                        static_cast<float>(GetBValue(color)) / 255.0f,
                        alpha);
}

D2D1_COLOR_F BlendColors(D2D1_COLOR_F base, D2D1_COLOR_F tint, float amount) {
    return D2D1::ColorF(base.r + (tint.r - base.r) * amount,
                        base.g + (tint.g - base.g) * amount,
                        base.b + (tint.b - base.b) * amount,
                        1.0f);
}

void AddTooltip(HWND tooltip, HWND owner, HWND control, const wchar_t* text) {
    if (!tooltip) {
        return;
    }
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    info.hwnd = owner;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

}  // namespace

PlaylistPanel::PlaylistPanel(HWND mainWindow, HINSTANCE instance, Callbacks callbacks)
    : mainWindow_(mainWindow), instance_(instance), callbacks_(std::move(callbacks)) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    RegisterPanelClass(instance_);
    dpi_ = GetDpiForWindow(mainWindow_);

    window_ = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        kPanelClassName,
        kPanelTitle,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, Dip(panelClientWidthDip_), Dip(480),
        mainWindow_, nullptr, instance_, this);
    if (!window_) {
        throw std::runtime_error("CreateWindowEx failed for PlaylistPanel");
    }

    ImmAssociateContextEx(window_, nullptr, 0);

    CreateGraphics();
    CreateControls();
    UpdateFonts();
    RefreshHeaderCount();
    RefreshOptionButtons();
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

void PlaylistPanel::Toggle() {
    SetOpen(!open_);
}

void PlaylistPanel::SetOpen(bool open) {
    if (open == open_) {
        if (open) {
            SetForegroundWindow(window_);
            SetFocus(files_.empty() ? autoAdvanceButton_ : list_);
        }
        return;
    }
    open_ = open;

    if (open) {
        docked_ = true;
        const RECT target = ComputeDockedRect();
        SetWindowPos(window_, HWND_TOP, target.left, target.top, target.right - target.left, target.bottom - target.top,
                     SWP_NOACTIVATE);
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        SetFocus(files_.empty() ? autoAdvanceButton_ : list_);
    } else {
        hoveredButton_ = nullptr;
        trackingHover_ = false;
        const bool returnFocus = GetActiveWindow() == window_ || HasFocus();
        ShowWindow(window_, SW_HIDE);
        if (returnFocus) {
            SetForegroundWindow(mainWindow_);
            SetFocus(mainWindow_);
        }
    }
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
            if (previousIndex < files_.size()) {
                ListView_RedrawItems(list_, static_cast<int>(previousIndex), static_cast<int>(previousIndex));
            }
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
    RefreshOptionButtons();
}

void PlaylistPanel::SyncToMainWindow() {
    if (!open_ || !docked_ || !IsWindowVisible(window_) || IsIconic(mainWindow_)) {
        return;
    }
    const RECT target = ComputeDockedRect();
    SetWindowPos(window_, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

RECT PlaylistPanel::ComputeDockedRect() const {
    const RECT mainFrame = VisibleWindowBounds(mainWindow_);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(mainWindow_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return mainFrame;
    }

    const UINT targetDpi = GetDpiForWindow(mainWindow_);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE));
    RECT frameOffset{};
    AdjustWindowRectExForDpi(&frameOffset, style, FALSE, exStyle, targetDpi);

    // On Windows 10/11, the left/right/bottom thick frame includes an invisible resize border
    // outside DWMWA_EXTENDED_FRAME_BOUNDS; compensate so the visible edges sit flush.
    const int borderX = GetSystemMetricsForDpi(SM_CXSIZEFRAME, targetDpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, targetDpi);
    const int borderY = GetSystemMetricsForDpi(SM_CYSIZEFRAME, targetDpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, targetDpi);

    const bool isFullscreen = mainFrame.left <= monitor.rcMonitor.left && mainFrame.top <= monitor.rcMonitor.top &&
                              mainFrame.right >= monitor.rcMonitor.right && mainFrame.bottom >= monitor.rcMonitor.bottom;
    const RECT& area = isFullscreen ? monitor.rcMonitor : monitor.rcWork;

    const int clientWidth = MulDiv(panelClientWidthDip_, static_cast<int>(targetDpi), 96);
    const int windowWidth = clientWidth + (frameOffset.right - frameOffset.left);
    const int visibleWidth = windowWidth - borderX * 2;
    const int visibleHeight = std::min<int>(mainFrame.bottom - mainFrame.top, area.bottom - area.top);
    const int windowHeight = visibleHeight + borderY;

    int visibleX = mainFrame.right;
    if (visibleX + visibleWidth > area.right) {
        visibleX = std::max<int>(area.left, area.right - visibleWidth);
    }
    const int visibleY = std::clamp<int>(mainFrame.top, area.top, std::max<int>(area.top, area.bottom - visibleHeight));

    const int left = visibleX - borderX;
    const int top = visibleY;
    return RECT{left, top, left + windowWidth, top + windowHeight};
}

void PlaylistPanel::UpdateDockedState() {
    const RECT mainFrame = VisibleWindowBounds(mainWindow_);
    const RECT selfFrame = VisibleWindowBounds(window_);
    const int snap = Dip(kDockSnapDip);
    docked_ = std::abs(selfFrame.left - mainFrame.right) <= snap && std::abs(selfFrame.top - mainFrame.top) <= snap;
}

void PlaylistPanel::CreateGraphics() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) {
        throw std::runtime_error("D2D1CreateFactory failed for PlaylistPanel");
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi_), static_cast<float>(dpi_));
    if (FAILED(d2dFactory_->CreateDCRenderTarget(&props, &dcTarget_)) ||
        FAILED(dcTarget_->CreateSolidColorBrush(ColorFromSys(COLOR_WINDOWTEXT), &brush_))) {
        throw std::runtime_error("CreateDCRenderTarget failed for PlaylistPanel");
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf()))) ||
        FAILED(writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, 8.5f, L"ko-KR", &badgeFormat_))) {
        throw std::runtime_error("CreateTextFormat failed for PlaylistPanel");
    }
    badgeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    badgeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void PlaylistPanel::CreateControls() {
    const auto subclass = [this](HWND control) {
        SetWindowSubclass(control, &PlaylistPanel::ControlSubclassProc, kControlSubclassId, reinterpret_cast<DWORD_PTR>(this));
    };

    tooltip_ = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        window_, nullptr, instance_, nullptr);

    autoAdvanceButton_ = CreateWindowExW(
        0, L"BUTTON", L"다음 파일 자동 재생",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdAutoAdvance)), instance_, nullptr);
    subclass(autoAdvanceButton_);
    AddTooltip(tooltip_, window_, autoAdvanceButton_, L"다음 파일 자동 재생");

    repeatOneButton_ = CreateWindowExW(
        0, L"BUTTON", L"한 개 반복",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdRepeatOne)), instance_, nullptr);
    subclass(repeatOneButton_);
    AddTooltip(tooltip_, window_, repeatOneButton_, L"한 개 반복");

    repeatAllButton_ = CreateWindowExW(
        0, L"BUTTON", L"전체 목록 반복",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdRepeatAll)), instance_, nullptr);
    subclass(repeatAllButton_);
    AddTooltip(tooltip_, window_, repeatAllButton_, L"전체 목록 반복");

    countLabel_ = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX,
        0, 0, 0, 0, window_, nullptr, instance_, nullptr);

    list_ = CreateWindowExW(
        0, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdList)), instance_, nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ImmAssociateContextEx(list_, nullptr, 0);
    subclass(list_);

    LVCOLUMNW col{};
    col.mask = LVCF_FMT | LVCF_TEXT;
    col.fmt = LVCFMT_LEFT;
    wchar_t col0[] = L"파일";
    col.pszText = col0;
    ListView_InsertColumn(list_, 0, &col);

    emptyLabel_ = CreateWindowExW(
        0, L"STATIC", L"재생 목록이 비어 있습니다.\n파일을 끌어다 놓으면 추가됩니다.",
        WS_CHILD | SS_CENTER | SS_NOPREFIX,
        0, 0, 0, 0, window_, nullptr, instance_, nullptr);
}

void PlaylistPanel::DrawToggleButton(const DRAWITEMSTRUCT& draw) {
    const int id = static_cast<int>(draw.CtlID);
    const bool checked = (id == kIdAutoAdvance && autoAdvance_) ||
                         (id == kIdRepeatOne && repeat_ == RepeatMode::One) ||
                         (id == kIdRepeatAll && repeat_ == RepeatMode::All);
    const bool hovered = draw.hwndItem == hoveredButton_;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0 && (draw.itemState & ODS_NOFOCUSRECT) == 0;

    dcTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    if (FAILED(dcTarget_->BindDC(draw.hDC, &draw.rcItem))) {
        return;
    }

    const float width = static_cast<float>(draw.rcItem.right - draw.rcItem.left) * 96.0f / static_cast<float>(dpi_);
    const float height = static_cast<float>(draw.rcItem.bottom - draw.rcItem.top) * 96.0f / static_cast<float>(dpi_);
    const float cx = width * 0.5f;
    const float cy = height * 0.5f;

    const D2D1_COLOR_F windowBg = ColorFromSys(COLOR_WINDOW);
    const D2D1_COLOR_F textColor = ColorFromSys(COLOR_WINDOWTEXT);
    const D2D1_COLOR_F mutedColor = ColorFromSys(COLOR_GRAYTEXT);
    const D2D1_COLOR_F accent = ColorFromSys(COLOR_HIGHLIGHT);
    const D2D1_COLOR_F activeStroke = ColorFromSys(COLOR_HOTLIGHT);

    dcTarget_->BeginDraw();
    dcTarget_->Clear(windowBg);

    const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(D2D1::RectF(1.0f, 1.0f, width - 1.0f, height - 1.0f), 4.0f, 4.0f);
    if (checked) {
        const float fillAmount = pressed ? 0.28f : hovered ? 0.22f : 0.14f;
        const float borderAlpha = hovered || pressed ? 0.70f : 0.45f;
        brush_->SetColor(BlendColors(windowBg, accent, fillAmount));
        dcTarget_->FillRoundedRectangle(pill, brush_.Get());
        brush_->SetColor(D2D1::ColorF(activeStroke.r, activeStroke.g, activeStroke.b, borderAlpha));
        dcTarget_->DrawRoundedRectangle(pill, brush_.Get(), 1.0f);
    } else if (pressed || hovered) {
        const float fillAmount = pressed ? 0.14f : 0.08f;
        brush_->SetColor(BlendColors(windowBg, textColor, fillAmount));
        dcTarget_->FillRoundedRectangle(pill, brush_.Get());
        brush_->SetColor(BlendColors(windowBg, textColor, 0.18f));
        dcTarget_->DrawRoundedRectangle(pill, brush_.Get(), 1.0f);
    }

    const D2D1_COLOR_F iconColor = checked ? activeStroke : (hovered || pressed ? textColor : mutedColor);
    brush_->SetColor(iconColor);

    if (id == kIdAutoAdvance) {
        // Play-next icon (triangle + vertical bar).
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> triangle;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&triangle)) && SUCCEEDED(triangle->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(cx - 5.5f, cy - 5.0f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(cx + 1.8f, cy));
            sink->AddLine(D2D1::Point2F(cx - 5.5f, cy + 5.0f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (SUCCEEDED(sink->Close())) {
                dcTarget_->FillGeometry(triangle.Get(), brush_.Get());
            }
        }
        dcTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(cx + 3.5f, cy - 4.8f, cx + 5.5f, cy + 4.8f), 0.8f, 0.8f), brush_.Get());
    } else {
        // Repeat loop: top arc/bar pointing right, bottom arc/bar pointing left.
        const float stroke = 1.6f;
        const auto drawLoop = [&](float dir) {
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(cx - dir * 6.5f, cy - dir * 1.0f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(cx - dir * 6.5f, cy - dir * 2.8f));
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(cx - dir * 4.5f, cy - dir * 4.8f),
                    D2D1::SizeF(2.0f, 2.0f), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->AddLine(D2D1::Point2F(cx + dir * 4.8f, cy - dir * 4.8f));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (SUCCEEDED(sink->Close())) {
                    dcTarget_->DrawGeometry(path.Get(), brush_.Get(), stroke);
                }
            }

            Microsoft::WRL::ComPtr<ID2D1PathGeometry> arrow;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> arrowSink;
            if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&arrow)) && SUCCEEDED(arrow->Open(&arrowSink))) {
                const float tipX = cx + dir * 6.8f;
                const float baseX = cx + dir * 3.4f;
                const float tipY = cy - dir * 4.8f;
                arrowSink->BeginFigure(D2D1::Point2F(baseX, tipY - 2.6f), D2D1_FIGURE_BEGIN_FILLED);
                arrowSink->AddLine(D2D1::Point2F(tipX, tipY));
                arrowSink->AddLine(D2D1::Point2F(baseX, tipY + 2.6f));
                arrowSink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (SUCCEEDED(arrowSink->Close())) {
                    dcTarget_->FillGeometry(arrow.Get(), brush_.Get());
                }
            }
        };
        drawLoop(1.0f);
        drawLoop(-1.0f);

        if (id == kIdRepeatOne) {
            const D2D1_RECT_F badgeRect = D2D1::RectF(cx - 4.0f, cy - 5.0f, cx + 4.0f, cy + 5.0f);
            dcTarget_->DrawTextW(L"1", 1, badgeFormat_.Get(), badgeRect, brush_.Get());
        }
    }

    if (focused) {
        brush_->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.85f));
        dcTarget_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(2.5f, 2.5f, width - 2.5f, height - 2.5f), 3.0f, 3.0f), brush_.Get(), 1.0f);
    }

    dcTarget_->EndDraw();
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
    bold.lfWeight = FW_BOLD;
    HFONT nextBold = CreateFontIndirectW(&bold);

    const std::array<HWND, 3> regularControls{countLabel_, list_, emptyLabel_};
    for (HWND control : regularControls) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(nextRegular), TRUE);
    }

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
    if (clientWidth_ <= 0 || clientHeight_ <= 0) {
        return;
    }
    const int pad = Dip(kPaddingDip);
    const int toolbarHeight = Dip(kToolbarHeightDip);
    const int btnWidth = Dip(kToggleWidthDip);
    const int btnHeight = Dip(kToggleHeightDip);
    const int gap = Dip(kToggleGapDip);
    const int btnY = (toolbarHeight - btnHeight) / 2;

    int x = pad;
    MoveWindow(autoAdvanceButton_, x, btnY, btnWidth, btnHeight, TRUE);
    x += btnWidth + gap;
    MoveWindow(repeatOneButton_, x, btnY, btnWidth, btnHeight, TRUE);
    x += btnWidth + gap;
    MoveWindow(repeatAllButton_, x, btnY, btnWidth, btnHeight, TRUE);
    x += btnWidth + gap;

    const int countRight = std::max(x, clientWidth_ - pad);
    MoveWindow(countLabel_, x, 0, countRight - x, toolbarHeight, TRUE);

    const int listTop = toolbarHeight + 1;  // 1px horizontal divider below the toolbar.
    const int listHeight = std::max(0, clientHeight_ - listTop);
    MoveWindow(list_, 0, listTop, clientWidth_, listHeight, TRUE);

    const int emptyHeight = Dip(40);
    const int emptyY = listTop + std::max(0, (listHeight - emptyHeight) / 2);
    const int emptyWidth = std::max(0, clientWidth_ - pad * 2);
    MoveWindow(emptyLabel_, pad, emptyY, emptyWidth, emptyHeight, TRUE);
    FitColumns();
    InvalidateRect(window_, nullptr, FALSE);
}

void PlaylistPanel::FitColumns() {
    RECT rect{};
    GetClientRect(list_, &rect);
    ListView_SetColumnWidth(list_, 0, std::max(0, static_cast<int>(rect.right)));
}

void PlaylistPanel::RefreshHeaderCount() {
    if (files_.empty()) {
        SetWindowTextW(countLabel_, L"0개");
    } else {
        const std::wstring text = std::to_wstring(currentIndex_ + 1) + L" / " + std::to_wstring(files_.size());
        SetWindowTextW(countLabel_, text.c_str());
    }
}

void PlaylistPanel::RefreshOptionButtons() {
    for (HWND button : {autoAdvanceButton_, repeatOneButton_, repeatAllButton_}) {
        if (button) {
            InvalidateRect(button, nullptr, FALSE);
        }
    }
}

void PlaylistPanel::RefreshList() {
    const bool empty = files_.empty();
    ShowWindow(list_, empty ? SW_HIDE : SW_SHOW);
    ShowWindow(emptyLabel_, empty ? SW_SHOW : SW_HIDE);

    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);

    for (size_t i = 0; i < files_.size(); ++i) {
        std::wstring title = std::to_wstring(i + 1) + L". " + std::filesystem::path(files_[i]).filename().wstring();

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = title.data();
        ListView_InsertItem(list_, &item);
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
    std::vector<HWND> stops{autoAdvanceButton_, repeatOneButton_, repeatAllButton_};
    if (!files_.empty()) {
        stops.push_back(list_);
    }

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
    if (self) {
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && self->HandleControlKeyDown(control, wParam)) {
            return 0;
        }
        if (control != self->list_) {
            if (message == WM_MOUSEMOVE) {
                if (self->hoveredButton_ != control) {
                    HWND previous = self->hoveredButton_;
                    self->hoveredButton_ = control;
                    if (previous) {
                        InvalidateRect(previous, nullptr, FALSE);
                    }
                    InvalidateRect(control, nullptr, FALSE);
                }
                if (!self->trackingHover_) {
                    TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, control, 0};
                    self->trackingHover_ = TrackMouseEvent(&track) != FALSE;
                }
            } else if (message == WM_MOUSELEAVE) {
                self->trackingHover_ = false;
                if (self->hoveredButton_ == control) {
                    self->hoveredButton_ = nullptr;
                    InvalidateRect(control, nullptr, FALSE);
                }
            } else if (message == WM_LBUTTONDBLCLK) {
                // Fast repeated clicks on an icon toggle should act like a second click.
                message = WM_LBUTTONDOWN;
            }
        }
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
    case WM_SIZE:
        clientWidth_ = LOWORD(lParam);
        clientHeight_ = HIWORD(lParam);
        LayoutControls();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        UpdateFonts();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutControls();
        RefreshOptionButtons();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Dip(kMinWindowWidthDip);
        info->ptMinTrackSize.y = Dip(kMinWindowHeightDip);
        return 0;
    }
    case WM_EXITSIZEMOVE:
        panelClientWidthDip_ = std::max(kMinWindowWidthDip - 20, MulDiv(clientWidth_, 96, static_cast<int>(dpi_)));
        UpdateDockedState();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC hdc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        FillRect(hdc, &client, GetSysColorBrush(COLOR_WINDOW));

        const int dividerY = Dip(kToolbarHeightDip);
        RECT divider{0, dividerY, client.right, dividerY + 1};
        FillRect(hdc, &divider, GetSysColorBrush(COLOR_BTNFACE));

        EndPaint(window_, &paint);
        return 0;
    }
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (draw && draw->CtlType == ODT_BUTTON) {
            DrawToggleButton(*draw);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
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
        SetFocus(files_.empty() ? autoAdvanceButton_ : list_);
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        std::vector<std::wstring> paths;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length > 0) {
                std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
                DragQueryFileW(drop, i, buffer.data(), length + 1);
                paths.emplace_back(buffer.data(), length);
            }
        }
        DragFinish(drop);
        if (!paths.empty() && callbacks_.onFilesDropped) {
            callbacks_.onFilesDropped(paths);
        }
        return 0;
    }
    case WM_CLOSE:
        SetOpen(false);
        return 0;
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}
