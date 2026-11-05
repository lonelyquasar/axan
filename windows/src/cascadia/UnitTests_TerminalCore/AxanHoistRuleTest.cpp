// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan #34: the hoisted group's match rule (src/inc/AxanHoistRule.h): sidebar.hoistStates,
// the per-entry "hoist" override, and the canonical string form the layers pass around.

#include "pch.h"
#include <WexTestClass.h>

#include <AxanHoistRule.h>

#include <string>
#include <vector>

using namespace WEX::Logging;
namespace H = Axan::HoistRule;

namespace TerminalCoreUnitTests
{
    class AxanHoistRuleTest
    {
        TEST_CLASS(AxanHoistRuleTest);

        static H::Mask M(std::initializer_list<std::wstring_view> names)
        {
            return H::MaskFromNames(std::vector<std::wstring_view>{ names });
        }

        TEST_METHOD(DefaultIsTheAttentionTrio)
        {
            VERIFY_ARE_EQUAL(M({ L"blocked", L"error", L"done" }), H::kDefault);
            VERIFY_IS_TRUE(H::Matches(H::kDefault, L"blocked", L"question"));
            VERIFY_IS_TRUE(H::Matches(H::kDefault, L"error", L""));
            VERIFY_IS_TRUE(H::Matches(H::kDefault, L"done", L""));
            VERIFY_IS_FALSE(H::Matches(H::kDefault, L"working", L""));
            VERIFY_IS_FALSE(H::Matches(H::kDefault, L"running", L""));
            VERIFY_IS_FALSE(H::Matches(H::kDefault, L"idle", L""));
            Log::Comment(L"No slot state (a plain shell at its prompt) never matches");
            VERIFY_IS_FALSE(H::Matches(H::kAll, L"", L""));
        }

        TEST_METHOD(BlockedMatchesAnyKind)
        {
            const auto m = M({ L"blocked" });
            VERIFY_IS_TRUE(H::Matches(m, L"blocked", L"permission"));
            VERIFY_IS_TRUE(H::Matches(m, L"blocked", L"question"));
            VERIFY_IS_TRUE(H::Matches(m, L"blocked", L"auth"));
            VERIFY_IS_TRUE(H::Matches(m, L"blocked", L""));
        }

        TEST_METHOD(KindMatchesOnlyThatKind)
        {
            const auto m = M({ L"question" });
            VERIFY_IS_TRUE(H::Matches(m, L"blocked", L"question"));
            VERIFY_IS_FALSE(H::Matches(m, L"blocked", L"permission"));
            VERIFY_IS_FALSE(H::Matches(m, L"blocked", L"auth"));
            Log::Comment(L"A kind name does not match a blocked node with no kind");
            VERIFY_IS_FALSE(H::Matches(m, L"blocked", L""));
            Log::Comment(L"A kind is not a slot state of its own");
            VERIFY_IS_FALSE(H::Matches(m, L"question", L""));
            Log::Comment(L"blocked plus a kind: blocked wins for every kind");
            VERIFY_IS_TRUE(H::Matches(M({ L"blocked", L"auth" }), L"blocked", L"permission"));
        }

        TEST_METHOD(KindOnBlockedOnly)
        {
            Log::Comment(L"A stray kind on a non-blocked node is ignored");
            VERIFY_IS_FALSE(H::Matches(M({ L"permission" }), L"working", L"permission"));
            VERIFY_IS_TRUE(H::Matches(M({ L"working" }), L"working", L"permission"));
        }

        TEST_METHOD(UnknownNamesIgnored)
        {
            std::vector<std::wstring> unknown;
            const auto m = H::MaskFromNames(std::vector<std::wstring>{ L"running", L"bogus", L" error ", L"" }, &unknown);
            VERIFY_ARE_EQUAL(M({ L"running", L"error" }), m);
            VERIFY_ARE_EQUAL(size_t{ 1 }, unknown.size());
            VERIFY_ARE_EQUAL(std::wstring{ L"bogus" }, unknown[0]);
        }

        TEST_METHOD(EntryOverridesGlobal)
        {
            const auto global = H::kDefault;
            Log::Comment(L"Default follows the global list");
            VERIFY_IS_TRUE(H::IsHoisted(L"error", L"", H::Parse(L""), global));
            VERIFY_IS_FALSE(H::IsHoisted(L"running", L"", H::Parse(L""), global));
            Log::Comment(L"A custom list replaces it (not a union)");
            const auto custom = H::Parse(L"running");
            VERIFY_IS_TRUE(H::IsHoisted(L"running", L"", custom, global));
            VERIFY_IS_FALSE(H::IsHoisted(L"error", L"", custom, global));
            Log::Comment(L"never hoists nothing, whatever the global list");
            const auto never = H::Parse(L"never");
            VERIFY_IS_FALSE(H::IsHoisted(L"blocked", L"permission", never, H::kAll));
            VERIFY_IS_FALSE(H::IsHoisted(L"error", L"", never, global));
            Log::Comment(L"An empty global list still lets a custom entry hoist");
            VERIFY_IS_TRUE(H::IsHoisted(L"running", L"", custom, 0));
            VERIFY_IS_FALSE(H::IsHoisted(L"error", L"", H::Parse(L""), 0));
        }

        TEST_METHOD(CanonicalForm)
        {
            VERIFY_IS_TRUE(H::Parse(L"").mode == H::Mode::Default);
            VERIFY_IS_TRUE(H::Parse(L"default").mode == H::Mode::Default);
            VERIFY_IS_TRUE(H::Parse(L" never ").mode == H::Mode::Never);
            Log::Comment(L"Names come out deduplicated, in vocabulary order, without spaces");
            VERIFY_ARE_EQUAL(std::wstring{ L"blocked,running,question" }, H::Normalize(L"question, running,blocked,running"));
            Log::Comment(L"A list with no known names is never");
            VERIFY_ARE_EQUAL(std::wstring{ L"never" }, H::Normalize(L"bogus"));
            VERIFY_ARE_EQUAL(std::wstring{ L"never" }, H::Format(H::FromMask(0)));
            VERIFY_ARE_EQUAL(std::wstring{}, H::Format(H::Override{}));
            const auto o = H::Parse(L"done,error");
            VERIFY_IS_TRUE(o == H::Parse(H::Format(o)));
        }

        TEST_METHOD(HeaderWording)
        {
            VERIFY_IS_TRUE(H::IsAttentionOnly(H::kDefault));
            VERIFY_IS_TRUE(H::IsAttentionOnly(M({ L"permission", L"question", L"auth", L"error" })));
            VERIFY_IS_TRUE(H::IsAttentionOnly(0));
            VERIFY_IS_FALSE(H::IsAttentionOnly(M({ L"blocked", L"running" })));
            VERIFY_IS_FALSE(H::IsAttentionOnly(M({ L"idle" })));
            VERIFY_IS_FALSE(H::IsAttentionOnly(M({ L"working" })));
        }

        TEST_METHOD(WithToggles)
        {
            auto m = H::With(H::kDefault, L"running", true);
            VERIFY_IS_TRUE(H::Has(m, L"running"));
            m = H::With(m, L"blocked", false);
            VERIFY_IS_FALSE(H::Has(m, L"blocked"));
            VERIFY_ARE_EQUAL(M({ L"error", L"done", L"running" }), m);
            VERIFY_ARE_EQUAL(m, H::With(m, L"bogus", true));
        }
    };
}
