// SPDX-License-Identifier: MIT
// The painter and WinUI's controls drawn with it: Button, AccentButton, ToggleSwitch, ComboBox (closed), Settings
// cards and section headers, with WinUI's dark-theme colours and sizes.
#include "panel.h"

#include <algorithm>
#include <cmath>

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

void Ui::selectable(int id, const std::wstring& s, Font f, D2D1_RECT_F r, UINT32 c, float a, int run)
{
    selectables_.push_back({{r.left, r.top - offset, r.right, r.bottom - offset}, id, s, f, run, clip_});
    IDWriteTextLayout* l = layout(s, f, r);
    if (!l)
        return;
    UINT32 start, end;
    if (part(id, run, UINT32(s.size()), &start, &end)) {
        DWRITE_HIT_TEST_METRICS m[16];
        UINT32 count = 0;
        if (SUCCEEDED(l->HitTestTextRange(start, end - start, r.left, r.top, m, 16, &count)))
            for (UINT32 i = 0; i < count; i++) {
                color(accentBase);
                rt_->FillRectangle({m[i].left, m[i].top, m[i].left + m[i].width, m[i].top + m[i].height}, brush_);
            }
    }
    color(c, a);
    rt_->DrawTextLayout({r.left, r.top}, l, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    l->Release();
}

// The selection's ends in order: the first text and position, then the last.
void Ui::ends(int* firstId, UINT32* firstPos, int* lastId, UINT32* lastPos) const
{
    bool forward = selId_ < caretId_ || (selId_ == caretId_ && selAnchor_ <= selCaret_);
    *firstId = forward ? selId_ : caretId_;
    *firstPos = forward ? selAnchor_ : selCaret_;
    *lastId = forward ? caretId_ : selId_;
    *lastPos = forward ? selCaret_ : selAnchor_;
}

// The selected part of text `id`, `length` long.
bool Ui::part(int id, int run, UINT32 length, UINT32* start, UINT32* end) const
{
    if (selId_ == kNoId || (id != selId_ && id != caretId_ && (!run || run != selRun_)))
        return false;
    int firstId, lastId;
    UINT32 firstPos, lastPos;
    ends(&firstId, &firstPos, &lastId, &lastPos);
    if (id < firstId || id > lastId)
        return false;
    *start = id == firstId ? std::min(firstPos, length) : 0;
    *end = id == lastId ? std::min(lastPos, length) : length;
    return *start < *end;
}

bool Ui::selected(int id) const
{
    const Selectable* s = findSelectable(id);
    UINT32 start, end;
    return s && part(id, s->run, UINT32(s->s.size()), &start, &end);
}

bool Ui::textAt(float x, float y, int* id, UINT32* pos) const
{
    for (const Selectable& s : selectables_) {
        if (x < s.r.left || x >= s.r.right || y < s.r.top || y >= s.r.bottom || y < s.clip.top || y >= s.clip.bottom)
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

void Ui::select(int id, UINT32 anchor, UINT32 caret)
{
    const Selectable* s = findSelectable(id);
    selId_ = caretId_ = id;
    selRun_ = s ? s->run : 0;
    selAnchor_ = anchor;
    selCaret_ = caret;
}

void Ui::selectStart(int id, UINT32 pos) { select(id, pos, pos); }

void Ui::selectTo(float x, float y)
{
    const Selectable* s = findSelectable(selId_);
    if (!s)
        return;
    UINT32 pos = UINT32(-1);  // the end of the text
    if (selRun_) {
        // The line of the run at the pointer's height; above the first drawn, its start; below the last, its end.
        const Selectable *top = nullptr, *bottom = nullptr;
        s = nullptr;
        for (const Selectable& t : selectables_) {
            if (t.run != selRun_)
                continue;
            if (!top || t.r.top < top->r.top)
                top = &t;
            if (!bottom || t.r.bottom > bottom->r.bottom)
                bottom = &t;
            if (y >= t.r.top && y < t.r.bottom)
                s = &t;
        }
        if (!s && top && y < top->r.top) {
            s = top;
            pos = 0;
        } else if (!s && bottom) {
            s = bottom;
        }
        if (!s)
            return;
    }
    if (pos == UINT32(-1) || (y >= s->r.top && y < s->r.bottom)) {
        IDWriteTextLayout* l = layout(s->s, s->f, s->r);
        if (!l)
            return;
        BOOL trailing, inside;
        DWRITE_HIT_TEST_METRICS m{};
        l->HitTestPoint(x - s->r.left, std::clamp(y, s->r.top, s->r.bottom - 1) - s->r.top, &trailing, &inside, &m);
        l->Release();
        pos = (selRun_ && y >= s->r.bottom) ? UINT32(s->s.size()) : m.textPosition + (trailing ? m.length : 0);
    }
    caretId_ = s->id;
    selCaret_ = pos;
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
    select(id, start, end);
}

void Ui::selectAll(int id)
{
    if (const Selectable* s = findSelectable(id))
        select(id, 0, UINT32(s->s.size()));
}

std::wstring Ui::selectedText() const
{
    if (selId_ == kNoId)
        return {};
    int firstId, lastId;
    UINT32 firstPos, lastPos;
    ends(&firstId, &firstPos, &lastId, &lastPos);
    auto text = [&](int id, std::wstring* s) {
        if (const Selectable* t = findSelectable(id)) {
            *s = t->s;
            return true;
        }
        auto run = runs_.find(selRun_);
        return selRun_ && run != runs_.end() && run->second(id, s);
    };
    std::wstring out, s;
    bool firstLine = true;
    for (int id = firstId; id <= lastId; id++) {
        if (!text(id, &s))
            continue;
        UINT32 start = id == firstId ? std::min<UINT32>(firstPos, s.size()) : 0;
        UINT32 end = id == lastId ? std::min<UINT32>(lastPos, s.size()) : s.size();
        if (!firstLine)
            out += L"\r\n";
        out += s.substr(start, end > start ? end - start : 0);
        firstLine = false;
    }
    return out;
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

void Ui::hot(D2D1_RECT_F r, int id)
{
    D2D1_RECT_F w = {std::max(r.left, clip_.left), std::max(r.top - offset, clip_.top), std::min(r.right, clip_.right),
                     std::min(r.bottom - offset, clip_.bottom)};
    if (w.left < w.right && w.top < w.bottom)
        hots_.push_back({w, id});
}

void Ui::beginScroll(D2D1_RECT_F r, float height)
{
    clip_ = scrollArea = {r.left, r.top - offset, r.right, r.bottom - offset};
    scrollHeight = height;
    rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
    rt_->GetTransform(&unscrolled_);
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll) * unscrolled_);
    offset += scroll;
}

void Ui::endScroll()
{
    offset -= scroll;
    rt_->SetTransform(unscrolled_);
    rt_->PopAxisAlignedClip();
    clip_ = kNoClip;
}

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
void Ui::toggle(float right, float cy, int id, bool on, bool enabled)
{
    bool h = enabled && hover == id, p = h && pressed == id;
    float x = right - 40, knob = p || h ? 7 : 6;
    D2D1_RECT_F r = {x, cy - 10, x + 40, cy + 10};
    if (!enabled) {
        if (on)
            fill(r, 10, kWhite, kStrokeDisabled);
        else
            stroke(r, 10, kWhite, kStrokeDisabled);
        color(kWhite, on ? kTextDisabled : kStrokeDisabled);
        rt_->FillEllipse(D2D1::Ellipse({on ? x + 30 : x + 10, cy}, knob, knob), brush_);
    } else if (on) {
        fill(r, 10, accent, h ? 0.9f : 1);
        color(kBlack);
        rt_->FillEllipse(D2D1::Ellipse({x + 30, cy}, knob, knob), brush_);
    } else {
        fill(r, 10, kWhite, h ? kControlHover : kControl);
        stroke(r, 10, kWhite, kStrongStroke);
        color(kWhite, kTextSecondary);
        rt_->FillEllipse(D2D1::Ellipse({x + 10, cy}, knob, knob), brush_);
    }
    text(on ? L"On" : L"Off", kBody, {x - 60, cy - 10, x - 12, cy + 10}, kWhite, enabled ? kTextPrimary : kTextDisabled,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (enabled)
        hot({x - 44, cy - 16, right, cy + 16}, id);
}

void Ui::comboBox(float right, float cy, int id, const std::vector<std::wstring>& options, int selected, bool enabled)
{
    float w = 0;
    for (const auto& o : options)
        w = std::max(w, measure(o, kBody));
    w = std::max(160.f, w + 12 + 40);
    D2D1_RECT_F r = {right - w, cy - 16, right, cy + 16};
    bool h = enabled && hover == id, p = h && pressed == id;
    fill(r, 4, kWhite, !enabled ? kControlDisabled : p ? kControlPressed : h ? kControlHover : kControl);
    stroke(r, 4, kWhite, kControlStroke);
    text(options[selected], kBody, {r.left + 12, r.top, r.right - 36, r.bottom}, kWhite,
         enabled ? kTextPrimary : kTextDisabled);
    text(L"", kIconSmall, {r.right - 32, r.top, r.right - 12, r.bottom}, kWhite,
         enabled ? kTextSecondary : kTextDisabled, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (enabled)
        hot(r, id);
}

void Ui::card(D2D1_RECT_F r, bool h)
{
    fill(r, 4, kWhite, h ? kCardHover : kCardFill);
    stroke(r, 4, kBlack, 0.1f);
}

void Ui::iconButton(const wchar_t* glyph, float x, float y, int id)
{
    D2D1_RECT_F r = {x, y, x + 32, y + 32};
    bool h = hover == id, p = h && pressed == id;
    if (h)
        fill(r, 4, kWhite, p ? kControlPressed : kControlHover);
    text(glyph, kIcon, r, kWhite, p ? kTextSecondary : kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    hot(r, id);
}

// An arc of the circle around (cx, cy), from `from` to `to` degrees clockwise from the top.
void Ui::arc(float cx, float cy, float r, float from, float to, float thickness)
{
    if (to - from >= 359.9f) {
        rt_->DrawEllipse(D2D1::Ellipse({cx, cy}, r, r), brush_, thickness);
        return;
    }
    auto at = [&](float deg) {
        float a = deg * 3.14159265f / 180;
        return D2D1::Point2F(cx + r * std::sin(a), cy - r * std::cos(a));
    };
    ID2D1Factory* factory;
    rt_->GetFactory(&factory);
    if (!round_)
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                               D2D1_CAP_STYLE_ROUND),
                                   nullptr, 0, &round_);
    ID2D1PathGeometry* path;
    ID2D1GeometrySink* sink;
    if (SUCCEEDED(factory->CreatePathGeometry(&path))) {
        if (SUCCEEDED(path->Open(&sink))) {
            sink->BeginFigure(at(from), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(at(to), {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                          to - from > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            rt_->DrawGeometry(path, brush_, thickness, round_);
        }
        path->Release();
    }
    factory->Release();
}

// WinUI's ProgressRing: determinate, an accent arc over a faint track; indeterminate, an arc whose head runs ahead
// and whose tail catches up, round and round.
void Ui::progressRing(float cx, float cy, float size, float value)
{
    float thickness = size / 8, r = (size - thickness) / 2;
    if (value >= 0) {
        color(kWhite, 0.16f);
        arc(cx, cy, r, 0, 360, thickness);
        color(accent);
        if (value > 0)
            arc(cx, cy, r, 0, std::min(value, 1.f) * 360, thickness);
        return;
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double t = double(now.QuadPart) / freq.QuadPart;
    constexpr double kCycle = 1.4;  // seconds for the head to run ahead and the tail to catch up
    double cycles = t / kCycle, p = cycles - std::floor(cycles);
    auto ease = [](double x) { return x * x * (3 - 2 * x); };
    // each cycle starts where the last ended, and the whole ring turns as well
    double base = std::fmod(std::floor(cycles) * 270 + t * 120, 360);
    double head = base + 270 * ease(std::min(1.0, p * 2)), tail = base + 270 * ease(std::max(0.0, p * 2 - 1));
    color(accent);
    arc(cx, cy, r, float(tail), float(head + 12), thickness);
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
