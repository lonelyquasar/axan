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

        // Apply a BlockMove to the ids, the way the view-model applies it to its vector: the
        // block rows in the order listed, the rest in list order.
        static std::vector<std::wstring> Apply(const T::Nodes& nodes, const T::BlockMove& move)
        {
            std::vector<std::wstring> block, rest;
            std::vector<bool> inBlock(nodes.size(), false);
            for (const auto i : move.block)
            {
                block.push_back(nodes[i].id);
                inBlock[i] = true;
            }
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                if (!inBlock[i])
                {
                    rest.push_back(nodes[i].id);
                }
            }
            rest.insert(rest.begin() + move.insertAt, block.begin(), block.end());
            return rest;
        }

        // Apply a drag fix-up (the head's new parent, then the block move) and return the
        // resulting nodes, so a test can check order, parents and depths together.
        static T::Nodes ApplyDrag(const T::Nodes& after, size_t headIndex, const T::DragFixup& fix)
        {
            auto withParent = after;
            withParent[headIndex].parentId = fix.headParentId;
            T::Nodes out;
            for (const auto& id : Apply(withParent, fix.move))
            {
                for (const auto& n : withParent)
                {
                    if (n.id == id)
                    {
                        out.push_back(n);
                        break;
                    }
                }
            }
            return out;
        }

        static std::wstring JoinNodes(const T::Nodes& nodes)
        {
            std::vector<std::wstring> ids;
            for (const auto& n : nodes)
            {
                ids.push_back(n.id);
            }
            return Join(ids);
        }

        static std::wstring ParentOf(const T::Nodes& nodes, const wchar_t* id)
        {
            for (const auto& n : nodes)
            {
                if (n.id == id)
                {
                    return n.parentId;
                }
            }
            return L"<absent>";
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

        // ---- drag reorder fix-up (NormalizeAfterDrag) ----
        // Each case builds the list as the ListView's built-in reorder leaves it: only the
        // head has moved, its ParentId and its children untouched.

        TEST_METHOD(DragAmongSiblings)
        {
            // Before: p, a, b, c (all children of p). Drag c up between a and b.
            const T::Nodes after{ S(L"p"), S(L"a", L"p"), S(L"c", L"p"), S(L"b", L"p") };
            const auto fix = T::NormalizeAfterDrag(after, 2, 1);
            VERIFY_IS_TRUE(fix.has_value());
            const auto out = ApplyDrag(after, 2, *fix);
            VERIFY_ARE_EQUAL(std::wstring{ L"p,a,c,b" }, JoinNodes(out));
            Log::Comment(L"Still a child of p");
            VERIFY_ARE_EQUAL(std::wstring{ L"p" }, ParentOf(out, L"c"));
            VERIFY_IS_TRUE(T::IsWellFormed(out));
        }

        TEST_METHOD(DragIntoAnotherSubtree)
        {
            // Before: p, p1, p2 (children of p), r (root) with child r1. Drag r up between p1
            // and p2: it lands inside p's family and becomes p's child; r1 follows it.
            const T::Nodes after{ S(L"p"), S(L"p1", L"p"), S(L"r"), S(L"p2", L"p"), S(L"r1", L"r") };
            const auto fix = T::NormalizeAfterDrag(after, 2, 0);
            VERIFY_IS_TRUE(fix.has_value());
            const auto out = ApplyDrag(after, 2, *fix);
            VERIFY_ARE_EQUAL(std::wstring{ L"p,p1,r,r1,p2" }, JoinNodes(out));
            VERIFY_ARE_EQUAL(std::wstring{ L"p" }, ParentOf(out, L"r"));
            VERIFY_ARE_EQUAL(std::wstring{ L"r" }, ParentOf(out, L"r1"));
            const auto d = T::Depths(out);
            VERIFY_ARE_EQUAL(1, d[2]);
            VERIFY_ARE_EQUAL(2, d[3]);
            VERIFY_IS_TRUE(T::IsWellFormed(out));

            Log::Comment(L"Dropped between a parent and its first child: a child of that parent");
            // Before: p, p1 (child of p), r. Drag r between p and p1.
            const T::Nodes first{ S(L"p"), S(L"r"), S(L"p1", L"p") };
            const auto fix2 = T::NormalizeAfterDrag(first, 1, 0);
            VERIFY_IS_TRUE(fix2.has_value());
            const auto out2 = ApplyDrag(first, 1, *fix2);
            VERIFY_ARE_EQUAL(std::wstring{ L"p,r,p1" }, JoinNodes(out2));
            VERIFY_ARE_EQUAL(std::wstring{ L"p" }, ParentOf(out2, L"r"));
        }

        TEST_METHOD(DragToRootEnd)
        {
            // Before: a with child a1, p with child p1. Drag root a to the very end: it stays
            // a root (nothing below constrains it) and a1 follows it.
            const T::Nodes after{ S(L"a1", L"a"), S(L"p"), S(L"p1", L"p"), S(L"a") };
            const auto fix = T::NormalizeAfterDrag(after, 3, 0);
            VERIFY_IS_TRUE(fix.has_value());
            const auto out = ApplyDrag(after, 3, *fix);
            VERIFY_ARE_EQUAL(std::wstring{ L"p,p1,a,a1" }, JoinNodes(out));
            VERIFY_ARE_EQUAL(std::wstring{}, ParentOf(out, L"a"));
            VERIFY_IS_TRUE(T::IsWellFormed(out));

            Log::Comment(L"Dropped at the top: always a root");
            // Before: p, c (child of p), q. Drag c to the top.
            const T::Nodes top{ S(L"c", L"p"), S(L"p"), S(L"q") };
            const auto fix2 = T::NormalizeAfterDrag(top, 0, 1);
            VERIFY_IS_TRUE(fix2.has_value());
            const auto out2 = ApplyDrag(top, 0, *fix2);
            VERIFY_ARE_EQUAL(std::wstring{ L"c,p,q" }, JoinNodes(out2));
            VERIFY_ARE_EQUAL(std::wstring{}, ParentOf(out2, L"c"));
        }

        TEST_METHOD(DragHeadWithNonContiguousChildren)
        {
            // Before: h, x (root, left there by an outdent), h1 (child of h), h2 (child of
            // h1), y. Drag h to the end, below its own children: the whole family follows it
            // in its original order, and x stays behind.
            const T::Nodes after{ S(L"x"), S(L"h1", L"h"), S(L"h2", L"h1"), S(L"y"), S(L"h") };
            const auto fix = T::NormalizeAfterDrag(after, 4, 0);
            VERIFY_IS_TRUE(fix.has_value());
            const auto out = ApplyDrag(after, 4, *fix);
            VERIFY_ARE_EQUAL(std::wstring{ L"x,y,h,h1,h2" }, JoinNodes(out));
            VERIFY_ARE_EQUAL(std::wstring{}, ParentOf(out, L"h"));
            VERIFY_ARE_EQUAL(std::wstring{ L"h" }, ParentOf(out, L"h1"));
            VERIFY_ARE_EQUAL(std::wstring{ L"h1" }, ParentOf(out, L"h2"));
            const auto d = T::Depths(out);
            VERIFY_ARE_EQUAL(0, d[2]);
            VERIFY_ARE_EQUAL(1, d[3]);
            VERIFY_ARE_EQUAL(2, d[4]);
            VERIFY_IS_TRUE(T::IsWellFormed(out));

            Log::Comment(L"Dropped among its own children: the family closes up below the row above it");
            // Before: a, h, h1, h2 (h1, h2 children of h). Drag h between h1 and h2.
            const T::Nodes among{ S(L"a"), S(L"h1", L"h"), S(L"h"), S(L"h2", L"h") };
            const auto fix2 = T::NormalizeAfterDrag(among, 2, 0);
            VERIFY_IS_TRUE(fix2.has_value());
            const auto out2 = ApplyDrag(among, 2, *fix2);
            VERIFY_ARE_EQUAL(std::wstring{ L"a,h,h1,h2" }, JoinNodes(out2));
            VERIFY_ARE_EQUAL(std::wstring{}, ParentOf(out2, L"h"));
        }

        TEST_METHOD(DragOntoSeparator)
        {
            // The reorder inserts between rows, so "onto" a separator means directly below
            // it. Before: p, c (child of p), s (root separator), q. Drag c below s: at its old
            // depth it would become s's child, but a separator is never a parent (axan #28),
            // so it goes after s at s's depth.
            const T::Nodes after{ S(L"p"), Sep(L"s"), S(L"c", L"p"), S(L"q") };
            const auto fix = T::NormalizeAfterDrag(after, 2, 1);
            VERIFY_IS_TRUE(fix.has_value());
            const auto out = ApplyDrag(after, 2, *fix);
            VERIFY_ARE_EQUAL(std::wstring{ L"p,s,c,q" }, JoinNodes(out));
            VERIFY_ARE_EQUAL(std::wstring{}, ParentOf(out, L"c"));
            VERIFY_IS_TRUE(T::IsWellFormed(out));

            Log::Comment(L"A nested separator: the head becomes its sibling under the same parent");
            // Before: p, s (separator child of p), x, x1 (child of x), c (child of x1, depth
            // 2). Drag c directly below s.
            const T::Nodes nested{ S(L"p"), Sep(L"s", L"p"), S(L"c", L"x1"), S(L"x"), S(L"x1", L"x") };
            const auto fix2 = T::NormalizeAfterDrag(nested, 2, 2);
            VERIFY_IS_TRUE(fix2.has_value());
            const auto out2 = ApplyDrag(nested, 2, *fix2);
            VERIFY_ARE_EQUAL(std::wstring{ L"p,s,c,x,x1" }, JoinNodes(out2));
            VERIFY_ARE_EQUAL(std::wstring{ L"p" }, ParentOf(out2, L"c"));
            VERIFY_IS_TRUE(T::IsWellFormed(out2));
        }

        TEST_METHOD(IsWellFormedChecksForwardReferencesAndSeparatorParents)
        {
            VERIFY_IS_TRUE(T::IsWellFormed(T::Nodes{ S(L"a"), S(L"b", L"a") }));
            VERIFY_IS_FALSE(T::IsWellFormed(T::Nodes{ S(L"b", L"a"), S(L"a") }));
            VERIFY_IS_FALSE(T::IsWellFormed(T::Nodes{ Sep(L"s"), S(L"b", L"s") }));
            VERIFY_IS_FALSE(T::IsWellFormed(T::Nodes{ S(L"b", L"missing") }));
        }
    };
}
