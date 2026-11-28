#!/usr/bin/env bash
# Checks to run before opening or updating a pull request (Ubuntu or WSL).
#
#   keyboards/svalboard/tools/pre_pr.sh          check; change nothing
#   keyboards/svalboard/tools/pre_pr.sh --fix    apply the formatting, then check
#
# 1. Format: qmk format-c, format-python and format-text on every Svalboard
#    file (keyboards/svalboard, modules/svalboard, tests/sval_*), in the
#    qmk_cli image, so the formatter versions match svalboard-lint.yml.
# 2. Keyboard lint: qmk info for every svalboard/ keyboard, and the Svalboard
#    targets and aliases (qmk ci-validate-*).
# 3. The settings area budget (tools/settings_budget.py): fails on overflow,
#    warns at half full.
# 4. Every tests/sval_* suite, found by its files, so a new one is never left
#    out:
#    - util/updater_test/run.sh (tests/sval_updater and the C tests it
#      compiles) and util/test_native_ptp.sh (tests/sval_native_ptp);
#    - python3 -m unittest in every other folder with test_*.py files
#      (sval_storage, sval_pointing, sval_keytest, sval_layers,
#      sval_runtime_tap, ...);
#    - make test:<folder> for every folder with a test.mk, subfolders too,
#      and tests/sval_runtime_tap/build_non_sval.py (in the qmk_cli image);
#    - a check that fails if a tests/sval_* folder is run by none of these.
#
# Needs docker (for the qmk_cli image), cc and python3. Exits nonzero if any
# step fails. Releases run the same checks, and more, in release.yml.
set -u

FIX=0
case "${1:-}" in
    --fix) FIX=1 ;;
    "") ;;
    *) echo "usage: $0 [--fix]" >&2; exit 2 ;;
esac

root=$(git rev-parse --show-toplevel) || exit 2
cd "$root" || exit 2
IMAGE=${QMK_CLI_IMAGE:-ghcr.io/qmk/qmk_cli}
command -v docker > /dev/null || { echo "pre_pr: docker is required (the qmk_cli image)" >&2; exit 2; }

failed=()
step() { printf '\n== %s\n' "$1"; }
result() { # name exit
    if [ "$2" -eq 0 ]; then echo "-- ok: $1"; else echo "-- FAILED: $1"; failed+=("$1"); fi
}

# Format and keyboard lint, inside the qmk_cli image as the calling user.
# Without --fix the files are copied first and restored afterwards, so the
# check never changes the work tree.
step "Format and keyboard lint (qmk_cli image)"
# A worktree's git directory, and the lib/ links of an sval-wt worktree, live
# in the main clone: mount it too.
main=$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")
mounts=(-v "$root:$root")
[ "$main" != "$root" ] && mounts+=(-v "$main:$main")
docker run --rm -i --user "$(id -u):$(id -g)" -e HOME=/tmp -e FIX=$FIX \
    "${mounts[@]}" -w "$root" "$IMAGE" bash -s <<'IN'
set -u
git config --global --add safe.directory '*'
mapfile -t files < <(git ls-files -co --exclude-standard 'keyboards/svalboard/**' 'modules/svalboard/**' 'tests/sval_*/**')
[ ${#files[@]} -gt 0 ] || { echo "no Svalboard files found (git ls-files failed)"; exit 1; }
echo "${#files[@]} Svalboard files"
snap=$(mktemp -d)
tar -cf "$snap/before.tar" "${files[@]}"
mkdir "$snap/before" && tar -xf "$snap/before.tar" -C "$snap/before"
restore() { [ "$FIX" = 1 ] || tar -xf "$snap/before.tar"; }
trap restore EXIT
qmk format-c --core-only "${files[@]}" > /dev/null 2>&1
qmk format-python "${files[@]}" > /dev/null 2>&1
qmk format-text "${files[@]}" > /dev/null 2>&1
changed=()
for f in "${files[@]}"; do
    cmp -s "$f" "$snap/before/$f" || changed+=("$f")
done
fmt=0
if [ ${#changed[@]} -gt 0 ]; then
    if [ "$FIX" = 1 ]; then
        printf 'formatted: %s\n' "${changed[@]}"
    else
        for f in "${changed[@]}"; do diff -u "$snap/before/$f" "$f" | head -40; done
        printf 'needs formatting: %s\n' "${changed[@]}"
        echo "run with --fix to apply it"
        fmt=1
    fi
fi
restore; trap - EXIT
echo "-- format: $([ $fmt = 0 ] && echo ok || echo FAILED)"

info=0
for kb in $(qmk list-keyboards | grep '^svalboard/'); do
    qmk info -l --keyboard "$kb" > /tmp/info.txt 2>&1 || { echo "qmk info failed: $kb"; tail -5 /tmp/info.txt; info=1; }
done
echo "-- keyboard definitions: $([ $info = 0 ] && echo ok || echo FAILED)"

{ qmk ci-validate-keyboard-targets; qmk ci-validate-aliases; } > /tmp/validate.txt 2>&1
val=0
if grep -i svalboard /tmp/validate.txt; then val=1; fi
echo "-- targets and aliases: $([ $val = 0 ] && echo ok || echo FAILED)"
exit $((fmt + info + val))
IN
result "format and keyboard lint" $?

step "Updater host tests (util/updater_test/run.sh)"
log=$(mktemp)
bash util/updater_test/run.sh > "$log" 2>&1
ec=$?; [ $ec -eq 0 ] && tail -2 "$log" || tail -30 "$log"
result "updater host tests" $ec

step "Settings area budget (tools/settings_budget.py)"
python3 keyboards/svalboard/tools/settings_budget.py
result "settings area budget" $?

# The tests/sval_* suites, found by their files, so a new one is run without
# editing this script. Folders whose test_*.py files take arguments are run by
# their own scripts instead (own_runner).
own_runner="sval_updater sval_native_ptp"
py_suites=()
for d in tests/sval_*/; do
    d=${d%/}
    case " $own_runner " in *" ${d#tests/} "*) continue ;; esac
    compgen -G "$d/test_*.py" > /dev/null && py_suites+=("$d")
done
mapfile -t gtests < <(find tests -path 'tests/sval_*' -name test.mk -printf '%h\n' | sed 's|^tests/||' | sort)

step "Python host tests (python3 -m unittest, ${#py_suites[@]} folders)"
py=0
for d in "${py_suites[@]}"; do
    if python3 -m unittest discover -s "$d" -p 'test_*.py' > "$log" 2>&1; then
        echo "$d: $(grep -a '^Ran ' "$log")"
    else
        tail -30 "$log"
        echo "FAILED: $d"
        py=1
    fi
done
result "python host tests" $py

step "QMK host tests (make test:..., ${#gtests[@]} suites, qmk_cli image)"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp "${mounts[@]}" -w "$root" "$IMAGE" \
    bash -c 'git config --global --add safe.directory "*"; make "${@/#/test:}"' make "${gtests[@]}" > "$log" 2>&1
ec=$?
if [ $ec -eq 0 ]; then
    echo "${gtests[*]}"
    grep -ac PASSED "$log" | sed 's/$/ suites passed/'
else
    tail -30 "$log"
fi
result "QMK host tests" $ec

step "A non-Svalboard build with TT() and Grave Escape (tests/sval_runtime_tap/build_non_sval.py)"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp "${mounts[@]}" -w "$root" "$IMAGE" \
    bash -c 'git config --global --add safe.directory "*"; python3 tests/sval_runtime_tap/build_non_sval.py' > "$log" 2>&1
ec=$?; [ $ec -eq 0 ] && grep -a '^Linking' "$log" | tail -1 || tail -30 "$log"
rm -f handwired_onekey_rp2040_runtime_tap_*.uf2
result "non-Svalboard build" $ec

step "Native touchpad host tests (util/test_native_ptp.sh)"
bash util/test_native_ptp.sh > "$log" 2>&1
ec=$?; [ $ec -eq 0 ] && tail -1 "$log" || tail -30 "$log"
result "native touchpad host tests" $ec
rm -f "$log"

# Every tests/sval_* folder must be run by a step above: unittest, make test,
# or by name in util/updater_test/run.sh or util/test_native_ptp.sh.
step "Every tests/sval_* folder is run"
missed=()
for d in tests/sval_*/; do
    d=${d%/}
    n=${d#tests/}
    printf '%s\n' "${py_suites[@]}" | grep -qx "$d" && continue
    printf '%s\n' "${gtests[@]}" | grep -qE "^$n(/|$)" && continue
    grep -qE "tests/$n([/\"' ]|$)" util/updater_test/run.sh util/test_native_ptp.sh && continue
    missed+=("$d")
done
if [ ${#missed[@]} -gt 0 ]; then
    printf 'not run: %s\n' "${missed[@]}"
    echo "give it test_*.py files or a test.mk, or run it from util/updater_test/run.sh"
fi
result "every tests/sval_* folder is run" ${#missed[@]}

echo
if [ ${#failed[@]} -eq 0 ]; then
    echo "pre_pr: all checks passed"
else
    printf 'pre_pr: FAILED: %s\n' "${failed[@]}"
    exit 1
fi
