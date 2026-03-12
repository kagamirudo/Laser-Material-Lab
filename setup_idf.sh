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
        "$HOME/esp-idf-v5.5.2"
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
    printf "  Would you like to clone ESP-IDF v5.5.2 to ${_W}~/esp-idf${_N}? [y/N] "
    read -r _answer
    case "$_answer" in
        [yY]|[yY][eE][sS])
            _info "Cloning ESP-IDF v5.5.2 (this may take a few minutes)..."
            if ! git clone --recursive -b v5.5.2 https://github.com/espressif/esp-idf.git "$HOME/esp-idf"; then
                _err "Failed to clone ESP-IDF."
                IDF_SETUP_OK=1; return 1
            fi
            _idf_dir="$HOME/esp-idf"
            _ok "ESP-IDF cloned to $_idf_dir"
            ;;
        *)
            _err "Aborted. Install ESP-IDF manually and re-run:"
            _err "  git clone --recursive -b v5.5.2 https://github.com/espressif/esp-idf.git ~/esp-idf"
            IDF_SETUP_OK=1; return 1
            ;;
    esac
fi

_info "Using ESP-IDF at: $_idf_dir"

# ── 2b. Version check — ensure we're on v5.5.2 ──────────────
_IDF_REQUIRED_TAG="v5.5.2"

_current_tag=""
if [ -d "$_idf_dir/.git" ]; then
    _current_tag="$(git -C "$_idf_dir" describe --tags --exact-match 2>/dev/null || true)"
fi

if [ "$_current_tag" != "$_IDF_REQUIRED_TAG" ]; then
    if [ -n "$_current_tag" ]; then
        _warn "ESP-IDF is on $_current_tag, but this project requires $_IDF_REQUIRED_TAG."
    else
        _warn "Could not determine ESP-IDF version (expected $_IDF_REQUIRED_TAG)."
    fi
    printf "\n"
    printf "  Upgrade ESP-IDF in ${_W}$_idf_dir${_N} to ${_W}$_IDF_REQUIRED_TAG${_N}? [y/N] "
    read -r _answer
    case "$_answer" in
        [yY]|[yY][eE][sS])
            _info "Fetching latest tags..."
            git -C "$_idf_dir" fetch --tags || { _err "git fetch failed."; IDF_SETUP_OK=1; return 1; }
            _info "Checking out $_IDF_REQUIRED_TAG..."
            git -C "$_idf_dir" checkout "$_IDF_REQUIRED_TAG" || { _err "git checkout failed."; IDF_SETUP_OK=1; return 1; }
            _info "Updating submodules (this may take a few minutes)..."
            git -C "$_idf_dir" submodule update --init --recursive || { _err "Submodule update failed."; IDF_SETUP_OK=1; return 1; }
            _ok "ESP-IDF upgraded to $_IDF_REQUIRED_TAG."
            _needs_install=1
            ;;
        *)
            _err "Aborted. This project requires ESP-IDF $_IDF_REQUIRED_TAG."
            _err "Upgrade manually:  cd $_idf_dir && git fetch --tags && git checkout $_IDF_REQUIRED_TAG && git submodule update --init --recursive"
            IDF_SETUP_OK=1; return 1
            ;;
    esac
else
    _ok "ESP-IDF version $_IDF_REQUIRED_TAG confirmed."
fi

unset _current_tag _IDF_REQUIRED_TAG

export IDF_PATH="$_idf_dir"

# ── 3. Run install.sh if tools are missing ──────────────────
# _needs_install may already be set to 1 by the version upgrade step
if [ "${_needs_install:-0}" -ne 1 ]; then
    _needs_install=0
    if [ ! -d "$HOME/.espressif/tools" ]; then
        _needs_install=1
    elif ! ls "$HOME/.espressif/python_env"/idf*_py* >/dev/null 2>&1; then
        _needs_install=1
    fi
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
_label_w=14
_val_w=${#_idf_ver}
[ ${#IDF_PATH}   -gt $_val_w ] && _val_w=${#IDF_PATH}
[ ${#IDF_TARGET} -gt $_val_w ] && _val_w=${#IDF_TARGET}
[ ${#_py_path}   -gt $_val_w ] && _val_w=${#_py_path}

_inner=$(( _label_w + _val_w + 4 ))
_title="ESP-IDF environment ready"
_title_len=${#_title}
[ $_title_len -gt $(( _inner - 2 )) ] && _inner=$(( _title_len + 2 ))

_hline=""; _i=0; while [ $_i -lt $_inner ]; do _hline="${_hline}─"; _i=$((_i+1)); done

_pad_r() {
    local text="$1" width="$2"
    printf "%-${width}s" "$text"
}

_row() {
    local label="$1" value="$2"
    local content
    content="$(printf "  %-${_label_w}s %s" "$label" "$value")"
    printf "${_G}│${_N} %s${_G}%*s│${_N}\n" "$content" $(( _inner - ${#content} )) ""
}

_title_pad_l=$(( (_inner - _title_len) / 2 ))
_title_pad_r=$(( _inner - _title_len - _title_pad_l ))

printf "\n"
printf "${_G}┌${_hline}┐${_N}\n"
printf "${_G}│${_N}%*s${_W}%s${_N}%*s${_G}│${_N}\n" $_title_pad_l "" "$_title" $_title_pad_r ""
printf "${_G}├${_hline}┤${_N}\n"
_row "IDF version :" "$_idf_ver"
_row "IDF path    :" "$IDF_PATH"
_row "Target      :" "$IDF_TARGET"
_row "Python      :" "$_py_path"
printf "${_G}├${_hline}┤${_N}\n"
_hint="idf.py build  or  make"
_hint_pad_l=$(( (_inner - ${#_hint}) / 2 ))
_hint_pad_r=$(( _inner - ${#_hint} - _hint_pad_l ))
printf "${_G}│${_N}%*s${_C}%s${_N}%*s${_G}│${_N}\n" $_hint_pad_l "" "$_hint" $_hint_pad_r ""
printf "${_G}└${_hline}┘${_N}\n"
printf "\n"

unset _label_w _val_w _inner _title _title_len _hline _i
unset _title_pad_l _title_pad_r _hint _hint_pad_l _hint_pad_r
unset -f _pad_r _row

IDF_SETUP_OK=0
unset _idf_dir _idf_ver _py_path _answer
unset -f _find_idf _info _ok _warn _err
unset _R _G _Y _B _C _W _N
