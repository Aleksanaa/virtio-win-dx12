// SPDX-License-Identifier: MIT
// vgd-panel.exe, the control panel: a Win32 window painted with Direct2D on the CPU (a software render target, so it
// never needs our driver) and DirectWrite text, in WinUI 3's dark theme: its colour tokens, a left NavigationView and
// Settings-style cards. Everything is laid out in 96-DPI units and scaled to the window's DPI.
#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace panel {

// ---- WinUI 3 dark theme tokens (alpha fills are drawn as alpha) ---------------------------------------------------
namespace theme {
constexpr UINT32 kWhite = 0xffffff, kBlack = 0x000000;
constexpr UINT32 kBase = 0x202020;   // SolidBackgroundFillColorBase (Mica's fallback)
constexpr UINT32 kLayer = 0x282828;  // LayerFillColorDefault (#3A3A3A at 30 %) over the base
constexpr float kTextPrimary = 1, kTextSecondary = 0.786f, kTextTertiary = 0.544f;
constexpr float kCardFill = 0.0512f, kCardHover = 0.0837f;  // CardBackgroundFillColorDefault, and pointer over
constexpr float kControl = 0.0605f, kControlHover = 0.0837f, kControlPressed = 0.0326f, kControlStroke = 0.0698f;
constexpr float kStrongStroke = 0.544f;  // ControlStrongStrokeColorDefault
constexpr UINT32 kSuccess = 0x6ccb5f, kCaution = 0xfce100, kCritical = 0xff99a4;
constexpr UINT32 kCautionBg = 0x433519, kCriticalBg = 0x442726;
extern UINT32 accent;      // SystemAccentColorLight2, from Windows' accent palette
extern UINT32 accentBase;  // SystemAccentColor: the text selection highlight
}  // namespace theme

// A clickable thing's id: the window owns kNavId..kPageId-1, pages number theirs from kPageId.
constexpr int kNoId = 0, kNavId = 100, kPageId = 1000;

// The painter and the controls drawn with it. Pages draw in DIPs through it and register what can be clicked.
class Ui {
public:
    enum Font { kTitle, kSubtitle, kBody, kBodyStrong, kCaption, kMono, kIcon, kIconLarge, kIconSmall, kGlyph, kArrow,
                kFontCount };

    void init();
    void begin(ID2D1RenderTarget* target)
    {
        rt_ = target;
        hots_.clear();
        selectables_.clear();
    }
    ID2D1RenderTarget* rt() const { return rt_; }

    // Drawing; colours are 0xRRGGBB with an alpha.
    void color(UINT32 rgb, float a = 1);
    void fill(D2D1_RECT_F r, float radius, UINT32 c, float a = 1);
    void stroke(D2D1_RECT_F r, float radius, UINT32 c, float a = 1);
    void text(const std::wstring& s, Font f, D2D1_RECT_F r, UINT32 c = theme::kWhite, float a = theme::kTextPrimary,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float measure(const std::wstring& s, Font f);

    // Clickable areas, in window DIPs; `offset` is the scroll offset of what is being drawn.
    void hot(D2D1_RECT_F r, int id);
    int hit(float x, float y) const;
    float offset = 0;
    int hover = kNoId, pressed = kNoId;

    // WinUI controls. Buttons are 32 high; the toggle and the combo box are placed by their right edge.
    float button(const std::wstring& s, float x, float y, int id, bool accent);
    float buttonWidth(const std::wstring& s);
    void toggle(float right, float cy, int id, bool on);
    void comboBox(float right, float cy, int id, const std::vector<std::wstring>& options, int selected);
    // A Settings card (68 high): icon, title, description; returns the vertical centre for the control on its right.
    float settingsCard(float x, float y, float w, float h, const wchar_t* icon, const std::wstring& title,
                       const std::wstring& desc, int clickId = kNoId, UINT32 iconColor = theme::kWhite,
                       float iconAlpha = 1);
    void card(D2D1_RECT_F r, bool hover = false);
    void sectionHeader(const std::wstring& s, float x, float& y);

    // Selectable text (a TextBlock with IsTextSelectionEnabled): drawn like text(), and the window lets the user select
    // in it with the mouse and copy. One text's selection at a time; `id` names the text across frames.
    void selectable(int id, const std::wstring& s, Font f, D2D1_RECT_F r, UINT32 c = theme::kWhite,
                    float a = theme::kTextPrimary);
    // The selectable text under a point (window DIPs) and the character position there.
    bool textAt(float x, float y, int* id, UINT32* pos) const;
    void selectStart(int id, UINT32 pos);
    void selectTo(float x, float y);  // drag: moves the caret of the current selection towards the point
    void selectWord(int id, UINT32 pos);
    void selectAll(int id);
    void clearSelection() { selId_ = kNoId; }
    bool selected(int id) const { return selId_ == id && selAnchor_ != selCaret_; }
    int selectionId() const { return selId_; }
    std::wstring selectedText() const;

    // The render target and its brush, made per window (they go when the target has to be recreated).
    void setBrush(ID2D1SolidColorBrush* b) { brush_ = b; }

private:
    IDWriteTextFormat* format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight);
    bool hasFont(const wchar_t* family);
    IDWriteTextLayout* layout(const std::wstring& s, Font f, D2D1_RECT_F r) const;
    struct Selectable { D2D1_RECT_F r; int id; std::wstring s; Font f; };
    const Selectable* findSelectable(int id) const;

    struct Hot { D2D1_RECT_F r; int id; };
    ID2D1RenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    IDWriteTextFormat* fonts_[kFontCount] = {};
    std::vector<Hot> hots_;
    std::vector<Selectable> selectables_;  // as drawn in the last frame
    int selId_ = kNoId;
    UINT32 selAnchor_ = 0, selCaret_ = 0;
};

// Puts text on the clipboard.
void setClipboard(HWND owner, const std::wstring& text);

// A page of the panel. The window scrolls it and tells it about clicks; it repaints after each.
class Page {
public:
    virtual ~Page() = default;
    virtual const wchar_t* title() const = 0;
    virtual const wchar_t* icon() const = 0;
    // Draws from the top of the page's content at y, w wide; leaves y below what it drew.
    virtual void draw(Ui& ui, float x, float w, float& y) = 0;
    virtual void click(int id) {}
    // The display configuration or a system setting changed.
    virtual void refresh() {}
    // A message the window does not handle itself (kPageMessage.., timers from kPageTimer); true when it was this
    // page's. The window repaints after it.
    virtual bool message(UINT msg, WPARAM wp, LPARAM lp) { return false; }
};

// Messages and timer ids the window passes on to its pages.
constexpr UINT kPageMessage = WM_APP + 1;
constexpr UINT_PTR kPageTimer = 100;

std::unique_ptr<Page> makeOverview(HWND window);
std::unique_ptr<Page> makeSettings();
std::unique_ptr<Page> makeLogs(HWND window);
std::unique_ptr<Page> makeTests();
std::unique_ptr<Page> makeTools();

// ---- what the panel reads about the system (sysinfo.cpp) ----------------------------------------------------------

struct Display {
    DWORD width = 0, height = 0, hz = 0;
    UINT scale = 100;  // percent
};

// Read in the panel's own process: the registry, SetupAPI, the displays and the firmware's SMBIOS tables. Nothing
// here loads a graphics driver, so it works whatever state the driver is in.
struct SystemInfo {
    // The VirtIO GPU and the driver Windows runs it with.
    bool deviceFound = false;
    std::wstring driverDesc, provider, driverVersion, driverDate;
    bool ours = false;            // the driver is vgd's package
    ULONG problem = 0;            // Device Manager's problem code, 0 for none
    // vgd as Apps & features lists it.
    std::wstring productVersion, installDate;
    bool testSigning = false;
    std::wstring machine;         // SMBIOS: manufacturer, product and version of the (virtual) machine
    std::vector<Display> displays;
};
SystemInfo readSystemInfo();
std::vector<Display> readDisplays(const std::wstring& adapterName);

// ---- the graphics probe (probe.cpp) -------------------------------------------------------------------------------
// `vgd-panel.exe --probe` creates a Vulkan instance and D3D12, D3D11 and OpenGL devices on the VirtIO GPU and prints
// what it found as key=value lines. It runs as a child process, so a driver that crashes or hangs takes only the
// probe down. Each API's lines are written before the next is tried.
int runProbe();
using ProbeResult = std::map<std::string, std::wstring>;
// Starts the probe on a thread; posts `msg` to `window` with a ProbeResult* (the receiver deletes it) when it is done.
// A probe that does not finish in `timeoutMs` is ended, and the result has "timeout".
void startProbe(HWND window, UINT msg, DWORD timeoutMs);

// UTF-8 / UTF-16.
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

}  // namespace panel
