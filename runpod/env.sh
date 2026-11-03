# Loads KEY=value lines from .env at the repo root into the environment.
# A variable already set in the environment keeps its value. Sourced by the
# runpod/ scripts after they cd to the repo root.
if [ -f .env ]; then
  while IFS='=' read -r key value; do
    case "$key" in ''|\#*) continue ;; esac
    value="${value%\"}"; value="${value#\"}"
    value="${value/#\~/$HOME}"
    [ -n "${!key:-}" ] || export "$key=$value"
  done < .env
fi
