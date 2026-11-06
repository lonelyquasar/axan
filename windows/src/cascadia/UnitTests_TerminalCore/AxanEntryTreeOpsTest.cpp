// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan #38: the startup-sessions tree rules (src/inc/AxanEntryTreeOps.h) the settings
// editor's view-model applies: sibling moves, indent/outdent, duplicate placement and the
// selection after a delete.

#include "pch.h"
#include <WexTestClass.h>

#include <AxanEntryTreeOps.h>

#include <string>
#include <vector>

using namespace WEX::Logging;
namespace T = Axan::EntryTree;

namespace TerminalCoreUnitTests
{
    class AxanEntryTreeOpsTest
    {
        TEST_CLASS(AxanEntryTreeOpsTest);

        static T::Node S(const wchar_t* id, const wchar_t* parent = L"")
        {
            return { id, parent, false };
        }

        static T::Node Sep(const wchar_t* id, const wchar_t* parent = L"")
        {
            return { id, parent, true };
        }

        // Apply a BlockMove to the ids, the way the view-model applies it to its vector.
        static std::vector<std::wstring> Apply(const T::Nodes& nodes, const T::BlockMove& move)
        {
            std::vector<std::wstring> block, rest;
            size_t b = 0;
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                if (b < move.block.size() && move.block[b] == i)
                {
                    block.push_back(nodes[i].id);
                    ++b;
                }
                else
                {
                    rest.push_back(nodes[i].id);
                }
            }
            rest.insert(rest.begin() + move.insertAt, block.begin(), block.end());
            return rest;
        }

        static std::wstring Join(const std::vector<std::wstring>& ids)
        {
            std::wstring out;
            for (const auto& id : ids)
            {
                out += out.empty() ? id : L"," + id;
            }
            return out;
        }

        TEST_METHOD(DepthsFollowParentChain)
        {
            const T::Nodes n{ S(L"a"), S(L"b", L"a"), S(L"c", L"b"), S(L"d"), S(L"e", L"missing") };
            const auto d = T::Depths(n);
            VERIFY_ARE_EQUAL(0, d[0]);
            VERIFY_ARE_EQUAL(1, d[1]);
            VERIFY_ARE_EQUAL(2, d[2]);
            VERIFY_ARE_EQUAL(0, d[3]);
            Log::Comment(L"An unresolved parent reads as a root");
            VERIFY_ARE_EQUAL(0, d[4]);
        }

        TEST_METHOD(DuplicateGoesAfterTheWholeSubtree)
        {
            // a
            //   b
            //     c
            //   d
            // e
            const T::Nodes n{ S(L"a"), S(L"b", L"a"), S(L"c", L"b"), S(L"d", L"a"), S(L"e") };
            Log::Comment(L"Duplicating a puts the copy after d, not between a and its children");
            VERIFY_ARE_EQUAL(4u, T::DuplicateInsertIndex(n, 0));
            Log::Comment(L"Duplicating b puts the copy after c");
            VERIFY_ARE_EQUAL(3u, T::DuplicateInsertIndex(n, 1));
            Log::Comment(L"A leaf's copy goes right after it");
            VERIFY_ARE_EQUAL(3u, T::DuplicateInsertIndex(n, 2));
            VERIFY_ARE_EQUAL(5u, T::DuplicateInsertIndex(n, 4));
        }

        TEST_METHOD(DuplicateHandlesNonContiguousSubtree)
        {
            // An outdent can leave an unrelated row inside a subtree's index span:
            // a, x (root), b (child of a). The copy of a must still follow b.
            const T::Nodes n{ S(L"a"), S(L"x"), S(L"b", L"a"), S(L"y") };
            VERIFY_ARE_EQUAL(3u, T::DuplicateInsertIndex(n, 0));
        }

        TEST_METHOD(SiblingsSkipOtherLevels)
        {
            const T::Nodes n{ S(L"a"), S(L"b", L"a"), S(L"c") };
            VERIFY_IS_FALSE(T::PrevSibling(n, 0).has_value());
            VERIFY_ARE_EQUAL(2u, *T::NextSibling(n, 0));
            VERIFY_ARE_EQUAL(0u, *T::PrevSibling(n, 2));
            VERIFY_IS_FALSE(T::NextSibling(n, 2).has_value());
            Log::Comment(L"An only child has no siblings");
            VERIFY_IS_FALSE(T::PrevSibling(n, 1).has_value());
            VERIFY_IS_FALSE(T::NextSibling(n, 1).has_value());
        }

        TEST_METHOD(MoveCarriesTheSubtree)
        {
            const T::Nodes n{ S(L"a"), S(L"a1", L"a"), S(L"b"), S(L"b1", L"b"), S(L"b2", L"b1"), S(L"c") };
            const auto down = T::MoveAmongSiblings(n, 0, false);
            VERIFY_IS_TRUE(down.has_value());
            VERIFY_ARE_EQUAL(std::wstring{ L"b,b1,b2,a,a1,c" }, Join(Apply(n, *down)));

            const auto up = T::MoveAmongSiblings(n, 2, true);
            VERIFY_IS_TRUE(up.has_value());
            VERIFY_ARE_EQUAL(std::wstring{ L"b,b1,b2,a,a1,c" }, Join(Apply(n, *up)));

            Log::Comment(L"First / last among siblings is a no-op");
            VERIFY_IS_FALSE(T::MoveAmongSiblings(n, 0, true).has_value());
            VERIFY_IS_FALSE(T::MoveAmongSiblings(n, 5, false).has_value());
            VERIFY_IS_FALSE(T::MoveAmongSiblings(n, 1, false).has_value());
        }

        TEST_METHOD(IndentNestsUnderPreviousSessionSibling)
        {
            const T::Nodes n{ S(L"a"), S(L"b") };
            VERIFY_ARE_EQUAL(std::wstring{ L"a" }, *T::IndentParent(n, 1));
            Log::Comment(L"The first row cannot indent");
            VERIFY_IS_FALSE(T::IndentParent(n, 0).has_value());
        }

        TEST_METHOD(IndentSkipsSeparatorsAndStopsAtParent)
        {
            const T::Nodes n{ S(L"a"), Sep(L"s"), S(L"b") };
            Log::Comment(L"A separator sibling is skipped; b nests under a");
            VERIFY_ARE_EQUAL(std::wstring{ L"a" }, *T::IndentParent(n, 2));

            const T::Nodes onlySep{ Sep(L"s"), S(L"b") };
            VERIFY_IS_FALSE(T::IndentParent(onlySep, 1).has_value());

            // p, c1 (child of p), then c2 (child of p) whose only earlier same-depth row is c1
            const T::Nodes nested{ S(L"p"), Sep(L"c1", L"p"), S(L"c2", L"p") };
            Log::Comment(L"The search stops at the parent boundary instead of crossing it");
            VERIFY_IS_FALSE(T::IndentParent(nested, 2).has_value());
        }

        TEST_METHOD(OutdentGoesToGrandparent)
        {
            const T::Nodes n{ S(L"a"), S(L"b", L"a"), S(L"c", L"b"), S(L"d", L"gone") };
            VERIFY_IS_FALSE(T::OutdentParent(n, 0).has_value());
            VERIFY_ARE_EQUAL(std::wstring{}, *T::OutdentParent(n, 1));
            VERIFY_ARE_EQUAL(std::wstring{ L"a" }, *T::OutdentParent(n, 2));
            Log::Comment(L"An unresolved parent outdents to a root");
            VERIFY_ARE_EQUAL(std::wstring{}, *T::OutdentParent(n, 3));
        }

        TEST_METHOD(SelectionAfterDelete)
        {
            // Before: a, b, b1 (child of b), c. Delete b: b1 is promoted to root.
            const T::Nodes promoted{ S(L"a"), S(L"b1"), S(L"c") };
            Log::Comment(L"The promoted child takes the deleted row's place");
            VERIFY_ARE_EQUAL(1u, *T::SelectionAfterDelete(promoted, 1, L""));

            // Before: p, x (child of p), q. Delete x: no next sibling under p.
            const T::Nodes noNext{ S(L"p"), S(L"q") };
            Log::Comment(L"No next sibling: the row above");
            VERIFY_ARE_EQUAL(0u, *T::SelectionAfterDelete(noNext, 1, L"p"));

            // Before: a, b. Delete b.
            const T::Nodes last{ S(L"a") };
            VERIFY_ARE_EQUAL(0u, *T::SelectionAfterDelete(last, 1, L""));

            Log::Comment(L"Deleting the only row selects nothing");
            VERIFY_IS_FALSE(T::SelectionAfterDelete(T::Nodes{}, 0, L"").has_value());
        }
    };
}
