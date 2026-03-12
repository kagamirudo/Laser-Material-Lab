#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# setup_idf.sh — Bootstrap ESP-IDF environment for this project
#
# Usage:  source setup_idf.sh
#
# Handles conda / virtualenv / pyenv conflicts automatically.
# Safe to run repeatedly (idempotent).
# ──────────────────────────────────────────────────────────────

# ── Source-only guard ────────────────────────────────────────
_sourced=0
if [ -n "${ZSH_EVAL_CONTEXT-}" ]; then
    case $ZSH_EVAL_CONTEXT in *:file*) _sourced=1 ;; esac
elif [ -n "${BASH_VERSION-}" ]; then
    (return 0 2>/dev/null) && _sourced=1
fi
if [ "$_sourced" -eq 0 ]; then
    echo "ERROR: This script must be sourced, not executed."
    echo ""
    echo "  source setup_idf.sh"
    echo ""
    exit 1
fi
unset _sourced

# ── Color helpers (disabled when not a terminal) ─────────────
if [ -t 1 ]; then
    _R='\033[0;31m' _G='\033[0;32m' _Y='\033[0;33m'
    _B='\033[0;34m' _C='\033[0;36m' _W='\033[1m' _N='\033[0m'
else
    _R='' _G='' _Y='' _B='' _C='' _W='' _N=''
fi
_info()  { printf "${_C}[idf-setup]${_N} %s\n" "$*"; }
_ok()    { printf "${_G}[idf-setup]${_N} %s\n" "$*"; }
_warn()  { printf "${_Y}[idf-setup]${_N} %s\n" "$*"; }
_err()   { printf "${_R}[idf-setup]${_N} %s\n" "$*"; }

IDF_SETUP_OK=0

# ── 1. Deactivate conflicting Python environments ───────────
_deactivated_something=0

# Conda — deactivate all stacked environments (including base)
if [ "${CONDA_SHLVL:-0}" -gt 0 ] 2>/dev/null; then
    _info "Deactivating conda (active: ${CONDA_DEFAULT_ENV:-base})..."
    while [ "${CONDA_SHLVL:-0}" -gt 0 ] 2>/dev/null; do
        conda deactivate 2>/dev/null
    done
    _ok "Conda fully deactivated."
    _deactivated_something=1
fi

# Virtualenv / venv
if [ -n "${VIRTUAL_ENV-}" ]; then
    _info "Deactivating virtualenv ($VIRTUAL_ENV)..."
    deactivate 2>/dev/null || true
    _ok "Virtualenv deactivated."
    _deactivated_something=1
fi

# Pyenv virtualenv
if [ -n "${PYENV_VIRTUAL_ENV-}" ]; then
    _info "Deactivating pyenv-virtualenv ($PYENV_VIRTUAL_ENV)..."
    pyenv deactivate 2>/dev/null || true
    _ok "Pyenv virtualenv deactivated."
    _deactivated_something=1
fi

if [ "$_deactivated_something" -eq 0 ]; then
    _info "No conflicting Python environments detected."
fi
unset _deactivated_something

# ── 2. Locate ESP-IDF ───────────────────────────────────────
_find_idf() {
    local candidates=(
        "${IDF_PATH-}"
        "$HOME/esp-idf"
        "$HOME/esp/esp-idf"
        "$HOME/esp-idf-v5.5.1"
    )

    # Also glob for any versioned installs
    for d in "$HOME"/esp-idf-v*; do
        [ -d "$d" ] && candidates+=("$d")
    done

    for dir in "${candidates[@]}"; do
        [ -z "$dir" ] && continue
        if [ -f "$dir/export.sh" ]; then
            echo "$dir"
            return 0
        fi
    done
    return 1
}

_idf_dir="$(_find_idf)"

if [ -z "$_idf_dir" ]; then
    _warn "ESP-IDF not found in any standard location."
    printf "\n"
    printf "  Would you like to clone ESP-IDF v5.5.1 to ${_W}~/esp-idf${_N}? [y/N] "
    read -r _answer
    case "$_answer" in
        [yY]|[yY][eE][sS])
            _info "Cloning ESP-IDF v5.5.1 (this may take a few minutes)..."
            if ! git clone --recursive -b v5.5.1 https://github.com/espressif/esp-idf.git "$HOME/esp-idf"; then
                _err "Failed to clone ESP-IDF."
                IDF_SETUP_OK=1; return 1
            fi
            _idf_dir="$HOME/esp-idf"
            _ok "ESP-IDF cloned to $_idf_dir"
            ;;
        *)
            _err "Aborted. Install ESP-IDF manually and re-run:"
            _err "  git clone --recursive -b v5.5.1 https://github.com/espressif/esp-idf.git ~/esp-idf"
            IDF_SETUP_OK=1; return 1
            ;;
    esac
fi

_info "Using ESP-IDF at: $_idf_dir"
export IDF_PATH="$_idf_dir"

# ── 3. Run install.sh if tools are missing ──────────────────
_needs_install=0

if [ ! -d "$HOME/.espressif/tools" ]; then
    _needs_install=1
elif ! ls "$HOME/.espressif/python_env"/idf*_py* >/dev/null 2>&1; then
    _needs_install=1
fi

if [ "$_needs_install" -eq 1 ]; then
    _info "Running ESP-IDF install for esp32s3 target..."
    if ! "$IDF_PATH/install.sh" esp32s3; then
        _err "install.sh failed. Check output above."
        IDF_SETUP_OK=1; return 1
    fi
    _ok "ESP-IDF tools installed."
else
    _info "ESP-IDF tools already installed — skipping install.sh"
fi
unset _needs_install

# ── 4. Source export.sh ─────────────────────────────────────
_info "Sourcing ESP-IDF export.sh..."
if ! . "$IDF_PATH/export.sh"; then
    _err "export.sh failed. Try running install.sh manually:"
    _err "  $IDF_PATH/install.sh esp32s3"
    IDF_SETUP_OK=1; return 1
fi

# ── 5. Set project defaults ────────────────────────────────
export IDF_TARGET=esp32s3

# ── 6. Verify ──────────────────────────────────────────────
if ! command -v idf.py >/dev/null 2>&1; then
    _err "idf.py not found on PATH after sourcing export.sh."
    IDF_SETUP_OK=1; return 1
fi

_idf_ver="$(idf.py --version 2>/dev/null || echo 'unknown')"
_py_path="$(command -v python 2>/dev/null || echo 'unknown')"

# ── 7. Summary ─────────────────────────────────────────────
printf "\n"
printf "${_G}╔══════════════════════════════════════════════╗${_N}\n"
printf "${_G}║${_N}  ${_W}ESP-IDF environment ready${_N}                   ${_G}║${_N}\n"
printf "${_G}╠══════════════════════════════════════════════╣${_N}\n"
printf "${_G}║${_N}  IDF version : %-27s ${_G}║${_N}\n" "$_idf_ver"
printf "${_G}║${_N}  IDF path    : %-27s ${_G}║${_N}\n" "$IDF_PATH"
printf "${_G}║${_N}  Target      : %-27s ${_G}║${_N}\n" "$IDF_TARGET"
printf "${_G}║${_N}  Python      : %-27s ${_G}║${_N}\n" "$_py_path"
printf "${_G}╚══════════════════════════════════════════════╝${_N}\n"
printf "\n"
_ok "Run 'idf.py build' or 'make' to build the project."

IDF_SETUP_OK=0
unset _idf_dir _idf_ver _py_path _answer
unset -f _find_idf _info _ok _warn _err
unset _R _G _Y _B _C _W _N
