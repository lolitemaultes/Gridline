#!/bin/sh
# Gridline Suite / Gridline Suite Mini: the pick-your-plugins installer for Linux and the BSDs.
#
# It runs from the unpacked installer (the .run file unpacks itself and starts it), next to:
#   suite.txt        "<suite id>\t<suite name>\t<version>"
#   catalogue.tsv    "<type>\t<name>\t<instrument|effect>\t<category>", one plugin per line
#   engine/          the suite's bundles for this processor: "<suite name>.vst3" and
#                    "<suite name>.clap" with "<suite name>-assets" beside it
#
# Every plugin of a suite is the same engine; a member list in the bundle (gridline-members.txt)
# says which plugins hosts see. So whatever you pick, one bundle per format is installed:
#   VST3  ~/.vst3/<suite name>.vst3                        (--system: /usr/local/lib/vst3)
#   CLAP  ~/.clap/<suite name>.clap and its -assets folder (--system: /usr/local/lib/clap)
# Running it again changes the picks. A receipt of every file it wrote is kept in
# ~/.local/share/gridline/suite/<suite id>/ (--system: /usr/local/share/gridline/suite/<suite id>/),
# with an "uninstall" script. /usr/lib belongs to the package manager and is never touched.
#
# Options:
#   --list                     the plugins, then exit
#   --plugins LIST             comma separated: plugin names, types (gl.plasma), all, instruments, effects
#   --formats LIST             vst3,clap (default: both)
#   --user | --system          this user's folders (default) or the system's (needs root)
#   --dest DIR                 install into DIR/vst3 and DIR/clap instead (receipt in DIR/share)
#   --replace                  replace a copy Gridline itself put there (otherwise it is kept)
#   --no-update-check          the plugins do not look for new versions by themselves
#   --yes                      no questions: the options above, or everything
#   --uninstall                remove what this installer put there
#   --text                     the text picker (the only one here; accepted for scripts)
set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OWNER="suite-installer"

say() { printf '%s\n' "$*"; }
die() { printf 'Error: %s\n' "$*" >&2; exit 1; }

IFS_TAB=$(printf '\t')
read_suite() {
  [ -f "$HERE/suite.txt" ] || die "suite.txt is missing next to the installer"
  SUITE_ID=$(cut -f1 "$HERE/suite.txt"); SUITE_NAME=$(cut -f2 "$HERE/suite.txt"); VERSION=$(cut -f3 "$HERE/suite.txt")
}

PLUGINS=""; FORMATS=""; SCOPE=""; DEST=""; YES=0; UNINSTALL=0; LIST=0; REPLACE=0; NOUPDATE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --list) LIST=1 ;;
    --plugins) shift; PLUGINS=${1:-} ;;
    --plugins=*) PLUGINS=${1#*=} ;;
    --formats) shift; FORMATS=${1:-} ;;
    --formats=*) FORMATS=${1#*=} ;;
    --user) SCOPE=user ;;
    --system) SCOPE=system ;;
    --dest) shift; DEST=${1:-}; SCOPE=dest ;;
    --dest=*) DEST=${1#*=}; SCOPE=dest ;;
    --replace) REPLACE=1 ;;
    --no-update-check) NOUPDATE=1 ;;
    --yes|-y) YES=1 ;;
    --uninstall) UNINSTALL=1 ;;
    --text) ;;
    -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
  shift
done

read_suite
if [ -z "$SCOPE" ]; then
  if [ "$(id -u)" = 0 ]; then SCOPE=system; else SCOPE=user; fi
fi
case "$SCOPE" in
  user) VST3_DIR="$HOME/.vst3"; CLAP_DIR="$HOME/.clap"; SHARE="${XDG_DATA_HOME:-$HOME/.local/share}/gridline/suite/$SUITE_ID" ;;
  system) VST3_DIR=/usr/local/lib/vst3; CLAP_DIR=/usr/local/lib/clap; SHARE="/usr/local/share/gridline/suite/$SUITE_ID" ;;
  dest) [ -n "$DEST" ] || die "--dest needs a folder"; mkdir -p "$DEST" && DEST=$(cd "$DEST" && pwd)
        VST3_DIR="$DEST/vst3"; CLAP_DIR="$DEST/clap"; SHARE="$DEST/share/gridline/suite/$SUITE_ID" ;;
esac
if [ "$SCOPE" = system ] && [ "$(id -u)" != 0 ]; then die "--system needs root (sudo), or install for this user with --user"; fi
RECEIPT="$SHARE/receipt.txt"

ask() {  # ask "question" default -> answer in REPLY
  if [ "$YES" = 1 ] || [ ! -t 0 ]; then REPLY=$2; return; fi
  printf '%s [%s]: ' "$1" "$2"
  read -r REPLY || REPLY=$2
  [ -n "$REPLY" ] || REPLY=$2
}
yes_no() {  # yes_no "question" y|n -> status 0 for yes
  ask "$1 (y/n)" "$2"
  case "$REPLY" in [Yy]*) return 0 ;; *) return 1 ;; esac
}

# ---------------------------------------------------------------- uninstall
uninstall() {
  if [ ! -f "$RECEIPT" ]; then say "$SUITE_NAME is not installed by this installer in $(dirname "$VST3_DIR")."; return 0; fi
  while IFS= read -r p; do
    [ -n "$p" ] && [ -e "$p" ] && rm -rf -- "$p" && say "Removed $p"
  done < "$RECEIPT"
  rm -rf -- "$SHARE"
  say "$SUITE_NAME is removed. Rescan plugins in your music program."
}
if [ "$UNINSTALL" = 1 ]; then uninstall; exit 0; fi

# ---------------------------------------------------------------- the plugins
N=$(grep -c . "$HERE/catalogue.tsv")
if [ "$LIST" = 1 ]; then
  say "$SUITE_NAME $VERSION: $N plugins"
  awk -F'\t' '{ printf "%4d  %-24s %-11s %s\n", NR, $2, $3, $4 }' "$HERE/catalogue.tsv"
  exit 0
fi

lower() { printf '%s' "$1" | tr 'A-Z' 'a-z'; }
# Picks: line numbers of catalogue.tsv, one per line, from a list of words.
picks_from_words() {
  out=""
  old_ifs=$IFS; IFS=','
  for w in $1; do
    IFS=$old_ifs
    w=$(printf '%s' "$w" | sed 's/^ *//; s/ *$//'); lw=$(lower "$w")
    case "$lw" in
      "") ;;
      all) out="$out $(awk '{print NR}' "$HERE/catalogue.tsv")" ;;
      instruments) out="$out $(awk -F'\t' '$3=="instrument"{print NR}' "$HERE/catalogue.tsv")" ;;
      effects) out="$out $(awk -F'\t' '$3=="effect"{print NR}' "$HERE/catalogue.tsv")" ;;
      *[!0-9-]*)
        n=$(awk -F'\t' -v w="$lw" 'tolower($1)==w || tolower($2)==w {print NR; exit}' "$HERE/catalogue.tsv")
        [ -n "$n" ] || die "no plugin called \"$w\" (see --list)"
        out="$out $n" ;;
      *-*) a=${lw%-*}; b=${lw#*-}; [ -n "$a" ] && [ -n "$b" ] || die "bad range: $w"
           out="$out $(awk -v a="$a" -v b="$b" 'BEGIN{for(i=a;i<=b;i++)print i}')" ;;
      *) out="$out $lw" ;;
    esac
    IFS=','
  done
  IFS=$old_ifs
  for n in $out; do
    [ "$n" -ge 1 ] 2>/dev/null && [ "$n" -le "$N" ] || die "no plugin number $n (1 to $N)"
    echo "$n"
  done | sort -n -u
}

say "$SUITE_NAME $VERSION: choose plugins"
if [ -z "$PLUGINS" ]; then
  if [ "$YES" = 1 ] || [ ! -t 0 ]; then PLUGINS=all
  else
    awk -F'\t' '{ printf "%4d  %-24s %-11s %s\n", NR, $2, $3, $4 }' "$HERE/catalogue.tsv"
    say "Pick by number (3,5,10-12), by name, or all, instruments, effects."
    ask "Plugins" all; PLUGINS=$(printf '%s' "$REPLY" | tr ' ' ',')
  fi
fi
PICKS=$(picks_from_words "$PLUGINS") || exit 1
[ -n "$PICKS" ] || die "no plugins picked"
TYPES=$(for n in $PICKS; do sed -n "${n}p" "$HERE/catalogue.tsv" | cut -f1; done)

ENGINE="$HERE/engine"
HAVE=""
[ -d "$ENGINE/$SUITE_NAME.vst3" ] && HAVE="$HAVE vst3"
[ -f "$ENGINE/$SUITE_NAME.clap" ] && HAVE="$HAVE clap"
if [ -z "$FORMATS" ]; then
  FORMATS=$(echo $HAVE | tr ' ' ',')
  if [ "$YES" != 1 ] && [ -t 0 ]; then ask "Formats:$(echo "$HAVE" | sed 's/ /, /g; s/^,//')" "$FORMATS"; FORMATS=$REPLY; fi
fi
FMTS=""
old_ifs=$IFS; IFS=', '
for f in $FORMATS; do
  f=$(lower "$f")
  case " $HAVE " in *" $f "*) FMTS="$FMTS $f" ;; *) die "format \"$f\" is not in this installer (it has:$HAVE)" ;; esac
done
IFS=$old_ifs
[ -n "$FMTS" ] || die "no formats picked"

# ---------------------------------------------------------------- install
MEMBERS="# Gridline plugin bundle: the plugins it shows to hosts
suite $SUITE_ID
$TYPES"
NEW_RECEIPT=$(mktemp "${TMPDIR:-/tmp}/gridline-receipt.XXXXXX") || die "no temporary file"
trap 'rm -f "$NEW_RECEIPT"' EXIT
[ -f "$RECEIPT" ] && cat "$RECEIPT" > "$NEW_RECEIPT"

ours() {  # ours <file or bundle path>: installed by this installer (its ownership file says so)
  case "$1" in *.clap) o="${1%.clap}-assets/installed-by.txt" ;; *) o="$1/Contents/Resources/installed-by.txt" ;; esac
  [ -f "$o" ] && grep -q "^$OWNER" "$o"
}
place() {  # place <format> <target folder>
  fmt=$1; dir=$2
  case "$fmt" in
    vst3) target="$dir/$SUITE_NAME.vst3"; pkgcopy="/usr/lib/vst3/$SUITE_NAME.vst3" ;;
    clap) target="$dir/$SUITE_NAME.clap"; pkgcopy="/usr/lib/clap/$SUITE_NAME.clap" ;;
  esac
  if [ -e "$pkgcopy" ] && [ "$SCOPE" != dest ]; then
    say "Skipped $fmt: your Gridline package already provides $SUITE_NAME system-wide ($pkgcopy), with every plugin."
    return 0
  fi
  if [ -e "$target" ] && ! ours "$target"; then
    if [ "$REPLACE" = 1 ] || { [ "$YES" != 1 ] && yes_no "Gridline already put $SUITE_NAME (every plugin) in $dir. Replace it with your picks?" n; }; then :
    else say "Kept the copy Gridline put in $dir ($fmt)."; return 0; fi
  fi
  mkdir -p "$dir" || die "cannot create $dir"
  # Single-plugin downloads of this suite in the same folder would show plugins twice.
  if [ -d "$dir/Gridline Suite" ] && ls "$dir/Gridline Suite" 2>/dev/null | grep -q .; then
    say "Note: $dir/Gridline Suite holds single-plugin downloads; remove the ones you picked here so they do not show twice."
  fi
  rm -rf -- "$target"
  if [ "$fmt" = clap ]; then
    rm -rf -- "${target%.clap}-assets"
    cp -p "$ENGINE/$SUITE_NAME.clap" "$target" && cp -Rp "$ENGINE/$SUITE_NAME-assets" "${target%.clap}-assets" || die "copy failed into $dir"
    meta="${target%.clap}-assets"
    printf '%s\n%s\n' "$target" "$meta" >> "$NEW_RECEIPT"
  else
    cp -Rp "$ENGINE/$SUITE_NAME.vst3" "$target" || die "copy failed into $dir"
    meta="$target/Contents/Resources"; mkdir -p "$meta"
    printf '%s\n' "$target" >> "$NEW_RECEIPT"
  fi
  printf '%s\n' "$MEMBERS" > "$meta/gridline-members.txt"
  printf '%s %s\n' "$OWNER" "$VERSION" > "$meta/installed-by.txt"
  say "Installed $fmt: $target"
}
for f in $FMTS; do
  case "$f" in vst3) place vst3 "$VST3_DIR" ;; clap) place clap "$CLAP_DIR" ;; esac
done

# The receipt and an uninstall script (a copy of this installer and its lists, without the engine).
mkdir -p "$SHARE" || die "cannot create $SHARE"
sort -u "$NEW_RECEIPT" > "$RECEIPT"
cp "$HERE/setup.sh" "$HERE/suite.txt" "$HERE/catalogue.tsv" "$SHARE/" 2>/dev/null
extra=""; [ "$SCOPE" = dest ] && extra=" --dest '$DEST'"; [ "$SCOPE" = system ] && extra=" --system"
printf '#!/bin/sh\nexec sh "$(dirname "$0")/setup.sh" --uninstall%s "$@"\n' "$extra" > "$SHARE/uninstall"
chmod +x "$SHARE/uninstall"

# The plugins' daily update check: on unless turned off here (this user only).
if [ "$NOUPDATE" = 0 ] && [ "$YES" != 1 ] && [ -t 0 ] && [ "$SCOPE" != system ]; then
  yes_no "Let the plugins check for updates (once a day, no personal data)?" y || NOUPDATE=1
fi
if [ "$NOUPDATE" = 1 ] && [ "$SCOPE" != system ]; then
  cfg="${GRIDLINE_CONFIG_HOME:-${XDG_CONFIG_HOME:-$HOME/.config}}/gridline"; state="$cfg/update-check.json"
  mkdir -p "$cfg"
  if [ -f "$state" ] && grep -q '"pluginsAuto"' "$state"; then
    sed 's/"pluginsAuto" *: *true/"pluginsAuto":false/' "$state" > "$state.tmp" && mv "$state.tmp" "$state"
  elif [ -f "$state" ]; then
    sed 's/^{/{"pluginsAuto":false,/' "$state" > "$state.tmp" && mv "$state.tmp" "$state"
  else
    printf '{"pluginsAuto":false}\n' > "$state"
  fi
  say "The plugins will not check for updates by themselves (turn it back on in any plugin's menu)."
fi
say "Done: $(echo "$PICKS" | grep -c .) plugins. Rescan plugins in your music program. To remove: $SHARE/uninstall"
