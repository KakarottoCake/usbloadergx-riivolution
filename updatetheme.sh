#!/bin/sh

GETTHEME_C="$1"
THEME_FILE="$2"
TMP_DEFAULTS="$(mktemp)"
TMP_OUT="$(mktemp)"

# Extract setMSG("msgid", "msgstr") pairs from LoadNewTheme()
awk '
/void[ \t]+LoadNewTheme[ \t]*\(/,/\}/ {
    # Handle block comments
    if ($0 ~ /\/\*/) in_comment = 1
    if (in_comment) {
        if ($0 ~ /\*\//) in_comment = 0
        next
    }
    # Ignore single-line comments
    if ($0 ~ /^[ \t]*\/\//) next
    # Ignore inline comments after code
    sub(/[ \t]*\/\/.*$/, "", $0)
    if ($0 ~ /setMSG\("/) {
        match($0, /setMSG\("([^"]+)",[ \t]*"([^"]*)"\)/, arr)
        if (arr[1] != "") {
            print arr[1] "\t" arr[2]
        }
    }
}
' "$GETTHEME_C" > "$TMP_DEFAULTS"

# Update .them file
awk -v defs="$TMP_DEFAULTS" '
BEGIN {
    FS = "\t"
    while ((getline < defs) > 0) {
        theme_defaults[$1] = $2
    }
    FS = ""
}
{
    if ($1 == "m" && substr($0,1,5) == "msgid") {
        key = substr($0, 8, length($0)-8)
        print $0
        getline
        if ($1 == "m" && substr($0,1,6) == "msgstr") {
            val = theme_defaults[key]
            if (val != "" || (key in theme_defaults)) {
                printf("msgstr \"%s\"\n", val)
            } else {
                print $0
            }
        } else {
            print $0
        }
    } else {
        print $0
    }
}
' "$THEME_FILE" > "$TMP_OUT" && mv "$TMP_OUT" "$THEME_FILE"

rm -f "$TMP_DEFAULTS"