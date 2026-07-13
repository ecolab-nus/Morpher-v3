# TODO — Improvement Roadmap

> **Instruction:** Keep this file current. Strike off items (~~like this~~) or check their boxes as soon as they are done, and add a one-line outcome note (result, commit, or "moved to REFERENCE.md"). Remove items that become irrelevant. Do not let this file go stale.

Compiled 2026-07-13 from code exploration, REFERENCE.md, bot.md, and random_recommendations.md. Confidence markers included where the root cause is a hypothesis, not verified.

---

## Tier 0 — Correctness & trust in results (do first)

### [~] 1. OutLoop live-out slot bug — DIAGNOSED, BLOCKED ON DESIGN DECISION (2026-07-13)
**Original hypothesis (AssignOutLoopAddr Y-split) was WRONG.** Reproduced doitgen uf2 (`li_tree` reduction variant) and traced it to the end.

**Actual root cause:** the mismatch is the **unrolled reduction accumulator `morpher.red.update`**, not a generic OutLoop live-out. The tree-reduction lowering (`morpherLoopUnrollPass.cpp:634`) creates `PartialCount` partial accumulators. For uf2 there are 2 partials. The DFG generator is **internally inconsistent** about this variable:
- Its **instrumentation** records `morpher.red.update` as **8 bytes** (2 partials; trace shows offsets 0–7, value 707264 duplicated in both 4-byte slots).
- Its **mem_alloc + DFG XML** model it as **4 bytes** (`<BasePointerName size="4">`) with a **single OSTORE**.
- Result: validation expects slot 1 (base+4 = 196612..196615) which the mapped kernel never writes → 4 byte mismatches. Slot 0 is written correctly and matches. Confirmed on a clean successful II=5 map (matches=5, mismatches=4, all 4 = slot 1).

**The decision (needs domain input — opposite fixes):**
1. **Instrumentation over-records** → the DFG correctly carries the reduction in-fabric (CMERGE/SELECT) and writes the final result once; fix = make instrumentation/externalized size reflect the single result. Zero change to generated DFG/hardware.
2. **DFG under-models** → both tree partials are genuine loop-carried outputs that must persist; fix = size `morpher.red.update` = PartialCount×4 and emit PartialCount stores. Changes DFG semantics.

A validation-side clamp (validate only within the DFG-declared size) would make the symptom disappear under *either* interpretation — i.e. it could mask a real under-modeling bug — so it is deliberately NOT applied.

**Lean:** interpretation 1 (the kernel produces the correct value at slot 0; the 2nd slot is a phantom from the reference IR spilling both partials). But not certain enough to implement — reduction-externalization semantics have downstream effects. **Awaiting user's call.**

**Further investigation (2026-07-13, user asked to dig before deciding) — interpretation 1 now STRONGLY supported:**
- Same bug across ranked-search variants: `_ol_11` (the `li` variant, = the name in old memory) and `morpher.red.update` (`li_tree`) both show the identical 8-byte / duplicated-value pattern.
- In the externalized DFG, **array A has no STORE node** (only LOADs; addr_trace has 256 loads, 0 stores). So the externalized reduction live-out *is* the kernel's real output (host copies it back to A), not an intermediate — and mem_alloc models it as ONE entry, the DFG as ONE 4-byte OSTORE, and it validates correct at slot 0.
- Mechanism of the size split: `array_pointer_sizes[base] = DL.getTypeAllocSize(...)` (`DFGPartPred.cpp:2540`, `dfg.cpp:12282`) drives the XML `<BasePointerName size>` (=4 here), but the **instrumentation dumps the reference IR's full backing alloca** (8 bytes = 2 tree partials) independently → the 8-byte expected image. slot 1 duplicates slot 0's value (707264).
- **Residual check before a fix is safe:** confirm slot 0 == the true combined reduction result (not merely partial0) — e.g. compare against the uf1 clean result. If confirmed, fix = interpretation 1 (record/expect only the DFG live-out width; zero change to generated DFG). Still holding for user confirmation of the direction.

**Repro:** `config/regression.yaml`-style but `auto_unroll: {enabled: true, factors: [2]}`, kernel `polybench/doitgen/doitgen.c`; or map the pre-generated `workspace/morpher_runs/doitgen_uf2/.../doitgen_uf2_li_tree` DFG and run the artifact simulator on its trace.

### [x] 2. Make mapping deterministic and seedable ✅ DONE (2026-07-13)
On investigation, most of this was **already fixed** by a prior `getAllPEListDeterministic()` refactor (candidate base order is sorted T,Y,X,name — no longer hash-ordered) and SA already seeds its RNG from `--sa_seed` (default 1) with a total-order candidate sort (penalty → distance → `getFullName()`). The **one genuine remaining gap** was the PF proximity sort (`PathFinderMapper.cpp:871`) comparing Manhattan distance only, no tiebreak — fixed by adding a `getFullName()` total-order tiebreak.
- Empirically verified: conv2 + bicg mapping.json / mapping_r.txt byte-identical across repeated runs with the proximity-on `balanced` preset (SA active).
- Note: same-binary runs were likely already reproducible (std::sort is deterministic given deterministic input); the tiebreak guarantees stability **across builds/STL versions** and if upstream candidate ordering ever changes.
- SA seed recorded in the mapper log ("seed = N"); did not add to result.csv (low value — seed is a fixed config knob, default 1).

### [x] 3. One-command regression gate ✅ DONE (2026-07-13)
`scripts/regression_gate.py` + `scripts/regression_expectations.yaml` + `config/regression.yaml` (auto-unroll off for a stable uf1 DFG). Builds mapper, runs conv2/fir/bicg/doitgen end-to-end, asserts II does not regress + validation clean (mismatches=0, matches match) + mapper time within 3x. Baseline: all four map II=1, clean. Negative-tested (wrong expectation → exit 1). Run: `python3 scripts/regression_gate.py`.

---

## Tier 1 — Mapper quality (active research direction)

### [ ] 4. Step 3c: pre-mapping bank constraints for LD/ST (`candidateDests` filtering)
**Why:** Missing middle of the shipped 3-graph/layout-solver work. Layout solver runs post-mapping and falls back to preset on RW cross-bank conflicts — the mapper freely scatters same-array LD/STs across incompatible memPEs, then the solver can't fix it. Constraining same-array accesses to one bank-compatibility class *before* mapping turns solver fallbacks into successes.

**Plan:**
1. Reuse `blacklistDest` (same mechanism as node-pin) — no new mapper machinery.
2. Pre-mapping: group LD/ST nodes by base pointer; via `buildMemPE2Bank()`, blacklist DPs whose memPE is outside the chosen bank class. Class choice per array: greedy, largest-footprint arrays claim banks first.
3. Gate with existing `replicate_data` / future `crossbar` knobs (both off → constrain; replicate on → no constraint).
4. Evaluate: solver fallback rate + II on multi-bank arch (`hycube_edgePEMEMs.json`) benchmarks.

**Risk:** over-constraining can raise II where one bank class lacks memPE throughput — that data is exactly what motivates the crossbar (item 8), so a negative result is still useful.

**Effort:** Medium — a few days. Highest research-value-per-effort item.

### [ ] 5. Split SA from PathFinder; give SA a real objective
**Why:** `SAMapper::getCost()` (`SimulatedAnnealingMapper.cpp:766`) is `unmapped*100 + congestion*10 + conflict*10 + port_usage`. No route length, recurrence slack, locality, or lookahead — SA explores nearly the same space as PF with a shallower signal (explains mults2 956s, gemm 1395s). Target architecture per random_recommendations.md: shared legality/route layer, SA with own move set + pluggable objective.

**Plan (staged, each step measurable):**
1. Extract shared legality layer (op support, DP availability, latency legality, route feasibility) into functions both mappers call — code motion only; regression gate (#3) protects it.
2. Enrich SA objective incrementally: total routed wirelength first (cheapest, likely biggest win), then recurrence-cycle slack and producer–consumer distance for unmapped-node routability.
3. New moves beyond relocate: swap two nodes, relocate a connected cluster, rip-up-most-congested.
4. Benchmark SA-dominated cases (mults2, gemm_no_unroll_flattened) after each step.

**Effort:** Big-ticket, 1–2 weeks. Treat as an experiment — current SA underperformance doesn't prove SA is a bad fit.

### [x] 6. Restart with different node order before escalating II ✅ DONE (2026-07-13)
The perturbed-order restart mechanism **already existed** end-to-end (`CGRA_xml_compiler.cpp` `try_map_at_current_ii` loops `restart` in `[0, ii_restarts+1)`, calling `setRestartOffset(restart)`; `PathFinderMapper::Map` rotates/reverses `sortedNodeList` when `restartOffset != 0`, lines 1757-1787). On a failed/stagnation-aborted attempt it prints "Restarting same II" and retries before the outer loop escalates II. fast/balanced/quality/exhaust already set `ii_restarts` 1–2 and are exercised by the test suite.
- **The only gap:** `trusted_pf` (the default preset, used by `config/default.yaml`) had `ii_restarts: 0` → escalated immediately. Fixed by setting `trusted_pf.ii_restarts = 1` in `morpher_runner/context.py`. Confirmed the flag reaches the mapper (`ii_restarts=1`) and the PF phase (mapping_method 0, `CGRA_xml_compiler.cpp:655`) receives `attempts_per_ii=2`.
- Regression gate unchanged (easy kernels map first-attempt, restart never fires).
- Cost tradeoff (documented in the config comment): failing II levels now cost 2 PF attempts instead of 1 before escalating — bounded, and the point of the change.

---

## Tier 2 — Bigger bets (each gated on the previous)

### [ ] 7. Simulator: bank/port contention modeling
**Why:** `Memory.cpp` is 74 lines — flat memory, no banks/port conflicts/contention (verified). The simulator can *validate* layouts but cannot *evaluate* them: consolidation vs cyclic vs replication simulate identically. Layout-solver quality claims and crossbar motivation both need a simulator that charges for bank conflicts.

**Plan:** Give `Memory` a bank map from arch JSON (reuse the memPE→bank wiring the mapper already parses); cycle engine serializes same-cycle accesses to one bank port. Report conflict-stall counts alongside validation. Functional mode untouched.

**Effort:** Medium — simulator is small and clean.

### [ ] 8. Crossbar between SPM banks and PE_MEMs (step 4)
**Why:** Stated hardware direction; relief valve that makes bank constraints (#4) optional. Do *after* #4 and #7: (a) evidence of when rigid wiring fails, (b) a simulator that measures rather than assumes the benefit.

**Plan:** Arch JSON crossbar flag replacing 1:1 `PE_MEM.MEMPORT → SPM_Bx` wiring; mapper drops bank constraints when set; simulator adds `(memPE, addr) → bank` lookup in memory-op execution. Evaluate against #4's failure cases.

**Effort:** Medium-large, localized on the simulator side.

### [ ] 9. E-graph DFG normalization — narrow prototype only
**Why:** Highest-value narrow slice is **address-expression canonicalization**: AGI provenance / affine-pattern recovery is already where the layout solver falls back (missing-affine-pattern fallback exists; per-lane SIMD provenance is lossy).

**Plan:** Standalone pass between DFG gen and AGI pruning canonicalizing GEP/address arithmetic into one affine normal form (egg, or a hand-rolled rewriter — for affine-only that may be enough and cheaper). Success metric: layout-solver fallback rate drops across the suite. Defer MAC-fusion/reassociation rewrites until this proves out.

**Effort:** Big-ticket, research-grade. Only after Tier 1 is stable.

---

## Tier 3 — Hygiene (batch into one cleanup day)

- [ ] Wire up or **delete** `selected_simd_artifact_only` (write-only; REFERENCE.md). Recommend delete — no consumer, no compat requirement.
- [ ] Drop the `--recmin_policy` no-op CLI flag — nothing in-repo passes it.
- [ ] Decode `gep_offset` in layout observation (currently -1; recover as `node.constant − Variable2SPMAddr[base]`) — makes observation JSON usable for #4's per-array grouping.
- [ ] Document-or-verify unpredicated-CMERGE HyCUBE *binary* semantics — a REFERENCE.md paragraph suffices until `emit_binary` matters.
- [ ] `dfg.cpp` (13k lines) / `AssignOutLoopAddr` Y-split: no wholesale refactor. Rule: any function touched by Tier 0/1 work gets extracted into sensible units as you go. The Y-split dies as part of #1.

---

## Suggested execution order

- **Week 1:** #3 regression gate → #2 determinism → #1 OutLoop bug → #6 PF restart.
- **Weeks 2–3:** #4 bank constraints (fold Tier 3 items into files you touch anyway).
- **After:** #5 SA split as the parallel experimental track; #7 → #8 as the hardware-direction track; #9 when a paper-shaped bet is wanted.

**Anti-goal:** no further PF heuristic tuning (topk caps, thresholds) until #2 and #3 exist — a heuristic win can't currently be distinguished from hash-order luck.
