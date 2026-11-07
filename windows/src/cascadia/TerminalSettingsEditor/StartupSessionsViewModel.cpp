// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#include "pch.h"
#include "StartupSessionsViewModel.h"
#include "StartupSessionsViewModel.g.cpp"

#include "../../types/inc/utils.hpp" // GuidToString / GuidFromString / CreateGuid

// axan D19: portable TOML import for the global startup tree.
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <toml.hpp>
#include <AxanLaunchEntryWire.h> // shared wire format: TOML keys, icon mapping, version (src/inc)
#include <AxanHoistRule.h> // axan #34
#include <AxanLog.h>

using namespace winrt::Windows::Foundation::Collections;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    // MainPage constructs a fresh view-model per navigation, so the settings clone this
    // edits is fixed for the page's lifetime (no UpdateSettings re-target exists).
    StartupSessionsViewModel::StartupSessionsViewModel(const Model::CascadiaSettings& settings) :
        _settings{ settings }
    {
        _rebuildFromSettings();
    }

    IObservableVector<Model::Profile> StartupSessionsViewModel::AvailableProfiles() const
    {
        return _settings ? _settings.ActiveProfiles() : nullptr;
    }

    hstring StartupSessionsViewModel::DefaultProfileGuid() const
    {
        if (_settings)
        {
            return hstring{ ::Microsoft::Console::Utils::GuidToString(_settings.GlobalSettings().DefaultProfile()) };
        }
        return {};
    }

    // Build the row view-models from the global StartupSessions tree.
    void StartupSessionsViewModel::_rebuildFromSettings()
    {
        auto rows = winrt::single_threaded_observable_vector<Editor::LaunchEntryViewModel>();
        if (_settings)
        {
            if (const auto entries = _settings.GlobalSettings().StartupSessions())
            {
                for (const auto& e : entries)
                {
                    const auto vm = _rowFromModel(e);
                    rows.Append(vm);
                    _hookEntry(vm);
                }
            }
        }
        _Entries = rows;
        _recomputeDepths();
        _NotifyChanges(L"Entries");
        _select(rows.Size() ? rows.GetAt(0) : nullptr); // axan #38
    }

    // axan #19: the ONLY place a LaunchEntryViewModel is constructed, and (with
    // _modelFromRow below) one of the two places that own the entry field list, one per
    // direction. Every row (settings rebuild, add, add-separator, duplicate, import, live
    // capture) is built from a Model::LaunchEntry through here, so a new field threaded
    // through these two functions can't be silently dropped by one construction site.
    // axan #3: the separator fields ride along (Kind decides which editor the row template
    // shows); on a session row they stay at their defaults.
    Editor::LaunchEntryViewModel StartupSessionsViewModel::_rowFromModel(const Model::LaunchEntry& e)
    {
        auto vm = winrt::make<LaunchEntryViewModel>(e.Id());
        vm.ParentId(e.ParentId());
        vm.Profile(e.Profile());
        vm.Name(e.Name());
        vm.Directory(e.Directory());
        vm.Command(e.Command());
        vm.Icon(e.Icon());
        vm.Color(e.Color());
        vm.ColorTarget(e.ColorTarget());
        vm.Kind(e.Kind());
        vm.SeparatorStyle(e.SeparatorStyle());
        vm.Height(e.Height());
        vm.Placement(e.Placement());
        vm.Collapsed(e.Collapsed()); // axan #33
        vm.Hoist(e.Hoist()); // axan #34
        return vm;
    }

    // axan #19: the reverse direction, one model entry from a row. Used by _commit and by
    // DuplicateEntry's round trip.
    Model::LaunchEntry StartupSessionsViewModel::_modelFromRow(const Editor::LaunchEntryViewModel& vm)
    {
        Model::LaunchEntry e{};
        e.Id(vm.Id());
        e.ParentId(vm.ParentId());
        e.Profile(vm.Profile());
        e.Name(vm.Name());
        e.Directory(vm.Directory());
        e.Command(vm.Command());
        e.Icon(vm.Icon());
        e.Color(vm.Color());
        e.ColorTarget(vm.ColorTarget());
        // axan #3: separator fields. LaunchEntry::ToJson writes them sparsely and only on a
        // separator, so a session row's on-disk shape is unchanged.
        e.Kind(vm.Kind());
        e.SeparatorStyle(vm.SeparatorStyle());
        e.Height(vm.Height());
        e.Placement(vm.Placement());
        e.Collapsed(vm.Collapsed()); // axan #33 (the header label is Name, above)
        e.Hoist(vm.IsSeparator() ? hstring{} : vm.Hoist()); // axan #34 (sessions only)
        return e;
    }

    // axan #19: a fresh row id for add / duplicate / an imported entry without one.
    static hstring _newEntryId()
    {
        return hstring{ ::Microsoft::Console::Utils::GuidToString(::Microsoft::Console::Utils::CreateGuid()) };
    }

    // Re-commit the global tree whenever a row's PERSISTED field changes (profile/name/dir/
    // cmd/icon/color, a separator's style/height/placement/label, or ParentId via indent/
    // outdent). axan #38: derived properties are raised alongside the field they mirror
    // (Icon also raises IconPortable + IconPreview, Kind raises four more, Depth raises
    // IndentMargin, ...), and committing on each re-ran the O(N) _commit up to five times
    // per edit. They are filtered out by name. A denylist rather than an allowlist, so a
    // newly added persisted field commits without touching this. Weak capture breaks the
    // this->vector->vm->handler cycle.
    void StartupSessionsViewModel::_hookEntry(const Editor::LaunchEntryViewModel& vm)
    {
        vm.PropertyChanged([weakThis = get_weak()](const auto& /*sender*/, const Windows::UI::Xaml::Data::PropertyChangedEventArgs& args) {
            static const std::unordered_set<std::wstring_view> derived{
                L"IconPortable", L"IconPreview", L"ColorPreviewBrush", L"Depth", L"IndentMargin",
                L"IsSeparator", L"IsSession", L"SessionVisibility", L"SeparatorVisibility",
                L"SeparatorStyleIndex", L"PlacementIndex"
            };
            if (derived.count(std::wstring_view{ args.PropertyName() }))
            {
                return;
            }
            if (auto self{ weakThis.get() })
            {
                if (!self->_suspendCommit)
                {
                    self->_commit();
                }
            }
        });
    }

    // ---- axan #38: selection and the structural rules (src/inc/AxanEntryTreeOps.h) ----

    Axan::EntryTree::Nodes StartupSessionsViewModel::_nodes() const
    {
        Axan::EntryTree::Nodes nodes;
        if (_Entries)
        {
            nodes.reserve(_Entries.Size());
            for (const auto& e : _Entries)
            {
                nodes.push_back({ std::wstring{ e.Id() }, std::wstring{ e.ParentId() }, e.IsSeparator() });
            }
        }
        return nodes;
    }

    std::optional<uint32_t> StartupSessionsViewModel::_indexOf(const Editor::LaunchEntryViewModel& vm) const
    {
        uint32_t index;
        if (vm && _Entries && _Entries.IndexOf(vm, index))
        {
            return index;
        }
        return std::nullopt;
    }

    // In place rather than a fresh vector + "Entries": rebinding re-realized every row
    // (profile combo populate, icon/color previews, text boxes), a visible pause per click
    // on longer lists. Removing then re-inserting only the block touches just those rows.
    // After the removals the vector is the block-less list, so insertAt is a live index.
    // The block is reinserted in the order listed (a drag fix-up lists the head first even
    // when it sits below its children), so removal goes by descending index on a copy.
    void StartupSessionsViewModel::_applyBlockMove(const Axan::EntryTree::BlockMove& move)
    {
        std::vector<Editor::LaunchEntryViewModel> block;
        block.reserve(move.block.size());
        for (const auto i : move.block)
        {
            block.push_back(_Entries.GetAt(static_cast<uint32_t>(i)));
        }
        auto removal = move.block;
        std::sort(removal.begin(), removal.end(), std::greater<>{});
        for (const auto i : removal)
        {
            _Entries.RemoveAt(static_cast<uint32_t>(i));
        }
        for (size_t k = 0; k < block.size(); ++k)
        {
            _Entries.InsertAt(static_cast<uint32_t>(move.insertAt + k), block[k]);
        }
    }

    void StartupSessionsViewModel::SelectedEntry(const Editor::LaunchEntryViewModel& value)
    {
        if (_selectedEntry != value)
        {
            _select(value);
        }
    }

    void StartupSessionsViewModel::_select(const Editor::LaunchEntryViewModel& vm)
    {
        _selectedEntry = vm;
        _NotifyChanges(L"SelectedEntry");
        _notifySelectionState();
    }

    void StartupSessionsViewModel::_notifySelectionState()
    {
        _NotifyChanges(L"HasSelection", L"CanMoveUp", L"CanMoveDown", L"CanIndent", L"CanOutdent");
    }

    bool StartupSessionsViewModel::CanMoveUp() const
    {
        const auto i = _indexOf(_selectedEntry);
        return i && Axan::EntryTree::PrevSibling(_nodes(), *i).has_value();
    }

    bool StartupSessionsViewModel::CanMoveDown() const
    {
        const auto i = _indexOf(_selectedEntry);
        return i && Axan::EntryTree::NextSibling(_nodes(), *i).has_value();
    }

    bool StartupSessionsViewModel::CanIndent() const
    {
        const auto i = _indexOf(_selectedEntry);
        return i && Axan::EntryTree::IndentParent(_nodes(), *i).has_value();
    }

    bool StartupSessionsViewModel::CanOutdent() const
    {
        const auto i = _indexOf(_selectedEntry);
        return i && Axan::EntryTree::OutdentParent(_nodes(), *i).has_value();
    }

    // depth(row) = depth(parent) + 1, parent looked up by Id; a single forward pass suffices
    // because a parent always precedes its children (forward-reference rule).
    // axan #19: Depth is derived (never persisted), but each change raises Depth and
    // IndentMargin, and the _hookEntry handler would re-run the O(N) _commit for each:
    // O(N^2) for a rebuild/import/capture of a nested tree. Suspend commits for the pass;
    // every caller that changed persisted state commits once afterwards.
    void StartupSessionsViewModel::_recomputeDepths()
    {
        const auto wasSuspended = std::exchange(_suspendCommit, true);
        const auto restore = wil::scope_exit([&]() noexcept { _suspendCommit = wasSuspended; });
        std::unordered_map<winrt::hstring, int32_t> depthById;
        for (const auto& vm : _Entries)
        {
            int32_t depth = 0;
            if (const auto parentId = vm.ParentId(); !parentId.empty())
            {
                if (const auto it = depthById.find(parentId); it != depthById.end())
                {
                    depth = it->second + 1;
                }
            }
            vm.Depth(depth);
            depthById[vm.Id()] = depth;
        }
    }

    void StartupSessionsViewModel::_commit()
    {
        if (!_settings)
        {
            return;
        }
        std::vector<Model::LaunchEntry> entries;
        entries.reserve(_Entries.Size());
        for (const auto& vm : _Entries)
        {
            entries.push_back(_modelFromRow(vm));
        }
        _settings.GlobalSettings().StartupSessions(winrt::single_threaded_vector(std::move(entries)));
    }

    void StartupSessionsViewModel::AddEntry()
    {
        Model::LaunchEntry e{};
        e.Id(_newEntryId());
        // Default a new row to the default profile so it shows a real shell immediately.
        e.Profile(DefaultProfileGuid());
        const auto vm = _rowFromModel(e);
        _Entries.Append(vm);
        _hookEntry(vm);
        _recomputeDepths();
        _commit();
        _NotifyChanges(L"Entries");
        _select(vm); // axan #38
    }

    // axan #3: append a root separator — a line, one session row tall, inline. Mirrors
    // AddEntry; the row is hooked so edits to its style/height/placement re-commit.
    void StartupSessionsViewModel::AddSeparator()
    {
        namespace Wire = Axan::LaunchEntryWire;
        const auto id = _newEntryId();
        Model::LaunchEntry e{};
        e.Id(id);
        e.Kind(hstring{ Wire::KindSeparatorW });
        e.SeparatorStyle(hstring{ Wire::StyleLineW });
        e.Height(Wire::kSeparatorHeightDefault);
        e.Placement(hstring{ Wire::PlacementInlineW });
        const auto vm = _rowFromModel(e);
        _Entries.Append(vm);
        _hookEntry(vm);
        _recomputeDepths();
        _commit();
        _NotifyChanges(L"Entries");
        _select(vm); // axan #38
        Axan::Log::Info("StartupSessionsViewModel", "added a separator row", { { "id", winrt::to_string(id) } });
    }

    void StartupSessionsViewModel::DeleteEntry(const Editor::LaunchEntryViewModel& vm)
    {
        const auto found = _indexOf(vm);
        if (!found)
        {
            return;
        }
        const auto index = *found;
        _suspendCommit = true;
        // Promote the deleted row's children to its parent so they aren't orphaned.
        const auto deletedId = vm.Id();
        const auto newParent = vm.ParentId();
        for (const auto& other : _Entries)
        {
            if (other.ParentId() == deletedId)
            {
                other.ParentId(newParent);
            }
        }
        _Entries.RemoveAt(index);
        _recomputeDepths();
        _suspendCommit = false;
        _commit();
        _NotifyChanges(L"Entries");
        // axan #38: select the next sibling (a promoted child counts), else the row above,
        // else nothing.
        const auto next = Axan::EntryTree::SelectionAfterDelete(_nodes(), index, std::wstring{ newParent });
        _select(next ? _Entries.GetAt(static_cast<uint32_t>(*next)) : nullptr);
    }

    void StartupSessionsViewModel::DuplicateEntry(const Editor::LaunchEntryViewModel& vm)
    {
        const auto index = _indexOf(vm);
        if (!index)
        {
            return;
        }
        // A fresh id (so links stay unambiguous); same profile + recipe, same parent → the
        // original's next sibling, placed after the original's whole subtree. axan #38: it
        // used to land at index + 1, between the original and its children, so the children
        // were drawn under the copy. Only the row is copied, not its children. The common
        // "second claude in the same dir" move.
        // axan #19: a round trip through the model, so every field (incl. a separator's
        // kind/style/height/placement) is copied by the two functions that own the field list.
        auto model = _modelFromRow(vm);
        model.Id(_newEntryId());
        const auto copy = _rowFromModel(model);
        const auto at = Axan::EntryTree::DuplicateInsertIndex(_nodes(), *index);
        _Entries.InsertAt(static_cast<uint32_t>(at), copy);
        _hookEntry(copy);
        _recomputeDepths();
        _commit();
        _NotifyChanges(L"Entries");
        _select(copy); // axan #38
    }

    // Nest under the nearest preceding SESSION row at this row's own depth (its previous
    // sibling). axan #3: a separator is never a parent, so a separator sibling is skipped
    // and the search keeps walking up (still stopping at the parent boundary). With none,
    // indent is a no-op. The rule lives in AxanEntryTreeOps.h (IndentParent).
    void StartupSessionsViewModel::IndentEntry(const Editor::LaunchEntryViewModel& vm)
    {
        const auto index = _indexOf(vm);
        if (!index)
        {
            return;
        }
        const auto parent = Axan::EntryTree::IndentParent(_nodes(), *index);
        if (!parent)
        {
            return;
        }
        _suspendCommit = true;
        vm.ParentId(hstring{ *parent });
        _recomputeDepths();
        _suspendCommit = false;
        _commit();
        _NotifyChanges(L"Entries");
        _select(vm); // axan #38
    }

    // New parent = the current parent's parent (un-nest one level); a root is a no-op.
    void StartupSessionsViewModel::OutdentEntry(const Editor::LaunchEntryViewModel& vm)
    {
        const auto index = _indexOf(vm);
        if (!index)
        {
            return;
        }
        const auto parent = Axan::EntryTree::OutdentParent(_nodes(), *index);
        if (!parent)
        {
            return;
        }
        _suspendCommit = true;
        vm.ParentId(hstring{ *parent });
        _recomputeDepths();
        _suspendCommit = false;
        _commit();
        _NotifyChanges(L"Entries");
        _select(vm); // axan #38
    }

    // axan #13: reorder among siblings, carrying the whole subtree. Launch order is list
    // order (D19), so this is the in-place alternative to delete-and-re-add. The list is
    // only ordered by the forward-reference rule (a parent precedes its children), not
    // family-contiguous, so the move works on index sets rather than contiguous ranges
    // (AxanEntryTreeOps.h MoveAmongSiblings). No row outside the moved block moves.
    void StartupSessionsViewModel::_moveEntry(const Editor::LaunchEntryViewModel& vm, bool up)
    {
        const auto index = _indexOf(vm);
        if (!index)
        {
            return;
        }
        const auto move = Axan::EntryTree::MoveAmongSiblings(_nodes(), *index, up);
        if (!move)
        {
            return;
        }
        _applyBlockMove(*move);
        // Sibling moves never change depth (the guard in Depth() keeps this silent), and
        // the observable vector already told the view about the rows it needs to redraw.
        _recomputeDepths();
        _commit();
        // axan #38: the RemoveAt / InsertAt cleared a ListView's selection; put it back.
        _select(vm);
    }

    void StartupSessionsViewModel::MoveEntryUp(const Editor::LaunchEntryViewModel& vm)
    {
        _moveEntry(vm, true);
    }

    void StartupSessionsViewModel::MoveEntryDown(const Editor::LaunchEntryViewModel& vm)
    {
        _moveEntry(vm, false);
    }

    // axan #38: drag reorder. The ListView's built-in reorder RemoveAt/InsertAt's only the
    // dragged row (the head) on Entries; nothing else changes until the drop. Commits are
    // suspended for the duration so no intermediate state can reach the settings clone
    // (the reorder itself raises no row PropertyChanged, but an edit racing the drag must
    // not commit a list whose head sits away from its children either).
    void StartupSessionsViewModel::BeginDrag(const Editor::LaunchEntryViewModel& head)
    {
        if (!_dragging)
        {
            _dragWasSuspended = std::exchange(_suspendCommit, true);
            _dragging = true;
        }
        _dragHead = head;
        _dragStartIndex = _indexOf(head);
        _dragOldDepth = head ? head.Depth() : 0;
        Axan::Log::Debug("StartupSessionsViewModel", "drag starting", { { "id", head ? winrt::to_string(head.Id()) : std::string{} }, { "index", _dragStartIndex ? std::to_string(*_dragStartIndex) : std::string{ "none" } } });
    }

    // After the drop: carry the head's subtree to sit right after it and re-derive the
    // head's parent from where it landed (rules: AxanEntryTreeOps.h NormalizeAfterDrag),
    // recompute depths, then commit once and re-select the head (the reorder cleared the
    // ListView's selection).
    void StartupSessionsViewModel::NormalizeAfterDrag(const Editor::LaunchEntryViewModel& head)
    {
        const auto startIndex = std::exchange(_dragStartIndex, std::nullopt);
        const auto oldDepth = _dragOldDepth;
        const auto expected = std::exchange(_dragHead, nullptr);
        const auto wasDragging = std::exchange(_dragging, false);
        const auto releaseCommit = [&]() {
            if (wasDragging)
            {
                _suspendCommit = _dragWasSuspended;
            }
        };

        const auto index = _indexOf(head);
        if (!index || head != expected || index == startIndex)
        {
            // Cancelled, dropped in place, or not the drag BeginDrag saw: nothing moved.
            releaseCommit();
            Axan::Log::Debug("StartupSessionsViewModel", "drag ended without a move");
            _select(_selectedEntry);
            return;
        }

        const auto fix = Axan::EntryTree::NormalizeAfterDrag(_nodes(), *index, oldDepth);
        if (fix)
        {
            head.ParentId(hstring{ fix->headParentId }); // still suspended: no commit
            _applyBlockMove(fix->move);
        }
        _recomputeDepths();

        if (!Axan::EntryTree::IsWellFormed(_nodes()))
        {
            // Should be unreachable (the fix-up only ever parents onto an earlier session
            // row). The settings clone still holds the last committed tree; restore it.
            releaseCommit();
            Axan::Log::Error("StartupSessionsViewModel", "drag fix-up left a parent after its child or under a separator; restoring the last committed tree", { { "id", winrt::to_string(head.Id()) } });
            _rebuildFromSettings();
            return;
        }

        releaseCommit();
        _commit();
        Axan::Log::Info("StartupSessionsViewModel", "drag reorder applied", { { "id", winrt::to_string(head.Id()) }, { "from", startIndex ? std::to_string(*startIndex) : std::string{ "none" } }, { "to", std::to_string(_indexOf(head).value_or(0)) }, { "parent", fix ? winrt::to_string(hstring{ fix->headParentId }) : std::string{} }, { "rows", fix ? std::to_string(fix->move.block.size()) : std::string{ "0" } } });
        _select(head);
    }

    // D19: resolve an imported entry's profile reference against the live profiles —
    // GUID-first, then the portable profile-name fallback (so a hand-created profile
    // re-matches on a machine where its GUID differs). Empty stays empty ("follow the
    // global default profile"). An unresolvable reference is kept verbatim with a logged
    // warning: spawn falls back to the default profile until that profile (re)appears.
    hstring StartupSessionsViewModel::_resolveImportedProfile(const std::string& bareGuid, const std::string& name)
    {
        if (bareGuid.empty())
        {
            return {};
        }
        const auto braced = bareGuid.front() == '{' ? bareGuid : "{" + bareGuid + "}";
        const auto bracedHstr = winrt::to_hstring(braced);
        if (_settings)
        {
            try
            {
                const auto g = ::Microsoft::Console::Utils::GuidFromString(std::wstring{ bracedHstr }.c_str());
                if (const auto p = _settings.FindProfile(winrt::guid{ g }))
                {
                    return hstring{ ::Microsoft::Console::Utils::GuidToString(p.Guid()) };
                }
            }
            catch (...)
            {
                // Malformed GUID string in the file; fall through to the name match.
            }
            if (!name.empty())
            {
                if (const auto profiles = _settings.ActiveProfiles())
                {
                    const auto nameHstr = winrt::to_hstring(name);
                    for (const auto& p : profiles)
                    {
                        if (p.Name() == nameHstr)
                        {
                            Axan::Log::Info("StartupSessionsViewModel", "import: profile GUID not found; re-matched by name", { { "profile", braced }, { "name", name } });
                            return hstring{ ::Microsoft::Console::Utils::GuidToString(p.Guid()) };
                        }
                    }
                }
            }
        }
        Axan::Log::Warn("StartupSessionsViewModel", "import: entry references a profile this machine doesn't have; it will spawn under the default profile", { { "profile", braced }, { "name", name } });
        return bracedHstr;
    }

    // Replace the tree with the startup sessions parsed from a portable startup-sessions
    // TOML — the file the auto-exporter writes (schema + rationale:
    // src/inc/AxanLaunchEntryWire.h; D18/D19). Canonical kebab keys; builtin:
    // icons map to the native Segoe glyph (an UNKNOWN builtin: name falls back to the
    // placeholder with a logged warning, per D18); each entry's `profile` GUID resolves
    // with the `profile-name` fallback. Returns false — with the current list untouched —
    // on any read/parse/validation failure or when the file yields zero entries; every
    // failure path logs its reason to axan.log (the page surfaces the boolean as an
    // InfoBar, #435).
    bool StartupSessionsViewModel::ImportFromToml(hstring path)
    {
        namespace Wire = Axan::LaunchEntryWire;
        const auto pathU8 = winrt::to_string(path);

        std::string text;
        try
        {
            std::ifstream in{ std::filesystem::path{ path.c_str() }, std::ios::binary };
            if (!in)
            {
                Axan::Log::Warn("StartupSessionsViewModel", "import: cannot open file", { { "path", pathU8 } });
                return false;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
        catch (...)
        {
            Axan::Log::Warn("StartupSessionsViewModel", "import: cannot read file", { { "path", pathU8 } });
            return false;
        }

        toml::table doc;
        try
        {
            doc = toml::parse(text, std::string_view{ pathU8 });
        }
        catch (const toml::parse_error& e)
        {
            Axan::Log::Warn("StartupSessionsViewModel", "import: TOML parse failed", { { "path", pathU8 }, { "error", std::string{ e.description() } } });
            return false;
        }

        // [meta] version gate (#431 item 1). Absent is treated as v1; a newer-than-known
        // version is refused rather than silently half-applied. axan #3: version 2 adds
        // separator rows, so anything up to kTomlVersionMax is accepted.
        if (const auto ver = doc["meta"]["axan-toml-version"].value<int64_t>(); ver && *ver > Wire::kTomlVersionMax)
        {
            Axan::Log::Warn("StartupSessionsViewModel", "import: file declares a newer axan-toml-version than this build supports", { { "path", pathU8 }, { "fileVersion", std::to_string(*ver) }, { "supported", std::to_string(Wire::kTomlVersionMax) } });
            return false;
        }

        const auto* entries = doc[Wire::TomlKey::Table].as_array();
        if (!entries)
        {
            Axan::Log::Warn("StartupSessionsViewModel", "import: no [[startup-sessions]] array in file (not a startup-sessions export?)", { { "path", pathU8 } });
            return false;
        }

        auto rows = winrt::single_threaded_observable_vector<Editor::LaunchEntryViewModel>();
        uint32_t separators = 0;
        for (const auto& el : *entries)
        {
            const auto* t = el.as_table();
            if (!t)
            {
                continue;
            }
            const auto e = Wire::FromTomlTable(*t);

            const auto idHstr = e.id.empty() ? _newEntryId() : winrt::to_hstring(e.id);

            // axan #3: an unrecognized row kind (hand edit, or a newer build's kind) would be
            // imported as a default-profile shell and lose its kind on save — skip it loudly.
            if (!e.kind.empty() && e.kind != Wire::KindSession && e.kind != Wire::KindSeparator)
            {
                Axan::Log::Warn("StartupSessionsViewModel", "import: unrecognized row kind; skipping the entry", { { "kind", e.kind }, { "entry", e.id } });
                continue;
            }

            // axan #3: a separator row has no profile/icon/etc. to resolve — only its own
            // fields (FromTomlTable already normalized the height).
            if (e.IsSeparator())
            {
                Model::LaunchEntry m{};
                m.Id(idHstr);
                m.ParentId(winrt::to_hstring(e.parentId));
                m.Kind(hstring{ Wire::KindSeparatorW });
                m.SeparatorStyle(winrt::to_hstring(e.style));
                m.Height(e.height);
                m.Placement(winrt::to_hstring(e.placement));
                m.Name(winrt::to_hstring(e.name)); // axan #33: header label
                m.Collapsed(e.collapsed);
                const auto sep = _rowFromModel(m);
                rows.Append(sep);
                _hookEntry(sep);
                ++separators;
                continue;
            }

            // builtin: -> native glyph; an unknown builtin: name becomes the empty
            // placeholder with a warning instead of storing the raw token (D18, #431 item 7).
            bool unknownBuiltin = false;
            const auto nativeIcon = Wire::ImportIcon(std::wstring{ winrt::to_hstring(e.icon) }, unknownBuiltin);
            if (unknownBuiltin)
            {
                Axan::Log::Warn("StartupSessionsViewModel", "import: unknown builtin: icon; using the default placeholder", { { "icon", e.icon }, { "entry", e.id } });
            }

            Model::LaunchEntry m{};
            m.Id(idHstr);
            m.ParentId(winrt::to_hstring(e.parentId));
            m.Profile(_resolveImportedProfile(e.profile, e.profileName));
            m.Name(winrt::to_hstring(e.name));
            m.Directory(winrt::to_hstring(e.directory));
            m.Command(winrt::to_hstring(e.command));
            m.Icon(hstring{ nativeIcon });
            m.Color(winrt::to_hstring(e.color));
            m.ColorTarget(winrt::to_hstring(e.colorTarget));
            if (!e.hoist.empty()) // axan #34: normalized (unknown names dropped)
            {
                std::vector<std::wstring> unknown;
                const auto hoist = Axan::HoistRule::Parse(std::wstring{ winrt::to_hstring(e.hoist) }, &unknown);
                if (!unknown.empty())
                {
                    Axan::Log::Warn("StartupSessionsViewModel", "import: unknown hoist state name(s) ignored", { { "hoist", e.hoist }, { "entry", e.id } });
                }
                m.Hoist(hstring{ Axan::HoistRule::Format(hoist) });
            }
            const auto vm = _rowFromModel(m);
            rows.Append(vm);
            _hookEntry(vm);
        }

        // Zero parsed entries is a failure, not "replace the tree with nothing" (#431
        // item 2) — an empty or alien file must never wipe the user's curated tree.
        if (rows.Size() == 0)
        {
            Axan::Log::Warn("StartupSessionsViewModel", "import: file yielded no entries; leaving the current tree untouched", { { "path", pathU8 } });
            return false;
        }

        _Entries = rows;
        _recomputeDepths();
        _commit();
        _NotifyChanges(L"Entries");
        _select(rows.GetAt(0)); // axan #38 (rows is non-empty here)
        Axan::Log::Info("StartupSessionsViewModel", "import: replaced the startup tree from TOML", { { "path", pathU8 }, { "count", std::to_string(rows.Size()) }, { "separators", std::to_string(separators) } });
        return true;
    }

    // axan #14: "Save current as startup" — replace the tree with a snapshot of the LIVE
    // session tree, pulled through the provider TerminalApp registered on MainPage (the
    // editor edits a settings clone and can't see live sessions itself). Same replace
    // semantics as ImportFromToml, including the zero-entry guard: no live sessions must
    // never wipe the user's curated tree — return 0 and leave the list untouched (the
    // page surfaces it). Captured rows carry hierarchy/profile/cwd/name/icon/color but an
    // EMPTY command — the launch command isn't recoverable from a live session, the
    // accepted limitation the page's confirm prompt warns about.
    uint32_t StartupSessionsViewModel::CaptureLiveStartup()
    {
        if (!_liveEntriesProvider)
        {
            Axan::Log::Warn("StartupSessionsViewModel", "capture: no live-entries provider registered; leaving the list untouched");
            return 0;
        }
        const auto entries = _liveEntriesProvider();
        if (!entries || entries.Size() == 0)
        {
            Axan::Log::Warn("StartupSessionsViewModel", "capture: no live sessions to capture; leaving the list untouched");
            return 0;
        }
        auto rows = winrt::single_threaded_observable_vector<Editor::LaunchEntryViewModel>();
        for (const auto& e : entries)
        {
            // axan #3: the live tree may contain separator rows; _rowFromModel carries
            // Kind/SeparatorStyle/Height/Placement so they survive the capture.
            const auto vm = _rowFromModel(e);
            rows.Append(vm);
            _hookEntry(vm);
        }
        _Entries = rows;
        _recomputeDepths();
        _commit();
        _NotifyChanges(L"Entries");
        _select(rows.GetAt(0)); // axan #38 (rows is non-empty here)
        Axan::Log::Info("StartupSessionsViewModel", "capture: replaced the startup tree from the live window", { { "count", std::to_string(rows.Size()) } });
        return rows.Size();
    }
}
