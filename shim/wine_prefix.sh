# Sourced by the Wine checks. `wine_prefix DIR` seeds DIR with the prefix release/ci.sh booted once (FH2MP_WINE_TEMPLATE), so each
# check starts in about 0.2 s instead of booting its own prefix for about 5 s. Without a template Wine creates DIR as before.
wine_prefix() {
    if [ -n "${FH2MP_WINE_TEMPLATE:-}" ] && [ ! -e "$1" ]; then
        cp -a --reflink=auto "$FH2MP_WINE_TEMPLATE" "$1"
    fi
}
