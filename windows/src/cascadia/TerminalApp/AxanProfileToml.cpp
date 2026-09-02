// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#include "pch.h"
#include "AxanProfileToml.h"
#include <AxanLaunchEntryWire.h> // shared LaunchEntry wire format: TOML/JSON keys, quoting, icon mapping (src/inc)

#include <cctype>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <json/json.h>

using namespace std::string_view_literals;
namespace Wire = Axan::LaunchEntryWire;

namespace
{
    // ---- small UTF-8 <-> UTF-16 bridges (the JSON layer is UTF-8 std::string; the icon
    // registry works in wchar_t glyphs) ----
    std::wstring u8ToW(std::string_view s)
    {
        return std::wstring{ winrt::to_hstring(s) };
    }
    std::string wToU8(std::wstring_view w)
    {
        return winrt::to_string(winrt::hstring{ w });
    }

    // ---- GUID normalization. WT stores braced guids ("{....}"); the portable uuid is bare
    // (Linux form) so the same identity reads the same on both platforms. ----
    std::string stripBraces(std::string_view guid)
    {
        if (guid.size() >= 2 && guid.front() == '{' && guid.back() == '}')
        {
            return std::string{ guid.substr(1, guid.size() - 2) };
        }
        return std::string{ guid };
    }
    bool guidEqual(std::string_view a, std::string_view b)
    {
        const auto na = stripBraces(a);
        const auto nb = stripBraces(b);
        if (na.size() != nb.size())
        {
            return false;
        }
        for (size_t i = 0; i < na.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(na[i])) != std::tolower(static_cast<unsigned char>(nb[i])))
            {
                return false;
            }
        }
        return true;
    }

    // ---- TOML emit helpers (the writer is hand-rolled; toml++ drops comments on serialize, so
    // the export path mirrors the Linux build's hand-rolled writer, profile-toml.cc). String
    // quoting (incl. \uXXXX control-char escapes) is the shared Wire::TomlQuote. ----
    bool isBareKey(std::string_view k)
    {
        if (k.empty())
        {
            return false;
        }
        for (const char c : k)
        {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
            {
                return false;
            }
        }
        return true;
    }
    std::string tomlKey(std::string_view k)
    {
        return isBareKey(k) ? std::string{ k } : Wire::TomlQuote(k);
    }

    // Recursively emit a JSON value as a TOML right-hand-side value (objects -> inline tables,
    // arrays -> arrays, scalars typed). Used for the opaque [profile.native.windows] block so an
    // arbitrarily nested WT profile key (e.g. "font = { face = ..., size = ... }") round-trips
    // within the platform. Inline tables keep each key on one line — fine for config.
    //
    // JSON null is the one lossy spot (#431 item 4): TOML has no null, so null-valued KEYS are
    // skipped by the callers/object branch (re-import then leaves them untouched — the sparse
    // contract — rather than stamping ""). A null inside an ARRAY can't be skipped without
    // shifting later elements, so it degrades to "" there; WT profiles don't use null-in-array.
    std::string jsonToTomlValue(const Json::Value& v)
    {
        switch (v.type())
        {
        case Json::nullValue:
            return "\"\"";
        case Json::booleanValue:
            return v.asBool() ? "true" : "false";
        case Json::intValue:
            return std::to_string(v.asInt64());
        case Json::uintValue:
            return std::to_string(v.asUInt64());
        case Json::realValue:
        {
            std::ostringstream oss;
            oss << std::setprecision(17) << v.asDouble();
            auto s = oss.str();
            // TOML needs a float to look like one; integral-valued doubles get a ".0".
            if (s.find_first_of(".eEnN") == std::string::npos)
            {
                s += ".0";
            }
            return s;
        }
        case Json::stringValue:
            return Wire::TomlQuote(v.asString());
        case Json::arrayValue:
        {
            std::string out = "[";
            for (Json::ArrayIndex i = 0; i < v.size(); ++i)
            {
                if (i)
                {
                    out += ", ";
                }
                out += jsonToTomlValue(v[i]);
            }
            out += "]";
            return out;
        }
        case Json::objectValue:
        {
            std::string out = "{ ";
            bool first = true;
            for (const auto& key : v.getMemberNames())
            {
                if (v[key].isNull())
                {
                    continue; // no TOML null; skip the key (see the function comment)
                }
                if (!first)
                {
                    out += ", ";
                }
                first = false;
                out += tomlKey(key) + " = " + jsonToTomlValue(v[key]);
            }
            out += " }";
            return out;
        }
        default:
            return "\"\"";
        }
    }

    // ---- settings.json read ----
    bool readFile(const std::filesystem::path& path, std::string& out, std::string& error)
    {
        std::ifstream in{ path, std::ios::binary };
        if (!in)
        {
            error = "cannot open " + path.string();
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    bool parseSettings(const std::string& content, Json::Value& root, std::string& error)
    {
        Json::CharReaderBuilder builder;
        builder["allowComments"] = true; // WT settings.json permits // comments
        const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
        std::string parseErr;
        if (!reader->parse(content.data(), content.data() + content.size(), &root, &parseErr))
        {
            error = "settings.json parse failed: " + parseErr;
            return false;
        }
        return true;
    }

    // A string field off a JSON object, "" when absent/non-string.
    std::string jsonStr(const Json::Value& obj, std::string_view key)
    {
        const std::string k{ key };
        return obj.isMember(k) && obj[k].isString() ? obj[k].asString() : std::string{};
    }

    // Resolve a profile's display name by guid from profiles.list — the D19 profile-name
    // fallback the exported startup tree carries beside each entry's GUID.
    std::string lookupProfileName(const Json::Value& root, std::string_view bareGuid)
    {
        if (!root.isMember("profiles") || !root["profiles"].isObject() ||
            !root["profiles"].isMember("list") || !root["profiles"]["list"].isArray())
        {
            return {};
        }
        for (const auto& profile : root["profiles"]["list"])
        {
            if (profile.isObject() && guidEqual(jsonStr(profile, "guid"), bareGuid))
            {
                return jsonStr(profile, "name");
            }
        }
        return {};
    }

    // Serialize one WT profile JSON object to portable TOML text (spec: root README, Configuration):
    // [meta] + [profile] identity + the verbatim native.windows passthrough. Post-D19 the
    // launch entries are NOT here — the startup tree is global and goes to
    // startup-sessions.toml (buildStartupSessionsToml). `bare` is the brace-stripped uuid.
    std::string buildProfileToml(const Json::Value& profile, const std::string& bare)
    {
        std::ostringstream out;
        out << "# axan portable profile. Canonical config is each platform's native store;\n"
               "# this file is the portable mirror; import writes into the native store. Sparse: omit a\n"
               "# key to leave it untouched on import. The startup-session tree is global\n"
               "# (D19) and lives in startup-sessions.toml beside this file.\n\n";
        out << "[meta]\n";
        out << "axan-toml-version = " << Wire::kTomlVersion << "\n";
        out << "exported-by       = \"windows\"\n\n";

        out << "[profile]\n";
        out << "uuid = " << Wire::TomlQuote(bare) << "\n";
        if (profile.isMember("name") && profile["name"].isString())
        {
            out << "name = " << Wire::TomlQuote(profile["name"].asString()) << "\n";
        }
        out << "\n";

        // --- native passthrough: every other WT profile key, verbatim, under native.windows ---
        // "defaultLaunchEntries" stays excluded although the setting is retired (D19/#432): a
        // stale settings.json may still carry it, and it must not ride along as opaque native
        // data only to be re-imported later.
        static const std::unordered_set<std::string> kCoreKeys{ "guid", "name", "defaultLaunchEntries" };
        bool wroteNativeHeader = false;
        for (const auto& key : profile.getMemberNames())
        {
            if (kCoreKeys.count(key))
            {
                continue;
            }
            if (profile[key].isNull())
            {
                continue; // no TOML null; skip the key (see jsonToTomlValue)
            }
            if (!wroteNativeHeader)
            {
                out << "[profile.native.windows]\n";
                wroteNativeHeader = true;
            }
            out << tomlKey(key) << " = " << jsonToTomlValue(profile[key]) << "\n";
        }
        if (wroteNativeHeader)
        {
            out << "\n";
        }
        return out.str();
    }

    // Serialize the GLOBAL startup tree (settings.json "startupSessions", D19) as the portable
    // [[startup-sessions]] array-of-tables. Schema + rationale: src/inc/AxanLaunchEntryWire.h.
    // Each entry carries its referenced profile's GUID (bare) plus the profile-name fallback so
    // it re-resolves on a machine where the GUID differs; an empty profile ("follow the global
    // default") is omitted entirely, sparse like color/color-target.
    //
    // axan #3: separator rows carry kind/style/height/placement instead (ToTomlBlock emits
    // the right shape per kind). The [meta] version is decided AFTER the entries are
    // gathered — Wire::TomlVersionFor writes 1 for a separator-free tree (so it still
    // round-trips through a version-1-only reader) and 2 once any row is a separator.
    std::string buildStartupSessionsToml(const Json::Value& root)
    {
        std::vector<Wire::Entry> entries;
        const std::string sessionsKey{ Wire::JsonKey::StartupSessions };
        if (root.isMember(sessionsKey) && root[sessionsKey].isArray())
        {
            for (const auto& e : root[sessionsKey])
            {
                if (!e.isObject())
                {
                    continue;
                }
                Wire::Entry entry;
                entry.id = jsonStr(e, Wire::JsonKey::Id);
                entry.parentId = jsonStr(e, Wire::JsonKey::Parent);
                entry.kind = jsonStr(e, Wire::JsonKey::Kind);
                if (entry.IsSeparator())
                {
                    entry.style = jsonStr(e, Wire::JsonKey::Style);
                    entry.placement = jsonStr(e, Wire::JsonKey::Placement);
                    // height: written by LaunchEntry::ToJson as a double, but a hand-edited
                    // file may hold an integer ("height": 2); both are numeric to jsoncpp.
                    const std::string heightKey{ Wire::JsonKey::Height };
                    if (e.isMember(heightKey) && e[heightKey].isNumeric())
                    {
                        entry.height = e[heightKey].asDouble();
                    }
                    entry.height = Wire::NormalizeSeparatorHeight(entry.height);
                    entries.push_back(std::move(entry));
                    continue;
                }
                entry.profile = stripBraces(jsonStr(e, Wire::JsonKey::Profile));
                entry.profileName = entry.profile.empty() ? std::string{} : lookupProfileName(root, entry.profile);
                entry.name = jsonStr(e, Wire::JsonKey::Name);
                entry.directory = jsonStr(e, Wire::JsonKey::Directory);
                entry.command = jsonStr(e, Wire::JsonKey::Command);
                // icon: a known Segoe glyph exports as builtin:NAME for portability; anything
                // else (emoji, path, unmapped glyph) carries verbatim (D18 v1 contract).
                entry.icon = wToU8(Wire::ExportIcon(u8ToW(jsonStr(e, Wire::JsonKey::Icon))));
                entry.color = jsonStr(e, Wire::JsonKey::Color);
                entry.colorTarget = jsonStr(e, Wire::JsonKey::ColorTarget);
                entries.push_back(std::move(entry));
            }
        }

        std::ostringstream out;
        out << "# axan startup sessions — the global launch-entry tree.\n"
               "# Canonical config is settings.json (\"startupSessions\"); this file is the\n"
               "# portable mirror. Import it from the Settings > Startup sessions page on\n"
               "# another machine to carry the tree across.\n\n";
        out << "[meta]\n";
        out << "axan-toml-version = " << Wire::TomlVersionFor(entries) << "\n";
        out << "exported-by       = \"windows\"\n\n";
        for (const auto& entry : entries)
        {
            out << Wire::ToTomlBlock(entry);
        }
        return out.str();
    }

    // Write text to a file (plain, non-atomic — a fresh export artifact, not a live store).
    bool writeTextFile(const std::filesystem::path& outPath, const std::string& text, std::string& error)
    {
        std::ofstream file{ outPath, std::ios::binary | std::ios::trunc };
        if (!file)
        {
            error = "cannot open output file: " + outPath.string();
            return false;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file)
        {
            error = "write failed: " + outPath.string();
            return false;
        }
        return true;
    }

    // True iff `name` looks exactly like a mirror file this exporter writes:
    // a bare lowercase-or-uppercase uuid (8-4-4-4-12 hex) + ".toml". The prune path (#431
    // item 5) only ever deletes files matching this pattern — never anything else.
    bool isUuidTomlFilename(std::wstring_view name)
    {
        static constexpr std::wstring_view suffix{ L".toml" };
        static constexpr size_t uuidLen = 36;
        if (name.size() != uuidLen + suffix.size() || name.substr(uuidLen) != suffix)
        {
            return false;
        }
        for (size_t i = 0; i < uuidLen; ++i)
        {
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (name[i] != L'-')
                {
                    return false;
                }
            }
            else if (!std::iswxdigit(name[i]))
            {
                return false;
            }
        }
        return true;
    }

    std::wstring toLower(std::wstring_view s)
    {
        std::wstring out{ s };
        for (auto& c : out)
        {
            c = std::towlower(c);
        }
        return out;
    }
}

namespace Axan::ProfileToml
{
    int ExportAllProfiles(const std::filesystem::path& settingsPath,
                          const std::filesystem::path& outDir,
                          std::string& error)
    {
        std::string content;
        if (!readFile(settingsPath, content, error))
        {
            return -1;
        }
        Json::Value root;
        if (!parseSettings(content, root, error))
        {
            return -1;
        }
        if (!root.isMember("profiles") || !root["profiles"].isObject() ||
            !root["profiles"].isMember("list") || !root["profiles"]["list"].isArray())
        {
            error = "settings.json has no profiles.list";
            return -1;
        }

        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        if (ec)
        {
            error = "cannot create output directory " + outDir.string() + ": " + ec.message();
            return -1;
        }

        // The per-profile identity/passthrough mirrors.
        int written = 0;
        std::unordered_set<std::wstring> liveFilenames; // lowercase, for the prune below
        for (const auto& profile : root["profiles"]["list"])
        {
            if (!profile.isObject() || !profile.isMember("guid") || !profile["guid"].isString())
            {
                continue; // a profile with no guid can't be addressed portably; skip it
            }
            const auto bare = stripBraces(profile["guid"].asString());
            const auto filename = u8ToW(bare) + L".toml";
            liveFilenames.insert(toLower(filename));
            std::string writeErr;
            if (writeTextFile(outDir / filename, buildProfileToml(profile, bare), writeErr))
            {
                ++written;
            }
            else if (error.empty())
            {
                error = writeErr; // remember the first failure, keep going
            }
        }

        // The global startup tree (D19) — the file the Startup-sessions importer round-trips.
        {
            std::string writeErr;
            if (!writeTextFile(outDir / L"startup-sessions.toml", buildStartupSessionsToml(root), writeErr) && error.empty())
            {
                error = writeErr;
            }
        }

        // Prune stale mirrors for deleted profiles (#431 item 5). Only files matching the
        // <bare-uuid>.toml pattern axan itself writes are candidates; startup-sessions.toml
        // and anything user-dropped never match.
        for (const auto& dirEntry : std::filesystem::directory_iterator{ outDir, ec })
        {
            if (!dirEntry.is_regular_file(ec))
            {
                continue;
            }
            const auto filename = dirEntry.path().filename().wstring();
            if (isUuidTomlFilename(filename) && !liveFilenames.count(toLower(filename)))
            {
                std::error_code removeEc;
                std::filesystem::remove(dirEntry.path(), removeEc);
                if (removeEc && error.empty())
                {
                    error = "cannot prune stale mirror " + dirEntry.path().string() + ": " + removeEc.message();
                }
            }
        }

        return written;
    }
}
