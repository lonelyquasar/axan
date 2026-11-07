// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

// axan #34: which sessions the sidebar's hoisted group lists ("Needs you" / "Hoisted").
//
// Global setting (settings.json):
//
//   "sidebar.hoistStates": [ "blocked", "error", "done" ]      // the default
//
// Per startup entry (settings.json / portable TOML):
//
//   "hoist": "never"                 // never hoist this session
//   "hoist": [ "running", "blocked" ] // its own list instead of the global one
//   (absent)                         // follow the global list
//
// Names are the slot-state vocabulary of AxanStateGlyphs.h: idle, working, blocked, done,
// error, running, plus the blocked kinds permission, question, auth. A node is matched on
// its slot state (TerminalApp/AxanSessionState.h SlotKey) and blocked kind:
//   * `blocked` matches a blocked node of any kind;
//   * a kind name matches only a blocked node of that kind;
//   * `done` is an unseen done report (a seen one is the idle slot);
//   * `idle` is an agent at rest (a plain shell at its prompt has no slot state and never
//     matches anything).
// Unknown names are ignored (the caller logs them once).
//
// In memory a list is a bit mask over those nine names. A per-entry value travels between
// the layers as one canonical string: "" = follow the global list, "never", or the names
// comma-joined in vocabulary order ("blocked,running"). A custom list with no known names
// is equivalent to "never" and is written as such.
//
// Header-only in the shared src/inc and pure (no WinRT), so TerminalApp, the Settings Model,
// the settings editor and UnitTests_TerminalCore share one definition.

#include <AxanNameMask.h> // the name list <-> bit mask parsing, shared with AxanIdleEffects.h

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Axan::HoistRule
{
    using Mask = uint16_t;

    // Every name, in the order the settings UI lists them (the AxanStateGlyphs.h keys, then
    // the blocked kinds).
    inline constexpr std::array<std::wstring_view, 9> kNames{ L"idle", L"working", L"blocked", L"done", L"error", L"running", L"permission", L"question", L"auth" };
    inline constexpr std::wstring_view kNever{ L"never" };

    // The bit for a name, 0 for an unknown one.
    constexpr Mask Bit(std::wstring_view name) noexcept
    {
        return NameMask::Bit<Mask>(kNames, name);
    }

    inline constexpr Mask kAll = NameMask::All<Mask>(kNames);
    // ["blocked", "error", "done"]: the attention states (the group's original behaviour).
    inline constexpr Mask kDefault = static_cast<Mask>(Bit(L"blocked") | Bit(L"error") | Bit(L"done"));
    // The names that keep the "Needs you" header: blocked and its kinds, error, done.
    inline constexpr Mask kAttention = static_cast<Mask>(Bit(L"blocked") | Bit(L"permission") | Bit(L"question") | Bit(L"auth") | Bit(L"error") | Bit(L"done"));

    constexpr bool Has(Mask m, std::wstring_view name) noexcept
    {
        const auto b = Bit(name);
        return b != 0 && (m & b) != 0;
    }

    using NameMask::Trim;

    // A list of names -> mask. Names are trimmed; unknown ones are appended to `unknown`
    // (when given) and otherwise ignored.
    template<typename Range>
    Mask MaskFromNames(const Range& names, std::vector<std::wstring>* unknown = nullptr)
    {
        return NameMask::MaskFromNames<Mask>(kNames, names, unknown);
    }

    // mask -> names, in vocabulary order.
    inline std::vector<std::wstring_view> NamesFromMask(Mask m)
    {
        return NameMask::NamesFromMask(kNames, m);
    }

    // ---- the per-entry value ----
    enum class Mode : uint8_t
    {
        Default = 0, // follow the global list
        Never,
        Custom,
    };

    struct Override
    {
        Mode mode{ Mode::Default };
        Mask mask{ 0 }; // Custom only

        friend constexpr bool operator==(const Override& a, const Override& b) noexcept
        {
            return a.mode == b.mode && (a.mode != Mode::Custom || a.mask == b.mask);
        }
    };

    // A custom list -> its Override (an empty one is "never").
    constexpr Override FromMask(Mask m) noexcept
    {
        return m == 0 ? Override{ Mode::Never, 0 } : Override{ Mode::Custom, m };
    }

    // Canonical string -> Override. "" (or "default") = Default, "never" = Never, else a
    // comma-separated name list.
    inline Override Parse(std::wstring_view canonical, std::vector<std::wstring>* unknown = nullptr)
    {
        const auto s = Trim(canonical);
        if (s.empty() || s == L"default")
        {
            return {};
        }
        if (s == kNever)
        {
            return { Mode::Never, 0 };
        }
        std::vector<std::wstring_view> parts;
        size_t start = 0;
        while (start <= s.size())
        {
            const auto comma = s.find(L',', start);
            const auto end = comma == std::wstring_view::npos ? s.size() : comma;
            parts.push_back(s.substr(start, end - start));
            if (comma == std::wstring_view::npos)
            {
                break;
            }
            start = comma + 1;
        }
        return FromMask(MaskFromNames(parts, unknown));
    }

    // Override -> canonical string.
    inline std::wstring Format(const Override& o)
    {
        switch (o.mode)
        {
        case Mode::Never:
            return std::wstring{ kNever };
        case Mode::Custom:
        {
            if (o.mask == 0)
            {
                return std::wstring{ kNever };
            }
            std::wstring out;
            for (const auto n : NamesFromMask(o.mask))
            {
                if (!out.empty())
                {
                    out += L',';
                }
                out.append(n);
            }
            return out;
        }
        case Mode::Default:
        default:
            return {};
        }
    }

    // Normalize a canonical string (drops unknown names, fixes order and spacing).
    inline std::wstring Normalize(std::wstring_view canonical)
    {
        return Format(Parse(canonical));
    }

    // ---- matching ----

    // Does a list match a node? slotKey is AxanSessionState.h SlotKey ("" = no slot state),
    // kind the blocked kind ("permission" / "question" / "auth" / "").
    constexpr bool Matches(Mask m, std::wstring_view slotKey, std::wstring_view kind) noexcept
    {
        if (slotKey.empty())
        {
            return false;
        }
        if (slotKey == L"blocked")
        {
            if (Has(m, L"blocked"))
            {
                return true;
            }
            return (kind == L"permission" || kind == L"question" || kind == L"auth") && Has(m, kind);
        }
        if (slotKey == L"permission" || slotKey == L"question" || slotKey == L"auth")
        {
            return false; // kinds are not slot states
        }
        return Has(m, slotKey);
    }

    // The effective list for an entry: its own (Custom), none (Never), or the global one.
    constexpr Mask Effective(const Override& entry, Mask global) noexcept
    {
        switch (entry.mode)
        {
        case Mode::Never:
            return 0;
        case Mode::Custom:
            return entry.mask;
        case Mode::Default:
        default:
            return global;
        }
    }

    constexpr bool IsHoisted(std::wstring_view slotKey, std::wstring_view kind, const Override& entry, Mask global) noexcept
    {
        return Matches(Effective(entry, global), slotKey, kind);
    }

    // The group header: "Needs you" while the global list holds attention states only.
    constexpr bool IsAttentionOnly(Mask global) noexcept
    {
        return (global & ~kAttention) == 0;
    }

    // ---- UI vocabulary (the three pickers: Settings > Sidebar, the Startup sessions row,
    // the sidebar's node editor) ----

    // The order the pickers list the names in.
    inline constexpr std::array<std::wstring_view, 9> kDisplayOrder{ L"idle", L"working", L"blocked", L"permission", L"question", L"auth", L"done", L"error", L"running" };

    // The checkbox label for a name: the words the row's state slot uses.
    constexpr std::wstring_view DisplayName(std::wstring_view name) noexcept
    {
        if (name == L"blocked")
        {
            return L"needs you (any)";
        }
        if (name == L"permission")
        {
            return L"needs permission";
        }
        if (name == L"question")
        {
            return L"needs answer";
        }
        if (name == L"auth")
        {
            return L"needs login";
        }
        return name; // idle / working / done / error / running
    }

    // "needs you (any), done, error" for a tooltip / summary; "nothing" for an empty list.
    inline std::wstring Describe(Mask m)
    {
        std::wstring out;
        for (const auto n : kDisplayOrder)
        {
            if (Has(m, n))
            {
                if (!out.empty())
                {
                    out += L", ";
                }
                out.append(DisplayName(n));
            }
        }
        return out.empty() ? std::wstring{ L"nothing" } : out;
    }

    // A list toggled by one name (the pickers' checkbox handler).
    constexpr Mask With(Mask m, std::wstring_view name, bool on) noexcept
    {
        const auto b = Bit(name);
        return static_cast<Mask>(on ? (m | b) : (m & ~b));
    }
}
