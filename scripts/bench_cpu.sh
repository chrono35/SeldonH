#!/bin/bash
# Compare la vitesse de génération CPU : ancien llama.cpp (build universel en un passage, GGML_CPU_GENERIC)
# et nouveau (une compilation par architecture), avec les anciens threads (logiques − 2) et les nouveaux (automatique).
#   ./scripts/bench_cpu.sh                         (modèle : models/Qwen3.5-2B-Q4_K_M.gguf)
#   ./scripts/bench_cpu.sh models/autre.gguf 128   (modèle, nombre de tokens générés)
#   SWEEP=1 ./scripts/bench_cpu.sh                 (nouveau llama.cpp seul : 4 à 12 threads, chaque mesure 2 fois)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
MODEL="${1:-models/Qwen3.5-2B-Q4_K_M.gguf}"; MAX="${2:-128}"
OUT="$ROOT/build/bench"; mkdir -p "$OUT"
SRCS=(tests/llm_cli.cpp src/llm/LlmCore.cpp src/llm/CpuTopology.cpp)
FW=(-framework Accelerate -framework Foundation)

echo "== Compilation de llm_cli (nouveau llama.cpp : third_party/llama-install)"
NEWLIBS=(-lllama -lggml -lggml-cpu -lggml-base)
[[ -f third_party/llama-install/lib/libggml-blas.a ]]  && NEWLIBS+=(-lggml-blas)
[[ -f third_party/llama-install/lib/libggml-metal.a ]] && NEWLIBS+=(-lggml-metal) && FWN=(-framework Metal -framework MetalKit) || FWN=()
c++ -std=c++17 -O2 -I third_party/llama-install/include "${SRCS[@]}" -L third_party/llama-install/lib "${NEWLIBS[@]}" \
    "${FW[@]}" ${FWN[@]+"${FWN[@]}"} -o "$OUT/llm_cli_new"

OLD=third_party/llama-build
if [[ -f $OLD/src/libllama.a ]]; then
  echo "== Compilation de llm_cli (ancien llama.cpp : $OLD)"
  c++ -std=c++17 -O2 -I third_party/llama-install/include "${SRCS[@]}" \
      -L $OLD/src -L $OLD/ggml/src -L $OLD/ggml/src/ggml-blas -L $OLD/ggml/src/ggml-metal \
      -lllama -lggml -lggml-cpu -lggml-base -lggml-blas -lggml-metal \
      "${FW[@]}" -framework Metal -framework MetalKit -o "$OUT/llm_cli_old"
fi

"$OUT/llm_cli_new" --cpuinfo
LOGICAL=$(sysctl -n hw.logicalcpu); OLDT=$(( LOGICAL > 4 ? LOGICAL - 2 : LOGICAL ))
Q="Explique en trois phrases ce qu'est une datation radiocarbone."

run() {   # $1 = étiquette, $2 = exécutable, $3 = threads (0 = automatique)
  local line
  line=$(printf '%s\n/quit\n' "$Q" | "$2" "$MODEL" --cpu --max "$MAX" --threads "$3" 2>/dev/null | grep -o '[0-9]* tokens, [0-9.]* tok/s' | tail -1)
  printf '%-40s %s\n' "$1" "${line:-échec}"
}
echo; echo "== Modèle : $MODEL, $MAX tokens, CPU seul"
if [[ "${SWEEP:-0}" == 1 ]]; then
  for pass in 1 2; do for t in 4 5 6 8 10 12; do run "passe $pass, $t threads" "$OUT/llm_cli_new" "$t"; done; done
  exit 0
fi
[[ -x "$OUT/llm_cli_old" ]] && run "ancien llama.cpp, $OLDT threads (avant)" "$OUT/llm_cli_old" "$OLDT"
[[ -x "$OUT/llm_cli_old" ]] && run "ancien llama.cpp, automatique"          "$OUT/llm_cli_old" 0
run "nouveau llama.cpp, $OLDT threads"         "$OUT/llm_cli_new" "$OLDT"
run "nouveau llama.cpp, automatique (après)"   "$OUT/llm_cli_new" 0
