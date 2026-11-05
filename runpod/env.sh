# Exports KEY=value lines from the repo-root .env; variables already set keep their value.
if [ -f .env ]; then
  while IFS='=' read -r key value; do
    case "$key" in ''|\#*) continue ;; esac
    value="${value%\"}"; value="${value#\"}"
    value="${value/#\~/$HOME}"
    [ -n "${!key:-}" ] || export "$key=$value"
  done < .env
fi
