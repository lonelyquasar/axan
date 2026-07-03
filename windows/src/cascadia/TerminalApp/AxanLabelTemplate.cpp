// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#include "pch.h"
#include "AxanLabelTemplate.h"

#include <filesystem>
#include <fstream>
#include <optional>

namespace
{
    // Read an environment variable into a wstring, "" if unset. Win32 (thread-safe,
    // unlike _wgetenv) since label computation can run off the throttled OutputIdle path.
    std::wstring _env(const wchar_t* name)
    {
        const auto needed = GetEnvironmentVariableW(name, nullptr, 0);
        if (needed == 0)
        {
            return {};
        }
        std::wstring value(needed, L'\0');
        const auto written = GetEnvironmentVariableW(name, value.data(), needed);
        value.resize(written);
        return value;
    }

    // Lowercase ASCII in place (template var names are ASCII identifiers).
    void _asciiLowerInPlace(std::wstring& s)
    {
        for (auto& c : s)
        {
            if (c >= L'A' && c <= L'Z')
            {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
        }
    }

    // Treat '/' and '\' the same and ignore case, the way Windows paths compare. Used
    // only for the $~ home-prefix test; the displayed path keeps its original casing.
    wchar_t _foldPathChar(wchar_t c)
    {
        if (c == L'/')
        {
            return L'\\';
        }
        if (c >= L'A' && c <= L'Z')
        {
            return static_cast<wchar_t>(c - L'A' + L'a');
        }
        return c;
    }

    // Strip a defensive "file://[host]" scheme if a cwd ever arrives as a URI. WT's
    // OSC 9;9 path is already plain, so this is belt-and-suspenders for OSC-7-style input.
    std::wstring_view _stripFileScheme(std::wstring_view path)
    {
        constexpr std::wstring_view scheme{ L"file://" };
        if (path.substr(0, scheme.size()) == scheme)
        {
            path.remove_prefix(scheme.size());
            // Drop an optional host segment up to the next '/'.
            if (const auto slash = path.find(L'/'); slash != std::wstring_view::npos)
            {
                path.remove_prefix(slash);
            }
        }
        return path;
    }

    // Basename of a path, tolerating trailing separators ("C:\foo\" -> "foo"). Returns ""
    // for a bare root. std::filesystem handles drive letters, forward slashes and \\wsl$.
    std::wstring _basename(std::wstring_view path)
    {
        path = _stripFileScheme(path);
        while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/'))
        {
            path.remove_suffix(1);
        }
        if (path.empty())
        {
            return {};
        }
        return std::filesystem::path{ path }.filename().wstring();
    }

    // Collapse a leading user-profile prefix to "~" ("C:\Users\me\repo" -> "~\repo").
    // Matches only on a path-component boundary so C:\Users\meadow isn't collapsed.
    std::wstring _collapseHome(std::wstring_view cwd)
    {
        cwd = _stripFileScheme(cwd);
        if (cwd.empty())
        {
            return {};
        }
        const auto home = _env(L"USERPROFILE");
        if (home.empty() || cwd.size() < home.size())
        {
            return std::wstring{ cwd };
        }
        for (size_t i = 0; i < home.size(); ++i)
        {
            if (_foldPathChar(cwd[i]) != _foldPathChar(home[i]))
            {
                return std::wstring{ cwd };
            }
        }
        // The match must end at a separator or end-of-string.
        if (cwd.size() != home.size() && cwd[home.size()] != L'\\' && cwd[home.size()] != L'/')
        {
            return std::wstring{ cwd };
        }
        return L"~" + std::wstring{ cwd.substr(home.size()) };
    }

    // Is `p` rooted somewhere we're willing to do filesystem probes? The cwd arrives via
    // OSC 9;9 — terminal output — so it's attacker-influenceable (displaying a malicious
    // file can emit any escape sequence). Probing exists() under an arbitrary UNC root
    // makes Windows open an SMB session to the named host: that blocks on network
    // timeouts and leaks a NetNTLM handshake to an attacker-chosen server. Allowed:
    //  * local drive-letter roots ("C:\..."), including drives the user mapped themselves;
    //  * the WSL filesystem providers "\\wsl$\..." and "\\wsl.localhost\..." (the
    //    documented B.5 behavior: WSL repos resolve identically to local ones).
    // Every other UNC / network-provider / device root is refused.
    bool _isAllowedGitWalkRoot(const std::filesystem::path& p)
    {
        std::wstring root = p.root_name().native();
        for (auto& c : root)
        {
            c = _foldPathChar(c);
        }
        if (root.size() == 2 && root[1] == L':' && root[0] >= L'a' && root[0] <= L'z')
        {
            return true;
        }
        return root == L"\\\\wsl$" || root == L"\\\\wsl.localhost";
    }

    // Walk up from `startDir` for a directory containing a ".git" entry; return that
    // toplevel or "" if none before the root. A .git *file* (worktree/submodule pointer)
    // counts the same — we only need git to consider this a repo. std::filesystem walks
    // \\wsl$ UNC paths natively (B.5: WSL repos resolve identically to local ones).
    // Only roots passing _isAllowedGitWalkRoot are walked; see there for why. Callers
    // (_gitBranchAt's .git/HEAD read included) only ever touch paths this returned, so
    // this is the single validation point for the OSC-supplied cwd.
    std::wstring _findGitToplevel(std::wstring_view startDir)
    {
        startDir = _stripFileScheme(startDir);
        if (startDir.empty())
        {
            return {};
        }
        std::error_code ec;
        std::filesystem::path cur{ startDir };
        // A bare POSIX path ("/home/x") isn't rooted on a Windows drive; don't chase it.
        if (!cur.has_root_name() && !cur.is_absolute())
        {
            return {};
        }
        // An attacker-named network root ("\\evil-host\share\...") is never contacted.
        if (!_isAllowedGitWalkRoot(cur))
        {
            return {};
        }
        for (;;)
        {
            if (std::filesystem::exists(cur / L".git", ec))
            {
                return cur.wstring();
            }
            auto parent = cur.parent_path();
            if (parent.empty() || parent == cur)
            {
                return {};
            }
            cur = std::move(parent);
        }
    }

    // Read up to `max` bytes, return the trimmed first line as UTF-8-decoded text.
    std::wstring _readFirstLine(const std::filesystem::path& path, size_t max)
    {
        std::ifstream stream{ path, std::ios::binary };
        if (!stream)
        {
            return {};
        }
        std::string buf(max, '\0');
        stream.read(buf.data(), static_cast<std::streamsize>(max));
        buf.resize(static_cast<size_t>(stream.gcount()));
        if (const auto nl = buf.find('\n'); nl != std::string::npos)
        {
            buf.resize(nl);
        }
        while (!buf.empty() && (buf.back() == ' ' || buf.back() == '\t' || buf.back() == '\r'))
        {
            buf.pop_back();
        }
        if (buf.empty())
        {
            return {};
        }
        const auto needed = MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0);
        if (needed <= 0)
        {
            return {};
        }
        std::wstring out(needed, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), out.data(), needed);
        return out;
    }

    // Parse the branch out of "<top>/.git/HEAD" ("ref: refs/heads/<branch>"). Detached
    // HEAD (raw SHA), an unreadable file, or a .git-file worktree all yield "" — we keep
    // the label short rather than show a 40-char hash.
    std::wstring _gitBranchAt(std::wstring_view gitToplevel)
    {
        if (gitToplevel.empty())
        {
            return {};
        }
        const auto head = std::filesystem::path{ gitToplevel } / L".git" / L"HEAD";
        auto line = _readFirstLine(head, 256);
        constexpr std::wstring_view prefix{ L"ref: refs/heads/" };
        if (line.substr(0, prefix.size()) == prefix)
        {
            return line.substr(prefix.size());
        }
        return {};
    }

    // ${file:path} body. '~' (alone or '~/...') expands to the user profile; everything
    // else passes through. Reads the first line so a sidecar file can drive a label.
    std::wstring _readFileVar(std::wstring_view path)
    {
        if (path.empty())
        {
            return {};
        }
        std::wstring resolved;
        if (path[0] == L'~' && (path.size() == 1 || path[1] == L'/' || path[1] == L'\\'))
        {
            resolved = _env(L"USERPROFILE");
            resolved.append(path.substr(1));
        }
        else
        {
            resolved.assign(path);
        }
        return _readFirstLine(std::filesystem::path{ resolved }, 128);
    }

    // Look up one variable. std::nullopt means "unknown name" so the expander can emit it
    // literally; an empty string means "known but currently has no value".
    std::optional<std::wstring> _lookupVar(std::wstring_view rawName, const Axan::LabelContext& ctx)
    {
        // $~ — home-collapsed cwd. Spotted before normalization, which would drop the '~'.
        if (rawName == L"~")
        {
            return _collapseHome(ctx.cwd);
        }

        // Normalize: lowercase + strip '_' and '-' so $host_name == $hostname.
        std::wstring key;
        key.reserve(rawName.size());
        for (const auto c : rawName)
        {
            if (c != L'_' && c != L'-')
            {
                key.push_back(c);
            }
        }
        _asciiLowerInPlace(key);

        if (key == L"pwd" || key == L"cwd")
        {
            return std::wstring{ _stripFileScheme(ctx.cwd) };
        }
        if (key == L"dir")
        {
            return _basename(ctx.cwd);
        }
        if (key == L"user")
        {
            return _env(L"USERNAME");
        }
        if (key == L"host" || key == L"hostname")
        {
            return _env(L"COMPUTERNAME");
        }
        if (key == L"shell")
        {
            return ctx.shell;
        }
        if (key == L"cmd" || key == L"title")
        {
            return ctx.title;
        }
        if (key == L"branch" || key == L"gitbranch")
        {
            return _gitBranchAt(_findGitToplevel(ctx.cwd));
        }
        if (key == L"repo" || key == L"gitrepo")
        {
            return _basename(_findGitToplevel(ctx.cwd));
        }
        return std::nullopt;
    }
}

namespace Axan
{
    std::wstring ExpandLabelTemplate(std::wstring_view tmpl, const LabelContext& ctx)
    {
        std::wstring out;
        out.reserve(tmpl.size());
        size_t i = 0;
        while (i < tmpl.size())
        {
            if (tmpl[i] != L'$')
            {
                out.push_back(tmpl[i++]);
                continue;
            }

            // Brace form: ${...}. No nesting/escaping — the body is a path or identifier,
            // neither of which legitimately contains '}'. Today only "file:" is recognized;
            // anything else (or an unterminated brace) emits literally so the user sees it.
            if (i + 1 < tmpl.size() && tmpl[i + 1] == L'{')
            {
                const auto bodyStart = i + 2;
                const auto close = tmpl.find(L'}', bodyStart);
                if (close == std::wstring_view::npos)
                {
                    out.append(L"${");
                    i += 2;
                    continue;
                }
                const auto body = tmpl.substr(bodyStart, close - bodyStart);
                constexpr std::wstring_view filePrefix{ L"file:" };
                if (body.substr(0, filePrefix.size()) == filePrefix)
                {
                    out.append(_readFileVar(body.substr(filePrefix.size())));
                }
                else
                {
                    out.push_back(L'$');
                    out.push_back(L'{');
                    out.append(body);
                    out.push_back(L'}');
                }
                i = close + 1;
                continue;
            }

            // $~ — special single-char name (it isn't in the identifier charset).
            if (i + 1 < tmpl.size() && tmpl[i + 1] == L'~')
            {
                if (const auto val = _lookupVar(L"~", ctx))
                {
                    out.append(*val);
                }
                i += 2;
                continue;
            }

            // $name — collect an [A-Za-z0-9_-]+ identifier.
            const auto nameStart = i + 1;
            auto end = nameStart;
            while (end < tmpl.size())
            {
                const auto c = tmpl[end];
                const auto isIdent = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                                     (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
                if (!isIdent)
                {
                    break;
                }
                ++end;
            }
            if (end == nameStart)
            {
                // Lone '$' — emit literally.
                out.push_back(L'$');
                ++i;
                continue;
            }
            const auto name = tmpl.substr(nameStart, end - nameStart);
            if (const auto val = _lookupVar(name, ctx))
            {
                out.append(*val);
            }
            else
            {
                // Unknown name: emit "$name" so the typo is visible.
                out.push_back(L'$');
                out.append(name);
            }
            i = end;
        }
        return out;
    }

    std::wstring DefaultLabel(const LabelContext& ctx)
    {
        if (!ctx.title.empty())
        {
            return ctx.title;
        }
        if (auto base = _basename(ctx.cwd); !base.empty() && base != L"/" && base != L"\\")
        {
            return base;
        }
        return L"shell";
    }

    std::wstring ComputeLabel(std::wstring_view tmpl, const LabelContext& ctx)
    {
        if (!tmpl.empty())
        {
            return ExpandLabelTemplate(tmpl, ctx);
        }
        return DefaultLabel(ctx);
    }
}
