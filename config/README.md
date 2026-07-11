# Configurations

- `default.yaml`: scalar architecture, automatic bounded unroll and loop-order
  search, and conservative C++ AGI/iterator externalization.
- `scalar.yaml`: baseline that retains AGI and iterator/control nodes.
- `scalar_agi_pruned.yaml`: C++ AGI externalization without iterator removal.
- `simd4_verified_agi_iter.yaml`: verified SIMD-4 plus conservative iterator
  externalization and optional loop-interchange candidates.
- `simd4_verified_vred.yaml`: verified SIMD-4 reduction path at factor 4.
- `simd4_verified_vred_u8.yaml`: the same reduction path at factor 8.
- `xml_arch.yaml`: static architecture for direct XML input.

Architecture and experiment sweeps belong in `scripts/*.yaml`; they should not
be copied into this directory as near-duplicate user-facing configurations.
