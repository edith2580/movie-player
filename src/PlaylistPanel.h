#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>

#include "Playlist.h"
#include "Shortcuts.h"

// A separate modeless playlist dialog window docked outside the right edge
// of the main window, accepting dragged files to append to the playlist.
class PlaylistPanel {
public:
    struct Callbacks {
        std::function<void(size_t index)> onPlayItem;
        std::function<void()> onToggleAutoAdvance;
        std::function<void()> onToggleRepeatOne;
        std::function<void()> onToggleRepeatAll;
        std::function<void()> onTogglePause;
        std::function<void(const std::vector<std::wstring>& paths)> onFilesDropped;
    };

    // Throws std::runtime_error if the window or graphics factories can't be created.
    PlaylistPanel(HWND mainWindow, HINSTANCE instance, Callbacks callbacks);
    ~PlaylistPanel();

    PlaylistPanel(const PlaylistPanel&) = delete;
    PlaylistPanel& operator=(const PlaylistPanel&) = delete;

    void Toggle();
    void SetOpen(bool open);
    bool IsOpen() const { return open_; }
    bool HasFocus() const;

    void SetPlaylist(const std::vector<std::wstring>& files, size_t currentIndex);
    void SetOptions(bool autoAdvance, RepeatMode repeat);
    void SetToggleShortcut(KeyChord shortcut) { toggleShortcut_ = shortcut; }
    void SyncToMainWindow();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ControlSubclassProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR subclassId, DWORD_PTR refData);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool HandleControlKeyDown(HWND control, WPARAM key);

    void CreateControls();
    void CreateGraphics();
    void DrawToggleButton(const DRAWITEMSTRUCT& draw);
    void UpdateFonts();
    void LayoutControls();
    void FitColumns();
    void RefreshList();
    void RefreshHeaderCount();
    void RefreshOptionButtons();
    RECT ComputeDockedRect() const;
    void UpdateDockedState();
    void MoveFocusCycle(HWND current, bool backwards);
    int Dip(int value) const;

    HWND mainWindow_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Callbacks callbacks_;

    HWND window_ = nullptr;
    HWND tooltip_ = nullptr;
    HWND countLabel_ = nullptr;
    HWND autoAdvanceButton_ = nullptr;
    HWND repeatOneButton_ = nullptr;
    HWND repeatAllButton_ = nullptr;
    HWND list_ = nullptr;
    HWND emptyLabel_ = nullptr;
    HWND hoveredButton_ = nullptr;
    bool trackingHover_ = false;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcTarget_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> badgeFormat_;

    HFONT font_ = nullptr;
    HFONT boldFont_ = nullptr;
    UINT dpi_ = 96;
    int clientWidth_ = 0;
    int clientHeight_ = 0;
    int panelClientWidthDip_ = 300;
    bool docked_ = true;
    bool open_ = false;

    std::vector<std::wstring> files_;
    size_t currentIndex_ = 0;
    bool autoAdvance_ = true;
    RepeatMode repeat_ = RepeatMode::None;
    KeyChord toggleShortcut_ = VK_F6;
};
