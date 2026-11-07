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
//   collapse  a node with children collapses its expander once it AND every descendant
//             are idle; it re-expands when any of them becomes active again (only if the
//             collapse was automatic and the user hasn't expanded it since). The focused
//             session never counts as idle, so a node holding the focused pane never
//             collapses.
//
// Unknown names are ignored (the caller logs them once). Unset = kDefault.
//
// Header-only in the shared src/inc and pure (no WinRT), so TerminalApp, the settings
// editor and UnitTests_TerminalCore share one definition.

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
        for (size_t i = 0; i < kNames.size(); ++i)
        {
            if (kNames[i] == name)
            {
                return static_cast<Mask>(1u << i);
            }
        }
        return 0;
    }

    inline constexpr Mask kDim = Bit(L"dim");
    inline constexpr Mask kSmaller = Bit(L"smaller");
    inline constexpr Mask kGray = Bit(L"gray");
    inline constexpr Mask kCollapse = Bit(L"collapse");
    inline constexpr Mask kAll = static_cast<Mask>((1u << kNames.size()) - 1);
    inline constexpr Mask kDefault = static_cast<Mask>(kDim | kSmaller);

    inline std::wstring_view Trim(std::wstring_view s) noexcept
    {
        constexpr std::wstring_view ws{ L" \t\r\n" };
        const auto first = s.find_first_not_of(ws);
        if (first == std::wstring_view::npos)
        {
            return {};
        }
        return s.substr(first, s.find_last_not_of(ws) - first + 1);
    }

    // A list of names -> mask. Names are trimmed; unknown ones are appended to `unknown`
    // (when given) and otherwise ignored. An empty list is an empty mask (no effects).
    template<typename Range>
    Mask MaskFromNames(const Range& names, std::vector<std::wstring>* unknown = nullptr)
    {
        Mask m = 0;
        for (const auto& raw : names)
        {
            const auto name = Trim(std::wstring_view{ raw });
            if (name.empty())
            {
                continue;
            }
            const auto b = Bit(name);
            if (b == 0)
            {
                if (unknown)
                {
                    unknown->emplace_back(name);
                }
                continue;
            }
            m = static_cast<Mask>(m | b);
        }
        return m;
    }

    // mask -> names, in canonical order.
    inline std::vector<std::wstring_view> NamesFromMask(Mask m)
    {
        std::vector<std::wstring_view> out;
        for (size_t i = 0; i < kNames.size(); ++i)
        {
            if (m & (1u << i))
            {
                out.push_back(kNames[i]);
            }
        }
        return out;
    }
}
