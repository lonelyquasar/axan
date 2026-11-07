// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// axan #38: Settings > Sessions > Sidebar (see SidebarViewModel.idl). The members moved
// here verbatim from GlobalAppearanceViewModel.

#pragma once

#include "SidebarViewModel.g.h"
#include "ViewModelHelpers.h"
#include "Utils.h"

#include <AxanSidebarSizes.h> // axan: row size ranges and defaults

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct SidebarViewModel : SidebarViewModelT<SidebarViewModel>, ViewModelHelper<SidebarViewModel>
    {
    public:
        SidebarViewModel(Model::GlobalAppSettings globalSettings);

        // DON'T ADD A `WINRT_CALLBACK(PropertyChanged` TO A CLASS DERIVED FROM ViewModelHelper.
        using ViewModelHelper<SidebarViewModel>::PropertyChanged;

        GETSET_BINDABLE_ENUM_SETTING(SidebarStatusAfterSeen, Model::SidebarStatusAfterSeen, _GlobalSettings.SidebarStatusAfterSeen);

    public:
        // sidebar.stateGlyphs (see the idl)
        winrt::hstring SidebarStateGlyphText(const winrt::hstring& key) const;
        void SetSidebarStateGlyph(const winrt::hstring& key, const winrt::hstring& nativeIcon);
        void ResetSidebarStateGlyph(const winrt::hstring& key);
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarIdleGlyphPreview() const { return _StateGlyphPreview(L"idle"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarWorkingGlyphPreview() const { return _StateGlyphPreview(L"working"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarBlockedGlyphPreview() const { return _StateGlyphPreview(L"blocked"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarDoneGlyphPreview() const { return _StateGlyphPreview(L"done"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarErrorGlyphPreview() const { return _StateGlyphPreview(L"error"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarRunningGlyphPreview() const { return _StateGlyphPreview(L"running"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarPermissionGlyphPreview() const { return _StateGlyphPreview(L"permission"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarQuestionGlyphPreview() const { return _StateGlyphPreview(L"question"); }
        winrt::Windows::UI::Xaml::Controls::IconElement SidebarAuthGlyphPreview() const { return _StateGlyphPreview(L"auth"); }
        winrt::hstring SidebarIdleGlyphLabel() const { return _StateGlyphLabel(L"idle"); }
        winrt::hstring SidebarWorkingGlyphLabel() const { return _StateGlyphLabel(L"working"); }
        winrt::hstring SidebarBlockedGlyphLabel() const { return _StateGlyphLabel(L"blocked"); }
        winrt::hstring SidebarDoneGlyphLabel() const { return _StateGlyphLabel(L"done"); }
        winrt::hstring SidebarErrorGlyphLabel() const { return _StateGlyphLabel(L"error"); }
        winrt::hstring SidebarRunningGlyphLabel() const { return _StateGlyphLabel(L"running"); }
        winrt::hstring SidebarPermissionGlyphLabel() const { return _StateGlyphLabel(L"permission"); }
        winrt::hstring SidebarQuestionGlyphLabel() const { return _StateGlyphLabel(L"question"); }
        winrt::hstring SidebarAuthGlyphLabel() const { return _StateGlyphLabel(L"auth"); }

        // axan #34: sidebar.hoistStates (see the idl)
        bool SidebarHoistStateEnabled(const winrt::hstring& name) const;
        void SetSidebarHoistStateEnabled(const winrt::hstring& name, bool enabled);
        void ResetSidebarHoistStates();
        winrt::hstring SidebarHoistStatesSummary() const;

        // axan: sidebar.idleEffects (see the idl)
        bool SidebarIdleEffectEnabled(const winrt::hstring& name) const;
        void SetSidebarIdleEffectEnabled(const winrt::hstring& name, bool enabled);
        void ResetSidebarIdleEffects();
        winrt::hstring SidebarIdleEffectsSummary() const;
        // The localized checkbox label / help for an effect name ("" for an unknown one).
        static winrt::hstring IdleEffectLabel(std::wstring_view name);
        static winrt::hstring IdleEffectHelp(std::wstring_view name);

        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarMatchesProfileTransparency);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarIdleAfterMinutes); // axan #31
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarNeedsYouGroup); // axan #34
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarHoistPreview); // axan #49
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarStateLineMarquee); // axan #49
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarStateLabels);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarShowAgentName);

        // axan: the row sizes. A cleared NumberBox pushes NaN through its TwoWay binding; it
        // reads as the setting's default (and any value is clamped to the box's range), so NaN
        // never reaches settings.json or the sidebar's FontSize bindings. The change is
        // announced even when the stored value stays put, so a cleared box shows it again.
        double SidebarLabelFontSize() const { return _GlobalSettings.SidebarLabelFontSize(); }
        void SidebarLabelFontSize(double value)
        {
            _SetSize(value, Axan::SidebarSizes::kLabelFontSize, L"SidebarLabelFontSize", L"HasSidebarLabelFontSize", _GlobalSettings.SidebarLabelFontSize(), [&](double v) { _GlobalSettings.SidebarLabelFontSize(v); });
        }
        bool HasSidebarLabelFontSize() const { return _GlobalSettings.HasSidebarLabelFontSize(); }
        double SidebarStateLineFontSize() const { return _GlobalSettings.SidebarStateLineFontSize(); }
        void SidebarStateLineFontSize(double value)
        {
            _SetSize(value, Axan::SidebarSizes::kStateLineFontSize, L"SidebarStateLineFontSize", L"HasSidebarStateLineFontSize", _GlobalSettings.SidebarStateLineFontSize(), [&](double v) { _GlobalSettings.SidebarStateLineFontSize(v); });
        }
        bool HasSidebarStateLineFontSize() const { return _GlobalSettings.HasSidebarStateLineFontSize(); }
        double SidebarIconSize() const { return _GlobalSettings.SidebarIconSize(); }
        void SidebarIconSize(double value)
        {
            _SetSize(value, Axan::SidebarSizes::kIconSize, L"SidebarIconSize", L"HasSidebarIconSize", _GlobalSettings.SidebarIconSize(), [&](double v) { _GlobalSettings.SidebarIconSize(v); });
        }
        bool HasSidebarIconSize() const { return _GlobalSettings.HasSidebarIconSize(); }
        double SidebarStateGlyphSize() const { return _GlobalSettings.SidebarStateGlyphSize(); }
        void SidebarStateGlyphSize(double value)
        {
            _SetSize(value, Axan::SidebarSizes::kStateGlyphSize, L"SidebarStateGlyphSize", L"HasSidebarStateGlyphSize", _GlobalSettings.SidebarStateGlyphSize(), [&](double v) { _GlobalSettings.SidebarStateGlyphSize(v); });
        }
        bool HasSidebarStateGlyphSize() const { return _GlobalSettings.HasSidebarStateGlyphSize(); }
        // axan #47: the sizes the "smaller" idle effect gives a row.
        double SidebarIdleLabelFontSize() const { return _GlobalSettings.SidebarIdleLabelFontSize(); }
        void SidebarIdleLabelFontSize(double value)
        {
            _SetSize(value, Axan::SidebarSizes::kIdleLabelFontSize, L"SidebarIdleLabelFontSize", L"HasSidebarIdleLabelFontSize", _GlobalSettings.SidebarIdleLabelFontSize(), [&](double v) { _GlobalSettings.SidebarIdleLabelFontSize(v); });
        }
        bool HasSidebarIdleLabelFontSize() const { return _GlobalSettings.HasSidebarIdleLabelFontSize(); }
        double SidebarIdleRowHeight() const { return _GlobalSettings.SidebarIdleRowHeight(); }
        void SidebarIdleRowHeight(double value)
        {
            _SetSize(value, Axan::SidebarSizes::kIdleRowHeight, L"SidebarIdleRowHeight", L"HasSidebarIdleRowHeight", _GlobalSettings.SidebarIdleRowHeight(), [&](double v) { _GlobalSettings.SidebarIdleRowHeight(v); });
        }
        bool HasSidebarIdleRowHeight() const { return _GlobalSettings.HasSidebarIdleRowHeight(); }

        // axan: sidebar.iconFontScale (see the idl)
        winrt::Windows::Foundation::Collections::IVectorView<winrt::hstring> SidebarIconFontFamilies() const;
        double SidebarIconFontScale(const winrt::hstring& family) const;
        double SidebarIconFontScaleDefault(const winrt::hstring& family) const;
        bool IsSidebarIconFontScaleSet(const winrt::hstring& family) const;
        void SetSidebarIconFontScale(const winrt::hstring& family, double value);
        void ResetSidebarIconFontScale(const winrt::hstring& family);
        // axan #48: sidebar.iconFontWeight and sidebar.largeIconsGrowRow (see the idl)
        winrt::hstring SidebarIconFontWeight(const winrt::hstring& family) const;
        void SetSidebarIconFontWeight(const winrt::hstring& family, const winrt::hstring& weight);
        bool IsSidebarIconFontSet(const winrt::hstring& family) const;
        void ResetSidebarIconFont(const winrt::hstring& family);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, SidebarLargeIconsGrowRow);
        // Not projected: icon strings in use outside the globals (the live sidebar's icon
        // overrides, every profile's icon), whose families the list also shows. MainPage sets
        // it before navigating.
        void ExtraIconSources(std::vector<winrt::hstring> icons) { _extraIconSources = std::move(icons); }

    private:
        Model::GlobalAppSettings _GlobalSettings;
        template<typename Store>
        void _SetSize(double value, const Axan::SidebarSizes::SizeRange& range, std::wstring_view name, std::wstring_view hasName, double current, Store&& store)
        {
            const auto sanitized = Axan::SidebarSizes::SanitizeSize(value, range);
            if (current != sanitized)
            {
                store(sanitized);
                _NotifyChanges(hasName, name);
            }
            else if (sanitized != value)
            {
                _NotifyChanges(name);
            }
        }
        std::vector<winrt::hstring> _extraIconSources;
        void _StoreIconFontScales(const winrt::Windows::Foundation::Collections::IMap<winrt::hstring, double>& map);
        void _StoreIconFontWeights(const winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::hstring>& map); // axan #48

        // sidebar.stateGlyphs helpers. nullopt = the key is absent (default glyph).
        std::optional<std::wstring> _StateGlyphConfigured(std::wstring_view key) const;
        winrt::Windows::UI::Xaml::Controls::IconElement _StateGlyphPreview(std::wstring_view key) const;
        winrt::hstring _StateGlyphLabel(std::wstring_view key) const;
        void _StoreStateGlyphs(const winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::hstring>& map);
    };
};

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(SidebarViewModel);
}
