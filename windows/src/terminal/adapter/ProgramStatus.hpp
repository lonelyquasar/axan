// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

// axan #29: OSC 7501 Program Status Protocol (spec rev 0.4, 2026-10-10).
//
//   OSC 7501 ; ? ST                      feature detection (query)
//   OSC 7501 ; key=value(:key=value)* ST  report
//
// This header is the wire-format half: a pure, allocation-light parser that turns the OSC
// payload into either a query, a fully validated Report, or a discard with a reason. The
// per-terminal record store lives in TerminalCore (Terminal::_programStatus*), which is
// where the lifetime rules (RIS, OSC 133;A, connection close, the record cap) apply.
//
// Validation rules (spec section "Errors", with the choices the spec leaves open pinned
// down here so they are tested in one place):
// - A malformed pair (no '=', empty key, key not [a-z]+, or a value byte outside
//   [A-Za-z0-9_.,+/=-]) is skipped; the rest of the report is still processed.
// - Unknown keys are ignored. A repeated key: the last value wins.
// - The whole report is discarded (nothing applied) on: payload over the sequence limit,
//   a key over 16 bytes, an `app` over 32 bytes, a missing/unknown `state`, an invalid
//   `id`, base64 that doesn't decode, decoded text over its limit / not UTF-8 / holding a
//   control character. Key>16 and app>32 are treated as limit violations (discard), the
//   simplest reading of the spec's limits table; an `app` within 32 bytes but outside
//   [A-Za-z0-9_.+-] is treated as absent, as the spec's key table says.
// - `kind` only applies to `blocked`, `progress` only to `working`/`blocked`; an invalid
//   value for either is treated as absent.
// - Discarding on an invalid id includes `state=clear`: a malformed id must never fall
//   back to the root record (or, for clear, wipe everything). That is why an `id` pair
//   is never "skipped as malformed": any invalid id pair, even one followed by a valid
//   id pair, discards the whole report.
//
// Bidi controls (U+061C, U+200E/F, U+202A-E, U+2066-9) are stripped from decoded title/msg
// so display code never has to remember to (spec SHOULD for text shown outside the grid).

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Console::VirtualTerminal::ProgramStatus
{
    enum class State : uint8_t
    {
        Idle,
        Working,
        Done,
        Blocked,
        Error,
        Clear, // not a record state: removes the addressed record + subtree (or all)
    };

    enum class Kind : uint8_t
    {
        None,
        Permission,
        Question,
        Auth,
    };

    // Limits from the spec's limits table.
    inline constexpr size_t MaxSequenceBytes = 4096; // OSC through ST
    inline constexpr size_t MaxKeyBytes = 16;
    inline constexpr size_t MaxAppBytes = 32;
    inline constexpr size_t MaxMsgEncodedBytes = 2732;
    inline constexpr size_t MaxMsgDecodedBytes = 2048;
    inline constexpr size_t MaxTitleEncodedBytes = 256;
    inline constexpr size_t MaxTitleDecodedBytes = 192;
    inline constexpr size_t MaxIdBytes = 128;
    inline constexpr size_t MaxIdSegmentBytes = 32;
    inline constexpr size_t MaxIdSegments = 8;
    inline constexpr size_t MaxRecords = 256;

    // `ESC ] 7501 ;` plus the shortest ST (BEL). The payload limit is the sequence limit
    // minus this framing. The payload is counted in UTF-16 code units, which equals bytes
    // for every character a valid report can contain (all ASCII); a non-ASCII character
    // is malformed wherever it appears, so undercounting it can't admit a bad report.
    inline constexpr size_t SequenceFramingBytes = 8;
    inline constexpr size_t MaxPayloadChars = MaxSequenceBytes - SequenceFramingBytes;

    // The reply payload (without OSC/ST framing). Leads with the `?` pair, then the
    // optional capability lists. `clear` is not listed: it isn't a state.
    inline constexpr std::wstring_view QueryReply = L"7501;?:states=idle,working,done,blocked,error:kinds=permission,question,auth";

    struct Report
    {
        std::wstring id; // empty = root record (or "all records" for State::Clear)
        State state{ State::Idle };
        std::wstring app; // empty = absent (display inherits from the nearest ancestor)
        Kind kind{ Kind::None }; // only ever set for State::Blocked
        std::optional<uint8_t> progress; // only for Working/Blocked; nullopt = indeterminate
        std::wstring title; // decoded; empty = absent
        std::wstring msg; // decoded; empty = absent
    };

    enum class Outcome : uint8_t
    {
        Query,
        Report,
        Discarded,
    };

    struct ParseResult
    {
        Outcome outcome{ Outcome::Discarded };
        Report report; // meaningful only for Outcome::Report
        std::string_view reason; // static string, set for Outcome::Discarded
        size_t skippedPairs{ 0 }; // malformed pairs skipped (Report outcome)
    };

    namespace details
    {
        constexpr bool IsSpace(wchar_t ch) noexcept
        {
            return ch == L' ' || ch == L'\t';
        }

        constexpr std::wstring_view Trim(std::wstring_view s) noexcept
        {
            while (!s.empty() && IsSpace(s.front()))
            {
                s.remove_prefix(1);
            }
            while (!s.empty() && IsSpace(s.back()))
            {
                s.remove_suffix(1);
            }
            return s;
        }

        constexpr bool IsAlnum(wchar_t ch) noexcept
        {
            return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9');
        }

        // value := [A-Za-z0-9_.,+/=-]*
        constexpr bool IsValueChar(wchar_t ch) noexcept
        {
            return IsAlnum(ch) || ch == L'_' || ch == L'.' || ch == L',' || ch == L'+' || ch == L'/' || ch == L'=' || ch == L'-';
        }

        // segment / app := [A-Za-z0-9_.+-]
        constexpr bool IsNameChar(wchar_t ch) noexcept
        {
            return IsAlnum(ch) || ch == L'_' || ch == L'.' || ch == L'+' || ch == L'-';
        }

        constexpr bool IsKey(std::wstring_view key) noexcept
        {
            if (key.empty())
            {
                return false;
            }
            for (const auto ch : key)
            {
                if (ch < L'a' || ch > L'z')
                {
                    return false;
                }
            }
            return true;
        }

        // id := segment ("/" segment)* ; segment := [A-Za-z0-9_.+-]{1,32}; <= 8 deep, <= 128 bytes.
        constexpr bool IsValidId(std::wstring_view id) noexcept
        {
            if (id.empty() || id.size() > MaxIdBytes)
            {
                return false;
            }
            size_t segments = 1;
            size_t segmentLength = 0;
            for (const auto ch : id)
            {
                if (ch == L'/')
                {
                    if (segmentLength == 0 || ++segments > MaxIdSegments)
                    {
                        return false;
                    }
                    segmentLength = 0;
                }
                else if (!IsNameChar(ch) || ++segmentLength > MaxIdSegmentBytes)
                {
                    return false;
                }
            }
            return segmentLength != 0;
        }

        constexpr int Base64Value(wchar_t ch) noexcept
        {
            if (ch >= L'A' && ch <= L'Z')
            {
                return ch - L'A';
            }
            if (ch >= L'a' && ch <= L'z')
            {
                return ch - L'a' + 26;
            }
            if (ch >= L'0' && ch <= L'9')
            {
                return ch - L'0' + 52;
            }
            if (ch == L'+')
            {
                return 62;
            }
            if (ch == L'/')
            {
                return 63;
            }
            return -1;
        }

        // Standard alphabet, padding optional but correct when present. Returns false on any
        // character outside the alphabet, misplaced '=', or an impossible length.
        inline bool DecodeBase64(std::wstring_view in, std::string& out)
        {
            out.clear();
            size_t padding = 0;
            while (!in.empty() && in.back() == L'=')
            {
                in.remove_suffix(1);
                ++padding;
            }
            if (padding > 2)
            {
                return false;
            }
            // With padding, the padded length must be a multiple of 4.
            if (padding != 0 && (in.size() + padding) % 4 != 0)
            {
                return false;
            }
            if (in.size() % 4 == 1)
            {
                return false;
            }
            out.reserve(in.size() * 3 / 4);
            uint32_t acc = 0;
            int bits = 0;
            for (const auto ch : in)
            {
                const auto v = Base64Value(ch);
                if (v < 0)
                {
                    return false; // includes '=' in the middle
                }
                acc = (acc << 6) | static_cast<uint32_t>(v);
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back(static_cast<char>((acc >> bits) & 0xFF));
                }
            }
            return true;
        }

        // Strict UTF-8 -> UTF-16: rejects overlongs, surrogates, > U+10FFFF, truncation.
        // Also rejects C0/C1 controls and DEL (spec: decoded text MUST NOT contain them).
        // Returns the failure reason, or an empty view on success.
        inline std::string_view DecodeUtf8Text(const std::string& in, std::wstring& out)
        {
            out.clear();
            out.reserve(in.size());
            size_t i = 0;
            while (i < in.size())
            {
                const auto b0 = static_cast<uint8_t>(in[i]);
                uint32_t cp = 0;
                size_t len = 0;
                uint32_t min = 0;
                if (b0 < 0x80)
                {
                    cp = b0;
                    len = 1;
                }
                else if ((b0 & 0xE0) == 0xC0)
                {
                    cp = b0 & 0x1F;
                    len = 2;
                    min = 0x80;
                }
                else if ((b0 & 0xF0) == 0xE0)
                {
                    cp = b0 & 0x0F;
                    len = 3;
                    min = 0x800;
                }
                else if ((b0 & 0xF8) == 0xF0)
                {
                    cp = b0 & 0x07;
                    len = 4;
                    min = 0x10000;
                }
                else
                {
                    return "bad-utf8";
                }
                if (i + len > in.size())
                {
                    return "bad-utf8";
                }
                for (size_t k = 1; k < len; ++k)
                {
                    const auto b = static_cast<uint8_t>(in[i + k]);
                    if ((b & 0xC0) != 0x80)
                    {
                        return "bad-utf8";
                    }
                    cp = (cp << 6) | (b & 0x3F);
                }
                if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                {
                    return "bad-utf8";
                }
                if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F))
                {
                    return "control-char";
                }
                i += len;

                // Strip bidi controls (display-safety SHOULD); everything else is kept.
                if (cp == 0x061C || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069))
                {
                    continue;
                }
                if (cp >= 0x10000)
                {
                    cp -= 0x10000;
                    out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                    out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
                }
                else
                {
                    out.push_back(static_cast<wchar_t>(cp));
                }
            }
            return {};
        }

        // Decode a base64 text field with its encoded/decoded limits. Returns the discard
        // reason, or an empty view on success.
        inline std::string_view DecodeTextField(std::wstring_view encoded, size_t maxEncoded, size_t maxDecoded, std::wstring& out)
        {
            if (encoded.size() > maxEncoded)
            {
                return "text-too-long";
            }
            std::string bytes;
            if (!DecodeBase64(encoded, bytes))
            {
                return "bad-base64";
            }
            if (bytes.size() > maxDecoded)
            {
                return "text-too-long";
            }
            return DecodeUtf8Text(bytes, out);
        }

        inline std::optional<State> ParseState(std::wstring_view v) noexcept
        {
            if (v == L"idle")
            {
                return State::Idle;
            }
            if (v == L"working")
            {
                return State::Working;
            }
            if (v == L"done")
            {
                return State::Done;
            }
            if (v == L"blocked")
            {
                return State::Blocked;
            }
            if (v == L"error")
            {
                return State::Error;
            }
            if (v == L"clear")
            {
                return State::Clear;
            }
            return std::nullopt;
        }

        inline Kind ParseKind(std::wstring_view v) noexcept
        {
            if (v == L"permission")
            {
                return Kind::Permission;
            }
            if (v == L"question")
            {
                return Kind::Question;
            }
            if (v == L"auth")
            {
                return Kind::Auth;
            }
            return Kind::None;
        }

        // Integer 0..100, digits only; anything else is "absent" (indeterminate).
        inline std::optional<uint8_t> ParseProgress(std::wstring_view v) noexcept
        {
            if (v.empty() || v.size() > 3)
            {
                return std::nullopt;
            }
            unsigned value = 0;
            for (const auto ch : v)
            {
                if (ch < L'0' || ch > L'9')
                {
                    return std::nullopt;
                }
                value = value * 10 + (ch - L'0');
            }
            if (value > 100)
            {
                return std::nullopt;
            }
            return static_cast<uint8_t>(value);
        }

        inline ParseResult Discard(std::string_view reason)
        {
            ParseResult r;
            r.outcome = Outcome::Discarded;
            r.reason = reason;
            return r;
        }
    }

    // Parse the OSC 7501 payload (everything after "7501;", without the terminator).
    inline ParseResult Parse(std::wstring_view payload)
    {
        using namespace details;

        if (payload.size() > MaxPayloadChars)
        {
            return Discard("sequence-too-long");
        }

        // Feature detection. Whitespace around the '?' is tolerated.
        if (Trim(payload) == L"?")
        {
            ParseResult r;
            r.outcome = Outcome::Query;
            return r;
        }

        std::optional<std::wstring_view> state, id, app, kind, progress, title, msg;
        size_t skipped = 0;

        size_t pos = 0;
        while (pos <= payload.size())
        {
            const auto end = std::min(payload.find(L':', pos), payload.size());
            const auto pair = payload.substr(pos, end - pos);
            pos = end + 1;

            const auto eq = pair.find(L'=');
            if (eq == std::wstring_view::npos)
            {
                ++skipped; // also covers empty segments, e.g. a trailing ':'
                continue;
            }
            const auto key = Trim(pair.substr(0, eq));
            const auto value = Trim(pair.substr(eq + 1));
            if (!IsKey(key))
            {
                ++skipped;
                continue;
            }
            if (key.size() > MaxKeyBytes)
            {
                return Discard("key-too-long");
            }
            bool valueOk = true;
            for (const auto ch : value)
            {
                if (!IsValueChar(ch))
                {
                    valueOk = false;
                    break;
                }
            }
            if (!valueOk)
            {
                // A malformed pair is normally just skipped, but never for `id`: skipping
                // it would leave the report addressed to the root, and for `state=clear`
                // that wipes every record (Ghostty's issue 14790). Any bad id discards.
                if (key == L"id")
                {
                    return Discard("invalid-id");
                }
                ++skipped;
                continue;
            }

            if (key == L"state")
            {
                state = value;
            }
            else if (key == L"id")
            {
                // Validated per pair, so an invalid id anywhere discards the report even
                // if a later id pair is valid: the sender's addressing is untrustworthy.
                if (!IsValidId(value))
                {
                    return Discard("invalid-id");
                }
                id = value;
            }
            else if (key == L"app")
            {
                app = value;
            }
            else if (key == L"kind")
            {
                kind = value;
            }
            else if (key == L"progress")
            {
                progress = value;
            }
            else if (key == L"title")
            {
                title = value;
            }
            else if (key == L"msg")
            {
                msg = value;
            }
            // Unknown keys are ignored.
        }

        if (!state)
        {
            return Discard("missing-state");
        }
        const auto parsedState = ParseState(*state);
        if (!parsedState)
        {
            return Discard("unknown-state");
        }

        ParseResult result;
        result.outcome = Outcome::Report;
        result.skippedPairs = skipped;
        auto& report = result.report;
        report.state = *parsedState;

        if (id)
        {
            report.id.assign(*id);
        }

        if (app)
        {
            if (app->size() > MaxAppBytes)
            {
                return Discard("app-too-long");
            }
            bool appOk = !app->empty();
            for (const auto ch : *app)
            {
                appOk = appOk && IsNameChar(ch);
            }
            if (appOk)
            {
                report.app.assign(*app);
            }
        }

        if (title)
        {
            if (const auto why = DecodeTextField(*title, MaxTitleEncodedBytes, MaxTitleDecodedBytes, report.title); !why.empty())
            {
                return Discard(why);
            }
        }
        if (msg)
        {
            if (const auto why = DecodeTextField(*msg, MaxMsgEncodedBytes, MaxMsgDecodedBytes, report.msg); !why.empty())
            {
                return Discard(why);
            }
        }

        if (kind && report.state == State::Blocked)
        {
            report.kind = ParseKind(*kind);
        }
        if (progress && (report.state == State::Working || report.state == State::Blocked))
        {
            report.progress = ParseProgress(*progress);
        }

        return result;
    }

    constexpr std::string_view StateName(State state) noexcept
    {
        switch (state)
        {
        case State::Idle:
            return "idle";
        case State::Working:
            return "working";
        case State::Done:
            return "done";
        case State::Blocked:
            return "blocked";
        case State::Error:
            return "error";
        case State::Clear:
            return "clear";
        }
        return "?";
    }

    constexpr std::string_view KindName(Kind kind) noexcept
    {
        switch (kind)
        {
        case Kind::Permission:
            return "permission";
        case Kind::Question:
            return "question";
        case Kind::Auth:
            return "auth";
        case Kind::None:
            break;
        }
        return "";
    }

    // Live states are dropped on process exit and on a new shell prompt (OSC 133;A).
    constexpr bool IsLive(State state) noexcept
    {
        return state == State::Working || state == State::Blocked;
    }
}
