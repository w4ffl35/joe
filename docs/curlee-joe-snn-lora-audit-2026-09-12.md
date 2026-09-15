# CURLEE / JOE / SNN / LoRA architecture audit

Date: 2026-09-12

This is the durable handoff and decision record for the multi-repository audit
covering `curlee`, `joeos`, `joeos_finetune_data`, `nir-c-runtime`,
`snn_interpreter` / `spikeforge`, `spikeforge-dashboard`, `spikeforge-hub`, and
`spikeforge-targets`.

## Architectural decision

The learned component infers a bounded, typed event. Explicit Curlee state owns
the state-machine transition, timeout/hysteresis policy, and every side effect:

```text
event window -> fixed-point SNN -> event/score -> verified Curlee FSM -> action
```

State is explicit; the event is inferred. An SNN is never the authority for a
kernel action. Invalid model input/configuration maps to an `unknown` event and
must not authorize a disruptive action.

Small stateless functions whose complete input space can be enumerated should
normally be compiled to minimized Curlee decision logic. Retaining an SNN at
runtime is most useful for sparse temporal signals with persistent neuronal
state: interrupt storms, queue/device anomalies, and later audio keyword
events.

## Audit conclusions

1. CURLEE issue #313 was a soundness blocker: `return q[i]` without `ensures`
   skipped the bounds obligation. The 2026-09-12 implementation adds the
   missing return-expression obligation walk and a golden regression.
2. Enabling that proof exposed JOE accessors that depended on caller convention
   instead of locally stated bounds. Their contracts/guards and glyph table
   geometry were corrected before restoring the green JOE gate.
3. A training run must create a candidate, never update the champion link.
   `promote_adapter.py` is the explicit, fail-closed promotion path.
4. Qwen3.5's chat template has no `{% generation %}` markers. Flat-text SFT
   therefore trained on system/user/tool text as well as assistant outputs.
   Training now annotates assistant spans, pre-tokenizes simple columns, and
   masks every non-assistant label with `-100`.
5. Overlength examples must not be silently truncated. The 1,235-row corpus has
   16 examples above 1,024 tokens; training now fails before GPU model loading
   until they are curated, with an explicit drop-only ablation option.
6. n1235's reported ~86% was partial: coding and real ground-truth fabrication
   gates were absent. Scorecards now identify partial dimensions and expose an
   explicit `promotion_eligible` flag. The current adapter passed only 1/4 of
   the end-to-end behavioral sanity cases on 2026-09-12.
7. JOE is currently a static, single-address-space Ring-0 kernel. Initial SNN
   modules are statically linked components, not isolated dynamic processes.
8. `.spkf` modules already implement one-input/one-output composition, but the
   Spikeforge pipeline is a DAG, not an FSM, and requires Python/PyTorch.
9. `nir-c-runtime` remains a float/heap correctness runtime for general NIR.
   Its new `fixed_event` ABI is the separate allocation-free integer reference
   path for small event modules and has a parity fixture with JOE's Curlee
   guard.

## Current reference module

JOE's `kernel/irq_snn_guard.curlee` is a statically linked reference detector:

- four `U8`-range telemetry features;
- an integer leaky-integrate membrane in `0..4095`;
- event values `normal`, `elevated`, `critical`, `unknown`;
- explicit controller states `observe`, `suspect`, `confirmed`, `cooldown`;
- disruptive action only on `suspect + critical`;
- unknown input authorizes no action.

The checked-in weights are provisional parity/calibration constants, not a
claim of a trained production model.

The CPU-only follow-up adds a deliberately smaller CURLEE #316 profile with
four 2-bit features and a 5-bit retained membrane. Its complete transition
space is 32 × 4⁴ = 8,192 rows, exhaustively enumerated by both the Curlee VM
gate and `nir-c-runtime`. Both implementations pin the same truth-table
fingerprint (`score_sum=211482`, `spike_count=7248`). A generated-C harness
also compares every individual Curlee transition with the C reference, rather
than relying only on the aggregates. The C reference also
implements and tests the explicit guard-state/action boundary, including
fail-closed behavior for unknown events and invalid states. These constants
remain provisional until trace data exists; the point of this milestone is
that a future exported unit must fit and reproduce a finite, reviewable ABI.

## Training and evaluation policy

- Keep Qwen3.5-9B QLoRA rank 16 and the 1,024-token hardware ceiling until the
  data/evaluation defects are resolved.
- Curate the 16 overlength records into coherent final-file/function/hunk
  examples. Do not append `... (truncated)` to a target.
- Maintain separate reporting for code correctness and agent/tool behavior.
- Build a new immutable hidden suite only after the training corpus snapshot is
  frozen. Group splits by source/feature/issue; do not split overlapping
  variants or temporal windows across train and test.
- Add compiler-negative diagnostic/repair pairs and JOE-level tasks whose
  oracle is `curlee check`, runtime assertions, repository verification, and
  QEMU serial markers.
- Use multiple training/evaluation seeds. Do not promote on a dimension that
  was skipped, a quick scorecard, or regex-only fabrication proxies.
- Generated hardware code remains compiler/QEMU gated and human-approved.

## SNN data and acceptance metrics

The first trained target should be an interrupt/device anomaly detector. Gather
normal QEMU traces and fault-injected traces for burst, delay, drop, malformed
ring, timeout, reset, and recovery scenarios. Later add real-hardware sessions.
Split by complete scenario/device/session, never by overlapping windows.

Measure false positives per hour, missed critical events, detection latency,
state oscillation, abstention rate, static memory, integer operations per step,
and bit-exact parity across Spikeforge reference, C fixed-event runtime, Curlee
VM, generated C, and JOE/QEMU.

## Implementation sequence

- [x] Fix CURLEE returned-index soundness and add a golden regression.
- [x] Repair newly exposed JOE array contracts/guards and table geometry.
- [x] Remove automatic LoRA promotion and add atomic gated promotion.
- [x] Add assistant-only label masking and mask tests.
- [x] Replace silent truncation with a fail-closed overlength preflight.
- [x] Mark partial scorecards as non-promotion-eligible.
- [x] Update the model-facing CURLEE prompt for floats and `ingest`.
- [x] Add an allocation-free integer C event ABI and parity test.
- [x] Add the statically linked Curlee detector/FSM and VM gate.
- [x] Add a two-bit SNN logic-unit profile with an exhaustive 8,192-row
      Curlee/C truth-table parity gate (CURLEE #316 deployment contract).
- [x] Add filesystem-free raw blob placement/read-back tooling for the future
      native runtime/model disk, with fixed-LBA capacity guards (JOE #52).
- [x] Add the CURLEE VirtIO block read path and live QEMU two-sector DMA gate
      (`BLK: 1` -> `BLK: 2` -> `BLK: 3`, JOE #51).
- [ ] Curate the 16 overlength corpus records rather than dropping them.
- [ ] Freeze and author a new independently hidden evaluation suite.
- [ ] Train a new candidate after the corpus and hidden suite are ready.
- [ ] Train/calibrate the first detector from collected traces; replace the
      provisional reference weights only after differential parity gates pass.
- [ ] Add scheduling/isolation/dynamic loading only if JOE later needs these as
      true processes; they are not required for the static first deployment.

## 2026-09-13 LoRA candidate result

The assistant-masked, fail-closed training pipeline completed a full RTX 5080
run and early-stopped after epoch 2. The resulting candidate is
`checkpoints/lora-n1219-r16-20260912-214410` (1,219 usable examples; 1,175
train / 44 validation; epoch-2 eval loss 0.4385). Preflight reported 45.5%
assistant-supervised tokens and zero overlength inputs after the explicit
16-record exclusions file; frozen-v2 validation passed 5/5.

The complete non-quick scorecard is stored under `score_history` as
`2026-09-13T003943-checkpoints_lora-n1219-r16-20260912-214410.json`:

- coding: 33/41 (80.5%), heldout 5/9 (55.6%);
- frozen-v2 coding: 5/5 (100%);
- tool follow-through: 8/10 (80%), heldout 1/2 (50%);
- no-fabrication proxy: 10/10, with real ground truth 8/10 (80%) and heldout
  2/3 (66.7%);
- no-punting: 6/9 (66.7%), heldout 1/2 (50%);
- overall: 85.4%; heldout overall: 64.4%.

The scorecard is structurally `promotion_eligible` because every required
dimension and the real verifier results are present. It is deliberately **not
promoted**, however: primary coding correctness regressed from the deployed
n1096 champion's historical 90.2% to 80.5%, and candidate heldout overall
64.4% does not beat the champion's best recorded 65.3%. The champion symlink
therefore remains `checkpoints/lora-n1096-r16-20260901-112734`. The candidate
is useful as evidence that assistant masking and the new frozen suite work,
not as the OS-writing default.

The scorer now has `--manage-ground-truth-backend`: on a single 16 GB GPU it
finishes all direct-model dimensions, explicitly frees that model and CUDA
cache, then temporarily serves the same adapter for executable headlesscode
verification. This removes the former two-model VRAM conflict without using
the unavailable RTX 2080 Super. The first complete attempt also exposed a
missing verifier backend; the final run failed closed, recovered, and produced
all ten real verdicts rather than accepting a proxy-only result.

## Validation snapshot

Before implementation: CURLEE 105/105 tests; JOE `make verify` plus normal and
e1000 QEMU smoke gates; `nir-c-runtime` 3/3; SNN interpreter 1,178 passed and 1
skipped. Final post-evaluation rerun: CURLEE 105/105; JOE `make verify`,
`make qemu-smoke`, and `make qemu-e1000-smoke` all passed (including ordered
`E1000: 1` -> `E1000: 2` and clean serial tail); `nir-c-runtime` 4/4 including
`test_fixed_event`; fine-tune unit tests 6/6, assistant-mask preflight clean,
and frozen-v2 5/5.

The RTX 2080 Super is intentionally excluded from this plan until access is
available; no step depends on it.

## 2026-09-13 CPU-only continuation

No training, inference benchmark, or GPU-backed model process was run during
this continuation.

JOE now has a legacy VirtIO block transport and bounded synchronous raw-sector
read API. The live smoke image puts a 1,024-byte fixture at LBA 7 and verifies
bytes on both sides of the sector boundary after QEMU DMA. `make check`,
`make verify`, and `make qemu-blk-smoke` pass. QEMU 10 reports 1,024-entry
legacy VirtIO queues, so both block and network initialization now derive ring
offsets and indices from the device-reported queue size instead of assuming
256 entries. The independent socket-fed VirtIO-net RX smoke assertion still
does not receive its injected frame; initialization reaches `NET: 3`, but that
RX test remains open and must not be represented as passing.

CURLEE's C++ cleanup now records 79 columns, 200 code lines per file, 20 code
lines per function, and 150 code lines per class as the preferred limits.
Twelve unreferenced implementation snapshots (21,975 lines) were removed,
dependency-ID construction was extracted from the CLI implementation, and
duplicated emitter slot lookup was consolidated. The full CURLEE suite passes
105/105 tests. The quality audit scans `.ipp` files and detects Allman-style
functions and classes; its current migration baseline is 675 warnings across
131 of 143 files, so broad conformance remains future refactoring work rather
than a completed claim.
