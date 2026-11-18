#include "MainWindow.h"

#include <imm.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <stdexcept>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "ContextMenu.h"
#include "FileDialogs.h"
#include "InfoDialog.h"
#include "SettingsDialog.h"
#include "SmiSubtitles.h"
#include "TimeFormat.h"

namespace {

constexpr wchar_t kVideoWindowClass[] = L"MoviePlayer.VideoWindow";
constexpr wchar_t kAppTitle[] = L"Movie Player";
constexpr UINT WM_MPV_WAKEUP = WM_APP + 1;
constexpr int kAppIconId = 1;

constexpr int kVolumeStep = 5;
constexpr double kSubtitleDelayStep = 0.1;
constexpr double kAudioDelayStep = 0.1;
constexpr double kSpeedStep = 0.1;
constexpr int kVideoAdjustStep = 2;
constexpr int kDefaultWidth = 1280;   // Device-independent pixels, at 96 DPI.
constexpr int kDefaultHeight = 720;
constexpr int kMaxScreenPercent = 90;
// Device-independent pixels. Smaller videos are enlarged so the controls stay usable.
constexpr int kMinVideoWidth = 320;
constexpr UINT_PTR kIdleTimer = 1;
constexpr UINT kIdleDelayMs = 2000;
constexpr UINT_PTR kOpenRetryTimer = 3;
constexpr UINT_PTR kSeriesTimer = 4;
constexpr UINT WM_SERIES_FOUND = WM_APP + 2;
// Explorer opens each of several selected files as its own copy of the app, which hands
// the file to this window. Files arriving this close together are one selection.
constexpr UINT kExplorerBatchMs = 1500;

// A background search's answer, handed to the window with WM_SERIES_FOUND.
struct SeriesResult {
    unsigned search;
    std::wstring video;
    std::vector<std::wstring> series;
};
// Right after opening or dragging from Explorer, a file on a network or virtual drive
// such as RaiDrive can open before its real contents are readable.
constexpr UINT kOpenRetryDelaysMs[] = {500, 1000, 1500, 2000, 3000};

// After a seek is released, keep showing its target until mpv gets there, so the
// bar doesn't jump back to the old position. The expiry covers a seek that lands
// somewhere else, and the tolerance covers one that lands slightly off target.
constexpr ULONGLONG kSeekPreviewExpiryMs = 3000;
constexpr double kSeekPreviewToleranceSeconds = 2.0;

void RegisterWindowClass(HINSTANCE instance, const wchar_t* name, WNDPROC proc, UINT style) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = style;
    windowClass.lpfnWndProc = proc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kAppIconId));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = name;
    if (!RegisterClassExW(&windowClass)) {
        throw std::runtime_error("RegisterClassEx failed");
    }
}

std::filesystem::path SubtitleTempDirectory() {
    wchar_t temp[MAX_PATH + 1];
    const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
    const std::filesystem::path root = length > 0 ? std::filesystem::path(temp) : std::filesystem::path();
    return root / L"MoviePlayer" / std::to_wstring(GetCurrentProcessId());
}

POINT PointFromLParam(LPARAM lParam) {
    return {static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
}

}  // namespace

MainWindow::MainWindow(HINSTANCE instance, const Settings& settings)
    : preferences_(settings.preferences), subtitleTempDirectory_(SubtitleTempDirectory()) {
    playback_.volume = settings.volume;
    playback_.subtitleEncoding = settings.subtitleEncoding;
    playback_.adjustments = settings.videoAdjustments;

    RegisterWindowClass(instance, kClassName, &MainWindow::WindowProc, CS_DBLCLKS);
    RegisterWindowClass(instance, kVideoWindowClass, &DefWindowProcW, 0);

    window_ = CreateWindowExW(
        WS_EX_ACCEPTFILES, kClassName, kAppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        nullptr, nullptr, instance, this);
    if (!window_) {
        throw std::runtime_error("CreateWindowEx failed for the main window");
    }

    RECT client{};
    GetClientRect(window_, &client);
    videoWindow_ = CreateWindowExW(
        0, kVideoWindowClass, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, client.right, client.bottom,
        window_, nullptr, instance, nullptr);
    if (!videoWindow_) {
        throw std::runtime_error("CreateWindowEx failed for the video window");
    }
    // With a Korean input mode on, letter keys would arrive as VK_PROCESSKEY
    // instead of as themselves, and shortcuts like S and A would stop working.
    ImmAssociateContextEx(window_, nullptr, 0);

    // mpv's window lives inside this one, on mpv's own thread. Windows skips a
    // disabled child when deciding who gets mouse input, so disabling it sends
    // clicks, the wheel and dropped files to the main window instead of to mpv.
    EnableWindow(videoWindow_, FALSE);

    controlBar_ = std::make_unique<ControlBar>(window_);
    PlaylistPanel::Callbacks panelCallbacks;
    panelCallbacks.onAnimationStep = [this] { LayoutChildren(); };
    panelCallbacks.onPlayItem = [this](size_t index) { PlayPlaylistItem(index); };
    panelCallbacks.onToggleAutoAdvance = [this] { ToggleAutoAdvance(); };
    panelCallbacks.onToggleRepeatOne = [this] { ToggleRepeatOne(); };
    panelCallbacks.onToggleRepeatAll = [this] { ToggleRepeatAll(); };
    panelCallbacks.onTogglePause = [this] {
        player_->TogglePause();
        ShowControls();
    };
    playlistPanel_ = std::make_unique<PlaylistPanel>(window_, instance, std::move(panelCallbacks));
    playlistPanel_->SetToggleShortcut(preferences_.shortcuts[static_cast<size_t>(ShortcutAction::PlaylistShow)]);
    SyncPlaylistPanel();

    RestoreWindowPosition(settings);
    LayoutChildren();

    MpvPlayer::Callbacks callbacks;
    callbacks.wakeup = [window = window_] { PostMessageW(window, WM_MPV_WAKEUP, 0, 0); };
    callbacks.playbackError = [this](const std::wstring& reason, bool unrecognizedFormat) {
        HandlePlaybackError(reason, unrecognizedFormat);
    };
    callbacks.subtitleLoadFailed = [this] { player_->ShowMessage(L"자막 파일을 열 수 없습니다"); };
    callbacks.stateChanged = [this](const PlaybackState& state) { HandleStateChanged(state); };
    callbacks.videoReady = [this](int width, int height) { HandleVideoReady(width, height); };
    player_ = std::make_unique<MpvPlayer>(videoWindow_, std::move(callbacks), settings.volume, settings.subtitleEncoding,
                                          settings.videoAdjustments, settings.preferences.subtitleStyle);
    ApplyRepeatMode();
}

void MainWindow::Show(int showCommand) {
    const bool minimized = showCommand == SW_SHOWMINIMIZED || showCommand == SW_MINIMIZE || showCommand == SW_SHOWMINNOACTIVE;
    ShowWindow(window_, startMaximized_ && !minimized ? SW_SHOWMAXIMIZED : showCommand);
    UpdateWindow(window_);
}

void MainWindow::ArmOpenRetry(const std::wstring& path, const std::wstring& preferredSubtitle) {
    KillTimer(window_, kOpenRetryTimer);
    openRetryPath_ = path;
    openRetrySubtitle_ = preferredSubtitle;
    openRetryCount_ = 0;
}

void MainWindow::ClearOpenRetry() {
    KillTimer(window_, kOpenRetryTimer);
    openRetryPath_.clear();
    openRetrySubtitle_.clear();
    openRetryCount_ = 0;
}

void MainWindow::HandlePlaybackError(const std::wstring& reason, bool unrecognizedFormat) {
    if (unrecognizedFormat && !openRetryPath_.empty() && openRetryCount_ < std::size(kOpenRetryDelaysMs)) {
        const UINT delay = kOpenRetryDelaysMs[openRetryCount_++];
        if (openRetryCount_ >= 2 && player_) {
            player_->ShowMessage(L"파일을 읽는 중...");
        }
        SetTimer(window_, kOpenRetryTimer, delay, nullptr);
        return;
    }
    ClearOpenRetry();
    currentVideoPath_.clear();
    const std::wstring text = L"동영상을 재생할 수 없습니다.\n\n" + reason;
    MessageBoxW(window_, text.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
}

void MainWindow::Open(const std::wstring& path) {
    ClearOpenRetry();
    lastExplorerOpen_ = 0;
    if (IsSubtitleFile(path)) {
        AddSubtitle(path);
        ShowControls();
    } else {
        StartPlaylist({path}, 0, {}, true);
    }
}

void MainWindow::OpenFiles(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> videos;
    for (const std::wstring& path : paths) {
        if (!IsSubtitleFile(path)) {
            videos.push_back(path);
        }
    }
    if (videos.size() == 1) {
        Open(videos.front());
    } else if (videos.empty()) {
        for (const std::wstring& path : paths) {
            Open(path);
        }
    } else {
        ClearOpenRetry();
        lastExplorerOpen_ = 0;
        SortByName(videos);
        StartPlaylist(std::move(videos), 0, {}, false);
    }
}

void MainWindow::StartPlaylist(std::vector<std::wstring> files, size_t index, const std::wstring& preferredSubtitle, bool findSeries) {
    playlist_ = std::move(files);
    playlistIndex_ = index;
    keepWindowSize_ = false;
    ++seriesSearch_;
    KillTimer(window_, kSeriesTimer);
    ArmOpenRetry(playlist_[playlistIndex_], preferredSubtitle);
    OpenVideo(playlist_[playlistIndex_], preferredSubtitle);
    SyncPlaylistPanel();
    ShowControls();
    // The file plays at once; the rest of its series is looked for after a moment, which
    // also leaves time for more files of a selection to arrive from Explorer.
    if (findSeries) {
        SetTimer(window_, kSeriesTimer, kExplorerBatchMs, nullptr);
    }
}

void MainWindow::FindSeriesInBackground() {
    if (playlist_.empty()) {
        return;
    }
    const unsigned search = seriesSearch_;
    const std::wstring video = playlist_[playlistIndex_];
    // Reading a folder on a network drive can take seconds, so it's done off the UI thread.
    std::thread([window = window_, search, video] {
        auto result = std::make_unique<SeriesResult>(SeriesResult{search, video, FindSeries(video)});
        if (PostMessageW(window, WM_SERIES_FOUND, 0, reinterpret_cast<LPARAM>(result.get()))) {
            result.release();
        }
    }).detach();
}

void MainWindow::HandleSeriesFound(LPARAM message) {
    const std::unique_ptr<SeriesResult> result(reinterpret_cast<SeriesResult*>(message));
    if (result->search != seriesSearch_ || playlist_.size() != 1 || _wcsicmp(playlist_.front().c_str(), result->video.c_str()) != 0) {
        return;  // Something else was opened meanwhile.
    }
    const auto itself = std::find_if(result->series.begin(), result->series.end(),
                                     [&](const std::wstring& path) { return _wcsicmp(path.c_str(), result->video.c_str()) == 0; });
    if (itself != result->series.end()) {
        playlistIndex_ = static_cast<size_t>(itself - result->series.begin());
        playlist_ = std::move(result->series);
        SyncPlaylistPanel();
    }
}

void MainWindow::PlayPlaylistItem(size_t index) {
    if (index >= playlist_.size()) {
        return;
    }
    playlistIndex_ = index;
    keepWindowSize_ = !preferences_.fitWindowOnPlaylistChange;
    ArmOpenRetry(playlist_[index]);
    OpenVideo(playlist_[index]);
    SyncPlaylistPanel();
    player_->ShowMessage(std::to_wstring(index + 1) + L"/" + std::to_wstring(playlist_.size()) + L": " +
                         std::filesystem::path(playlist_[index]).filename().wstring());
    ShowControls();
}

void MainWindow::PlayNext(bool automatic) {
    if (playlist_.empty()) {
        return;
    }
    if (playlistIndex_ + 1 < playlist_.size()) {
        PlayPlaylistItem(playlistIndex_ + 1);
    } else if (preferences_.repeat == RepeatMode::All) {
        PlayPlaylistItem(0);
    } else if (!automatic) {
        player_->ShowMessage(L"마지막 파일입니다");
    }
}

void MainWindow::PlayPrevious() {
    if (playlist_.empty()) {
        return;
    }
    if (playlistIndex_ > 0) {
        PlayPlaylistItem(playlistIndex_ - 1);
    } else if (preferences_.repeat == RepeatMode::All) {
        PlayPlaylistItem(playlist_.size() - 1);
    } else {
        player_->ShowMessage(L"첫 파일입니다");
    }
}

void MainWindow::ApplyRepeatMode() {
    player_->SetRepeatFile(preferences_.repeat == RepeatMode::One);
}

PopupPlace MainWindow::PopupPlaceForKeys() const {
    POINT cursor{};
    RECT window{};
    GetCursorPos(&cursor);
    GetWindowRect(window_, &window);
    if (PtInRect(&window, cursor)) {
        return {cursor, false};
    }
    RECT client{};
    GetClientRect(window_, &client);
    POINT center{client.right / 2, client.bottom / 2};
    ClientToScreen(window_, &center);
    return {center, true};
}

void MainWindow::ShowPlaylist() {
    const PopupPlace place = PopupPlaceForKeys();
    menuOpen_ = true;
    SetCursorHidden(false);
    ShowControls();
    const PlaylistChoice choice = ShowPlaylistMenu(window_, place, playlist_, playlistIndex_, preferences_.autoAdvance, preferences_.repeat);
    menuOpen_ = false;
    ShowControls();

    const auto announce = [this](const wchar_t* name, bool on) { player_->ShowMessage(std::wstring(name) + (on ? L" 켬" : L" 끔")); };
    switch (choice.action) {
    case PlaylistChoice::Action::Play:
        PlayPlaylistItem(choice.index);
        return;
    case PlaylistChoice::Action::ToggleAutoAdvance:
        preferences_.autoAdvance = !preferences_.autoAdvance;
        // Repeating the whole list means moving on, so it can't stay without that.
        if (!preferences_.autoAdvance && preferences_.repeat == RepeatMode::All) {
            preferences_.repeat = RepeatMode::None;
        }
        announce(L"다음 파일 자동 재생", preferences_.autoAdvance);
        break;
    case PlaylistChoice::Action::ToggleRepeatOne:
        preferences_.repeat = preferences_.repeat == RepeatMode::One ? RepeatMode::None : RepeatMode::One;
        announce(L"한 개 반복", preferences_.repeat == RepeatMode::One);
        break;
    case PlaylistChoice::Action::ToggleRepeatAll:
        preferences_.repeat = preferences_.repeat == RepeatMode::All ? RepeatMode::None : RepeatMode::All;
        if (preferences_.repeat == RepeatMode::All) {
            preferences_.autoAdvance = true;
        }
        announce(L"전체 목록 반복", preferences_.repeat == RepeatMode::All);
        break;
    case PlaylistChoice::Action::None:
        return;
    }
    ApplyRepeatMode();
    SaveCurrentSettings();
}

void MainWindow::TogglePlaylistPanel() {
    if (!playlistPanel_) {
        return;
    }
    const bool opening = !playlistPanel_->IsOpen();
    if (opening) {
        SetCursorHidden(false);
        SyncPlaylistPanel();
    }
    playlistPanel_->SetOpen(opening);
    ShowControls();
}

void MainWindow::SyncPlaylistPanel() {
    if (!playlistPanel_) {
        return;
    }
    playlistPanel_->SetPlaylist(playlist_, playlistIndex_);
    playlistPanel_->SetOptions(preferences_.autoAdvance, preferences_.repeat);
}

void MainWindow::ToggleAutoAdvance() {
    preferences_.autoAdvance = !preferences_.autoAdvance;
    // Repeating the whole list means moving on, so it can't stay without that.
    if (!preferences_.autoAdvance && preferences_.repeat == RepeatMode::All) {
        preferences_.repeat = RepeatMode::None;
    }
    player_->ShowMessage(std::wstring(L"다음 파일 자동 재생") + (preferences_.autoAdvance ? L" 켬" : L" 끔"));
    ApplyRepeatMode();
    SaveCurrentSettings();
    SyncPlaylistPanel();
}

void MainWindow::ToggleRepeatOne() {
    preferences_.repeat = preferences_.repeat == RepeatMode::One ? RepeatMode::None : RepeatMode::One;
    player_->ShowMessage(std::wstring(L"한 개 반복") + (preferences_.repeat == RepeatMode::One ? L" 켬" : L" 끔"));
    ApplyRepeatMode();
    SaveCurrentSettings();
    SyncPlaylistPanel();
}

void MainWindow::ToggleRepeatAll() {
    preferences_.repeat = preferences_.repeat == RepeatMode::All ? RepeatMode::None : RepeatMode::All;
    if (preferences_.repeat == RepeatMode::All) {
        preferences_.autoAdvance = true;
    }
    player_->ShowMessage(std::wstring(L"전체 목록 반복") + (preferences_.repeat == RepeatMode::All ? L" 켬" : L" 끔"));
    ApplyRepeatMode();
    SaveCurrentSettings();
    SyncPlaylistPanel();
}

void MainWindow::OpenFromExplorer(const std::wstring& path) {
    if (!IsSubtitleFile(path)) {
        const ULONGLONG now = GetTickCount64();
        const bool sameSelection = lastExplorerOpen_ != 0 && now - lastExplorerOpen_ < kExplorerBatchMs && !playlist_.empty();
        if (sameSelection) {
            // Several files were selected: they are the playlist, not the series of the first.
            lastExplorerOpen_ = now;
            ++seriesSearch_;
            KillTimer(window_, kSeriesTimer);
            const std::wstring playing = playlist_[playlistIndex_];
            if (std::none_of(playlist_.begin(), playlist_.end(), [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; })) {
                playlist_.push_back(path);
            }
            SortByName(playlist_);
            playlistIndex_ = static_cast<size_t>(std::find(playlist_.begin(), playlist_.end(), playing) - playlist_.begin());
            SyncPlaylistPanel();
            return;
        }
        Open(path);
        lastExplorerOpen_ = now;
        return;
    }
    const std::wstring video = FindVideoForSubtitle(path);
    if (!video.empty() && _wcsicmp(video.c_str(), currentVideoPath_.c_str()) != 0) {
        ClearOpenRetry();
        lastExplorerOpen_ = 0;
        StartPlaylist({video}, 0, path, true);
    } else if (!currentVideoPath_.empty()) {
        // Its video is already playing, or it has none: use it with what's on screen.
        Open(path);
    } else {
        const std::wstring text = L"이 자막과 이름이 같은 영상 파일을 찾을 수 없습니다.\n\n" + path;
        MessageBoxW(window_, text.c_str(), kAppTitle, MB_OK | MB_ICONINFORMATION);
    }
}

void MainWindow::OpenVideo(const std::wstring& path, const std::wstring& preferredSubtitle) {
    videoWidth_ = 0;
    videoHeight_ = 0;
    currentVideoPath_ = path;
    pendingSubtitleName_.clear();
    // Language parts split out for the previous video aren't needed any more.
    std::error_code error;
    std::filesystem::remove_all(subtitleTempDirectory_, error);

    std::vector<std::wstring> subtitles;
    if (!preferredSubtitle.empty()) {
        for (const std::filesystem::path& file : IsSmiFile(preferredSubtitle) ? PrepareSmiFile(preferredSubtitle, subtitleTempDirectory_)
                                                                              : std::vector<std::filesystem::path>{preferredSubtitle}) {
            subtitles.push_back(file.wstring());
        }
        pendingSubtitleName_ = std::filesystem::path(subtitles.front()).filename().wstring();
    }
    for (const std::filesystem::path& file : PrepareSmiFilesFor(path, subtitleTempDirectory_)) {
        if (std::find(subtitles.begin(), subtitles.end(), file.wstring()) == subtitles.end()) {
            subtitles.push_back(file.wstring());
        }
    }
    player_->Open(path, subtitles);
}

void MainWindow::AddSubtitle(const std::wstring& path) {
    std::vector<std::filesystem::path> files{path};
    if (IsSmiFile(path)) {
        files = PrepareSmiFile(path, subtitleTempDirectory_);
    }
    // The preferred language comes first: add the others quietly, and switch to it last.
    for (size_t i = files.size(); i-- > 1;) {
        player_->AddSubtitleFile(files[i].wstring(), false);
    }
    player_->AddSubtitleFile(files.front().wstring(), true);
}

void MainWindow::ApplyDefaultWindowSize() {
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return;
    }
    const RECT& work = monitor.rcWork;
    const int dpi = static_cast<int>(GetDpiForWindow(window_));
    int width = MulDiv(kDefaultWidth, dpi, 96);
    int height = MulDiv(kDefaultHeight, dpi, 96);
    // Shrink to fit small screens, keeping the shape.
    const int maxWidth = (work.right - work.left) * kMaxScreenPercent / 100;
    const int maxHeight = (work.bottom - work.top) * kMaxScreenPercent / 100;
    if (width > maxWidth) {
        height = MulDiv(height, maxWidth, width);
        width = maxWidth;
    }
    if (height > maxHeight) {
        width = MulDiv(width, maxHeight, height);
        height = maxHeight;
    }
    SetWindowPos(window_, nullptr, work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void MainWindow::HandleVideoReady(int width, int height) {
    ClearOpenRetry();
    if (!pendingSubtitleName_.empty()) {
        for (const MediaTrack& track : playback_.tracks) {
            if (track.type == TrackType::Subtitle && _wcsicmp(track.externalFileName.c_str(), pendingSubtitleName_.c_str()) == 0) {
                player_->SelectSubtitle(track.id);
                break;
            }
        }
        pendingSubtitleName_.clear();
    }
    videoWidth_ = width;
    videoHeight_ = height;
    const bool center = std::exchange(centerOnFirstFit_, false);
    const bool keepSize = std::exchange(keepWindowSize_, false);
    // The user's chosen fullscreen or maximized state takes priority, and so does the
    // size they gave the window for an earlier file in the playlist.
    if (!keepSize && !fullscreen_ && !IsZoomed(window_) && !IsIconic(window_)) {
        FitWindowToVideo(1.0, center);
    }
}

void MainWindow::FitWindowToVideo(double requestedScale, bool center) {
    // Turned on its side from the menu, the picture needs the other shape.
    const bool sideways = playback_.rotation % 180 != 0;
    const int videoWidth = sideways ? videoHeight_ : videoWidth_;
    const int videoHeight = sideways ? videoWidth_ : videoHeight_;
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (videoWidth <= 0 || videoHeight <= 0 || !GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return;
    }
    const RECT& work = monitor.rcWork;
    const UINT dpi = GetDpiForWindow(window_);

    RECT frame{};
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)), dpi);
    // The frame's sides and bottom are invisible resize borders, which can hang off the
    // monitor. What has to fit is the picture and the title bar above it.
    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;
    const int titleHeight = -frame.top;

    // Scale 1 is one video pixel to one screen pixel. Too small a window gets enlarged, and the monitor sets the limit.
    double scale = requestedScale;
    const int minWidth = MulDiv(kMinVideoWidth, static_cast<int>(dpi), 96);
    if (videoWidth * scale < minWidth) {
        scale = static_cast<double>(minWidth) / videoWidth;
    }
    scale = std::min({scale, static_cast<double>(workWidth) / videoWidth, static_cast<double>(workHeight - titleHeight) / videoHeight});
    const int pictureWidth = static_cast<int>(std::lround(videoWidth * scale));
    const int pictureHeight = static_cast<int>(std::lround(videoHeight * scale));

    RECT current{};
    GetWindowRect(window_, &current);
    const int pictureLeft = center ? work.left + (workWidth - pictureWidth) / 2
                                   : std::clamp(static_cast<int>(current.left - frame.left), static_cast<int>(work.left),
                                                static_cast<int>(work.right) - pictureWidth);
    const int top = center ? work.top + (workHeight - titleHeight - pictureHeight) / 2
                           : std::clamp(static_cast<int>(current.top), static_cast<int>(work.top),
                                        static_cast<int>(work.bottom) - titleHeight - pictureHeight);
    SetWindowPos(window_, nullptr, pictureLeft + frame.left, top, pictureWidth + frame.right - frame.left,
                 pictureHeight + frame.bottom - frame.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void MainWindow::SetWindowSizePercent(int percent) {
    if (videoWidth_ <= 0) {
        player_->ShowMessage(L"크기를 맞출 영상이 없습니다");
        return;
    }
    // Asking for a size means leaving the states that fill the screen.
    SetFullscreen(false);
    if (IsZoomed(window_)) {
        ShowWindow(window_, SW_RESTORE);
    }
    FitWindowToVideo(percent / 100.0, false);
    player_->ShowMessage(L"화면 크기 " + std::to_wstring(percent) + L"%");
    ShowControls();
}

void MainWindow::RestoreWindowPosition(const Settings& settings) {
    if (!settings.windowPosition) {
        ApplyDefaultWindowSize();
        centerOnFirstFit_ = true;
        return;
    }
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    placement.showCmd = SW_HIDE;
    placement.rcNormalPosition = *settings.windowPosition;
    // Windows moves a window back on screen if its saved monitor is gone. It also
    // sends WM_DPICHANGED if the monitor differs, but the saved size is already right.
    restoringPosition_ = true;
    SetWindowPlacement(window_, &placement);
    restoringPosition_ = false;
    startMaximized_ = settings.windowMaximized;
}

void MainWindow::SaveCurrentSettings() {
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    if (fullscreen_) {
        placement = restorePlacement_;
    } else if (!GetWindowPlacement(window_, &placement)) {
        return;
    }
    Settings settings;
    settings.volume = playback_.volume;
    settings.subtitleEncoding = playback_.subtitleEncoding;
    settings.windowPosition = placement.rcNormalPosition;
    settings.videoAdjustments = playback_.adjustments;
    settings.preferences = preferences_;
    settings.windowMaximized = placement.showCmd == SW_SHOWMAXIMIZED ||
                               (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);
    SaveSettings(settings);
}

void MainWindow::HandleCopyData(const COPYDATASTRUCT& data) {
    if (data.dwData != kOpenPathMessageId) {
        return;
    }
    if (IsIconic(window_)) {
        ShowWindow(window_, SW_RESTORE);
    }
    SetForegroundWindow(GetLastActivePopup(window_));  // The settings window, if it's open.
    const size_t length = data.cbData / sizeof(wchar_t);
    if (data.lpData && length > 0) {
        OpenFromExplorer(std::wstring(static_cast<const wchar_t*>(data.lpData), length));
    }
}

void MainWindow::RunDialog(const std::function<void()>& show) {
    // Out of the way, so a subtitle being restyled can be seen. Nothing can bring
    // the bar back meanwhile, since the disabled window gets no mouse or key input.
    dialogOpen_ = true;
    KillTimer(window_, kIdleTimer);
    hoveredPart_ = ControlBar::Part::None;
    controlBar_->SetHoveredPart(hoveredPart_);
    controlBar_->SetVisible(false);
    SetCursorHidden(false);

    show();

    dialogOpen_ = false;
    ShowControls();
}

void MainWindow::OpenSettings() {
    RunDialog([this] {
        ShowSettingsDialog(window_, preferences_, [this](const Preferences& preferences) { ApplyPreferences(preferences); });
    });
}

void MainWindow::ShowChapterList() {
    if (playback_.chapters.empty()) {
        player_->ShowMessage(L"챕터가 없습니다");
        return;
    }
    const PopupPlace place = PopupPlaceForKeys();

    menuOpen_ = true;
    SetCursorHidden(false);
    ShowControls();
    const std::optional<size_t> picked = ShowChapterMenu(window_, place, playback_);
    menuOpen_ = false;
    ShowControls();

    // The list can change while the menu is open, if a file arrives from elsewhere.
    if (picked && *picked < playback_.chapters.size()) {
        const Chapter& chapter = playback_.chapters[*picked];
        SeekWithPreview(chapter.time, true);
        player_->ShowMessage(L"챕터 " + std::to_wstring(*picked + 1) + L"/" + std::to_wstring(playback_.chapters.size()) + L": " + chapter.title);
    }
}

void MainWindow::ApplyPreferences(const Preferences& preferences) {
    if (!(preferences.subtitleStyle == preferences_.subtitleStyle)) {
        player_->SetSubtitleStyle(preferences.subtitleStyle);
    }
    preferences_ = preferences;
    if (playlistPanel_) {
        playlistPanel_->SetToggleShortcut(preferences_.shortcuts[static_cast<size_t>(ShortcutAction::PlaylistShow)]);
    }
    SaveCurrentSettings();
}

LRESULT CALLBACK MainWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    self->window_ = window;
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        LayoutChildren();
        return 0;
    case WM_COPYDATA:
        HandleCopyData(*reinterpret_cast<const COPYDATASTRUCT*>(lParam));
        return TRUE;
    case WM_DPICHANGED: {
        if (restoringPosition_) {
            LayoutChildren();
            return 0;
        }
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutChildren();
        return 0;
    }
    case WM_MPV_WAKEUP:
        if (player_) {
            player_->ProcessEvents();
        }
        return 0;
    case WM_KEYDOWN:
        HandleKeyDown(wParam, (lParam & (1 << 30)) != 0);
        return 0;
    // Keys held with Alt, and F10, arrive as system keys.
    case WM_SYSKEYDOWN:
        if (HandleKeyDown(wParam, (lParam & (1 << 30)) != 0)) {
            swallowingSystemKeys_ = true;
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_SYSKEYUP:
        if (swallowingSystemKeys_) {
            swallowingSystemKeys_ = GetKeyState(VK_MENU) < 0;
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_LBUTTONDOWN:
        HandleLeftButtonDown(PointFromLParam(lParam));
        return 0;
    case WM_LBUTTONDBLCLK:
        // On the controls, the second click of a double-click is just another click.
        if (controlBar_->HitTest(PointFromLParam(lParam)) == ControlBar::Part::None) {
            ToggleFullscreen();
        } else {
            HandleLeftButtonDown(PointFromLParam(lParam));
        }
        return 0;
    case WM_LBUTTONUP:
        HandleLeftButtonUp(PointFromLParam(lParam));
        return 0;
    case WM_CAPTURECHANGED:
        HandleCaptureLost();
        return 0;
    case WM_MOUSEWHEEL:
        HandleMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_MOUSEMOVE:
        HandleMouseMove(PointFromLParam(lParam));
        return 0;
    case WM_MOUSELEAVE:
        HandleMouseLeave();
        return 0;
    case WM_TIMER:
        if (wParam == kIdleTimer) {
            KillTimer(window_, kIdleTimer);
            HandleIdleTimeout();
        } else if (wParam == ControlBar::kFadeTimerId && controlBar_) {
            controlBar_->HandleFadeTimer();
        } else if (wParam == kOpenRetryTimer) {
            KillTimer(window_, kOpenRetryTimer);
            if (!openRetryPath_.empty()) {
                OpenVideo(openRetryPath_, openRetrySubtitle_);
            }
        } else if (wParam == kSeriesTimer) {
            KillTimer(window_, kSeriesTimer);
            FindSeriesInBackground();
        }
        return 0;
    case WM_SERIES_FOUND:
        HandleSeriesFound(lParam);
        return 0;
    case WM_SETCURSOR:
        if (cursorHidden_ && LOWORD(lParam) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_CONTEXTMENU:
        HandleContextMenu(PointFromLParam(lParam));
        return 0;
    case WM_DROPFILES:
        HandleDroppedFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_CLOSE:
        if (menuOpen_) {
            // Likewise for a menu, whose caller goes on to use the bar.
            EndMenu();
            PostMessageW(window_, WM_CLOSE, 0, 0);
            return 0;
        }
        if (dialogOpen_) {
            // That window is running on top of a call in this one, so finish it first.
            if (HWND popup = GetWindow(window_, GW_ENABLEDPOPUP)) {
                PostMessageW(popup, WM_COMMAND, IDCANCEL, 0);
            }
            PostMessageW(window_, WM_CLOSE, 0, 0);
            return 0;
        }
        SaveCurrentSettings();
        ClearOpenRetry();
        // mpv renders into videoWindow_, so stop it before the window goes away.
        player_.reset();
        playlistPanel_.reset();
        controlBar_.reset();
        {
            std::error_code error;
            std::filesystem::remove_all(subtitleTempDirectory_, error);
        }
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

void MainWindow::LayoutChildren() {
    RECT client{};
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    const int panelWidth = playlistPanel_ ? playlistPanel_->Layout(client.right, client.bottom, dpi) : 0;
    if (videoWindow_) {
        MoveWindow(videoWindow_, 0, 0, client.right, client.bottom, TRUE);
    }
    if (controlBar_) {
        controlBar_->Resize(std::max(0, static_cast<int>(client.right) - panelWidth), client.bottom, dpi);
    }
}

void MainWindow::ShowControls() {
    controlBar_->SetVisible(true);
    SetTimer(window_, kIdleTimer, kIdleDelayMs, nullptr);
}

bool MainWindow::HandleKeyDown(WPARAM key, bool isRepeat) {
    const KeyChord chord = ChordFromKey(key);
    const std::optional<ShortcutAction> action = FindShortcut(preferences_.shortcuts, chord);
    if (chord == VK_ESCAPE && playlistPanel_ && playlistPanel_->IsOpen() && action != ShortcutAction::PlaylistShow) {
        MSG character{};
        while (PeekMessageW(&character, window_, WM_CHAR, WM_DEADCHAR, PM_REMOVE) ||
               PeekMessageW(&character, window_, WM_SYSCHAR, WM_SYSDEADCHAR, PM_REMOVE)) {
        }
        if (!isRepeat) {
            playlistPanel_->SetOpen(false);
        }
        return true;
    }
    if (!action) {
        return false;
    }
    // The message loop has already queued the key's character. A menu opened by this
    // shortcut would take it as a search for an item, and beep when none matched.
    MSG character{};
    while (PeekMessageW(&character, window_, WM_CHAR, WM_DEADCHAR, PM_REMOVE) ||
           PeekMessageW(&character, window_, WM_SYSCHAR, WM_SYSDEADCHAR, PM_REMOVE)) {
    }
    if (!isRepeat || GetShortcutInfo(*action).repeats) {
        RunShortcut(*action);
    }
    return true;
}

void MainWindow::RunShortcut(ShortcutAction action) {
    switch (action) {
    case ShortcutAction::TogglePause:
        player_->TogglePause();
        ShowControls();
        break;
    case ShortcutAction::SeekBackward:
        player_->SeekBy(-preferences_.seekSeconds);
        ShowControls();
        break;
    case ShortcutAction::SeekForward:
        player_->SeekBy(preferences_.seekSeconds);
        ShowControls();
        break;
    case ShortcutAction::VolumeUp:
        player_->ChangeVolume(kVolumeStep);
        ShowControls();
        break;
    case ShortcutAction::VolumeDown:
        player_->ChangeVolume(-kVolumeStep);
        ShowControls();
        break;
    case ShortcutAction::ToggleFullscreen:
        ToggleFullscreen();
        break;
    case ShortcutAction::ExitFullscreen:
        SetFullscreen(false);
        break;
    case ShortcutAction::CycleSubtitle:
        CycleSubtitle();
        break;
    case ShortcutAction::CycleAudio:
        CycleAudio();
        break;
    case ShortcutAction::SubtitleDelayEarlier:
        AdjustSubtitleDelay(-kSubtitleDelayStep);
        break;
    case ShortcutAction::SubtitleDelayLater:
        AdjustSubtitleDelay(kSubtitleDelayStep);
        break;
    case ShortcutAction::SubtitleDelayReset:
        announceSubtitleDelay_ = true;
        player_->ResetSubtitleDelay();
        break;
    case ShortcutAction::SpeedUp:
        ChangeSpeed(kSpeedStep);
        break;
    case ShortcutAction::SpeedDown:
        ChangeSpeed(-kSpeedStep);
        break;
    case ShortcutAction::SpeedReset:
        SetSpeed(1.0);
        break;
    case ShortcutAction::LoopStart:
        SetLoopStart();
        break;
    case ShortcutAction::LoopEnd:
        SetLoopEnd();
        break;
    case ShortcutAction::LoopClear:
        ClearLoop();
        break;
    case ShortcutAction::FrameForward:
        StepFrame(true);
        break;
    case ShortcutAction::FrameBackward:
        StepFrame(false);
        break;
    case ShortcutAction::BrightnessDown:
        AdjustVideo(VideoAdjustment::Brightness, -kVideoAdjustStep);
        break;
    case ShortcutAction::BrightnessUp:
        AdjustVideo(VideoAdjustment::Brightness, kVideoAdjustStep);
        break;
    case ShortcutAction::ContrastDown:
        AdjustVideo(VideoAdjustment::Contrast, -kVideoAdjustStep);
        break;
    case ShortcutAction::ContrastUp:
        AdjustVideo(VideoAdjustment::Contrast, kVideoAdjustStep);
        break;
    case ShortcutAction::SaturationDown:
        AdjustVideo(VideoAdjustment::Saturation, -kVideoAdjustStep);
        break;
    case ShortcutAction::SaturationUp:
        AdjustVideo(VideoAdjustment::Saturation, kVideoAdjustStep);
        break;
    case ShortcutAction::HueDown:
        AdjustVideo(VideoAdjustment::Hue, -kVideoAdjustStep);
        break;
    case ShortcutAction::HueUp:
        AdjustVideo(VideoAdjustment::Hue, kVideoAdjustStep);
        break;
    case ShortcutAction::GammaDown:
        AdjustVideo(VideoAdjustment::Gamma, -kVideoAdjustStep);
        break;
    case ShortcutAction::GammaUp:
        AdjustVideo(VideoAdjustment::Gamma, kVideoAdjustStep);
        break;
    case ShortcutAction::VideoReset:
        ResetVideo();
        break;
    case ShortcutAction::AudioDelayEarlier:
        AdjustAudioDelay(-kAudioDelayStep);
        break;
    case ShortcutAction::AudioDelayLater:
        AdjustAudioDelay(kAudioDelayStep);
        break;
    case ShortcutAction::AudioDelayReset:
        ResetAudioDelay();
        break;
    case ShortcutAction::ChapterList:
        ShowChapterList();
        break;
    case ShortcutAction::ShowInfo:
        RunDialog([this] {
            ShowInfoDialog(window_, *player_, playback_, preferences_.shortcuts[static_cast<size_t>(ShortcutAction::ShowInfo)]);
        });
        break;
    case ShortcutAction::OpenSettings:
        OpenSettings();
        break;
    case ShortcutAction::WindowSize50:
        SetWindowSizePercent(50);
        break;
    case ShortcutAction::WindowSize100:
        SetWindowSizePercent(100);
        break;
    case ShortcutAction::WindowSize150:
        SetWindowSizePercent(150);
        break;
    case ShortcutAction::WindowSize200:
        SetWindowSizePercent(200);
        break;
    case ShortcutAction::PlaylistShow:
        TogglePlaylistPanel();
        break;
    case ShortcutAction::PlaylistNext:
        PlayNext(false);
        break;
    case ShortcutAction::PlaylistPrevious:
        PlayPrevious();
        break;
    case ShortcutAction::Count:
        break;
    }
}

void MainWindow::ChangeSpeed(double delta) {
    wchar_t message[32];
    swprintf_s(message, L"배속 %g배", player_->ChangeSpeed(delta));
    player_->ShowMessage(message);
}

void MainWindow::SetSpeed(double speed) {
    player_->SetSpeed(speed);
    wchar_t message[32];
    swprintf_s(message, L"배속 %g배", speed);
    player_->ShowMessage(message);
}

void MainWindow::SetLoopStart() {
    announceLoop_ = true;
    player_->SetLoopStart();
    ShowControls();
}

void MainWindow::SetLoopEnd() {
    announceLoop_ = true;
    restartLoopPending_ = true;
    player_->SetLoopEnd();
    ShowControls();
}

void MainWindow::ClearLoop() {
    announceLoop_ = true;
    restartLoopPending_ = false;
    player_->ClearLoop();
    ShowControls();
}

void MainWindow::StepFrame(bool forward) {
    player_->StepFrame(forward);
    ShowControls();
}

void MainWindow::SetAspect(AspectMode aspect) {
    player_->SetAspect(aspect);
    player_->ShowMessage(std::wstring(L"화면 비율: ") + AspectLabel(aspect));
}

void MainWindow::SetRotation(int degrees) {
    player_->SetRotation(degrees);
    player_->ShowMessage(degrees == 0 ? std::wstring(L"회전 안 함") : L"회전 " + std::to_wstring(degrees) + L"도");
}

void MainWindow::AdjustVideo(VideoAdjustment adjustment, int delta) {
    const int value = player_->ChangeVideoAdjustment(adjustment, delta);
    wchar_t message[32];
    swprintf_s(message, L"%s %+d", VideoAdjustmentLabel(adjustment), value);
    player_->ShowMessage(message);
}

void MainWindow::ResetVideo() {
    player_->ResetVideoAdjustments();
    player_->ShowMessage(L"영상 조정 초기화");
}

void MainWindow::AdjustAudioDelay(double seconds) {
    wchar_t message[32];
    swprintf_s(message, L"음성 싱크 %+.1f초", player_->ChangeAudioDelay(seconds));
    player_->ShowMessage(message);
}

void MainWindow::ResetAudioDelay() {
    player_->ResetAudioDelay();
    player_->ShowMessage(L"음성 싱크 +0.0초");
}

void MainWindow::CycleSubtitle() {
    // Goes through the tracks in order, with "off" between the last and the first.
    const MediaTrack* first = nullptr;
    const MediaTrack* next = nullptr;
    bool passedSelected = false;
    for (const MediaTrack& track : playback_.tracks) {
        if (track.type != TrackType::Subtitle) {
            continue;
        }
        if (!first) {
            first = &track;
        }
        if (passedSelected && !next) {
            next = &track;
        }
        passedSelected = passedSelected || track.selected;
    }
    if (!first) {
        player_->ShowMessage(L"자막 없음");
    } else if (next || !passedSelected) {
        const MediaTrack& target = next ? *next : *first;
        player_->SelectSubtitle(target.id);
        player_->ShowMessage(L"자막: " + TrackLabel(target));
    } else {
        player_->SelectSubtitle(std::nullopt);
        player_->ShowMessage(L"자막 끔");
    }
}

void MainWindow::CycleAudio() {
    const MediaTrack* first = nullptr;
    const MediaTrack* next = nullptr;
    bool passedSelected = false;
    for (const MediaTrack& track : playback_.tracks) {
        if (track.type != TrackType::Audio) {
            continue;
        }
        if (!first) {
            first = &track;
        }
        if (passedSelected && !next) {
            next = &track;
        }
        passedSelected = passedSelected || track.selected;
    }
    if (const MediaTrack* target = next ? next : first) {
        player_->SelectAudio(target->id);
        player_->ShowMessage(L"오디오: " + TrackLabel(*target));
    } else {
        player_->ShowMessage(L"오디오 없음");
    }
}

void MainWindow::AdjustSubtitleDelay(double seconds) {
    announceSubtitleDelay_ = true;
    player_->ChangeSubtitleDelay(seconds);
}

void MainWindow::HandleContextMenu(POINT screenPoint) {
    if (screenPoint.x == -1 && screenPoint.y == -1) {
        GetCursorPos(&screenPoint);  // Opened from the keyboard.
    }
    menuOpen_ = true;
    SetCursorHidden(false);
    const MenuChoice choice = ShowContextMenu(window_, screenPoint, playback_, preferences_.shortcuts);
    menuOpen_ = false;
    ShowControls();

    switch (choice.action) {
    case MenuChoice::Action::OpenVideo: {
        const std::vector<std::wstring> paths = PickVideoFiles(window_);
        if (!paths.empty()) {
            OpenFiles(paths);
        }
        break;
    }
    case MenuChoice::Action::OpenSubtitle: {
        const std::wstring path = PickSubtitleFile(window_);
        if (!path.empty()) {
            AddSubtitle(path);
        }
        break;
    }
    case MenuChoice::Action::SelectSubtitle:
        player_->SelectSubtitle(choice.trackId);
        break;
    case MenuChoice::Action::SelectAudio:
        if (choice.trackId) {
            player_->SelectAudio(*choice.trackId);
        }
        break;
    case MenuChoice::Action::ResetSubtitleDelay:
        announceSubtitleDelay_ = true;
        player_->ResetSubtitleDelay();
        break;
    case MenuChoice::Action::SetSubtitleEncoding:
        player_->SetSubtitleEncoding(choice.encoding);
        break;
    case MenuChoice::Action::OpenSettings:
        OpenSettings();
        break;
    case MenuChoice::Action::SetSpeed:
        SetSpeed(choice.speed);
        break;
    case MenuChoice::Action::LoopStart:
        SetLoopStart();
        break;
    case MenuChoice::Action::LoopEnd:
        SetLoopEnd();
        break;
    case MenuChoice::Action::ClearLoop:
        ClearLoop();
        break;
    case MenuChoice::Action::StepForward:
        StepFrame(true);
        break;
    case MenuChoice::Action::StepBackward:
        StepFrame(false);
        break;
    case MenuChoice::Action::SetAspect:
        SetAspect(choice.aspect);
        break;
    case MenuChoice::Action::SetRotation:
        SetRotation(choice.rotation);
        break;
    case MenuChoice::Action::AdjustVideo:
        AdjustVideo(choice.adjustment, choice.adjustmentDirection * kVideoAdjustStep);
        break;
    case MenuChoice::Action::ResetVideo:
        ResetVideo();
        break;
    case MenuChoice::Action::ResetAudioDelay:
        ResetAudioDelay();
        break;
    case MenuChoice::Action::None:
        break;
    }
}

void MainWindow::HandleMouseWheel(int delta) {
    // Precision wheels report small deltas, so only act on whole notches.
    wheelRemainder_ += delta;
    const int notches = wheelRemainder_ / WHEEL_DELTA;
    wheelRemainder_ %= WHEEL_DELTA;
    if (notches != 0) {
        player_->ChangeVolume(notches * kVolumeStep);
        ShowControls();
    }
}

void MainWindow::HandleMouseMove(POINT point) {
    if (drag_ == Drag::Window) {
        // Only a real drag moves the window, so clicks and double-clicks work as they always did.
        const UINT dpi = GetDpiForWindow(window_);
        if (std::abs(point.x - dragStart_.x) > GetSystemMetricsForDpi(SM_CXDRAG, dpi) ||
            std::abs(point.y - dragStart_.y) > GetSystemMetricsForDpi(SM_CYDRAG, dpi)) {
            drag_ = Drag::None;
            ReleaseCapture();
            POINT pressed = dragStart_;
            ClientToScreen(window_, &pressed);
            POINT screen{};
            GetCursorPos(&screen);
            // Catch up with the pointer's travel so far. Windows only follows it from here on.
            if (!IsZoomed(window_)) {
                RECT current{};
                GetWindowRect(window_, &current);
                SetWindowPos(window_, nullptr, current.left + screen.x - pressed.x, current.top + screen.y - pressed.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            // From here Windows handles it as if the title bar had been pressed.
            SendMessageW(window_, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(screen.x, screen.y));
        }
        return;
    }
    if (drag_ == Drag::Seek) {
        lastDragX_ = point.x;
        SeekToPointer(point.x, false);
        return;
    }
    if (drag_ == Drag::Volume) {
        SetVolumeFromPointer(point.x);
        return;
    }

    // Windows also sends this when nothing moved, for example after a window
    // changes under the cursor. Those must not reveal a hidden cursor or the bar.
    POINT position{};
    GetCursorPos(&position);
    if (position.x == lastCursorPosition_.x && position.y == lastCursorPosition_.y) {
        return;
    }
    lastCursorPosition_ = position;

    if (!trackingMouseLeave_) {
        TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window_, 0};
        trackingMouseLeave_ = TrackMouseEvent(&track) != FALSE;
    }
    SetCursorHidden(false);
    ShowControls();
    hoveredPart_ = controlBar_->HitTest(point);
    controlBar_->SetHoveredPart(hoveredPart_);
}

void MainWindow::HandleMouseLeave() {
    trackingMouseLeave_ = false;
    hoveredPart_ = ControlBar::Part::None;
    controlBar_->SetHoveredPart(hoveredPart_);
    if (drag_ == Drag::None && !menuOpen_ && !playback_.paused) {
        KillTimer(window_, kIdleTimer);
        controlBar_->SetVisible(false);
    }
    // A real return to the window is always from elsewhere, so this keeps spurious
    // move messages from counting as movement while the pointer is outside.
    GetCursorPos(&lastCursorPosition_);
}

void MainWindow::HandleLeftButtonDown(POINT point) {
    if (GetFocus() != window_) {
        SetFocus(window_);
    }
    switch (controlBar_->HitTest(point)) {
    case ControlBar::Part::PlayPause:
        player_->TogglePause();
        ShowControls();
        break;
    case ControlBar::Part::SeekTrack:
        drag_ = Drag::Seek;
        lastDragX_ = point.x;
        SetCapture(window_);
        SeekToPointer(point.x, false);
        break;
    case ControlBar::Part::VolumeTrack:
        drag_ = Drag::Volume;
        SetCapture(window_);
        SetVolumeFromPointer(point.x);
        break;
    case ControlBar::Part::Fullscreen:
        ToggleFullscreen();
        break;
    case ControlBar::Part::None:
    case ControlBar::Part::Background:
        if (!fullscreen_) {
            drag_ = Drag::Window;
            dragStart_ = point;
            SetCapture(window_);
        }
        break;
    default:
        break;
    }
}

void MainWindow::HandleLeftButtonUp(POINT point) {
    if (drag_ == Drag::None) {
        return;
    }
    const Drag finished = drag_;
    drag_ = Drag::None;  // First, so losing the capture below isn't treated as a cancelled drag.
    if (finished == Drag::Seek) {
        SeekToPointer(point.x, true);
    } else if (finished == Drag::Volume) {
        SetVolumeFromPointer(point.x);
    }
    ReleaseCapture();
    ShowControls();
}

void MainWindow::HandleCaptureLost() {
    if (drag_ == Drag::None) {
        return;
    }
    const Drag finished = drag_;
    drag_ = Drag::None;
    if (finished == Drag::Seek) {
        SeekToPointer(lastDragX_, true);
    }
    ShowControls();
}

void MainWindow::SetVolumeFromPointer(int clientX) {
    player_->SetVolume(std::round(controlBar_->VolumeAt(clientX)));
}

void MainWindow::SeekToPointer(int clientX, bool isFinal) {
    if (!playback_.duration) {
        return;
    }
    // Keyframe seeks keep dragging responsive on heavy video; the final one lands exactly.
    SeekWithPreview(controlBar_->SeekFractionAt(clientX) * *playback_.duration, isFinal);
}

void MainWindow::SeekWithPreview(double target, bool exact) {
    player_->SeekTo(target, exact);
    seekPreview_ = target;
    seekPreviewExpiry_ = GetTickCount64() + kSeekPreviewExpiryMs;
    controlBar_->SetSeekPreview(seekPreview_);
}

void MainWindow::HandleIdleTimeout() {
    if (drag_ != Drag::None || menuOpen_ || hoveredPart_ != ControlBar::Part::None) {
        return;  // Leaving the bar, finishing the drag or closing the menu starts the timer again.
    }
    if (!playback_.paused || dialogOpen_) {
        controlBar_->SetVisible(false);
    }
    if (fullscreen_ && !dialogOpen_ && !(playlistPanel_ && playlistPanel_->IsOpen())) {
        SetCursorHidden(true);
    }
}

void MainWindow::HandleDroppedFiles(HDROP drop) {
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

    if (!paths.empty()) {
        OpenFiles(paths);
        SetForegroundWindow(window_);
    }
}

void MainWindow::HandleStateChanged(const PlaybackState& state) {
    if (!openRetryPath_.empty() && state.position && *state.position > 0.0 && !state.fileName.empty() &&
        _wcsicmp(std::filesystem::path(openRetryPath_).filename().c_str(), state.fileName.c_str()) == 0) {
        ClearOpenRetry();
    }
    const bool reachedEnd = state.atEnd && !playback_.atEnd && !state.fileName.empty();
    const bool delayChanged = state.subtitleDelay != playback_.subtitleDelay;
    const bool loopChanged = state.loopStart != playback_.loopStart || state.loopEnd != playback_.loopEnd;
    playback_ = state;
    UpdateTitle(state);

    if (delayChanged && announceSubtitleDelay_) {
        announceSubtitleDelay_ = false;
        wchar_t message[64];
        swprintf_s(message, L"자막 싱크 %+.1f초", state.subtitleDelay);
        player_->ShowMessage(message);
    }

    // Marking the points in the other order leaves them the wrong way round. Swap them;
    // the rest waits for the corrected state.
    if (state.loopStart && state.loopEnd && *state.loopEnd < *state.loopStart) {
        restartLoopPending_ = true;
        player_->SetLoop(*state.loopEnd, *state.loopStart);
        return;
    }
    // mpv only loops if it sees playback reach the end point, and a frame shown just as
    // the point is set can already be past it. So don't wait: this is the end, go back.
    if (loopChanged && restartLoopPending_ && state.loopEnd) {
        restartLoopPending_ = false;
        if (!state.paused) {
            player_->SeekTo(state.loopStart.value_or(0.0), true);
        }
    }
    if (loopChanged && announceLoop_) {
        announceLoop_ = false;
        if (state.loopStart && state.loopEnd) {
            player_->ShowMessage(L"구간 반복 " + FormatTime(*state.loopStart) + L" ~ " + FormatTime(*state.loopEnd));
        } else if (state.loopStart) {
            player_->ShowMessage(L"구간 시작 " + FormatTime(*state.loopStart));
        } else if (state.loopEnd) {
            player_->ShowMessage(L"구간 끝 " + FormatTime(*state.loopEnd));
        } else {
            player_->ShowMessage(L"구간 반복 해제");
        }
    }

    if (seekPreview_ && drag_ != Drag::Seek) {
        const bool arrived = state.position && std::abs(*state.position - *seekPreview_) < kSeekPreviewToleranceSeconds;
        if (arrived || GetTickCount64() >= seekPreviewExpiry_) {
            seekPreview_.reset();
            controlBar_->SetSeekPreview(std::nullopt);
        }
    }
    controlBar_->SetPlayback(state);

    if (reachedEnd && preferences_.autoAdvance && preferences_.repeat != RepeatMode::One) {
        PlayNext(true);
    }
}

void MainWindow::ToggleFullscreen() {
    SetFullscreen(!fullscreen_);
}

void MainWindow::SetFullscreen(bool fullscreen) {
    if (fullscreen == fullscreen_) {
        return;
    }

    const LONG_PTR style = GetWindowLongPtrW(window_, GWL_STYLE);
    if (fullscreen) {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetWindowPlacement(window_, &restorePlacement_) ||
            !GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            return;
        }
        SetWindowLongPtrW(window_, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        const RECT& area = monitor.rcMonitor;
        SetWindowPos(window_, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetCursorHidden(false);
        SetWindowLongPtrW(window_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(window_, &restorePlacement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen_ = fullscreen;
    controlBar_->SetFullscreenState(fullscreen);
    ShowControls();
}

void MainWindow::SetCursorHidden(bool hidden) {
    if (hidden == cursorHidden_) {
        return;
    }
    cursorHidden_ = hidden;
    SetCursor(hidden ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
}

void MainWindow::UpdateTitle(const PlaybackState& state) {
    std::wstring title = kAppTitle;
    if (!state.fileName.empty()) {
        // The file name goes last: long names get cut off in the title bar.
        title = FormatTime(state.position.value_or(0.0));
        if (state.duration) {
            title += L" / " + FormatTime(*state.duration);
        }
        title += L" | 볼륨 " + std::to_wstring(std::lround(state.volume)) + L"%";
        if (state.paused) {
            title += L" | 일시정지";
        }
        title += L" | " + state.fileName;
    }
    if (title != title_) {
        title_ = std::move(title);
        SetWindowTextW(window_, title_.c_str());
    }
}
