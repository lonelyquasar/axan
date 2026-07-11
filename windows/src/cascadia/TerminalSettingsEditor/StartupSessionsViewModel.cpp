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
                    auto vm = winrt::make<LaunchEntryViewModel>(e.Id(), e.ParentId(), e.Name(), e.Directory(), e.Command(), e.Icon(), e.Color(), e.ColorTarget());
                    vm.Profile(e.Profile());
                    rows.Append(vm);
                    _hookEntry(vm);
                }
            }
        }
        _Entries = rows;
        _recomputeDepths();
        _NotifyChanges(L"Entries");
    }

    // Re-commit the global tree whenever a row's field changes (profile/name/dir/cmd/icon/color,
    // or ParentId via indent/outdent). Weak capture breaks the this->vector->vm->handler cycle.
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
        const auto myDepth = vm.Depth();
        for (int32_t i = static_cast<int32_t>(index) - 1; i >= 0; --i)
        {
            const auto candidate = _Entries.GetAt(static_cast<uint32_t>(i));
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

        rest.insert(rest.begin() + static_cast<ptrdiff_t>(insertAt), block.begin(), block.end());
        _Entries = winrt::single_threaded_observable_vector<Editor::LaunchEntryViewModel>(std::move(rest));
        _recomputeDepths();
        _commit();
        _NotifyChanges(L"Entries");
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
        // version is refused rather than silently half-applied.
        if (const auto ver = doc["meta"]["axan-toml-version"].value<int64_t>(); ver && *ver > Wire::kTomlVersion)
        {
            Axan::Log::Warn("StartupSessionsViewModel", "import: file declares a newer axan-toml-version than this build supports", { { "path", pathU8 }, { "fileVersion", std::to_string(*ver) }, { "supported", std::to_string(Wire::kTomlVersion) } });
            return false;
        }

        const auto* entries = doc[Wire::TomlKey::Table].as_array();
        if (!entries)
        {
            Axan::Log::Warn("StartupSessionsViewModel", "import: no [[startup-sessions]] array in file (not a startup-sessions export?)", { { "path", pathU8 } });
            return false;
        }

        auto rows = winrt::single_threaded_observable_vector<Editor::LaunchEntryViewModel>();
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
        Axan::Log::Info("StartupSessionsViewModel", "import: replaced the startup tree from TOML", { { "path", pathU8 }, { "count", std::to_string(rows.Size()) } });
        return true;
    }
}
