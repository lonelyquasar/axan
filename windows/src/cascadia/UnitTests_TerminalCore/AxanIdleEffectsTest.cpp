// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan: the sidebar.idleEffects vocabulary (src/inc/AxanIdleEffects.h): name parsing,
// unknown names, the default and the canonical write-back order.

#include "pch.h"
#include <WexTestClass.h>

#include <AxanIdleEffects.h>

#include <string>
#include <vector>

using namespace WEX::Logging;
namespace E = Axan::IdleEffects;

namespace TerminalCoreUnitTests
{
    class AxanIdleEffectsTest
    {
        TEST_CLASS(AxanIdleEffectsTest);

        TEST_METHOD(DefaultIsDimAndSmaller)
        {
            VERIFY_ARE_EQUAL(E::MaskFromNames(std::vector<std::wstring>{ L"dim", L"smaller" }), E::kDefault);
            VERIFY_IS_TRUE((E::kDefault & E::kGray) == 0);
            VERIFY_IS_TRUE((E::kDefault & E::kCollapse) == 0);
        }

        TEST_METHOD(EmptyListIsNoEffects)
        {
            VERIFY_ARE_EQUAL(E::MaskFromNames(std::vector<std::wstring>{}), static_cast<E::Mask>(0));
        }

        TEST_METHOD(UnknownNamesAreReportedAndIgnored)
        {
            std::vector<std::wstring> unknown;
            const auto m = E::MaskFromNames(std::vector<std::wstring>{ L"gray", L"blink", L" collapse ", L"", L"Dim" }, &unknown);
            VERIFY_ARE_EQUAL(m, static_cast<E::Mask>(E::kGray | E::kCollapse));
            Log::Comment(L"Names are case-sensitive, like the rest of settings.json");
            VERIFY_ARE_EQUAL(unknown.size(), 2u);
            VERIFY_ARE_EQUAL(unknown[0], std::wstring{ L"blink" });
            VERIFY_ARE_EQUAL(unknown[1], std::wstring{ L"Dim" });
        }

        TEST_METHOD(NamesComeBackInCanonicalOrder)
        {
            const auto m = E::MaskFromNames(std::vector<std::wstring>{ L"collapse", L"dim", L"gray", L"smaller" });
            VERIFY_ARE_EQUAL(m, E::kAll);
            const auto names = E::NamesFromMask(m);
            VERIFY_ARE_EQUAL(names.size(), 4u);
            VERIFY_ARE_EQUAL(std::wstring{ names[0] }, std::wstring{ L"dim" });
            VERIFY_ARE_EQUAL(std::wstring{ names[1] }, std::wstring{ L"smaller" });
            VERIFY_ARE_EQUAL(std::wstring{ names[2] }, std::wstring{ L"gray" });
            VERIFY_ARE_EQUAL(std::wstring{ names[3] }, std::wstring{ L"collapse" });
        }
    };
}
