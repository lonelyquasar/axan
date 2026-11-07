// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

// axan #38: the structural rules of the startup-sessions tree, shared by the settings
// editor's view-model (StartupSessionsViewModel) and its unit tests.
//
// The tree is a flat, ordered list of rows (launch order = list order, D19). Each row has
// an Id and a ParentId ("" = root); depth is derived from the ParentId chain. The one
// ordering invariant is the forward-reference rule: a parent precedes its children. The
// list is NOT guaranteed family-contiguous (an outdent can leave a row between its former
// siblings), so subtrees are computed as index sets, never as contiguous ranges.
//
// A separator (axan #3) nests and orders like a session but is never a parent: it spawns
// nothing, so nothing can hang under it.
//
// Every function takes a snapshot of the rows (Node: id, parent id, separator flag) and an
// index into it, and answers a question or describes an edit; none mutates anything, so
// the caller applies the edit to its own list (the view-model does it in place on its
// observable vector to avoid re-realizing every row).
//
// Header-only in the shared src/inc and pure (no WinRT), so UnitTests_TerminalCore can
// test it alongside AxanHoistRule.h and AxanLaunchEntryWire.h.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Axan::EntryTree
{
    struct Node
    {
        std::wstring id;
        std::wstring parentId;
        bool separator{ false };
    };

    using Nodes = std::vector<Node>;

    // depth(row) = depth(parent) + 1; a single forward pass suffices because a parent
    // always precedes its children. A ParentId that doesn't resolve to an earlier row
    // reads as a root (depth 0), matching the view-model's _recomputeDepths.
    inline std::vector<int32_t> Depths(const Nodes& nodes)
    {
        std::vector<int32_t> depths(nodes.size(), 0);
        std::unordered_map<std::wstring, int32_t> byId;
        byId.reserve(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            if (!nodes[i].parentId.empty())
            {
                if (const auto it = byId.find(nodes[i].parentId); it != byId.end())
                {
                    depths[i] = it->second + 1;
                }
            }
            byId[nodes[i].id] = depths[i];
        }
        return depths;
    }

    // The transitive subtree of row `root` (the row itself included) as a per-index mask.
    // Forward pass, same reason as Depths.
    inline std::vector<bool> SubtreeMask(const Nodes& nodes, size_t root)
    {
        std::vector<bool> mask(nodes.size(), false);
        if (root >= nodes.size())
        {
            return mask;
        }
        std::unordered_set<std::wstring> ids{ nodes[root].id };
        mask[root] = true;
        for (size_t i = root + 1; i < nodes.size(); ++i)
        {
            if (!nodes[i].parentId.empty() && ids.count(nodes[i].parentId))
            {
                ids.insert(nodes[i].id);
                mask[i] = true;
            }
        }
        return mask;
    }

    // The index of the last row in `root`'s subtree (`root` itself when it has no children).
    inline size_t SubtreeEnd(const Nodes& nodes, size_t root)
    {
        const auto mask = SubtreeMask(nodes, root);
        size_t last = root;
        for (size_t i = root; i < mask.size(); ++i)
        {
            if (mask[i])
            {
                last = i;
            }
        }
        return last;
    }

    // The nearest row before / after `index` with the same parent. None -> the row is
    // already first / last among its siblings.
    inline std::optional<size_t> PrevSibling(const Nodes& nodes, size_t index)
    {
        if (index >= nodes.size())
        {
            return std::nullopt;
        }
        for (size_t i = index; i-- > 0;)
        {
            if (nodes[i].parentId == nodes[index].parentId)
            {
                return i;
            }
        }
        return std::nullopt;
    }

    inline std::optional<size_t> NextSibling(const Nodes& nodes, size_t index)
    {
        if (index >= nodes.size())
        {
            return std::nullopt;
        }
        for (size_t i = index + 1; i < nodes.size(); ++i)
        {
            if (nodes[i].parentId == nodes[index].parentId)
            {
                return i;
            }
        }
        return std::nullopt;
    }

    // Indent nests a row under the nearest preceding SESSION row at its own depth (its
    // previous session sibling). A separator at that depth is skipped (it can't be a
    // parent) and the search keeps walking up, stopping at the parent boundary (a
    // shallower row). Returns the new ParentId, or none when indent is a no-op.
    inline std::optional<std::wstring> IndentParent(const Nodes& nodes, size_t index)
    {
        if (index == 0 || index >= nodes.size())
        {
            return std::nullopt;
        }
        const auto depths = Depths(nodes);
        const auto myDepth = depths[index];
        for (size_t i = index; i-- > 0;)
        {
            if (depths[i] == myDepth)
            {
                if (nodes[i].separator)
                {
                    continue;
                }
                return nodes[i].id;
            }
            if (depths[i] < myDepth)
            {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // Outdent un-nests a row one level: the new ParentId is its parent's ParentId ("" when
    // the parent is a root or doesn't resolve). None when the row is already a root.
    inline std::optional<std::wstring> OutdentParent(const Nodes& nodes, size_t index)
    {
        if (index >= nodes.size() || nodes[index].parentId.empty())
        {
            return std::nullopt;
        }
        for (const auto& n : nodes)
        {
            if (n.id == nodes[index].parentId)
            {
                return n.parentId;
            }
        }
        return std::wstring{};
    }

    // A subtree move: lift the rows at `block` (original indices, reinserted in the order
    // listed; MoveAmongSiblings lists them ascending, NormalizeAfterDrag lists the head
    // first), then insert them as one run at `insertAt`, an index into the list with the
    // block removed. Rows outside the block keep their relative order.
    struct BlockMove
    {
        std::vector<size_t> block;
        size_t insertAt{ 0 };
    };

    // axan #13: reorder a row among its siblings, carrying its whole subtree. Up: the
    // block goes directly before the previous sibling. Down: directly after the last row
    // of the next sibling's subtree. Both keep every parent ahead of its children: the
    // block stays internally ordered and their shared parent stays ahead of both siblings.
    // None when there is no sibling to jump over.
    inline std::optional<BlockMove> MoveAmongSiblings(const Nodes& nodes, size_t index, bool up)
    {
        const auto sibling = up ? PrevSibling(nodes, index) : NextSibling(nodes, index);
        if (!sibling)
        {
            return std::nullopt;
        }
        const auto moved = SubtreeMask(nodes, index);
        const auto siblingTree = SubtreeMask(nodes, *sibling);

        BlockMove move;
        size_t restIndex = 0; // position within the list with the block removed
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            if (moved[i])
            {
                move.block.push_back(i);
                continue;
            }
            if (up && i == *sibling)
            {
                move.insertAt = restIndex;
            }
            else if (!up && siblingTree[i])
            {
                move.insertAt = restIndex + 1;
            }
            ++restIndex;
        }
        return move;
    }

    // Where Duplicate inserts the copy of row `index`: right after the source's whole
    // subtree, so the copy is the source's next sibling. (Inserting at index + 1 put the
    // copy between the source and its children, and the copy then read as their parent.)
    // The copy shares the source's ParentId, which precedes the source, so the
    // forward-reference rule holds wherever the subtree's last row sits.
    inline size_t DuplicateInsertIndex(const Nodes& nodes, size_t index)
    {
        if (index >= nodes.size())
        {
            return nodes.size();
        }
        return SubtreeEnd(nodes, index) + 1;
    }

    // What to select after deleting the row that was at `deletedIndex` with parent
    // `deletedParentId`. `after` is the list after the delete (its children already
    // promoted to that parent). The next sibling (a promoted child counts), else the row
    // above the deleted one, else nothing.
    inline std::optional<size_t> SelectionAfterDelete(const Nodes& after, size_t deletedIndex, const std::wstring& deletedParentId)
    {
        for (size_t i = deletedIndex; i < after.size(); ++i)
        {
            if (after[i].parentId == deletedParentId)
            {
                return i;
            }
        }
        if (deletedIndex > 0 && !after.empty())
        {
            return std::min(deletedIndex - 1, after.size() - 1);
        }
        return std::nullopt;
    }

    // axan #38: the fix-up after the editor list's built-in drag reorder. The ListView moves
    // only the dragged row (the head); its descendants stay where they were and its
    // ParentId is untouched. `after` is the list as the drop left it, `headIndex` the
    // head's index in it, `oldDepth` the head's depth before the drag.
    //
    // - The subtree is the transitive closure of ParentId from the head, by id rather than
    //   by position: the head may now sit below its own children, where the forward pass of
    //   SubtreeMask would no longer see them. Non-contiguous children are found the same way.
    // - The anchor is the nearest row above the head that is not in the subtree (none = the
    //   head was dropped at the top). A head dropped among its own children therefore
    //   anchors on the row above the whole family.
    // - The block (head first, then its descendants in their list order) is reinserted as
    //   one run directly after the anchor; every other row keeps its relative order.
    // - The head's depth is its old depth clamped to what the landing spot allows: at most
    //   anchor depth + 1 (a child of the anchor), at least the depth of the row that now
    //   follows the block (so a head dropped between a parent and its child becomes that
    //   child's sibling instead of visually adopting it), capped at the maximum. Dropped at
    //   the very end, nothing below constrains it and a root stays a root.
    // - The new parent is the anchor (depth + 1) or the anchor's ancestor at newDepth - 1.
    //   axan #28: a separator is never a parent, so a head that would become a separator's
    //   child becomes its next sibling instead (inserted after it at its depth).
    // Descendants keep their ParentIds, so their depths relative to the head are preserved.
    struct DragFixup
    {
        BlockMove move;
        std::wstring headParentId;
    };

    inline std::optional<DragFixup> NormalizeAfterDrag(const Nodes& after, size_t headIndex, int32_t oldDepth)
    {
        if (headIndex >= after.size())
        {
            return std::nullopt;
        }

        std::unordered_set<std::wstring> subtree{ after[headIndex].id };
        for (bool grew = true; grew;)
        {
            grew = false;
            for (const auto& n : after)
            {
                if (!n.parentId.empty() && subtree.count(n.parentId) && subtree.insert(n.id).second)
                {
                    grew = true;
                }
            }
        }

        DragFixup fix;
        fix.move.block.push_back(headIndex);
        Nodes rest;
        rest.reserve(after.size());
        std::optional<size_t> anchor; // index into rest
        for (size_t i = 0; i < after.size(); ++i)
        {
            if (subtree.count(after[i].id))
            {
                if (i != headIndex)
                {
                    fix.move.block.push_back(i);
                }
                continue;
            }
            if (i < headIndex)
            {
                anchor = rest.size();
            }
            rest.push_back(after[i]);
        }
        fix.move.insertAt = anchor ? *anchor + 1 : 0;

        // No rest row's parent is in the subtree (it would be in the subtree itself), so
        // the depths of the rest are the pre-drag depths.
        const auto depths = Depths(rest);
        const int32_t maxDepth = anchor ? depths[*anchor] + 1 : 0;
        const int32_t below = fix.move.insertAt < rest.size() ? depths[fix.move.insertAt] : 0;
        const int32_t minDepth = std::min(below, maxDepth);
        auto newDepth = std::clamp(oldDepth, minDepth, maxDepth);
        if (anchor && newDepth == maxDepth && rest[*anchor].separator)
        {
            newDepth = depths[*anchor];
        }

        if (newDepth > 0)
        {
            // Walk up from the anchor to its ancestor at newDepth - 1. Every row on the way
            // has depth > 0, so its parent resolves to an earlier rest row.
            std::unordered_map<std::wstring, size_t> byId;
            for (size_t i = 0; i < rest.size(); ++i)
            {
                byId.emplace(rest[i].id, i);
            }
            auto cur = *anchor;
            while (depths[cur] > newDepth - 1)
            {
                cur = byId.at(rest[cur].parentId);
            }
            fix.headParentId = rest[cur].id;
        }
        return fix;
    }

    // The tree's structural invariant: every ParentId is empty or names an EARLIER row
    // that is not a separator. The view-model checks it after a drag fix-up.
    inline bool IsWellFormed(const Nodes& nodes)
    {
        std::unordered_map<std::wstring, bool> seen; // id -> separator
        for (const auto& n : nodes)
        {
            if (!n.parentId.empty())
            {
                const auto it = seen.find(n.parentId);
                if (it == seen.end() || it->second)
                {
                    return false;
                }
            }
            seen.emplace(n.id, n.separator);
        }
        return true;
    }
}
