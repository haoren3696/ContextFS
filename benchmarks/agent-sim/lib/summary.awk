# benchmarks/agent-sim/lib/summary.awk
# Usage: awk -v results=<dir> -f summary.awk
# Reads $results/{perop.csv, rollback.csv, totals.csv, branch.csv, env.txt}
# Prints summary.md to stdout.

function percentile(arr, n, p,    idx) {
  if (n == 0) return ""
  idx = int(p * (n - 1)) + 1
  if (idx < 1) idx = 1
  if (idx > n) idx = n
  return arr[idx]
}

function bsort(arr, n,    i, j, tmp) {
  for (i = 2; i <= n; i++)
    for (j = i; j > 1 && arr[j] < arr[j-1]; j--) {
      tmp = arr[j]; arr[j] = arr[j-1]; arr[j-1] = tmp
    }
}

BEGIN {
  FS = ","
  if (results == "") { print "ERROR: pass -v results=<dir>" > "/dev/stderr"; exit 2 }

  while ((getline line < (results "/env.txt")) > 0) {
    split(line, kv, "=")
    env[kv[1]] = kv[2]
  }
  close(results "/env.txt")

  pf = results "/perop.csv"
  getline < pf   # header
  while ((getline < pf) > 0) {
    split($0, f, ",")
    m = f[1]; ckpt_us = f[4]; sz = f[5]; status = f[6]
    if (status == "ok" && ckpt_us != "") {
      n_ok[m]++
      ckpts[m, n_ok[m]] = ckpt_us + 0
    }
    if (sz != "") {
      seen[m]++
      sizes[m, seen[m]] = sz + 0
    }
  }
  close(pf)

  rf = results "/rollback.csv"
  getline < rf
  while ((getline < rf) > 0) {
    split($0, f, ",")
    rb_n[f[1]]++
    rb_label[f[1], rb_n[f[1]]] = f[2]
    rb_val[f[1], rb_n[f[1]]] = (f[4] == "ok" ? f[3] : "fail")
  }
  close(rf)

  tf = results "/totals.csv"
  getline < tf
  while ((getline < tf) > 0) {
    split($0, f, ",")
    tot[f[1], "wall"] = f[2]
    tot[f[1], "ok"]   = f[3]
    tot[f[1], "fail"] = f[4]
    tot[f[1], "peak"] = f[5]
  }
  close(tf)

  bf = results "/branch.csv"
  getline < bf
  while ((getline < bf) > 0) {
    split($0, f, ",")
    m = f[1]; bname = f[2]; bus = f[3]; bstatus = f[4]
    if (bstatus == "ok" && bus != "") {
      bc_n[m]++
      bc_name[m, bc_n[m]] = bname
      bc_us[m, bc_n[m]] = bus + 0
    }
  }
  close(bf)

  methods[1] = "agentvfs"; methods[2] = "agentfs"; methods[3] = "branchfs"

  ckpt_every = (env["ckpt_every"] + 0)
  n_branches = (env["branches"] + 0)
  if (ckpt_every == 0) ckpt_every = 20

  printf "# Agent-sim benchmark — %s\n\n", env["run_id"]
  printf "Kernel: %s · CPU: %s · SCALE=%s · BRANCHES=%s · TOTAL_Q=%s\n\n",
    env["kernel"], env["cpu"], env["scale"], env["branches"], env["total_q"]

  # --- branch create latency ---
  if (n_branches > 0) {
    printf "## Branch create latency (µs, %d branches)\n\n", n_branches
    printf "| method | median | p10 | p90 | max |\n|---|---|---|---|---|\n"
    for (i = 1; i <= 3; i++) {
      m = methods[i]; nn = bc_n[m] + 0
      if (nn == 0) { printf "| %s | — | — | — | — |\n", m; continue }
      delete tmp
      for (k = 1; k <= nn; k++) tmp[k] = bc_us[m, k]
      bsort(tmp, nn)
      printf "| %s | %d | %d | %d | %d |\n", m,
        percentile(tmp, nn, 0.5), percentile(tmp, nn, 0.1),
        percentile(tmp, nn, 0.9), tmp[nn]
    }
    printf "\n"
  }

  # --- checkpoint latency ---
  printf "## Checkpoint latency (µs, over %d ckpts)\n\n", (env["total_q"] + 0) / ckpt_every
  printf "| method | median | p10 | p90 | max |\n|---|---|---|---|---|\n"
  for (i = 1; i <= 3; i++) {
    m = methods[i]; nn = n_ok[m] + 0
    if (nn == 0) { printf "| %s | — | — | — | — |\n", m; continue }
    delete tmp
    for (k = 1; k <= nn; k++) tmp[k] = ckpts[m, k]
    bsort(tmp, nn)
    printf "| %s | %d | %d | %d | %d |\n", m,
      percentile(tmp, nn, 0.5), percentile(tmp, nn, 0.1),
      percentile(tmp, nn, 0.9), tmp[nn]
  }

  printf "\n## Storage size after checkpoint (MB)\n\n"
  printf "| method | first | mid | last | growth × |\n|---|---|---|---|---|\n"
  for (i = 1; i <= 3; i++) {
    m = methods[i]; nn = seen[m] + 0
    if (nn == 0) { printf "| %s | — | — | — | — |\n", m; continue }
    first = sizes[m, 1]; last = sizes[m, nn]; mid = sizes[m, int((nn + 1) / 2)]
    growth = (first > 0) ? sprintf("%.1fx", last / first) : "n/a"
    printf "| %s | %.1f | %.1f | %.1f | %s |\n", m, first/1048576, mid/1048576, last/1048576, growth
  }

  printf "\n## Rollback latency (µs)\n\n"
  printf "| method | target 1 | target 2 | target 3 |\n|---|---|---|---|\n"
  for (i = 1; i <= 3; i++) {
    m = methods[i]; nn = rb_n[m] + 0
    if (nn == 0) { printf "| %s | — | — | — |\n", m; continue }
    printf "| %s", m
    for (k = 1; k <= 3; k++) {
      v = (k <= nn) ? rb_val[m, k] : "—"
      l = (k <= nn) ? rb_label[m, k] : ""
      printf " | %s%s", v, (l != "" ? " ("l")" : "")
    }
    printf " |\n"
  }

  printf "\n## Totals\n\n"
  printf "| method | wall (s) | ckpts ok | ckpts failed | peak store (MB) |\n|---|---|---|---|---|\n"
  for (i = 1; i <= 3; i++) {
    m = methods[i]
    if (tot[m, "wall"] == "") { printf "| %s | — | — | — | — |\n", m; continue }
    printf "| %s | %s | %s | %s | %.1f |\n", m,
      tot[m, "wall"], tot[m, "ok"], tot[m, "fail"], tot[m, "peak"]/1048576
  }
}
