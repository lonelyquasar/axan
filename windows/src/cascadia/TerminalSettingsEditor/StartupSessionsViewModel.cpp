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
#include <toml.hpp>
#include <AxanLaunchEntryWire.h> // shared wire format: TOML keys, icon mapping, version (src/inc)
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
    }

    // One row from a model entry. axan #3: the separator fields ride along (Kind decides
    // which editor the row template shows); on a session row they stay at their defaults.
    Editor::LaunchEntryViewModel StartupSessionsViewModel::_rowFromModel(const Model::LaunchEntry& e)
    {
        auto vm = winrt::make<LaunchEntryViewModel>(e.Id(), e.ParentId(), e.Name(), e.Directory(), e.Command(), e.Icon(), e.Color(), e.ColorTarget());
        vm.Profile(e.Profile());
        vm.Kind(e.Kind());
        vm.SeparatorStyle(e.SeparatorStyle());
        vm.Height(e.Height());
        vm.Placement(e.Placement());
        return vm;
    }

    // Re-commit the global tree whenever a row's field changes (profile/name/dir/cmd/icon/color,
    // a separator's style/height/placement, or ParentId via indent/outdent). Weak capture
    // breaks the this->vector->vm->handler cycle.
    void StartupSessionsViewModel::_hookEntry(const Editor::LaunchEntryViewModel& vm)
    {
        vm.PropertyChanged([weakThis = get_weak()](const auto& /*sender*/, const auto& /*args*/) {
            if (auto self{ weakThis.get() })
            {
                if (!self->_suspendCommit)
                {
                    self->_commit();
                }
            }
        });
    }

    // depth(row) = depth(parent) + 1, parent looked up by Id; a single forward pass suffices
    // because a parent always precedes its children (forward-reference rule).
    void StartupSessionsViewModel::_recomputeDepths()
    {
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
            // axan #3: separator fields. LaunchEntry::ToJson writes them sparsely and only
            // on a separator, so a session row's on-disk shape is unchanged.
            e.Kind(vm.Kind());
            e.SeparatorStyle(vm.SeparatorStyle());
            e.Height(vm.Height());
            e.Placement(vm.Placement());
            entries.push_back(e);
        }
        _settings.GlobalSettings().StartupSessions(winrt::single_threaded_vector(std::move(entries)));
    }

    void StartupSessionsViewModel::AddEntry()
    {
        const auto id = hstring{ ::Microsoft::Console::Utils::GuidToString(::Microsoft::Console::Utils::CreateGuid()) };
        auto vm = winrt::make<LaunchEntryViewModel>(id, hstring{}, hstring{}, hstring{}, hstring{}, hstring{}, hstring{}, hstring{});
        // Default a new row to the default profile so it shows a real shell immediately.
        vm.Profile(DefaultProfileGuid());
        _Entries.Append(vm);
        _hookEntry(vm);
        _recomputeDepths();
        _commit();
        _NotifyChanges(L"Entries");
    }

    // axan #3: append a root separator — a line, one session row tall, inline. Mirrors
    // AddEntry; the row is hooked so edits to its style/height/placement re-commit.
    void StartupSessionsViewModel::AddSeparator()
    {
        namespace Wire = Axan::LaunchEntryWire;
        const auto id = hstring{ ::Microsoft::Console::Utils::GuidToString(::Microsoft::Console::Utils::CreateGuid()) };
        auto vm = winrt::make<LaunchEntryViewModel>(id, hstring{}, hstring{}, hstring{}, hstring{}, hstring{}, hstring{}, hstring{});
        vm.Kind(hstring{ Wire::KindSeparatorW });
        vm.SeparatorStyle(hstring{ Wire::StyleLineW });
        vm.Height(Wire::kSeparatorHeightDefault);
        vm.Placement(hstring{ Wire::PlacementInlineW });
        _Entries.Append(vm);
        _hookEntry(vm);
        _recomputeDepths();
        _commit();
        _NotifyChanges(L"Entries");
        Axan::Log::Info("StartupSessionsViewModel", "added a separator row", { { "id", winrt::to_string(id) } });
    }

    void StartupSessionsViewModel::DeleteEntry(const Editor::LaunchEntryViewModel& vm)
    {
        uint32_t index;
        if (!_Entries.IndexOf(vm, index))
        {
            return;
        }
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
    }

    void StartupSessionsViewModel::DuplicateEntry(const Editor::LaunchEntryViewModel& vm)
    {
        uint32_t index;
        if (!_Entries.IndexOf(vm, index))
        {
            return;
        }
        // A fresh id (so links stay unambiguous); same profile + recipe, same parent → a sibling
        // right after the original. The common "second claude in the same dir" move.
        const auto id = hstring{ ::Microsoft::Console::Utils::GuidToString(::Microsoft::Console::Utils::CreateGuid()) };
        auto copy = winrt::make<LaunchEntryViewModel>(id, vm.ParentId(), vm.Name(), vm.Directory(), vm.Command(), vm.Icon(), vm.Color(), vm.ColorTarget());
        copy.Profile(vm.Profile());
        // axan #3: a duplicated separator keeps its kind/style/height/placement.
        copy.Kind(vm.Kind());
        copy.SeparatorStyle(vm.SeparatorStyle());
        copy.Height(vm.Height());
        copy.Placement(vm.Placement());
        _Entries.InsertAt(index + 1, copy);
        _hookEntry(copy);
        _recomputeDepths();
        _commit();
        _NotifyChanges(L"Entries");
    }

    void StartupSessionsViewModel::IndentEntry(const Editor::LaunchEntryViewModel& vm)
    {
        uint32_t index;
        if (!_Entries.IndexOf(vm, index) || index == 0)
        {
            return; // the first row has no previous sibling to nest under
        }
        // Nest under the nearest preceding row at this row's own depth (its previous sibling).
        // axan #3: a separator is never a parent — it spawns nothing, so nothing can hang
        // under it. A separator previous-sibling is skipped past, and the search keeps
        // walking up for the previous SESSION sibling at this depth (still stopping at
        // the parent boundary). With none, indent is a no-op.
        const auto myDepth = vm.Depth();
        for (int32_t i = static_cast<int32_t>(index) - 1; i >= 0; --i)
        {
            const auto candidate = _Entries.GetAt(static_cast<uint32_t>(i));
            if (candidate.Depth() == myDepth && candidate.IsSeparator())
            {
                continue;
            }
            if (candidate.Depth() == myDepth)
            {
                _suspendCommit = true;
                vm.ParentId(candidate.Id());
                _recomputeDepths();
                _suspendCommit = false;
                _commit();
                _NotifyChanges(L"Entries");
                return;
            }
            if (candidate.Depth() < myDepth)
            {
                return; // hit the parent boundary with no same-depth sibling before it
            }
        }
    }

    void StartupSessionsViewModel::OutdentEntry(const Editor::LaunchEntryViewModel& vm)
    {
        const auto parentId = vm.ParentId();
        if (parentId.empty())
        {
            return; // already a root
        }
        // New parent = the current parent's parent (un-nest one level).
        winrt::hstring grandParent;
        for (const auto& other : _Entries)
        {
            if (other.Id() == parentId)
            {
                grandParent = other.ParentId();
                break;
            }
        }
        _suspendCommit = true;
        vm.ParentId(grandParent);
        _recomputeDepths();
        _suspendCommit = false;
        _commit();
        _NotifyChanges(L"Entries");
    }

    // axan #13: reorder among siblings, carrying the whole subtree. Launch order is list
    // order (D19), so this is the in-place alternative to delete-and-re-add. The list is
    // only ordered by the forward-reference rule (a parent precedes its children), NOT
    // family-contiguous (Duplicate inserts a sibling between a row and its children), so
    // the move works on id-sets rather than contiguous ranges: lift out the row's subtree,
    // then reinsert it directly before the previous sibling (up) or after the last row of
    // the next sibling's subtree (down). Both placements keep every parent ahead of its
    // children: the moved block stays internally ordered, its parent stays ahead of both
    // siblings, and no row outside the block moves at all.
    void StartupSessionsViewModel::_moveEntry(const Editor::LaunchEntryViewModel& vm, bool up)
    {
        uint32_t index;
        if (!_Entries.IndexOf(vm, index))
        {
            return;
        }

        std::vector<Editor::LaunchEntryViewModel> all;
        all.reserve(_Entries.Size());
        for (const auto& e : _Entries)
        {
            all.push_back(e);
        }

        // The transitive subtree of a row, as an id set; a single forward pass suffices
        // because a parent always precedes its children.
        const auto subtreeIds = [&all](const Editor::LaunchEntryViewModel& root) {
            std::unordered_set<winrt::hstring> ids{ root.Id() };
            for (const auto& e : all)
            {
                if (const auto p = e.ParentId(); !p.empty() && ids.count(p))
                {
                    ids.insert(e.Id());
                }
            }
            return ids;
        };

        // The sibling to jump over: the nearest row before/after this one with the same
        // parent. None -> already first/last among its siblings; nothing to do.
        Editor::LaunchEntryViewModel sibling{ nullptr };
        if (up)
        {
            for (auto i = static_cast<int32_t>(index) - 1; i >= 0; --i)
            {
                if (all[static_cast<size_t>(i)].ParentId() == vm.ParentId())
                {
                    sibling = all[static_cast<size_t>(i)];
                    break;
                }
            }
        }
        else
        {
            for (auto i = static_cast<size_t>(index) + 1; i < all.size(); ++i)
            {
                if (all[i].ParentId() == vm.ParentId())
                {
                    sibling = all[i];
                    break;
                }
            }
        }
        if (!sibling)
        {
            return;
        }

        const auto movedIds = subtreeIds(vm);
        std::vector<Editor::LaunchEntryViewModel> rest;
        std::vector<Editor::LaunchEntryViewModel> block;
        rest.reserve(all.size());
        for (const auto& e : all)
        {
            (movedIds.count(e.Id()) ? block : rest).push_back(e);
        }

        size_t insertAt = 0;
        if (up)
        {
            for (size_t i = 0; i < rest.size(); ++i)
            {
                if (rest[i].Id() == sibling.Id())
                {
                    insertAt = i;
                    break;
                }
            }
        }
        else
        {
            const auto siblingIds = subtreeIds(sibling);
            for (size_t i = 0; i < rest.size(); ++i)
            {
                if (siblingIds.count(rest[i].Id()))
                {
                    insertAt = i + 1;
                }
            }
        }

        // Apply the move to the live observable vector in place rather than swapping in a
        // fresh vector + re-notifying "Entries": that rebound the whole ItemsControl and
        // re-realized every row (profile combo populate, icon/color previews, three text
        // boxes each), which showed up as a visible pause per click on larger lists /
        // slower machines. Removing then re-inserting only the moved block means the
        // ItemsControl touches just those rows; the rest keep their containers. After the
        // removals, _Entries is exactly `rest`, so `insertAt` is the live index too.
        for (auto i = static_cast<int32_t>(all.size()) - 1; i >= 0; --i)
        {
            if (movedIds.count(all[static_cast<size_t>(i)].Id()))
            {
                _Entries.RemoveAt(static_cast<uint32_t>(i));
            }
        }
        for (size_t k = 0; k < block.size(); ++k)
        {
            _Entries.InsertAt(static_cast<uint32_t>(insertAt + k), block[k]);
        }
        // Sibling moves never change depth (the guard in Depth() keeps this silent), and
        // the observable vector already told the view about the rows it needs to redraw.
        _recomputeDepths();
        _commit();
    }

    void StartupSessionsViewModel::MoveEntryUp(const Editor::LaunchEntryViewModel& vm)
    {
        _moveEntry(vm, true);
    }

    void StartupSessionsViewModel::MoveEntryDown(const Editor::LaunchEntryViewModel& vm)
    {
        _moveEntry(vm, false);
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

            const auto idHstr = e.id.empty() ?
                                    hstring{ ::Microsoft::Console::Utils::GuidToString(::Microsoft::Console::Utils::CreateGuid()) } :
                                    winrt::to_hstring(e.id);

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
                auto sep = winrt::make<LaunchEntryViewModel>(idHstr, winrt::to_hstring(e.parentId), hstring{}, hstring{}, hstring{}, hstring{}, hstring{}, hstring{});
                sep.Kind(hstring{ Wire::KindSeparatorW });
                sep.SeparatorStyle(winrt::to_hstring(e.style));
                sep.Height(e.height);
                sep.Placement(winrt::to_hstring(e.placement));
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

            auto vm = winrt::make<LaunchEntryViewModel>(
                idHstr,
                winrt::to_hstring(e.parentId),
                winrt::to_hstring(e.name),
                winrt::to_hstring(e.directory),
                winrt::to_hstring(e.command),
                hstring{ nativeIcon },
                winrt::to_hstring(e.color),
                winrt::to_hstring(e.colorTarget));
            vm.Profile(_resolveImportedProfile(e.profile, e.profileName));
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
        Axan::Log::Info("StartupSessionsViewModel", "capture: replaced the startup tree from the live window", { { "count", std::to_string(rows.Size()) } });
        return rows.Size();
    }
}
