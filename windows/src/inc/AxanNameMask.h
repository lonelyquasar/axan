// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

// axan: a list of names from a fixed vocabulary, held as a bit mask (bit i = vocabulary[i]).
// The parsing behind sidebar.hoistStates (AxanHoistRule.h) and sidebar.idleEffects
// (AxanIdleEffects.h): each of those wraps these templates over its own name array and mask
// type, so the trimming, lookup and unknown-name reporting exist once.
//
// Names match exactly (case-sensitive) after trimming spaces, tabs and line breaks.
//
// Header-only in the shared src/inc and pure (no WinRT), like its users.

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Axan::NameMask
{
    template<size_t N>
    using Names = std::array<std::wstring_view, N>;

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

    // The bit for a name, 0 for an unknown one.
    template<typename Mask, size_t N>
    constexpr Mask Bit(const Names<N>& vocabulary, std::wstring_view name) noexcept
    {
        static_assert(N <= std::numeric_limits<Mask>::digits, "the mask type is too narrow for the vocabulary");
        for (size_t i = 0; i < N; ++i)
        {
            if (vocabulary[i] == name)
            {
                return static_cast<Mask>(1u << i);
            }
        }
        return 0;
    }

    // Every name's bit.
    template<typename Mask, size_t N>
    constexpr Mask All(const Names<N>& /*vocabulary*/) noexcept
    {
        static_assert(N <= std::numeric_limits<Mask>::digits, "the mask type is too narrow for the vocabulary");
        return static_cast<Mask>((1u << N) - 1);
    }

    // A list of names -> mask. Names are trimmed; empty ones are skipped; unknown ones are
    // appended to `unknown` (when given, trimmed) and otherwise ignored. An empty list is an
    // empty mask.
    template<typename Mask, size_t N, typename Range>
    Mask MaskFromNames(const Names<N>& vocabulary, const Range& names, std::vector<std::wstring>* unknown = nullptr)
    {
        Mask m = 0;
        for (const auto& raw : names)
        {
            const auto name = Trim(std::wstring_view{ raw });
            if (name.empty())
            {
                continue;
            }
            const auto b = Bit<Mask>(vocabulary, name);
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

    // mask -> names, in vocabulary order.
    template<typename Mask, size_t N>
    std::vector<std::wstring_view> NamesFromMask(const Names<N>& vocabulary, Mask m)
    {
        std::vector<std::wstring_view> out;
        for (size_t i = 0; i < N; ++i)
        {
            if (m & (1u << i))
            {
                out.push_back(vocabulary[i]);
            }
        }
        return out;
    }
}
