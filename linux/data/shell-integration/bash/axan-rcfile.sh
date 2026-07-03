# axan bash shell-integration wrapper.
#
# Loaded via `bash --rcfile <this>` when axan spawns a non-login interactive
# bash and shell integration is enabled (see TERMINAL_SETTING_SHELL_INTEGRATION_KEY).
#
# Why this exists: VTE's OSC 7 cwd-reporting and OSC 133 prompt-mark hooks
# live in /etc/profile.d/vte-2.91.sh (or vte.sh on Arch-family). That file
# is only sourced for login shells, but axan profiles default to login-shell
# = false to match gnome-terminal behavior. Without these hooks,
# vte_terminal_get_current_directory_uri() returns NULL for the lifetime of
# the shell, breaking right-click Copy Path / Copy Branch and any future
# feature that depends on cwd reporting. See lonelyquasar/axan#405.
#
# Passing --rcfile suppresses bash's normal ~/.bashrc read, so we re-source
# it explicitly first to preserve the user's environment, then layer the VTE
# integration on top.

if [ -r "${HOME}/.bashrc" ]; then
    . "${HOME}/.bashrc"
fi

# Try distro-specific paths in order of prevalence (Debian/Ubuntu/Fedora
# first, then Arch). First readable hit wins.
for __axan_vte_sh in \
    /etc/profile.d/vte-2.91.sh \
    /etc/profile.d/vte.sh \
    /usr/etc/profile.d/vte-2.91.sh \
    /usr/etc/profile.d/vte.sh; do
    if [ -r "${__axan_vte_sh}" ]; then
        . "${__axan_vte_sh}"
        break
    fi
done
unset __axan_vte_sh
