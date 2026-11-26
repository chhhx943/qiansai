#!/usr/bin/env bash
set -euo pipefail

traceability_file="${1:-docs/recovery/recovery-traceability.md}"

if [[ ! -f "$traceability_file" ]]; then
    echo "FAIL: traceability file not found: $traceability_file" >&2
    exit 1
fi

awk -F'|' '
function trim(value) {
    sub(/^[[:space:]]+/, "", value)
    sub(/[[:space:]]+$/, "", value)
    return value
}
function fail(message) {
    print "FAIL: " message > "/dev/stderr"
    failures++
}

/^\| ID \| design clause \| automated test \| environment \| status \| evidence \|$/ {
    header_count++
    next
}

/^\| R[0-9]+ / {
    if (NF != 8) {
        fail("R row has " (NF - 1) " columns at line " NR)
        next
    }

    id = trim($2)
    clause = trim($3)
    test_name = trim($4)
    status = trim($6)
    evidence = trim($7)
    number = substr(id, 2) + 0

    if (number < 1 || number > 165) fail("out-of-range row " id " at line " NR)
    if (seen[number]++) fail("duplicate row " id " at line " NR)
    if (clause != "§18 " id) fail("clause mismatch for " id " at line " NR)
    if (status != "PASS" && status != "NOT_RUN" && status != "CANDIDATE PASS / pending re-freeze") {
        fail("invalid status for " id ": " status)
    }
    if (status != "NOT_RUN" && (test_name == "TBD" || evidence == "—")) {
        fail("completed-status row lacks evidence for " id)
    }
    r_rows++
    next
}

/^\| §19\.[0-9]+ / {
    if (NF != 8) {
        fail("§19 row has " (NF - 1) " columns at line " NR)
        next
    }

    id = trim($2)
    clause = trim($3)
    status = trim($6)
    number = substr(id, 5) + 0

    if (number < 1 || number > 21) fail("out-of-range row " id " at line " NR)
    if (capability_seen[number]++) fail("duplicate row " id " at line " NR)
    if (clause != id) fail("clause mismatch for " id " at line " NR)
    if (status != "UNVERIFIED" && status != "VERIFIED" && status != "UNSUPPORTED" && status != "SKIP") {
        fail("invalid status for " id ": " status)
    }
    s_rows++
    next
}

END {
    if (header_count != 1) fail("expected exactly one traceability table header")
    if (r_rows != 165) fail("expected 165 R rows, found " r_rows)
    if (s_rows != 21) fail("expected 21 §19 rows, found " s_rows)
    for (i = 1; i <= 165; i++) if (!seen[i]) fail("missing row R" i)
    for (i = 1; i <= 21; i++) if (!capability_seen[i]) fail("missing row §19." i)
    if (failures) exit 1
    print "PASS: traceability has unique contiguous R1-R165 and §19.1-§19.21 rows"
}
' "$traceability_file"
