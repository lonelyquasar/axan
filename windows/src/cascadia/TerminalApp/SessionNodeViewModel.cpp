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
        for (auto* revoker : { &RevokeTitleSubscription, &RevokeOutputIdleSubscription, &RevokeActivePaneSubscription, &RevokeSessionStateSubscription })
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

    // axan #30: see the header. StateSince restarts only on a state/kind transition.
    bool SessionNodeViewModel::SetSessionState(const hstring& state, const hstring& message, const hstring& kind, winrt::Windows::Foundation::DateTime now, const SessionRestingState& resting)
    {
        const bool transition = state != _state || kind != _stateKind;
        if (transition)
        {
            _state = state;
            _stateKind = kind;
            _stateSince = now;
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"State" });
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"StateKind" });
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"StateSince" });
        }
        if (message != _stateMessage)
        {
            _stateMessage = message;
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"StateMessage" });
        }
        // axan #31: anything but idle promotes the row immediately.
        if (transition && _state != L"idle")
        {
            _setDeEmphasised(false);
        }
        // axan: the done/error report the age counts from. A new identity restarts it; the same
        // one (e.g. done-unseen -> kept after being seen, or done -> done + msg) keeps it.
        const bool reportChanged = resting.reportKey != _reportKey;
        if (reportChanged)
        {
            _reportKey = resting.reportKey;
            _reportSince = _reportKey.empty() ? winrt::Windows::Foundation::DateTime{} : now;
        }
        const bool restingChanged = reportChanged || resting.retainedWhat != _retainedWhat || resting.retainedMessage != _retainedMessage ||
                                    resting.slotKey != _slotKey || resting.slotGlyph != _slotGlyph || resting.slotWord != _slotWord ||
                                    resting.slotExplanation != _slotExplanation || resting.showSlotWord != _showSlotWord || resting.agentName != _agentName;
        if (restingChanged)
        {
            _retainedWhat = resting.retainedWhat;
            _retainedMessage = resting.retainedMessage;
            _slotKey = resting.slotKey;
            _slotGlyph = resting.slotGlyph;
            _slotWord = resting.slotWord;
            _slotExplanation = resting.slotExplanation;
            _showSlotWord = resting.showSlotWord;
            _agentName = resting.agentName;
        }
        if (transition || message != _subLabelMessage || restingChanged)
        {
            _subLabelMessage = message;
            std::tie(_subLabel, _subLabelAge) = _computeSubLabel(now);
            _raiseStatePresentation();
        }
        return transition;
    }

    // ----- axan #32: row presentation ------------------------------------------------------

    void SessionNodeViewModel::_raiseStatePresentation()
    {
        for (const auto name : { L"StateDotVisibility", L"StateDotFill", L"StateDotStroke", L"StateGlyph", L"StateGlyphVisibility", L"StateGlyphIcon", L"StateWord", L"StateWordVisibility", L"SlotVisibility", L"SlotToolTip", L"SubLabel", L"SubLabelAge", L"SubLabelVisibility", L"SubLabelOpacity", L"StateToolTip", L"StateToolTipVisibility" })
        {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        }
    }

    bool SessionNodeViewModel::ShowsAge() const noexcept
    {
        return _state == L"needs-you" || _state == L"done-unseen" || _state == L"error";
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::StateDotVisibility() const noexcept
    {
        return !_isSeparator && ShowsAge() ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed;
    }

    // The theme's text colour (white on dark / near-black on light), which is what the
    // inverted badge fills with; its ring is the opposite so the dot separates from the icon.
    winrt::Windows::UI::Color SessionNodeViewModel::_textColor() const
    {
        if (const auto solid = _defaultTextBrush.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>())
        {
            return solid.Color();
        }
        return _isDarkTheme ? winrt::Windows::UI::Color{ 255, 255, 255, 255 } : winrt::Windows::UI::Color{ 255, 0x1C, 0x1C, 0x1C };
    }

    winrt::Windows::UI::Xaml::Media::Brush SessionNodeViewModel::StateDotFill() const
    {
        using winrt::Windows::UI::Color;
        if (_state == L"error")
        {
            return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ _isDarkTheme ? Color{ 255, 0xFF, 0x6B, 0x6B } : Color{ 255, 0xC4, 0x2B, 0x1C } };
        }
        auto c = _textColor();
        if (_state == L"done-unseen")
        {
            c.A = 0x80; // softer than needs-you
        }
        return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ c };
    }

    winrt::Windows::UI::Xaml::Media::Brush SessionNodeViewModel::StateDotStroke() const
    {
        using winrt::Windows::UI::Color;
        return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ _isDarkTheme ? Color{ 255, 0x1C, 0x1C, 0x1C } : Color{ 255, 0xFF, 0xFF, 0xFF } };
    }

    // The right-edge slot, static and never animated: the sidebar.stateGlyphs glyph for the
    // node's slot state, resolved by the page (AxanStateGlyphs.h). By default a lightning
    // bolt for an agent working (OSC 7501), play for a shell command running (OSC 133), and
    // the Robot glyph (U+E99A) for idle, i.e. a status-reporting program present and at rest
    // (including a done/error report already seen). Robot rather than a circle or another
    // play-like shape: it can't be mistaken for running, and it says "agent", not "state".
    // blocked / done / error default to nothing: the attention dot on the icon carries them.
    hstring SessionNodeViewModel::StateGlyph() const noexcept
    {
        return _isSeparator ? hstring{} : _slotGlyph;
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::StateWordVisibility() const noexcept
    {
        return StateWord().empty() ? winrt::Windows::UI::Xaml::Visibility::Collapsed : winrt::Windows::UI::Xaml::Visibility::Visible;
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::SlotVisibility() const noexcept
    {
        return StateGlyph().empty() && StateWord().empty() ? winrt::Windows::UI::Xaml::Visibility::Collapsed : winrt::Windows::UI::Xaml::Visibility::Visible;
    }

    hstring SessionNodeViewModel::ToString() const
    {
        if (_isSeparator)
        {
            return hstring{ L"Separator" };
        }
        if (_slotWord.empty())
        {
            return _Label;
        }
        return _Label + L", " + _slotWord;
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::StateGlyphVisibility() const noexcept
    {
        return StateGlyph().empty() ? winrt::Windows::UI::Xaml::Visibility::Collapsed : winrt::Windows::UI::Xaml::Visibility::Visible;
    }

    // 12px like the old TextBlock glyph. IconWUX gives a glyph / emoji a 12pt FontIconSource
    // (Segoe Fluent for the PUA range, Segoe UI otherwise), tinted here with the label brush;
    // an image (bitmap / exe / dll icon) has no intrinsic small size, so it is boxed to 12x12.
    winrt::Windows::UI::Xaml::Controls::IconElement SessionNodeViewModel::StateGlyphIcon()
    {
        const auto glyph = StateGlyph();
        if (glyph.empty())
        {
            return nullptr;
        }
        const auto icon{ winrt::Microsoft::Terminal::UI::IconPathConverter::IconWUX(glyph) };
        if (!icon)
        {
            return nullptr;
        }
        const auto brush = LabelBrush();
        winrt::Windows::UI::Xaml::Controls::FontIconSource font{ nullptr };
        if (const auto ise = icon.try_as<winrt::Windows::UI::Xaml::Controls::IconSourceElement>())
        {
            font = ise.IconSource().try_as<winrt::Windows::UI::Xaml::Controls::FontIconSource>();
        }
        if (font)
        {
            font.Foreground(brush);
            icon.Foreground(brush);
        }
        else
        {
            icon.Width(12);
            icon.Height(12);
        }
        return icon;
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::SubLabelVisibility() const noexcept
    {
        return _subLabel.empty() && _subLabelAge.empty() ? winrt::Windows::UI::Xaml::Visibility::Collapsed : winrt::Windows::UI::Xaml::Visibility::Visible;
    }

    hstring SessionNodeViewModel::FormatAge(winrt::Windows::Foundation::TimeSpan age)
    {
        using namespace std::chrono;
        const auto secs = duration_cast<seconds>(age).count();
        if (secs < 60)
        {
            return hstring{ L"<1m" };
        }
        if (secs < 3600)
        {
            return hstring{ std::to_wstring(secs / 60) + L"m" };
        }
        if (secs < 86400)
        {
            return hstring{ std::to_wstring(secs / 3600) + L"h" };
        }
        return hstring{ std::to_wstring(secs / 86400) + L"d" };
    }

    // {message, " · <age>"}. The row renders the two in separate columns: the age keeps its
    // width at the end of the line and the message is ellipsized into whatever is left (the
    // full message is in the tooltip).
    static std::pair<hstring, hstring> _withAge(std::wstring what, winrt::Windows::Foundation::DateTime now, winrt::Windows::Foundation::DateTime since)
    {
        const auto age = now > since ? now - since : winrt::Windows::Foundation::TimeSpan{ 0 };
        return { hstring{ what }, hstring{ L" \u00B7 " + std::wstring{ SessionNodeViewModel::FormatAge(age) } } };
    }

    // Second line: what the state is about, plus how long it has been that way for the
    // states the user is looking for. Working shows only its message (no age); running
    // and idle show nothing, so their rows stay one line, unless a seen report is kept
    // (statusAfterSeen = keep): then it shows that report, muted, aged from when it arrived.
    std::pair<hstring, hstring> SessionNodeViewModel::_computeSubLabel(winrt::Windows::Foundation::DateTime now) const
    {
        if (_isSeparator)
        {
            return {};
        }
        std::wstring what{ _stateMessage };
        if (_state == L"working")
        {
            // axan: a working agent that sends no message shows its name instead.
            return { what.empty() && ShowsAgentName() ? _agentName : hstring{ what }, hstring{} };
        }
        if (!ShowsAge())
        {
            if (ShowsRetained())
            {
                std::wstring kept{ _retainedMessage.empty() ? _retainedWhat : _retainedMessage };
                return _withAge(std::move(kept), now, _reportSince != winrt::Windows::Foundation::DateTime{} ? _reportSince : _stateSince);
            }
            // axan: sidebar.showAgentName, lowest priority: an agent (idle, or running beside
            // a shell command) shows its name.
            if (ShowsAgentName())
            {
                return { _agentName, hstring{} };
            }
            return {};
        }
        if (what.empty())
        {
            if (_state == L"needs-you")
            {
                what = _stateKind.empty() ? std::wstring{ L"needs you" } : std::wstring{ _stateKind };
            }
            else if (_state == L"done-unseen")
            {
                what = L"done";
            }
            else
            {
                what = L"error";
            }
        }
        return _withAge(std::move(what), now, _stateSince);
    }

    // ----- axan #31: idle de-emphasis ------------------------------------------------------

    void SessionNodeViewModel::SetRowHeightPx(double px)
    {
        if (px > 0.0 && px != _rowHeightPx)
        {
            _rowHeightPx = px;
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"RowMinHeight" });
        }
    }

    void SessionNodeViewModel::_setDeEmphasised(bool value)
    {
        if (value == _deEmphasised)
        {
            return;
        }
        _deEmphasised = value;
        for (const auto name : { L"IsDeEmphasised", L"LabelFontSize", L"RowOpacity", L"RowMinHeight" })
        {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        }
        // "gray" repaints the glyph and the label; only a coloured node has anything to gray.
        if ((_idleEffects & Axan::IdleEffects::kGray) && !_iconColor.empty())
        {
            for (const auto name : { L"Icon", L"LabelBrush", L"StateGlyphIcon" })
            {
                PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
            }
        }
    }

    bool SessionNodeViewModel::SetIdleEffects(Axan::IdleEffects::Mask effects)
    {
        if (effects == _idleEffects)
        {
            return false;
        }
        const auto changed = static_cast<Axan::IdleEffects::Mask>(effects ^ _idleEffects);
        _idleEffects = effects;
        if (!_deEmphasised)
        {
            return true; // nothing on screen depends on the mask until the row goes idle
        }
        for (const auto name : { L"LabelFontSize", L"RowOpacity", L"RowMinHeight" })
        {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        }
        if ((changed & Axan::IdleEffects::kGray) && !_iconColor.empty())
        {
            for (const auto name : { L"Icon", L"LabelBrush", L"StateGlyphIcon" })
            {
                PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
            }
        }
        return true;
    }

    winrt::hstring SessionNodeViewModel::_effectiveColorToken() const
    {
        if (_idleEffect(Axan::IdleEffects::kGray) && !ResolveColorToken(_iconColor, _isDarkTheme).empty())
        {
            // Neutral mid-grays that stay legible on the sidebar in each theme.
            return _isDarkTheme ? winrt::hstring{ L"#8A8A8A" } : winrt::hstring{ L"#6E6E6E" };
        }
        return _iconColor;
    }

    // Idle long enough = no input/output since the threshold AND idle state for that long
    // (a session that just went idle after working isn't shrunk the moment it settles).
    bool SessionNodeViewModel::UpdateDeEmphasis(winrt::Windows::Foundation::DateTime now, int32_t idleAfterMinutes)
    {
        bool want = false;
        if (!_isSeparator && idleAfterMinutes > 0 && _state == L"idle")
        {
            const auto since = std::max(_lastActivity, _stateSince);
            want = since != winrt::Windows::Foundation::DateTime{} && now - since >= std::chrono::minutes{ idleAfterMinutes };
        }
        const bool flipped = want != _deEmphasised;
        _setDeEmphasised(want);
        return flipped;
    }

    void SessionNodeViewModel::RefreshAge(winrt::Windows::Foundation::DateTime now)
    {
        if (!ShowsAge() && !ShowsRetained())
        {
            return;
        }
        auto [text, age] = _computeSubLabel(now);
        const bool visibilityChanged = (_subLabel.empty() && _subLabelAge.empty()) != (text.empty() && age.empty());
        if (text != _subLabel)
        {
            _subLabel = std::move(text);
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SubLabel" });
        }
        if (age != _subLabelAge)
        {
            _subLabelAge = std::move(age);
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SubLabelAge" });
        }
        if (visibilityChanged)
        {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SubLabelVisibility" });
        }
    }

    // For a non-idle state: the state, its kind, the absolute local time it began and the
    // full message, e.g. "needs-you (permission) since 14:32:05" / "Allow Bash: ...".
    // Shown under the label in the row tooltip; Collapsed when idle.
    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::StateToolTipVisibility() const noexcept
    {
        return _isSeparator || (_state == L"idle" && !ShowsRetained() && _slotExplanation.empty()) ? winrt::Windows::UI::Xaml::Visibility::Collapsed : winrt::Windows::UI::Xaml::Visibility::Visible;
    }

    static void _appendSince(std::wstring& tip, winrt::Windows::Foundation::DateTime since)
    {
        if (since == winrt::Windows::Foundation::DateTime{})
        {
            return;
        }
        const FILETIME utc = winrt::clock::to_FILETIME(since);
        FILETIME local{};
        SYSTEMTIME st{};
        if (FileTimeToLocalFileTime(&utc, &local) && FileTimeToSystemTime(&local, &st))
        {
            wchar_t buf[32]{};
            swprintf_s(buf, L" since %02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
            tip += buf;
        }
    }

    // A kept report (statusAfterSeen = keep) adds "done (seen) since 14:32:05" and its full
    // message, after the live state's lines when the node is running.
    hstring SessionNodeViewModel::StateToolTip() const
    {
        if (_isSeparator)
        {
            return {};
        }
        std::wstring tip;
        // axan: the slot state's one-sentence explanation leads the state detail.
        if (!_slotExplanation.empty())
        {
            tip = _slotExplanation;
        }
        if (_state != L"idle")
        {
            if (!tip.empty())
            {
                tip += L"\n";
            }
            tip += _state;
            if (!_stateKind.empty())
            {
                tip += L" (";
                tip += _stateKind;
                tip += L")";
            }
            _appendSince(tip, _stateSince);
            if (!_stateMessage.empty())
            {
                tip += L"\n";
                tip += _stateMessage;
            }
        }
        if (ShowsRetained())
        {
            if (!tip.empty())
            {
                tip += L"\n";
            }
            tip += _retainedWhat;
            tip += L" (seen)";
            _appendSince(tip, _reportSince != winrt::Windows::Foundation::DateTime{} ? _reportSince : _stateSince);
            if (!_retainedMessage.empty())
            {
                tip += L"\n";
                tip += _retainedMessage;
            }
        }
        return hstring{ tip };
    }

    void SessionNodeViewModel::SetLastActivity(winrt::Windows::Foundation::DateTime value)
    {
        if (value != _lastActivity)
        {
            _lastActivity = value;
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"LastActivity" });
        }
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
        const auto iconColor = _targetIncludesIcon(ColorTarget) ? _effectiveColorToken() : winrt::hstring{};
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
            if (const auto c = _parseHexColor(ResolveColorToken(_effectiveColorToken(), _isDarkTheme)))
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
    bool SessionNodeViewModel::SetDefaultTextBrush(const winrt::Windows::UI::Xaml::Media::Brush& brush, bool isDarkTheme)
    {
        // axan #26: the page pushes this on every _updateThemeColors pass; when neither the
        // default text color nor the palette set changed there is nothing to repaint, and
        // re-raising LabelBrush+Icon on every node rebuilds every row's icon element. (The
        // palette is a compile-time table, so color + isDark fully determine the output.) A
        // new node has no brush yet, so it always takes the first push.
        if (isDarkTheme == _isDarkTheme && _defaultTextBrush && brush)
        {
            const auto oldSolid = _defaultTextBrush.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
            const auto newSolid = brush.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
            if (oldSolid && newSolid && oldSolid.Color() == newSolid.Color())
            {
                return false;
            }
        }
        _defaultTextBrush = brush;
        _isDarkTheme = isDarkTheme;
        // A theme switch flips which palette set a stored color NAME resolves against, so both
        // the label brush and the glyph tint can change — raise Icon too, not just LabelBrush.
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"LabelBrush" }); // axan: + StateGlyphIcon (tinted with it)
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"StateGlyphIcon" });
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"Icon" });
        // axan #32: the state dot is derived from the theme text colour too.
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"StateDotFill" });
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"StateDotStroke" });
        return true;
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
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"LabelBrush" }); // axan: + StateGlyphIcon (tinted with it)
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"StateGlyphIcon" });
    }

    // axan #3: the shared ExpandedNodeTemplate holds BOTH a session Grid and a separator Grid
    // and shows exactly one, driven by these two. Collapsed (not Opacity/Hidden) so the hidden
    // half takes no layout space — a 0.5-row separator must actually be half a row tall.
    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::SessionVisibility() const noexcept
    {
        return _isSeparator ? winrt::Windows::UI::Xaml::Visibility::Collapsed : winrt::Windows::UI::Xaml::Visibility::Visible;
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::SeparatorVisibility() const noexcept
    {
        return _isSeparator ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed;
    }

    // axan #3: the hairline shows only for the "line" style; "space" is an empty gap of the
    // same height.
    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::SeparatorLineVisibility() const noexcept
    {
        // axan #33: a labelled header draws its own rule after the label instead.
        return (_isSeparator && _separatorStyle == L"line" && _separatorLabel.empty()) ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed;
    }

    // ----- axan #33: section headers ---------------------------------------------------------

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::SeparatorHeaderVisibility() const noexcept
    {
        return HasSectionHeader() ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed;
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::SeparatorHeaderLineVisibility() const noexcept
    {
        return (HasSectionHeader() && _separatorStyle == L"line") ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed;
    }

    // Chevron down when expanded, chevron right when collapsed (Segoe Fluent / MDL2).
    hstring SessionNodeViewModel::CollapseGlyph() const noexcept
    {
        return _collapsed ? hstring{ L"\uE76C" } : hstring{ L"\uE70D" };
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::SectionBadgeVisibility() const noexcept
    {
        return (_collapsed && !_sectionBadge.empty()) ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed;
    }

    void SessionNodeViewModel::ActivateSectionBadge()
    {
        if (OnSectionBadgeActivated)
        {
            OnSectionBadgeActivated();
        }
    }

    void SessionNodeViewModel::SetSeparatorLabel(const hstring& label)
    {
        if (label == _separatorLabel)
        {
            return;
        }
        _separatorLabel = label;
        if (_separatorLabel.empty())
        {
            _collapsed = false; // an unlabelled separator has no section to collapse
        }
        for (const auto name : { L"SeparatorLabel", L"SeparatorHeaderVisibility", L"SeparatorHeaderLineVisibility", L"SeparatorLineVisibility", L"SeparatorPixelHeight", L"IsCollapsed", L"CollapseGlyph", L"SectionBadgeVisibility" })
        {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        }
    }

    void SessionNodeViewModel::SetCollapsed(bool collapsed)
    {
        collapsed = collapsed && HasSectionHeader();
        if (collapsed == _collapsed)
        {
            return;
        }
        _collapsed = collapsed;
        for (const auto name : { L"IsCollapsed", L"CollapseGlyph", L"SectionBadgeVisibility" })
        {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        }
    }

    void SessionNodeViewModel::SetSectionSummary(const hstring& summary, const hstring& badge)
    {
        if (summary != _sectionSummary)
        {
            _sectionSummary = summary;
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SectionSummary" });
        }
        if (badge != _sectionBadge)
        {
            _sectionBadge = badge;
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SectionBadge" });
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"SectionBadgeVisibility" });
        }
    }

    bool SessionNodeViewModel::SetSectionHidden(bool hidden)
    {
        if (hidden == _sectionHidden)
        {
            return false;
        }
        _sectionHidden = hidden;
        PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IsSectionHidden" });
        return true;
    }

    // axan #3: flip this VM into separator mode. Called once by the page when it mints a
    // separator node (never on a live session VM). The inputs are already normalized by
    // LoadStartupTree (style/placement non-empty, height clamped), but the fallbacks here keep
    // a bad value from rendering a zero-height or unstyled row. Raises every projected
    // separator property so a container recycled onto this node re-pulls the right half of
    // the template; Label is set too so the (hidden) label binding and tooltip are never blank.
    void SessionNodeViewModel::ConfigureSeparator(const winrt::hstring& style, double height, const winrt::hstring& placement, double rowHeightPx)
    {
        _isSeparator = true;
        _separatorStyle = (style == L"space") ? winrt::hstring{ L"space" } : winrt::hstring{ L"line" };
        _separatorHeight = (height > 0.0) ? height : 1.0;
        _placement = (placement == L"bottom") ? winrt::hstring{ L"bottom" } : winrt::hstring{ L"inline" };
        _rowHeightPx = (rowHeightPx > 0.0) ? rowHeightPx : 32.0;
        Label(winrt::hstring{ L"Separator" });
        for (const auto name : { L"IsSeparator", L"SeparatorStyle", L"SeparatorHeight", L"Placement", L"SessionVisibility", L"SeparatorVisibility", L"SeparatorLineVisibility", L"SeparatorPixelHeight" })
        {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        }
    }
}
