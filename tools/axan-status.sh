#!/usr/bin/env bash
# Emit one OSC 7501 Program Status report to the current terminal, for manually testing
# axan's sidebar session state. Run it in an axan pane (or redirect stdout to inspect it).
#
# Usage: axan-status.sh <state> [--kind permission|question|auth] [--msg "text"]
#                       [--title "text"] [--progress N] [--id a/b] [--app name]
#        axan-status.sh --query     send the feature-detection query
#        axan-status.sh --demo      walk working -> blocked (question) -> done
# state: idle | working | done | blocked | error | clear
set -u

usage() {
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit "${1:-2}"
}

b64() { printf '%s' "$1" | base64 | tr -d '\n'; }

# send <state> [key=value ...] : build and write the sequence to stdout.
emit() {
    local out="state=$1"
    shift
    local kv
    for kv in "$@"; do out="$out:$kv"; done
    printf '\033]7501;%s\033\' "$out"
}

state=""
kind="" msg="" title="" progress="" id="" app=""
have_msg=0 have_title=0
query=0 demo=0

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage 0 ;;
        --query) query=1 ;;
        --demo) demo=1 ;;
        --kind|--msg|--title|--progress|--id|--app)
            [ $# -ge 2 ] || { echo "axan-status: $1 needs a value" >&2; exit 2; }
            case "$1" in
                --kind) kind=$2 ;;
                --msg) msg=$2; have_msg=1 ;;
                --title) title=$2; have_title=1 ;;
                --progress) progress=$2 ;;
                --id) id=$2 ;;
                --app) app=$2 ;;
            esac
            shift
            ;;
        -*) echo "axan-status: unknown option $1" >&2; usage ;;
        *)
            [ -z "$state" ] || { echo "axan-status: only one state allowed" >&2; exit 2; }
            state=$1
            ;;
    esac
    shift
done

if [ "$query" = 1 ]; then
    printf '\033]7501;?\033\'
    exit 0
fi

# build_report <state> : sets REPORT_ARGS from the current flag variables.
send() {
    local st=$1 k=$2 m=$3 hm=$4
    case "$st" in
        idle|working|done|blocked|error|clear) ;;
        *) echo "axan-status: state must be idle|working|done|blocked|error|clear" >&2; exit 2 ;;
    esac
    local args=()
    [ -n "$id" ] && args+=("id=$id")
    [ -n "$app" ] && args+=("app=$app")
    [ -n "$k" ] && args+=("kind=$k")
    [ -n "$progress" ] && args+=("progress=$progress")
    [ "$have_title" = 1 ] && args+=("title=$(b64 "$title")")
    [ "$hm" = 1 ] && args+=("msg=$(b64 "$m")")
    emit "$st" "${args[@]+"${args[@]}"}"
}

if [ "$demo" = 1 ]; then
    echo "axan-status demo: working" >&2
    send working "" "Refactoring the parser" 1
    sleep 3
    echo "axan-status demo: blocked (question)" >&2
    send blocked question "Which branch should I target?" 1
    sleep 3
    echo "axan-status demo: done" >&2
    send done "" "Finished" 1
    echo "axan-status demo: end (focus the tab to acknowledge; 'clear' removes it)" >&2
    exit 0
fi

[ -n "$state" ] || usage
send "$state" "$kind" "$msg" "$have_msg"
