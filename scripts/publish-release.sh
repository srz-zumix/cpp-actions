#!/bin/bash
set -euo pipefail

: "${GH_REPO:?GH_REPO is required}"
: "${TAG:?TAG is required}"
: "${EXPECTED_SHA:?EXPECTED_SHA is required}"
: "${ARTIFACT_DIR:?ARTIFACT_DIR is required}"
release_id="${RELEASE_ID:-}"

if [[ ! "$TAG" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] ||
   [[ ! "$EXPECTED_SHA" =~ ^[0-9a-f]{40}$ ]] ||
   [[ -n "$release_id" && ! "$release_id" =~ ^[1-9][0-9]*$ ]]; then
  echo "::error::Invalid release tag, target commit, or draft ID"
  exit 1
fi

archive="cpp-actions-${TAG}-macos-arm64.tar.gz"
cd "$ARTIFACT_DIR"
if [[ "$(cat "$archive.sha256")" != "$(shasum -a 256 "$archive")" ]]; then
  echo "::error::The release checksum does not match $archive"
  exit 1
fi
shasum -a 256 -c "$archive.sha256"

tag_commit() {
  local matches found
  matches="$(gh api "repos/$GH_REPO/git/matching-refs/tags/$TAG")" || return
  found="$(jq -er --arg tag "$TAG" '
      if type == "array" then any(.[]; .ref == ("refs/tags/" + $tag)) | tostring
      else error("Invalid matching-refs response") end
    ' <<< "$matches")" || return
  if [[ "$found" == true ]]; then
    gh api "repos/$GH_REPO/commits/$TAG" --jq .sha
  fi
}

check_tag() {
  local actual
  actual="$(tag_commit)"
  if [[ -n "$actual" && "$actual" != "$EXPECTED_SHA" ]]; then
    echo "::error::Tag $TAG points to a different commit"
    exit 1
  fi
}

check_draft() {
  local release
  release="$(gh api "repos/$GH_REPO/releases/$release_id")" || return
  if ! jq -e --arg id "$release_id" --arg tag "$TAG" '
      (.id | tostring) == $id and .draft == true and .tag_name == $tag
    ' <<< "$release" >/dev/null; then
    echo "::error::Release $release_id is not the expected draft for $TAG"
    exit 1
  fi
  if [[ -n "${RELEASE_ID:-}" ]] && ! jq -e --arg sha "$EXPECTED_SHA" '
      .target_commitish == $sha
    ' <<< "$release" >/dev/null; then
    echo "::error::The draft target no longer matches the release commit"
    exit 1
  fi
  printf '%s\n' "$release"
}

check_tag
if [[ -z "$release_id" ]]; then
  actual_sha="$(tag_commit)"
  if [[ -z "$actual_sha" ]]; then
    echo "::error::Tag $TAG does not exist"
    exit 1
  fi
  releases="$(gh api --paginate --slurp "repos/$GH_REPO/releases?per_page=100")"
  matches="$(jq --arg tag "$TAG" '[.[][] | select(.tag_name == $tag)]' <<< "$releases")"
  count="$(jq 'length' <<< "$matches")"
  if [[ "$count" != 0 ]]; then
    if [[ "$count" != 1 ]] || ! jq -e '.[0].draft == true' <<< "$matches" >/dev/null; then
      echo "::error::A release for $TAG already exists or multiple drafts match"
      exit 1
    fi
    release_id="$(jq -r '.[0].id' <<< "$matches")"
  else
    release_id="$(gh api --method POST "repos/$GH_REPO/releases" \
      -f "tag_name=$TAG" -f "target_commitish=$EXPECTED_SHA" \
      -F draft=true -F generate_release_notes=true --jq .id)"
  fi
fi

release="$(check_draft)"
upload_url="$(jq -r '.upload_url | split("{")[0]' <<< "$release")"
if [[ "$upload_url" != "https://uploads.github.com/repos/$GH_REPO/releases/$release_id/assets" ]]; then
  echo "::error::Unexpected release asset upload URL"
  exit 1
fi

assets="$(gh api --paginate --slurp "repos/$GH_REPO/releases/$release_id/assets?per_page=100")"
if jq -e --arg archive "$archive" '
    any(.[][]; .name == $archive or .name == ($archive + ".sha256"))
  ' <<< "$assets" >/dev/null; then
  echo "::error::The draft already has release files; remove them before retrying"
  exit 1
fi
for file in "$archive" "$archive.sha256"; do
  gh api --method POST -H 'Content-Type: application/octet-stream' \
    "$upload_url?name=$file" --input "$file" --silent
done

check_draft >/dev/null
assets="$(gh api --paginate --slurp "repos/$GH_REPO/releases/$release_id/assets?per_page=100")"
if ! jq -e --arg archive "$archive" '
    any(.[][]; .name == $archive and .state == "uploaded") and
    any(.[][]; .name == ($archive + ".sha256") and .state == "uploaded")
  ' <<< "$assets" >/dev/null; then
  echo "::error::The draft does not contain both uploaded release files"
  exit 1
fi

check_tag
actual_sha="$(tag_commit)"
if [[ -z "$actual_sha" ]]; then
  gh api --method POST "repos/$GH_REPO/git/refs" \
    -f "ref=refs/tags/$TAG" -f "sha=$EXPECTED_SHA" --silent
fi
actual_sha="$(tag_commit)"
if [[ "$actual_sha" != "$EXPECTED_SHA" ]]; then
  echo "::error::Tag $TAG does not resolve to the release commit"
  exit 1
fi
check_draft >/dev/null
gh api --method PATCH "repos/$GH_REPO/releases/$release_id" -F draft=false --silent
