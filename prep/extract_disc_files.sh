#!/bin/bash
# Extract disc files/ into sd-files/rayman/files, skipping ones already complete; stop after ~150 s.
cd "$HOME/mnt/Rayman Origins (Europe) (EnFrDeEsItNl)"
D="Rayman Origins (Europe) (En,Fr,De,Es,It,Nl).rvz"
start=$(date +%s); done=0; left=0
while read -r p; do
  out="sd-files/rayman/files/$p"
  [ -f "$out.ok" ] && { done=$((done+1)); continue; }
  if [ $(( $(date +%s) - start )) -gt 150 ]; then left=$((left+1)); continue; fi
  mkdir -p "$(dirname "$out")"
  if ~/tools/dtk vfs cp "$D:files/$p" "$out" >/dev/null 2>&1; then : > "$out.ok"; done=$((done+1)); else left=$((left+1)); fi
done < ~/disc_paths.txt
echo "done=$done left=$left size=$(du -sh sd-files | cut -f1)"
