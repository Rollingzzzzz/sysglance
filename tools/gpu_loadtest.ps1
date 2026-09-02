# gpu_loadtest.ps1 — heavy GPU load for bge-m3 via Ollama
# Writes a heartbeat file so the caller can verify it is REALLY running.
$hf = "D:\Tools\load_heartbeat.txt"
"log started $(Get-Date -Format o)" | Out-File $hf
try {
    $texts = 1..96 | ForEach-Object {
        ("heavy gpu load sample {0} the quick brown fox jumps over the lazy dog again and again " -f $_) * 120
    }
    $body = @{ model = "bge-m3"; input = $texts } | ConvertTo-Json -Depth 3
    for ($i = 0; $i -lt 15; $i++) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $r = Invoke-RestMethod -Uri http://localhost:11434/api/embed `
              -Method Post -Body $body -ContentType "application/json"
        $sw.Stop()
        "batch {0} done in {1:n1}s ({2} vectors x {3} dim)" -f `
            ($i+1), $sw.Elapsed.TotalSeconds, $r.embeddings.Count, $r.embeddings[0].Count |
            Out-File $hf -Append
    }
    "ALL DONE $(Get-Date -Format o)" | Out-File $hf -Append
} catch {
    "ERROR: $($_.Exception.Message)" | Out-File $hf -Append
}
