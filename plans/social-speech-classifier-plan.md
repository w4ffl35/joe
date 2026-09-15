# Implementation plan: binary negative-speech classifier (Idea 2)

**Goal:** Fine-tune a small encoder classifier (ModernBERT-base preferred,
deberta-v3-base fallback) that flags "negative speech" (posts that should be
reviewed/deleted) in Joe's X archive, trained from the existing regex-derived
labels in `socialmedia/output/analysis.json` (6,119 flagged vs ~18,219 clean
of 24,338 total), gated on heldout ROC-AUC / F1 with a human-review
disagreement pool feeding a non-circular round 2.

**Why this is the fastest path (context):** the labeled split already exists
(regex output, no manual labeling); the model is a ~150M-param encoder so
training is minutes on the RTX 5080 with ~15 GB idle headroom (vs the Qwen
LoRA runs that pin 15.8-16.0 GB); the heavy `transformers`/`torch` venv at
`joeos_finetune_data/venv` is already proven on this GPU; and eval is an
objective metric (unlike style imitation).

## Locations

| Thing | Path |
|---|---|
| New classifier project | `../joeos_finetune_data/social_classifier/` (venv + GPU conventions live here) |
| Label source (read-only) | `../socialmedia/output/analysis.json` |
| Raw posts (read-only) | `/home/joe/Downloads/twitter-2026-09-02-.../data/tweets.js` |
| Runtime | `cd ../joeos_finetune_data && ./venv/bin/python3 <script>` |
| Optional toolkit integration | `../socialmedia/analyze_posts.py` (final slice) |

**Environment note:** run `nvidia-smi` before any GPU slice; free VRAM first
(stop resident `ollama` embedding model). Training peak for a 150M encoder is
a few GB — no contention with desktop.

---

## Slice 1 — pure dataset helpers + self-check fixture
- File(s): `../joeos_finetune_data/social_classifier/dataset.py` (new)
- Change: pure, GPU-free functions with a `--self-check` main:
  - `load_tweets(path) -> list[dict]` — parse `tweets.js` as data only
    (strip `window.YTD.tweets.part0 =`, `json.loads`), flatten each
    `{"tweet": {...}}` to `{id, text, lang, is_retweet, is_reply, created_at}`.
  - `load_flagged_ids(path) -> frozenset[str]` — ids from
    `analysis.json`'s `flagged[]`.
  - `label_and_filter(posts, flagged) -> (kept, dropped)` — keep English,
    non-retweet posts with text length >= 3; label `1` if id is flagged else
    `0`. `dropped` is a dict of reason -> count (stats, printed).
  - `split(posts, seed=42, fracs=(0.85, 0.10, 0.05)) -> (train, eval, review)`
    — deterministic, stratified on label (stable `random.Random(seed)`,
    shuffle-then-threshold).
- Verify: `./venv/bin/python3 social_classifier/dataset.py --self-check`
  runs an inline 5-post fixture (2 flagged) and asserts kept/dropped counts,
  label balance, and that the three splits have disjoint ids. Exit 0 = pass.
- Depends on: (none)

## Slice 2 — build the real corpus
- File(s): `../joeos_finetune_data/social_classifier/dataset.py` (same file,
  second edit)
- Change: add a `--build` CLI mode wiring the Slice-1 pure functions to the
  real absolute paths (archive + `analysis.json`), writing:
  `social_classifier/data/train.jsonl`, `eval.jsonl`, `review_pool.jsonl`
  (`{id, text, label}` per line) plus printing kept/dropped stats, per-split
  label counts, and a disjointness assertion between splits.
- Verify: run `--build` twice; `sha256sum` of the three outputs must be
  identical across runs (determinism). Printed positive ratio must be
  plausible vs the 6,119/24,338 source (~25% pre-filter).
- Depends on: Slice 1

## Slice 3 — model + hyperparameter config module
- File(s): `../joeos_finetune_data/social_classifier/train_config.py` (new)
- Change: pure helpers:
  - `pick_model_id() -> str` — returns `answerdotai/ModernBERT-base` if the
    installed `transformers` version supports it (runtime `importlib` check
    on `transformers.ModernBertModel` / version >= 4.50), else
    `microsoft/deberta-v3-base`; honors `CLASSIFIER_MODEL` env override;
    prints which was chosen and why.
  - Constants: `SEQ_LEN=256`, `LR=2e-5`, `EPOCHS=4`, `BATCH=32`,
    `LABELS=["clean","flagged"]`.
  - `class_weights(labels) -> list[float]` — inverse-frequency weights.
- Verify: `./venv/bin/python3 social_classifier/train_config.py --self-check`
  asserts `pick_model_id()` returns a non-empty known string and
  `class_weights([1,1,0,0])` returns `[1.0, 1.0]` (balanced) — exit 0.
- Depends on: Slice 1

## Slice 4 — trainer script
- File(s): `../joeos_finetune_data/social_classifier/train_classifier.py` (new)
- Change: tokenize via `AutoTokenizer` (pad to `SEQ_LEN`); load
  `AutoModelForSequenceClassification(..., num_labels=2)`; class-weighted
  loss override (`CrossEntropyLoss(weight=...)`); HF `Trainer` with
  `bf16=True`, eval every epoch, save best checkpoint to
  `social_classifier/out/`; `--epochs/--max-length/--smoke` args; a
  `--smoke` mode trains on a 300-example subset for 2 steps then exits 0.
  Keep the file composed of small functions; no logic beyond HF plumbing
  (all pure logic lives in Slice 1/3 helpers).
- Verify: `./venv/bin/python3 -m py_compile social_classifier/train_classifier.py`
  passes (no GPU needed for the syntax gate).
- Depends on: Slices 1, 2, 3

## Slice 5 — smoke run, then full training run (gate)
- File(s): none — build/gate change
- Change: (a) smoke: `./venv/bin/python3 social_classifier/train_classifier.py --smoke`
  — must complete without OOM (loss decreasing at step 2, checkpoint dir
  created). (b) full run:
  `./venv/bin/python3 social_classifier/train_classifier.py --epochs 4`
  — preflight `nvidia-smi` free first; expect a few GB peak, minutes of
  wall time; best checkpoint under `social_classifier/out/`.
- Verify: `nvidia-smi` shows no illegal-memory errors; best-checkpoint dir
  exists with `adapter-less` plain HF weights + `tokenizer/`; training log
  shows eval loss improving.
- Depends on: Slice 4

## Slice 6 — heldout eval + disagreement export
- File(s): `../joeos_finetune_data/social_classifier/eval_classifier.py` (new)
- Change: load best checkpoint + `eval.jsonl`; produce ranked logits, then
  (pure Python/numpy, no sklearn dependency) compute ROC-AUC via ranking,
  and precision/recall/F1 at a threshold chosen to maximize F1 on eval;
  write `social_classifier/report.md` (metrics + chosen threshold) and
  `social_classifier/data/predictions.jsonl`; export up to 150 disagreements
  (pred-flagged-but-clean + pred-clean-but-flagged) to
  `social_classifier/data/review_pool.jsonl` for human triage.
- Verify: `./venv/bin/python3 social_classifier/eval_classifier.py` prints
  eval F1 + ROC-AUC and exits 0. Gate (v1 = faithful reproduction of the
  regex labels, since labels ARE the regex output): `ROC-AUC >= 0.95`.
  Below that, stop and inspect (data bug, not model bug).
- Depends on: Slice 5

## Slice 7 — human review pass (non-GPU)
- File(s): none — data task by the user
- Change: review the ~150 samples in `review_pool.jsonl` (short markdown/
  terminal list is fine), record a true `label` per id into
  `social_classifier/data/human_review.jsonl` (`{id, text, regex_label,
  human_label}`). This is the only non-circular ground truth and makes
  round 2 honest.
- Verify: `human_review.jsonl` has >= 100 rows; every row has both labels.
- Depends on: Slice 6

## Slice 8 — round 2: retrain with human labels + human-heldout gate
- File(s): `../joeos_finetune_data/social_classifier/dataset.py` + reuse of
  Slice 4 trainer (sequential edits, one file each)
- Change: (a) add `merge_human(labels_path)` to `dataset.py` — a post is
  positive if regex OR human says flagged (human overrides regex on
  disagreement); (b) re-run Slice-4 training from scratch on the merged
  train split, but evaluate on a human-annotated heldout (split
  `human_review.jsonl` ~80/20, train rows folded in, eval rows held out).
- Verify: round-2 eval report prints F1 on the *human-labeled* heldout
  (non-circular); gate `F1 >= 0.90` on that set, and per-class precision/
  recall reported.
- Depends on: Slices 6, 7

## Slice 9 — optional toolkit integration: classifier server
- File(s): `../joeos_finetune_data/social_classifier/serve_classifier.py` (new)
- Change: stdlib-`http.server` wrapper (zero new deps) exposing
  `POST /classify` accepting `{"texts": [...]}` and returning
  `{"labels": [...], "scores": [...]}` from the round-2 checkpoint.
- Verify: `curl -X POST .../classify -d '{"texts":["normal post",
  "fucking idiot"]}'` returns `[0, 1]`-style labels; exit 0.
- Depends on: Slice 8

## Slice 10 — optional toolkit integration: analyze_posts.py hook
- File(s): `../socialmedia/analyze_posts.py` (single focused edit)
- Change: add `--classifier-url` (optional). When set, each post's text is
  sent in batches to the Slice-9 server; a `classifier_flag: true/false`
  field is merged into each post record while keeping the existing
  `output/analysis.json` contract (regex scoring untouched, so the
  dashboard works unchanged).
- Verify: run `analyze_posts.py <archive-dir> --classifier-url
  http://127.0.0.1:PORT` against the `testdata/` sample; output JSON has the
  new field and the dashboard renders.
- Depends on: Slice 9

---

## Notes / non-goals
- No Qwen/GGUF involved — this is a separate small-model artifact that runs
  on CPU at inference time.
- Idea 3 (post-drafting Qwen LoRA in Joe's voice) is a natural follow-on
  phase AFTER this: the clean (negative-filtered) corpus this classifier
  produces is exactly the hygiene step that idea needs. Idea 1 (category-
  conditional style) stays parked until a positive topic taxonomy exists;
  the current categories are all offense labels, unsuitable as style
  dimensions.
- Where the plan conflicts with machine limits (max VRAM during a joint
  serve + eval), stop and sequence the GPU steps rather than forcing them.
