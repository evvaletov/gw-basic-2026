# Shared helpers for run_tests.sh and run_compiler_tests.sh.

GW_TEST_TIMEOUT=${GW_TEST_TIMEOUT-5}
GW_TEST_COMPILE_TIMEOUT=${GW_TEST_COMPILE_TIMEOUT-60}

for setting in GW_TEST_TIMEOUT GW_TEST_COMPILE_TIMEOUT; do
    value=${!setting}
    if [[ ! "$value" =~ ^[0-9]+([.][0-9]+)?$ ]] ||
       ! awk -v seconds="$value" 'BEGIN { exit !(seconds > 0) }'; then
        echo "ERROR: $setting must be a positive number of seconds" >&2
        exit 1
    fi
done

runtime_timeout() {
    case "$1" in
        # PLAY drains the audio stream after each note. play_music takes about
        # 14 seconds with a live PulseAudio server on the development machine.
        play_music.bas|play_scale.bas|sound_test.bas|speaker_out.bas) echo 30 ;;
        on_timer.bas|timer_stop.bas) echo 10 ;;
        *) echo "$GW_TEST_TIMEOUT" ;;
    esac
}

report_process_failure() {
    local stage="$1" name="$2" status="$3" limit="$4" log="$5"
    local label="$stage-FAIL"
    case "$status" in
        124) label="$stage-TIMEOUT" ;;
        137) label="$stage-KILLED" ;;
    esac
    printf "  %s  %s (exit %s; limit %ss)\n" "$label" "$name" "$status" "$limit"
    cat "$log"
}

# Return 0 for a match, 1 for a mismatch, and 2 for a comparison error.
# Preserve leading spaces; only normalize CRs, trailing spaces and blank lines.
compare_output() {
    local expected="$1" actual="$2" work="$3"
    sed 's/\r//g; s/[[:space:]]*$//; /^$/d' "$expected" > "$work/expected.normalized" || return 2
    sed 's/\r//g; s/[[:space:]]*$//; /^$/d' "$actual" > "$work/actual.normalized" || return 2
    diff -u --label "$expected" --label "actual output" \
        "$work/expected.normalized" "$work/actual.normalized"
}
