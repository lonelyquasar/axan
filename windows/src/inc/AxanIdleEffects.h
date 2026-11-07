// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

// axan: what happens to an idle session's sidebar row once `sidebar.idleAfterMinutes` has
// passed without input/output (#31). The effects are user-chosen:
//
//   "sidebar.idleEffects": [ "dim", "smaller" ]      // the default (the original #31 look)
//   "sidebar.idleEffects": []                         // no visible effect
//
//   dim       the row renders at reduced opacity
//   smaller   smaller label and a shorter row
//   gray      a node with a colour renders its icon/label in a neutral gray while idle
//             (nodes without a colour are unaffected)
//   collapse  (axan #46) an expanded section header or session row with children tucks
//             away its idle members/children (a row counts as idle when it AND every
//             descendant are idle) while active ones stay visible; a hollow dot with the
//             hidden count marks the expander slot. A tucked row reappears as soon as it
//             is active again. A manual expand holds the node open until it is manually
//             collapsed. The focused session never counts as idle.
//
// Unknown names are ignored (the caller logs them once). Unset = kDefault.
//
// Header-only in the shared src/inc and pure (no WinRT), so TerminalApp, the settings
// editor and UnitTests_TerminalCore share one definition.

#include <AxanNameMask.h> // the name list <-> bit mask parsing, shared with AxanHoistRule.h

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Axan::IdleEffects
{
    using Mask = uint8_t;

    // Canonical order (the settings UI lists them in this order, and a list written back
    // to settings.json uses it).
    inline constexpr std::array<std::wstring_view, 4> kNames{ L"dim", L"smaller", L"gray", L"collapse" };

    constexpr Mask Bit(std::wstring_view name) noexcept
    {
        return NameMask::Bit<Mask>(kNames, name);
    }

    inline constexpr Mask kDim = Bit(L"dim");
    inline constexpr Mask kSmaller = Bit(L"smaller");
    inline constexpr Mask kGray = Bit(L"gray");
    inline constexpr Mask kCollapse = Bit(L"collapse");
    inline constexpr Mask kAll = NameMask::All<Mask>(kNames);
    inline constexpr Mask kDefault = static_cast<Mask>(kDim | kSmaller);

    using NameMask::Trim;

    // A list of names -> mask. Names are trimmed; unknown ones are appended to `unknown`
    // (when given) and otherwise ignored. An empty list is an empty mask (no effects).
    template<typename Range>
    Mask MaskFromNames(const Range& names, std::vector<std::wstring>* unknown = nullptr)
    {
        return NameMask::MaskFromNames<Mask>(kNames, names, unknown);
    }

    // mask -> names, in canonical order.
    inline std::vector<std::wstring_view> NamesFromMask(Mask m)
    {
        return NameMask::NamesFromMask(kNames, m);
    }
}
