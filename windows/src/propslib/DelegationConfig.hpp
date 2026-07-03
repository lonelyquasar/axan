/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- DelegationConfig.hpp

Abstract:
- This module is used for looking up delegation handlers for the launch of the default console hosting environment

Author(s):
- Michael Niksa (MiNiksa) 31-Aug-2020

--*/

#pragma once

class DelegationConfig
{
public:
    enum class DelegationPairKind : uint32_t
    {
        Undecided,
        Default,
        Conhost,
        Custom,
    };

    struct DelegationPair
    {
        // state contains a "pre parsed" idea of what console/terminal mean.
        // This might help make some code more readable.
        // If either are CLSID_Default, state will be Default.
        // If either are CLSID_Conhost, state will be Conhost.
        // Otherwise it'll be Custom.
        DelegationPairKind kind = DelegationPairKind::Undecided;
        CLSID console{};
        CLSID terminal{};

        constexpr bool IsUndecided() const noexcept { return kind == DelegationPairKind::Undecided; }
        constexpr bool IsDefault() const noexcept { return kind == DelegationPairKind::Default; }
        constexpr bool IsConhost() const noexcept { return kind == DelegationPairKind::Conhost; }
        constexpr bool IsCustom() const noexcept { return kind == DelegationPairKind::Custom; }

        constexpr bool operator==(const DelegationPair& other) const noexcept
        {
            static_assert(std::has_unique_object_representations_v<DelegationPair>);
            return __builtin_memcmp(this, &other, sizeof(*this)) == 0;
        }
    };

    struct PkgVersion
    {
        unsigned short major = 0;
        unsigned short minor = 0;
        unsigned short build = 0;
        unsigned short revision = 0;

        constexpr bool operator==(const PkgVersion& other) const noexcept
        {
            static_assert(std::has_unique_object_representations_v<PkgVersion>);
            return __builtin_memcmp(this, &other, sizeof(*this)) == 0;
        }
    };

    struct PackageInfo
    {
        std::wstring name;
        std::wstring author;
        std::wstring pfn;
        std::wstring logo;
        PkgVersion version;

        bool IsFromSamePackage(const PackageInfo& other) const noexcept
        {
            return name == other.name &&
                   author == other.author &&
                   pfn == other.pfn &&
                   version == other.version;
        }
    };

    struct DelegationBase
    {
        CLSID clsid{};
        PackageInfo info;
    };

    struct DelegationPackage
    {
        DelegationPair pair;
        PackageInfo info;

        bool operator==(const DelegationPackage& other) const noexcept
        {
            return pair == other.pair;
        }
    };

    [[nodiscard]] static HRESULT s_GetAvailablePackages(std::vector<DelegationPackage>& packages, DelegationPackage& def) noexcept;

    [[nodiscard]] static HRESULT s_SetDefaultByPackage(const DelegationPackage& pkg) noexcept;

    static constexpr CLSID CLSID_Default{};
    static constexpr CLSID CLSID_Conhost{ 0xb23d10c0, 0xe52e, 0x411e, { 0x9d, 0x5b, 0xc0, 0x9f, 0xdf, 0x70, 0x9c, 0x7d } };
    static constexpr CLSID CLSID_WindowsTerminalConsole{ 0x34418be8, 0x97da, 0x424a, { 0x89, 0xbd, 0x1f, 0x46, 0xcd, 0x53, 0x57, 0x7b } };
    static constexpr CLSID CLSID_WindowsTerminalTerminal{ 0xf7fea368, 0xe373, 0x4fab, { 0xa3, 0xe2, 0x80, 0x2b, 0x63, 0x41, 0x9c, 0x33 } };
    static constexpr CLSID CLSID_WindowsTerminalConsoleDev{ 0xa85b3275, 0x599b, 0x4182, { 0xb1, 0x1b, 0xa4, 0x26, 0xd1, 0x5f, 0xc0, 0xbe } };
    static constexpr CLSID CLSID_WindowsTerminalTerminalDev{ 0x8031c1c1, 0x1147, 0x47d6, { 0x8b, 0xde, 0x8a, 0x62, 0x8f, 0x29, 0x94, 0x92 } };
    static constexpr DelegationPair DefaultDelegationPair{ DelegationPairKind::Default, CLSID_Default, CLSID_Default };
    static constexpr DelegationPair ConhostDelegationPair{ DelegationPairKind::Conhost, CLSID_Conhost, CLSID_Conhost };
    static constexpr DelegationPair TerminalDelegationPair{ DelegationPairKind::Custom, CLSID_WindowsTerminalConsole, CLSID_WindowsTerminalTerminal };

    [[nodiscard]] static DelegationPair s_GetDelegationPair() noexcept;

private:
    [[nodiscard]] static HRESULT s_SetDefaultConsoleById(const IID& iid) noexcept;
    [[nodiscard]] static HRESULT s_SetDefaultTerminalById(const IID& iid) noexcept;

    [[nodiscard]] static HRESULT s_Set(PCWSTR value, const CLSID clsid) noexcept;
};
