#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "Playlist.h"
#include "Shortcuts.h"

// A slide-out playlist panel docked to the right edge of the main window.
// Uses native Windows controls and system theme colors to match the app's dialogs.
class PlaylistPanel {
public:
    struct Callbacks {
        std::function<void()> onAnimationStep;
        std::function<void(size_t index)> onPlayItem;
        std::function<void()> onToggleAutoAdvance;
        std::function<void()> onToggleRepeatOne;
        std::function<void()> onToggleRepeatAll;
        std::function<void()> onTogglePause;
    };

    // Throws std::runtime_error if the panel window can't be created.
    PlaylistPanel(HWND mainWindow, HINSTANCE instance, Callbacks callbacks);
    ~PlaylistPanel();

    PlaylistPanel(const PlaylistPanel&) = delete;
    PlaylistPanel& operator=(const PlaylistPanel&) = delete;

    void Toggle();
    void SetOpen(bool open);
    bool IsOpen() const { return targetOpen_; }
    bool IsVisible() const { return visibleWidth_ > 0 || animating_; }
    bool HasFocus() const;

    void SetPlaylist(const std::vector<std::wstring>& files, size_t currentIndex);
    void SetOptions(bool autoAdvance, RepeatMode repeat);
    void SetToggleShortcut(KeyChord shortcut) { toggleShortcut_ = shortcut; }

    // Updates panel bounds inside mainWindow and returns its visible width on the right edge.
    int Layout(int clientWidth, int clientHeight, UINT dpi);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ControlSubclassProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR subclassId, DWORD_PTR refData);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool HandleControlKeyDown(HWND control, WPARAM key);

    void CreateControls();
    void UpdateFonts();
    void LayoutControls();
    void FitColumns();
    void RefreshList();
    void RefreshHeaderCount();
    void RefreshOptionChecks();
    void AdvanceAnimation();
    void UpdatePanelBounds();
    void MoveFocusCycle(HWND current, bool backwards);
    int TargetPanelWidth(int clientWidth, UINT dpi) const;
    int Dip(int value) const;

    HWND mainWindow_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Callbacks callbacks_;

    HWND window_ = nullptr;
    HWND titleLabel_ = nullptr;
    HWND countLabel_ = nullptr;
    HWND closeButton_ = nullptr;
    HWND autoAdvanceCheck_ = nullptr;
    HWND repeatOneCheck_ = nullptr;
    HWND repeatAllCheck_ = nullptr;
    HWND list_ = nullptr;
    HWND emptyLabel_ = nullptr;

    HFONT font_ = nullptr;
    HFONT boldFont_ = nullptr;
    UINT dpi_ = 96;
    int clientWidth_ = 0;
    int clientHeight_ = 0;
    int panelWidth_ = 0;
    int visibleWidth_ = 0;

    bool targetOpen_ = false;
    bool animating_ = false;
    double progress_ = 0.0;
    double animFrom_ = 0.0;
    double animTo_ = 0.0;
    ULONGLONG animStart_ = 0;

    std::vector<std::wstring> files_;
    size_t currentIndex_ = 0;
    bool autoAdvance_ = true;
    RepeatMode repeat_ = RepeatMode::None;
    bool updatingChecks_ = false;
    KeyChord toggleShortcut_ = VK_F6;
};
