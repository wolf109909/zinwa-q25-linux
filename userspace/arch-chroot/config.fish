set -g fish_greeting ""

# Environment
fish_add_path -g $HOME/.local/bin $HOME/.cargo/bin /usr/local/sbin /usr/local/bin

set -gx EDITOR nvim
set -gx VISUAL nvim
set -gx PAGER "less -R"
set -gx COLORTERM truecolor

# CachyOS / Arch aliases & abbreviations
if type -q eza
    alias ls="eza --icons --group-directories-first"
    alias ll="eza -la --icons --group-directories-first"
    alias lt="eza --tree --level=2 --icons"
end

if type -q bat
    alias cat="bat --style=plain --paging=never"
end

alias v="nvim"
alias g="lazygit"
alias top="btop"
alias pacs="sudo pacman -S"
alias pacu="sudo pacman -Syu"
alias pacr="sudo pacman -Rns"
alias status-q25="/usr/local/bin/q25-status"
alias hotspot-on="/usr/local/bin/q25-hotspot on"
alias hotspot-off="/usr/local/bin/q25-hotspot off"
alias mouse-toggle="/usr/local/bin/q25-togglemouse"

# Keyboard cheatsheet helper
function keys
    echo -e "\033[1;36m=== Zinwa Q25 Physical Keyboard Shortcuts ===\033[0m"
    echo "  Call Answer (Left)   : Tab (Autocomplete)"
    echo "  Call End (Right)     : Tap = Esc | Hold = Ctrl"
    echo "  Back Button          : Tap = Back | Hold = Alt"
    echo "  LAlt (Bottom-Left)   : Shift (Tap = oneshot, 2x = lock)"
    echo "  RAlt (Sym)           : Numbers & Symbols (Tap = oneshot, 2x = lock)"
    echo "  RShift + T           : Toggle Trackpad Mouse / Arrow Keys"
    echo "  RShift + VolUp/Down  : Screen Brightness Up / Down"
    echo "  RShift + W/S/A/D     : PgUp / PgDn / Home / End"
    echo "  RShift + Call End    : Alt+F4 (Close window)"
    echo "  Sym + 0              : Toggle Keyboard Backlight"
end

if status is-interactive
    if type -q starship
        starship init fish | source
    end
    if type -q zoxide
        zoxide init fish | source
    end
    if type -q fzf
        fzf --fish | source
    end
    # Show fastfetch once per terminal session
    if not set -q Q25_FETCH_SHOWN
        set -gx Q25_FETCH_SHOWN 1
        fastfetch
        echo -e " \033[0;90mTip: run \033[1;32mkeys\033[0;90m for Q25 shortcuts, \033[1;32mstatus-q25\033[0;90m for battery/net, \033[1;32mhotspot-on\033[0;90m for AP.\033[0m\n"
    end
end
