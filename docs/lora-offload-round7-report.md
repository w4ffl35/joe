# CPU offload for the Curlee LoRA serving — findings + round-7 result (2026-09-05)

Goal: stop the 16 GiB RTX 5080 from OOMing during long agent sessions by
moving data off the GPU, then re-run the full architect→code→review pipeline.

## What was tried

### 1. transformers KV-cache CPU offload (`cache_implementation="offloaded"`)
Added `SERVE_LORA_KV_OFFLOAD=1` to `serve_lora.py`'s `generate()`.
- **OOM fixed** — GPU pinned at ~7.9 GiB even on a 131K-char prompt; host RAM
  grew (+2.5 GB) as KV moved to CPU.
- **BUT output DEGRADED to incoherent garbage** (A/B verified): the architect
  produced nonsense ("The user hasn't actually given me a real command…" +
  HTML soup) and each generation was very slow. **Reverted.**

### 2. Quantized KV cache (`cache_implementation="quantized"`)
- **Unsupported**: Qwen3.5-9B is a *hybrid* model (linear-attention + full
  attention layers). `QuantizedCache` throws: "only supported for models with
  only full attention layers. Found: linear_attention." **Not usable.**

### 3. GGUF + llama.cpp partial offload
- Assets exist: a 9.8 GB Q8_0 base GGUF + LoRA-adapter GGUFs (n778-era only),
  and ollama's bundled llama-server has `--lora`, `--n-gpu-layers`, KV offload.
- The bundled `llama-server` is **CPU-only** ("no usable GPU found" — it lacks
  the CUDA libs the separate ollama runner uses). Driving ollama's internal
  cuda_v13 runner directly is fragile. **Abandoned.**

## The working solution: context-length cap (keeps KV on GPU)

Since the OOM driver is the **growing prompt** (every `/api/chat` re-encodes
full history; >28K tokens exhausts GPU), I added `SERVE_LORA_MAX_CONTEXT_TOKENS`
to `serve_lora.py`: it tokenizes the rendered prompt and drops the **oldest
non-system turns** until under the cap, keeping KV on GPU (quality preserved)
and bounding memory.

- **First version had a bug**: it could drop the *final user message* too →
  `apply_chat_template` raised "No user query found in messages" → HTTP 500
  (hit live in round 7). **Fixed** to always preserve the last user turn.
- **Validated**: an adversarial 121-message (60-turn) conversation — which
  forces maximal truncation — completed in 9.4 s with coherent output, no 500,
  GPU at 8.7 GiB.

## Round 7/7b (n1235 + 24K cap + full pipeline architect→code→review)

The cap **eliminated the OOM and the 500s** — the infrastructure goal was met.
But it fully exposed that the model itself cannot do the task:

- The architect made real tool calls (e.g. `grep -c "virtio_blk"`), then hit
  **6 consecutive completion deferrals** ("last execute_command failed") — the
  model cannot recover from a command that exits non-zero (grep "no match" →
  exit 1) and spirals into repeated `attempt_completion` calls. Bounded failure
  at iteration 10 of 15. No PLAN.md written.
- The code worker then repeated `attempt_completion` with **no artifact-
  producing tool call** ("no artifact-producing tool call in this session")
  through iteration 4+ — it never calls `write_to_file`/`edit_file`.
- **Zero files written, zero commits.** GPU reached 15.2 GiB at the 24K cap
  (weights ~7.9 + 24K KV ~7), so the cap must be lower (e.g. 18-20K) to leave
  headroom, but the behavioral failure is independent of memory.

## Bottom line

The CPU-offload request is **solved in the practical sense**: a context cap in
`serve_lora.py` (`SERVE_LORA_MAX_CONTEXT_TOKENS`, default off) keeps long
sessions within GPU memory without the quality loss of KV offload or the
unsupported quantized cache. The full KV-cache-offload route is a dead end for
this hybrid model in this transformers version.

However, the round-7 result is definitive on the model: **n1235 cannot write
the driver regardless of memory.** Across every round and configuration —
n1096/n1235, worker-only/full-pipeline, no-cap/capped, offload-on/off — the
LoRA never produced a single line of driver code. Its failures are behavioral
(fabricated completions, inability to recover from non-zero exits, no
artifact-producing tool calls), not memory-bound. The harness's verification
gates correctly caught every failure; the model never passed them.
