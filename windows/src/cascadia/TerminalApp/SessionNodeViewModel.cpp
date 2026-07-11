// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#include "pch.h"
#include "SessionNodeViewModel.h"
#include "SessionNodeViewModel.g.cpp"

#include <array>
#include <cwctype>
#include <optional>
#include <string_view>

#include <AxanSessionPalette.h> // the shared six-name session color palette (#422, #13)

namespace winrt::TerminalApp::implementation
{
    // axan #427 item 3: tear down the recompute subscriptions _CreateSessionNode
    // (TabManagement.cpp) wired to this node. Called explicitly when the node is pruned; the
    // destructor is the backstop. Each unsubscriber weak-resolves its event source, so a
    // source that died first makes the revoke a harmless no-op.
    void SessionNodeViewModel::RevokeRecomputeSubscriptions()
    {
        for (auto* revoker : { &RevokeTitleSubscription, &RevokeOutputIdleSubscription, &RevokeActivePaneSubscription })
        {
            if (*revoker)
            {
                (*revoker)();
                *revoker = nullptr;
            }
        }
    }

    SessionNodeViewModel::~SessionNodeViewModel()
    {
        RevokeRecomputeSubscriptions();
    }

    // axan M13: parse a "#RRGGBB" or "#AARRGGBB" hex string into a Color. Returns nullopt for
    // anything else (empty, named colors, malformed) so the caller leaves the glyph on the
    // theme foreground. Kept tiny and dependency-free — the picker only ever writes #-hex.
    static std::optional<winrt::Windows::UI::Color> _parseHexColor(const winrt::hstring& value)
    {
        std::wstring_view s{ value };
        if (s.empty() || s.front() != L'#')
        {
            return std::nullopt;
        }
        s.remove_prefix(1);
        if (s.size() != 6 && s.size() != 8)
        {
            return std::nullopt;
        }
        uint32_t v = 0;
        for (const auto c : s)
        {
            uint32_t nibble;
            if (c >= L'0' && c <= L'9')
            {
                nibble = static_cast<uint32_t>(c - L'0');
            }
            else if (c >= L'a' && c <= L'f')
            {
                nibble = static_cast<uint32_t>(c - L'a' + 10);
            }
            else if (c >= L'A' && c <= L'F')
            {
                nibble = static_cast<uint32_t>(c - L'A' + 10);
            }
            else
            {
                return std::nullopt;
            }
            v = (v << 4) | nibble;
        }
        winrt::Windows::UI::Color color{};
        if (s.size() == 8)
        {
            color.A = static_cast<uint8_t>((v >> 24) & 0xFF);
            color.R = static_cast<uint8_t>((v >> 16) & 0xFF);
            color.G = static_cast<uint8_t>((v >> 8) & 0xFF);
            color.B = static_cast<uint8_t>(v & 0xFF);
        }
        else
        {
            color.A = 0xFF;
            color.R = static_cast<uint8_t>((v >> 16) & 0xFF);
            color.G = static_cast<uint8_t>((v >> 8) & 0xFF);
            color.B = static_cast<uint8_t>(v & 0xFF);
        }
        return color;
    }

    // axan #422/#13: the per-node color palette (name -> per-theme hex, Linux rationale and
    // the tables themselves) now lives in the shared src/inc/AxanSessionPalette.h so the
    // Startup sessions settings page paints the identical swatches. Windows picks the set
    // by the sidebar's ActualTheme.

// axan M8: a short (<=4 char) abbreviation of a display name for the minimized sidebar
    // cells. Multi-word names -> the leading letters of up to four words, uppercased
    // ("Windows PowerShell" -> "WP", "child pwsh.exe" -> "CPE"); a single word -> its first
    // up-to-4 characters as-is ("PowerShell" -> "Powe"). Tokens with no letter/digit (e.g. a
    // lone "~") are dropped so they don't eat an initials slot. Empty in -> empty out.
    winrt::hstring SessionNodeViewModel::Initials(const winrt::hstring& label)
    {
        const std::wstring_view s{ label };
        std::vector<std::wstring> words;
        std::wstring cur;
        const auto flush = [&]() {
            if (std::any_of(cur.begin(), cur.end(), [](wchar_t c) { return std::iswalnum(c) != 0; }))
            {
                words.push_back(cur);
            }
            cur.clear();
        };
        for (const auto ch : s)
        {
            if (ch == L' ' || ch == L'\t' || ch == L'.' || ch == L'-' || ch == L'_' || ch == L'/' || ch == L'\\')
            {
                flush();
            }
            else
            {
                cur.push_back(ch);
            }
        }
        flush();

        std::wstring out;
        if (words.size() >= 2)
        {
            for (const auto& w : words)
            {
                if (out.size() >= 4)
                {
                    break;
                }
                out.push_back(static_cast<wchar_t>(std::towupper(w.front())));
            }
        }
        else if (words.size() == 1)
        {
            out = words.front().substr(0, std::min<size_t>(4, words.front().size()));
        }
        return winrt::hstring{ out };
    }

    // axan M7: mint a NEW IconElement from the resolved path on every get — never cache.
    // See the header: a cached element re-parented during a drag reparent crashes the app.
    // IconPathConverter::IconWUX turns the path (file/SVG/glyph/emoji/exe) into an element,
    // sized to the 16x16 tab/palette icon convention. Empty path -> nullptr (no icon).
    // axan M13: does the recolor target paint the icon / the text? "icon" -> icon only,
    // "text" -> text only, "both" or empty -> both (the default).
    static bool _targetIncludesIcon(const winrt::hstring& target) { return target != L"text"; }
    static bool _targetIncludesText(const winrt::hstring& target) { return target == L"text" || target == L"both" || target.empty(); }

    winrt::Windows::UI::Xaml::Controls::IconElement SessionNodeViewModel::Icon()
    {
        // Only tint the icon when the recolor target includes the icon; otherwise leave the
        // glyph on the theme foreground (the text may still be recolored via LabelBrush).
        const auto iconColor = _targetIncludesIcon(ColorTarget) ? _iconColor : winrt::hstring{};
        const auto icon{ BuildIcon(_iconPath, iconColor, _isDarkTheme) };
        if (icon)
        {
            icon.Width(16);
            icon.Height(16);
        }
        return icon;
    }

    // axan M13 (design handoff): the label text brush. When the recolor targets the text
    // (Apply color to = Text / Both) and parses, the label paints in that color; otherwise it
    // falls back to the default theme text brush so the row matches every other label.
    winrt::Windows::UI::Xaml::Media::Brush SessionNodeViewModel::LabelBrush()
    {
        if (_targetIncludesText(ColorTarget))
        {
            // Resolve the stored token (palette name / "#hex") to the active theme's hex first,
            // so a name like "blue" paints the label in the theme-legible blue.
            if (const auto c = _parseHexColor(ResolveColorToken(_iconColor, _isDarkTheme)))
            {
                return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ *c };
            }
        }
        // Default: a concrete theme-correct text brush (a null Foreground renders black, not
        // inherited). The page derives this from the sidebar's ActualTheme — Application's
        // RequestedTheme is unreliable here because WT forces its look via element theme, not the
        // app theme. White-on-dark is the fallback before the page has pushed a brush.
        if (_defaultTextBrush)
        {
            return _defaultTextBrush;
        }
        return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Color{ 255, 255, 255, 255 } };
    }

    // axan M13: the page pushes the theme-correct default text brush (from the sidebar's
    // ActualTheme); re-raise LabelBrush so any node not text-recolored repaints to it.
    void SessionNodeViewModel::SetDefaultTextBrush(const winrt::Windows::UI::Xaml::Media::Brush& brush, bool isDarkTheme)
    {
        _defaultTextBrush = brush;
        _isDarkTheme = isDarkTheme;
        // A theme switch flips which palette set a stored color NAME resolves against, so both
        // the label brush and the glyph tint can change — raise Icon too, not just LabelBrush.
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"LabelBrush" });
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"Icon" });
    }

    // axan #422: see the header. "" -> ""; "#hex" -> verbatim; a palette name -> the active set's
    // hex; an unknown name -> "" (no recolor). Mirrors Linux sidebar_resolve_color_token.
    winrt::hstring SessionNodeViewModel::ResolveColorToken(const winrt::hstring& token, bool isDarkTheme)
    {
        return winrt::hstring{ Axan::SessionPalette::ResolveToken(token, isDarkTheme) };
    }

    winrt::Windows::UI::Xaml::Media::Brush SessionNodeViewModel::ResolveColorBrush(const winrt::hstring& token, bool isDarkTheme)
    {
        if (const auto c = _parseHexColor(ResolveColorToken(token, isDarkTheme)))
        {
            return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ *c };
        }
        return nullptr;
    }

    // axan M13: build a (recolored) IconElement from an already-resolved icon path. IconWUX
    // returns an IconSourceElement wrapping a FontIconSource (for a glyph/emoji) or an ImageIcon
    // (for an exe/dll/lnk) — it is NOT a FontIcon, so the recolor has to reach the FontIconSource
    // and set its Foreground (an earlier cut try_as<FontIcon>'d and silently no-op'd). The
    // recolor only affects monochrome glyph icons; a colored profile/image icon keeps its pixels.
    // A malformed/empty color leaves the glyph on the theme foreground (the M7 default).
    winrt::Windows::UI::Xaml::Controls::IconElement SessionNodeViewModel::BuildIcon(const winrt::hstring& path, const winrt::hstring& color, bool isDarkTheme)
    {
        if (path.empty())
        {
            return nullptr;
        }
        const auto icon{ winrt::Microsoft::Terminal::UI::IconPathConverter::IconWUX(path) };
        // Resolve the token (palette name / "#hex") to the active theme's hex before parsing.
        if (const auto c = _parseHexColor(ResolveColorToken(color, isDarkTheme)))
        {
            const winrt::Windows::UI::Xaml::Media::SolidColorBrush brush{ *c };
            // Set the element's own Foreground (covers a plain FontIcon, defensively) and, for
            // the IconWUX shape, the wrapped FontIconSource's Foreground (the one that actually
            // paints a glyph element).
            icon.Foreground(brush);
            if (const auto ise = icon.try_as<winrt::Windows::UI::Xaml::Controls::IconSourceElement>())
            {
                if (const auto fis = ise.IconSource().try_as<winrt::Windows::UI::Xaml::Controls::FontIconSource>())
                {
                    fis.Foreground(brush);
                }
            }
        }
        return icon;
    }

    void SessionNodeViewModel::IconPath(const winrt::hstring& value)
    {
        if (_iconPath != value)
        {
            _iconPath = value;
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"Icon" });
        }
    }

    // axan M13: set path + recolor + recolor-target together, always raising Icon AND LabelBrush
    // so a color/target-only change (same path) still repaints both the glyph and the label.
    void SessionNodeViewModel::ApplyAppearance(const winrt::hstring& path, const winrt::hstring& color, const winrt::hstring& target)
    {
        _iconPath = path;
        _iconColor = color;
        ColorTarget = target.empty() ? winrt::hstring{ L"both" } : target;
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"Icon" });
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"LabelBrush" });
    }
}
