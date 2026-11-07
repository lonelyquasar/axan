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
        // axan: while a preview is shown, keep the real state current underneath it. A real
        // state change ends the preview (restoring the real fields), then applies as usual.
        if (_realWhilePreviewing)
        {
            auto& real = *_realWhilePreviewing;
            if (state == real.state && kind == real.kind)
            {
                real.message = message;
                if (resting.reportKey != real.resting.reportKey)
                {
                    real.reportSince = resting.reportKey.empty() ? winrt::Windows::Foundation::DateTime{} : now;
                }
                real.resting = resting;
                return false;
            }
            ClearPreview(now);
        }
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
        if (resting.reportKey != _resting.reportKey)
        {
            _reportSince = resting.reportKey.empty() ? winrt::Windows::Foundation::DateTime{} : now;
        }
        const bool restingChanged = resting != _resting;
        if (restingChanged)
        {
            _resting = resting;
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

    // ----- axan: state preview -----------------------------------------------------------

    SessionNodeViewModel::StateFields SessionNodeViewModel::_captureState() const
    {
        return StateFields{ _state, _stateKind, _stateMessage, _stateSince, _reportSince, _resting };
    }

    void SessionNodeViewModel::_restoreState(const StateFields& f)
    {
        _state = f.state;
        _stateKind = f.kind;
        _stateMessage = f.message;
        _stateSince = f.since;
        _reportSince = f.reportSince;
        _resting = f.resting;
    }

    // Everything a preview can change on screen: the state presentation, and the idle
    // effects (a non-idle preview is never de-emphasised).
    void SessionNodeViewModel::_raisePreviewChange(winrt::Windows::Foundation::DateTime now)
    {
        _subLabelMessage = _stateMessage;
        std::tie(_subLabel, _subLabelAge) = _computeSubLabel(now);
        _raiseStatePresentation();
        for (const auto name : { L"LabelFontSize", L"RowOpacity", L"RowMinHeight", L"Icon", L"LabelBrush" })
        {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        }
    }

    void SessionNodeViewModel::SetPreview(const hstring& id, const hstring& state, const hstring& message, const hstring& kind, winrt::Windows::Foundation::DateTime since, winrt::Windows::Foundation::DateTime now, const SessionRestingState& resting)
    {
        if (_isSeparator)
        {
            return;
        }
        if (!_realWhilePreviewing)
        {
            _realWhilePreviewing = _captureState();
        }
        _previewId = id;
        const auto reportSince = resting.reportKey.empty() ? winrt::Windows::Foundation::DateTime{} : since;
        // The agent name is the real one (sidebar.showAgentName already applied), so an idle /
        // running preview shows what the row would.
        auto shown = resting;
        shown.agentName = _realWhilePreviewing->resting.agentName;
        _restoreState(StateFields{ state, kind, message, since, reportSince, std::move(shown) });
        _raisePreviewChange(now);
    }

    bool SessionNodeViewModel::ClearPreview(winrt::Windows::Foundation::DateTime now)
    {
        if (!_realWhilePreviewing)
        {
            return false;
        }
        _restoreState(*_realWhilePreviewing);
        _realWhilePreviewing.reset();
        _previewId = {};
        _raisePreviewChange(now);
        return true;
    }

    // ----- axan: sizes ---------------------------------------------------------------------

    bool SessionNodeViewModel::SetSizes(double labelFontSize, double stateLineFontSize, double iconSize, double stateGlyphSize, double idleLabelFontSize, double idleRowHeight)
    {
        bool changed = false;
        const auto raise = [this](const wchar_t* name) {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        };
        if (labelFontSize != _labelFontSize)
        {
            _labelFontSize = labelFontSize;
            raise(L"LabelFontSize");
            changed = true;
        }
        if (stateLineFontSize != _stateLineFontSize)
        {
            _stateLineFontSize = stateLineFontSize;
            raise(L"StateLineFontSize");
            // axan #46: the idle-collapsed badge is sized from (and gated on) this size.
            raise(L"IdleTuckBadgeFontSize");
            raise(L"IdleTuckDotSize");
            raise(L"IdleTuckBadgeVisibility");
            changed = true;
        }
        if (iconSize != _iconSize)
        {
            _iconSize = iconSize;
            raise(L"IconSize");
            raise(L"IconPresenterSize"); // axan #48: both follow the box
            raise(L"IconPresenterMargin");
            raise(L"Icon");
            changed = true;
        }
        if (stateGlyphSize != _stateGlyphSize)
        {
            _stateGlyphSize = stateGlyphSize;
            raise(L"StateGlyphIcon");
            changed = true;
        }
        // axan #47: the "smaller" idle effect's sizes (LabelFontSize / RowMinHeight read them
        // only while that effect applies; raising regardless is harmless).
        if (idleLabelFontSize != _idleLabelFontSize)
        {
            _idleLabelFontSize = idleLabelFontSize;
            raise(L"LabelFontSize");
            changed = true;
        }
        if (idleRowHeight != _idleRowHeight)
        {
            _idleRowHeight = idleRowHeight;
            raise(L"RowMinHeight");
            changed = true;
        }
        return changed;
    }

    // axan #48: sidebar.iconOverflow. While on, the icon is laid out in a box
    // kIconOverflowFactor times IconSize, pulled back to IconSize by a negative margin on every
    // side: the presenter's desired size (size + margins) is still the box, so the row and its
    // neighbours lay out as before and nothing clips it to the box. The factor matches
    // AxanGlyphScale::kMaxScale: a glyph at the largest sidebar.iconFontScale is a 36pt font in
    // a 16px box, whose line box is about 3x the box.
    static constexpr double kIconOverflowFactor = 3.0;

    bool SessionNodeViewModel::SetIconOverflow(bool overflow)
    {
        if (overflow == _iconOverflow)
        {
            return false;
        }
        _iconOverflow = overflow;
        for (const auto name : { L"IconPresenterSize", L"IconPresenterMargin", L"IconHitTestVisible", L"Icon" })
        {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        }
        return true;
    }

    double SessionNodeViewModel::IconPresenterSize() const noexcept
    {
        return _iconOverflow ? _iconSize * kIconOverflowFactor : _iconSize;
    }

    winrt::Windows::UI::Xaml::Thickness SessionNodeViewModel::IconPresenterMargin() const noexcept
    {
        const auto inset = -(IconPresenterSize() - _iconSize) / 2.0;
        return winrt::Windows::UI::Xaml::Thickness{ inset, inset, inset, inset };
    }

    void SessionNodeViewModel::RefreshIcons()
    {
        for (const auto name : { L"Icon", L"StateGlyphIcon" })
        {
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        }
    }

    // Multiply a glyph icon's font size (an IconWUX IconSourceElement over a FontIconSource,
    // whose size already carries the AxanGlyphScale per-font factor). False for an image.
    static bool _scaleGlyphIcon(const winrt::Windows::UI::Xaml::Controls::IconElement& icon, double factor)
    {
        if (const auto ise = icon.try_as<winrt::Windows::UI::Xaml::Controls::IconSourceElement>())
        {
            if (const auto fis = ise.IconSource().try_as<winrt::Windows::UI::Xaml::Controls::FontIconSource>())
            {
                fis.FontSize(fis.FontSize() * factor);
                return true;
            }
        }
        return false;
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
        return _isSeparator ? hstring{} : _resting.slotGlyph;
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
        if (_resting.slotWord.empty())
        {
            return _Label;
        }
        return _Label + L", " + _resting.slotWord;
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::StateGlyphVisibility() const noexcept
    {
        return StateGlyph().empty() ? winrt::Windows::UI::Xaml::Visibility::Collapsed : winrt::Windows::UI::Xaml::Visibility::Visible;
    }

    // sidebar.stateGlyphSize, default 12px like the old TextBlock glyph. IconWUX gives a glyph
    // / emoji a 12pt FontIconSource (Segoe Fluent for the PUA range, Segoe UI otherwise; times
    // the AxanGlyphScale factor), rescaled here to the setting and tinted with the label
    // brush; an image (bitmap / exe / dll icon) has no intrinsic small size, so it is boxed.
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
            font.FontSize(font.FontSize() * (_stateGlyphSize / 12.0));
            font.Foreground(brush);
            icon.Foreground(brush);
        }
        else
        {
            icon.Width(_stateGlyphSize);
            icon.Height(_stateGlyphSize);
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
            return { what.empty() && ShowsAgentName() ? _resting.agentName : hstring{ what }, hstring{} };
        }
        if (!ShowsAge())
        {
            if (ShowsRetained())
            {
                std::wstring kept{ _resting.retainedMessage.empty() ? _resting.retainedWhat : _resting.retainedMessage };
                return _withAge(std::move(kept), now, _reportSince != winrt::Windows::Foundation::DateTime{} ? _reportSince : _stateSince);
            }
            // axan: sidebar.showAgentName, lowest priority: an agent (idle, or running beside
            // a shell command) shows its name.
            if (ShowsAgentName())
            {
                return { _resting.agentName, hstring{} };
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
        if (!_isSeparator && idleAfterMinutes > 0 && State() == L"idle") // axan: the real state, never a preview
        {
            const auto since = std::max(_lastActivity, StateSince());
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
    // Shown under the label in the row tooltip; Collapsed when idle with nothing to explain.
    // A preview follows the same rule: its "Preview - " prefix alone is not worth a line.
    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::StateToolTipVisibility() const noexcept
    {
        return _isSeparator || (_state == L"idle" && !ShowsRetained() && _resting.slotExplanation.empty()) ? winrt::Windows::UI::Xaml::Visibility::Collapsed : winrt::Windows::UI::Xaml::Visibility::Visible;
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
        // axan: a preview says so first, so it is never mistaken for a real report.
        std::wstring tip{ _realWhilePreviewing ? L"Preview \u2014 " : L"" };
        const auto prefixLength = tip.size();
        // axan: the slot state's one-sentence explanation leads the state detail.
        if (!_resting.slotExplanation.empty())
        {
            tip += _resting.slotExplanation;
        }
        if (_state != L"idle")
        {
            if (tip.size() > prefixLength)
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
            if (tip.size() > prefixLength)
            {
                tip += L"\n";
            }
            tip += _resting.retainedWhat;
            tip += L" (seen)";
            _appendSince(tip, _reportSince != winrt::Windows::Foundation::DateTime{} ? _reportSince : _stateSince);
            if (!_resting.retainedMessage.empty())
            {
                tip += L"\n";
                tip += _resting.retainedMessage;
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
            // axan: sidebar.iconSize. A glyph's 12pt font size is 3/4 of the 16px default box,
            // so it scales with the box.
            icon.Width(_iconSize);
            icon.Height(_iconSize);
            // axan #48: a glyph fills the presenter, which is larger than the box while
            // sidebar.iconOverflow is on; the glyph stays centred and its size is unchanged.
            // An image keeps the box size (the presenter centres it).
            if (_scaleGlyphIcon(icon, _iconSize / 16.0) && _iconOverflow)
            {
                icon.Width(IconPresenterSize());
                icon.Height(IconPresenterSize());
            }
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

    // ----- axan #46: idle-collapsed indicator ----------------------------------------------
    // A hollow dot in the expander slot while the automatic idle tuck hides rows here, with
    // the hidden count as a tiny badge inside it ("9+" cap). Below a state-line size of 9 the
    // badge would be unreadable, so only the plain dot shows; the tooltip always has the count.

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::IdleTuckVisibility() const noexcept
    {
        return _idleTucked > 0 ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed;
    }

    hstring SessionNodeViewModel::IdleTuckBadge() const
    {
        if (_idleTucked == 0)
        {
            return {};
        }
        return _idleTucked > 9 ? hstring{ L"9+" } : hstring{ std::to_wstring(_idleTucked) };
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::IdleTuckBadgeVisibility() const noexcept
    {
        return (_idleTucked > 0 && _idleTuckShowsBadge()) ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed;
    }

    double SessionNodeViewModel::IdleTuckBadgeFontSize() const noexcept
    {
        // The state-line size scaled down, in half-point steps (11 -> 8.5, 9 -> 7).
        return std::max(6.0, std::round(_stateLineFontSize * 0.75 * 2.0) / 2.0);
    }

    double SessionNodeViewModel::IdleTuckDotSize() const noexcept
    {
        // Plain dot: 7px. With a badge the circle grows to hold the digit ("9+" overflows it
        // slightly, which reads fine at this size).
        return _idleTuckShowsBadge() ? IdleTuckBadgeFontSize() + 5.0 : 7.0;
    }

    winrt::Windows::UI::Xaml::Visibility SessionNodeViewModel::FoldToolTipVisibility() const noexcept
    {
        return _foldToolTip.empty() ? winrt::Windows::UI::Xaml::Visibility::Collapsed : winrt::Windows::UI::Xaml::Visibility::Visible;
    }

    // The section header's own tooltip: null (no tooltip at all) when nothing is hidden.
    winrt::Windows::Foundation::IInspectable SessionNodeViewModel::HeaderToolTip() const
    {
        return _foldToolTip.empty() ? nullptr : winrt::box_value(_foldToolTip);
    }

    bool SessionNodeViewModel::SetFold(uint32_t idleTucked, const hstring& toolTip)
    {
        bool changed = false;
        if (idleTucked != _idleTucked)
        {
            const bool visibilityChanged = (idleTucked == 0) != (_idleTucked == 0);
            _idleTucked = idleTucked;
            PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IdleTuckBadge" });
            if (visibilityChanged)
            {
                PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IdleTuckVisibility" });
                PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"IdleTuckBadgeVisibility" });
            }
            changed = true;
        }
        if (toolTip != _foldToolTip)
        {
            _foldToolTip = toolTip;
            for (const auto name : { L"FoldToolTip", L"FoldToolTipVisibility", L"HeaderToolTip" })
            {
                PropertyChanged.raise(*this, winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
            }
            changed = true;
        }
        return changed;
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
