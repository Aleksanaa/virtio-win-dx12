// SPDX-License-Identifier: MIT
// The painter and WinUI's controls drawn with it: Button, AccentButton, ToggleSwitch, ComboBox (closed), Settings
// cards and section headers, with WinUI's dark-theme colours and sizes.
#include "panel.h"

#include <algorithm>

namespace panel {

using namespace theme;

UINT32 theme::accent = 0x60cdff, theme::accentBase = 0x0078d4;

IDWriteTextFormat* Ui::format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight)
{
    IDWriteTextFormat* f = nullptr;
    dw_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                          &f);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* ellipsis = nullptr;
    dw_->CreateEllipsisTrimmingSign(f, &ellipsis);
    f->SetTrimming(&trimming, ellipsis);
    if (ellipsis)
        ellipsis->Release();
    return f;
}

bool Ui::hasFont(const wchar_t* family)
{
    IDWriteFontCollection* c = nullptr;
    dw_->GetSystemFontCollection(&c);
    UINT32 index;
    BOOL exists = FALSE;
    c->FindFamilyName(family, &index, &exists);
    c->Release();
    return exists;
}

void Ui::init()
{
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dw_));
    // Windows 11's fonts, Windows 10's where they are missing.
    const wchar_t* display = hasFont(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* text = hasFont(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* icons = hasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    // WinUI's type ramp: Title 28, Subtitle 20, Body 14, Body Strong 14, Caption 12.
    fonts_[kTitle] = format(display, 28, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fonts_[kSubtitle] = format(display, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fonts_[kBody] = format(text, 14, DWRITE_FONT_WEIGHT_NORMAL);
    fonts_[kBodyStrong] = format(text, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fonts_[kCaption] = format(text, 12, DWRITE_FONT_WEIGHT_NORMAL);
    fonts_[kMono] = format(hasFont(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas", 13, DWRITE_FONT_WEIGHT_NORMAL);
    fonts_[kIcon] = format(icons, 16, DWRITE_FONT_WEIGHT_NORMAL);
    fonts_[kIconLarge] = format(icons, 20, DWRITE_FONT_WEIGHT_NORMAL);
    fonts_[kIconSmall] = format(icons, 12, DWRITE_FONT_WEIGHT_NORMAL);
    fonts_[kGlyph] = format(icons, 40, DWRITE_FONT_WEIGHT_NORMAL);
    fonts_[kArrow] = format(icons, 8, DWRITE_FONT_WEIGHT_NORMAL);
    // The accent as WinUI's dark theme uses it: SystemAccentColorLight2 (the palette's second entry) for controls,
    // SystemAccentColor (the fourth) for text selection.
    BYTE palette[32];
    DWORD size = sizeof(palette);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, palette, &size) == ERROR_SUCCESS &&
        size >= 16) {
        accent = (palette[4] << 16) | (palette[5] << 8) | palette[6];
        accentBase = (palette[12] << 16) | (palette[13] << 8) | palette[14];
    }
}

IDWriteTextLayout* Ui::layout(const std::wstring& s, Font f, D2D1_RECT_F r) const
{
    fonts_[f]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    IDWriteTextLayout* l = nullptr;
    dw_->CreateTextLayout(s.c_str(), s.size(), fonts_[f], r.right - r.left, r.bottom - r.top, &l);
    return l;
}

const Ui::Selectable* Ui::findSelectable(int id) const
{
    for (const Selectable& s : selectables_)
        if (s.id == id)
            return &s;
    return nullptr;
}

void Ui::selectable(int id, const std::wstring& s, Font f, D2D1_RECT_F r, UINT32 c, float a)
{
    selectables_.push_back({{r.left, r.top - offset, r.right, r.bottom - offset}, id, s, f});
    IDWriteTextLayout* l = layout(s, f, r);
    if (!l)
        return;
    if (selected(id)) {
        UINT32 start = std::min(selAnchor_, selCaret_), length = std::max(selAnchor_, selCaret_) - start;
        DWRITE_HIT_TEST_METRICS m[16];
        UINT32 count = 0;
        if (SUCCEEDED(l->HitTestTextRange(start, length, r.left, r.top, m, 16, &count)))
            for (UINT32 i = 0; i < count; i++) {
                color(accentBase);
                rt_->FillRectangle({m[i].left, m[i].top, m[i].left + m[i].width, m[i].top + m[i].height}, brush_);
            }
    }
    color(c, a);
    rt_->DrawTextLayout({r.left, r.top}, l, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    l->Release();
}

bool Ui::textAt(float x, float y, int* id, UINT32* pos) const
{
    for (const Selectable& s : selectables_) {
        if (x < s.r.left || x >= s.r.right || y < s.r.top || y >= s.r.bottom)
            continue;
        IDWriteTextLayout* l = layout(s.s, s.f, s.r);
        if (!l)
            return false;
        DWRITE_TEXT_METRICS tm{};
        l->GetMetrics(&tm);
        BOOL trailing, inside;
        DWRITE_HIT_TEST_METRICS m{};
        l->HitTestPoint(x - s.r.left, y - s.r.top, &trailing, &inside, &m);
        l->Release();
        if (x - s.r.left > tm.left + tm.width + 8)  // past the end of the text: not on it
            return false;
        *id = s.id;
        *pos = m.textPosition + (trailing ? m.length : 0);
        return true;
    }
    return false;
}

void Ui::selectStart(int id, UINT32 pos)
{
    selId_ = id;
    selAnchor_ = selCaret_ = pos;
}

void Ui::selectTo(float x, float y)
{
    const Selectable* s = findSelectable(selId_);
    if (!s)
        return;
    IDWriteTextLayout* l = layout(s->s, s->f, s->r);
    if (!l)
        return;
    BOOL trailing, inside;
    DWRITE_HIT_TEST_METRICS m{};
    l->HitTestPoint(x - s->r.left, std::clamp(y, s->r.top, s->r.bottom - 1) - s->r.top, &trailing, &inside, &m);
    l->Release();
    selCaret_ = m.textPosition + (trailing ? m.length : 0);
}

// A word here is a run without spaces or commas, so a version or a path is selected whole.
void Ui::selectWord(int id, UINT32 pos)
{
    const Selectable* s = findSelectable(id);
    if (!s)
        return;
    auto boundary = [](wchar_t c) { return c == L' ' || c == L',' || c == L'(' || c == L')'; };
    UINT32 start = std::min<UINT32>(pos, s->s.size()), end = start;
    while (start > 0 && !boundary(s->s[start - 1]))
        start--;
    while (end < s->s.size() && !boundary(s->s[end]))
        end++;
    selId_ = id;
    selAnchor_ = start;
    selCaret_ = end;
}

void Ui::selectAll(int id)
{
    if (const Selectable* s = findSelectable(id)) {
        selId_ = id;
        selAnchor_ = 0;
        selCaret_ = s->s.size();
    }
}

std::wstring Ui::selectedText() const
{
    const Selectable* s = findSelectable(selId_);
    if (!s || selAnchor_ == selCaret_)
        return {};
    UINT32 start = std::min(selAnchor_, selCaret_), end = std::min<UINT32>(std::max(selAnchor_, selCaret_), s->s.size());
    return start < end ? s->s.substr(start, end - start) : std::wstring();
}

void setClipboard(HWND owner, const std::wstring& text)
{
    if (!OpenClipboard(owner))
        return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
            GlobalFree(mem);
    }
    CloseClipboard();
}

void Ui::color(UINT32 rgb, float a) { brush_->SetColor(D2D1::ColorF(rgb, a)); }

void Ui::fill(D2D1_RECT_F r, float radius, UINT32 c, float a)
{
    color(c, a);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_);
}

void Ui::stroke(D2D1_RECT_F r, float radius, UINT32 c, float a)
{
    color(c, a);
    D2D1_RECT_F inner = {r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f};
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius, radius), brush_, 1);
}

void Ui::text(const std::wstring& s, Font f, D2D1_RECT_F r, UINT32 c, float a, DWRITE_TEXT_ALIGNMENT align)
{
    fonts_[f]->SetTextAlignment(align);
    color(c, a);
    rt_->DrawText(s.c_str(), s.size(), fonts_[f], r, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Ui::measure(const std::wstring& s, Font f)
{
    IDWriteTextLayout* layout = nullptr;
    dw_->CreateTextLayout(s.c_str(), s.size(), fonts_[f], 10000, 100, &layout);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return m.widthIncludingTrailingWhitespace;
}

void Ui::hot(D2D1_RECT_F r, int id) { hots_.push_back({{r.left, r.top - offset, r.right, r.bottom - offset}, id}); }

int Ui::hit(float x, float y) const
{
    for (auto it = hots_.rbegin(); it != hots_.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom)
            return it->id;
    return kNoId;
}

float Ui::buttonWidth(const std::wstring& s) { return std::max(measure(s, kBody) + 24, 96.f); }

float Ui::button(const std::wstring& s, float x, float y, int id, bool isAccent)
{
    float w = buttonWidth(s);
    D2D1_RECT_F r = {x, y, x + w, y + 32};
    bool h = hover == id, p = h && pressed == id;
    if (isAccent) {
        fill(r, 4, accent, p ? 0.8f : h ? 0.9f : 1);
        text(s, kBody, r, kBlack, p ? 0.5f : 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        fill(r, 4, kWhite, p ? kControlPressed : h ? kControlHover : kControl);
        stroke(r, 4, kWhite, kControlStroke);
        text(s, kBody, r, kWhite, p ? kTextSecondary : kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    hot(r, id);
    return w;
}

// With its On/Off label on the left, as Settings shows it.
void Ui::toggle(float right, float cy, int id, bool on)
{
    bool h = hover == id, p = h && pressed == id;
    float x = right - 40, knob = p || h ? 7 : 6;
    D2D1_RECT_F r = {x, cy - 10, x + 40, cy + 10};
    if (on) {
        fill(r, 10, accent, h ? 0.9f : 1);
        color(kBlack);
        rt_->FillEllipse(D2D1::Ellipse({x + 30, cy}, knob, knob), brush_);
    } else {
        fill(r, 10, kWhite, h ? kControlHover : kControl);
        stroke(r, 10, kWhite, kStrongStroke);
        color(kWhite, kTextSecondary);
        rt_->FillEllipse(D2D1::Ellipse({x + 10, cy}, knob, knob), brush_);
    }
    text(on ? L"On" : L"Off", kBody, {x - 60, cy - 10, x - 12, cy + 10}, kWhite, kTextPrimary,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    hot({x - 44, cy - 16, right, cy + 16}, id);
}

void Ui::comboBox(float right, float cy, int id, const std::vector<std::wstring>& options, int selected)
{
    float w = 0;
    for (const auto& o : options)
        w = std::max(w, measure(o, kBody));
    w = std::max(160.f, w + 12 + 40);
    D2D1_RECT_F r = {right - w, cy - 16, right, cy + 16};
    bool h = hover == id, p = h && pressed == id;
    fill(r, 4, kWhite, p ? kControlPressed : h ? kControlHover : kControl);
    stroke(r, 4, kWhite, kControlStroke);
    text(options[selected], kBody, {r.left + 12, r.top, r.right - 36, r.bottom});
    text(L"", kIconSmall, {r.right - 32, r.top, r.right - 12, r.bottom}, kWhite, kTextSecondary,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    hot(r, id);
}

void Ui::card(D2D1_RECT_F r, bool h)
{
    fill(r, 4, kWhite, h ? kCardHover : kCardFill);
    stroke(r, 4, kBlack, 0.1f);
}

float Ui::settingsCard(float x, float y, float w, float h, const wchar_t* icon, const std::wstring& title,
                       const std::wstring& desc, int clickId, UINT32 iconColor, float iconAlpha)
{
    card({x, y, x + w, y + h}, clickId && hover == clickId);
    float tx = x + 16;
    if (icon) {
        text(icon, kIconLarge, {x + 16, y, x + 36, y + h}, iconColor, iconAlpha, DWRITE_TEXT_ALIGNMENT_CENTER);
        tx = x + 56;
    }
    if (!desc.empty()) {
        text(title, kBody, {tx, y + h / 2 - 19, x + w - 300, y + h / 2 + 1});
        text(desc, kCaption, {tx, y + h / 2 + 1, x + w - 300, y + h / 2 + 19}, kWhite, kTextSecondary);
    } else {
        text(title, kBody, {tx, y, x + w - 300, y + h});
    }
    if (clickId)
        hot({x, y, x + w, y + h}, clickId);
    return y + h / 2;
}

void Ui::sectionHeader(const std::wstring& s, float x, float& y)
{
    y += 22;
    text(s, kBodyStrong, {x + 1, y, x + 600, y + 28});
    y += 34;
}

std::wstring widen(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), s.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), s.size(), w.data(), w.size());
    return w;
}

std::string narrow(const std::wstring& s)
{
    std::string n(WideCharToMultiByte(CP_UTF8, 0, s.data(), s.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), s.size(), n.data(), n.size(), nullptr, nullptr);
    return n;
}

}  // namespace panel
