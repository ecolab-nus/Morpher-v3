# DFG transform limitations

The supported semantic transforms run in the C++ DFG pipeline. Python may
validate manifests and remap trace indices, but it does not rewrite the mapped
DFG for iterator pruning or SIMD packing.

## AGI and iterator externalization

`externalize_access: agi` removes address-generation work under the assumption
that addresses are supplied by the trace-driven external access path.
`externalize_access: agi_iter` additionally removes loop-start/end, PHI/control,
and synthetic iterator live-out nodes only when the candidate component is
closed. Any data, predicate, recurrence, reduction, or control edge from that
component to surviving mapped work causes iterator externalization to be
skipped with an identity manifest; AGI externalization remains applied.
Unrelated data-dependent control is retained. This is intentionally
conservative: a skipped loop is not evidence that externalization is
semantically impossible.

## Verified SIMD packing

`simd_pack: verified` requires a SIMD-capable architecture and a complete
SIMD-width unrolled group. It packs only lane-isomorphic groups whose relevant
boundaries are represented by the transform manifest. Partial groups remain
scalar. Scalar inputs require architecture-declared broadcast support.
Reduction-axis packing is limited to implemented associative reductions and,
when using vector partial sums, requires the unroll factor to be an integer
multiple of the SIMD width. Preheader identities, horizontal reduction, scalar
initializer combination, and final stores may be host-modelled rather than
cycle-scheduled; the manifest records that boundary scope.

Packing may expose a disconnected scalar iterator component. With
`externalize_access: agi_iter`, iterator externalization therefore runs after
verified packing where required. Manifest validation rejects inconsistent index
spaces instead of repairing the graph heuristically.

## Unrolling and loop restructuring

Unrolling applies only to the uniquely annotated mapping loop and preserves a
scalar remainder. A factor larger than the runtime trip count can leave the
unrolled body unexecuted and produce no traces. Auto-unroll compares mapped
candidates using effective `II / represented source iterations`; it does not
assume that a larger factor is profitable. Scalar candidates automatically try
both the preserved linear recurrence and a supported tree reduction unless an
explicit reduction policy overrides the search. Loop
interchange/fusion are attempted only where LLVM accepts them, and legality does
not imply a better mapping. Ambiguous or branched nests may be left unchanged or
rejected by the restricted pipeline.

Candidate ranking is an ordering device, not a routability proof. The initial
mapping beam is preset-dependent and expands whenever no candidate maps or an
untried lower bound can still match or beat the incumbent. Recurrence analysis
includes both `nextiter` data edges and reversed `RecParents` memory constraints.

Scalar unroll factor and SIMD width are independent. SIMD width is inferred
from the architecture JSON. Before unrolling, invariant-address integer
load/update/store reductions are promoted to an SSA accumulator when the pass
can prove its restricted single-block, no-conflicting-access contract. Patterns
outside that contract are left unchanged rather than speculatively rewritten.

The mapper schedules the steady-state DFG. Separate scheduled preheader and exit
DFGs, broader control predication, arbitrary recurrence packing, and proof of
source equivalence for source-template lowering remain outside this contract.
