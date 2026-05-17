$CXX = "g++"
$CXXFLAGS = @(
    "-std=c++17", "-O3", "-fno-strict-aliasing", "-Wall", "-Wcast-qual",
    "-fno-exceptions", "-Wextra", "-Wshadow", "-m64",
    "-msse", "-msse2", "-mssse3", "-msse4.1", "-mavx2", "-mbmi", "-mpopcnt",
    "-DIS_64BIT", "-DUSE_PREFETCH", "-DUSE_POPCNT", "-DUSE_SSE41",
    "-DUSE_SSSE3", "-DUSE_SSE2", "-DUSE_AVX2",
    "-DNDEBUG", "-DLARGEBOARDS", "-DNNUE_EMBEDDING_OFF"
)
$INCLUDES = @("-I.", "-Innue", "-Isyzygy")
$LDFLAGS = @("-static", "-m64")

Write-Host "Compiling Duffish (largeboard + avx2 + static)..."

$files = @(
    @("benchmark.cpp", "benchmark.o"),
    @("bitbase.cpp", "bitbase.o"),
    @("bitboard.cpp", "bitboard.o"),
    @("endgame.cpp", "endgame.o"),
    @("evaluate.cpp", "evaluate.o"),
    @("main.cpp", "main.o"),
    @("material.cpp", "material.o"),
    @("misc.cpp", "misc.o"),
    @("movegen.cpp", "movegen.o"),
    @("movepick.cpp", "movepick.o"),
    @("pawns.cpp", "pawns.o"),
    @("position.cpp", "position.o"),
    @("psqt.cpp", "psqt.o"),
    @("search.cpp", "search.o"),
    @("thread.cpp", "thread.o"),
    @("timeman.cpp", "timeman.o"),
    @("tt.cpp", "tt.o"),
    @("uci.cpp", "uci.o"),
    @("ucioption.cpp", "ucioption.o"),
    @("tune.cpp", "tune.o"),
    @("partner.cpp", "partner.o"),
    @("parser.cpp", "parser.o"),
    @("piece.cpp", "piece.o"),
    @("variant.cpp", "variant.o"),
    @("xboard.cpp", "xboard.o"),
    @("syzygy\tbprobe.cpp", "tbprobe.o"),
    @("nnue\evaluate_nnue.cpp", "evaluate_nnue.o"),
    @("nnue\features\half_ka_v2.cpp", "half_ka_v2.o"),
    @("nnue\features\half_ka_v2_variants.cpp", "half_ka_v2_variants.o")
)

$i = 0
foreach ($file in $files) {
    $i++
    $src = $file[0]
    $obj = $file[1]
    Write-Host "[$i/$($files.Count)] $src"
    $allArgs = $CXXFLAGS + $INCLUDES + @("-c", $src, "-o", $obj)
    $proc = Start-Process -FilePath $CXX -ArgumentList $allArgs -NoNewWindow -Wait -PassThru -RedirectStandardError "compile_err_$obj.txt"
    if ($proc.ExitCode -ne 0) {
        Write-Host "FAILED compiling $src"
        Get-Content "compile_err_$obj.txt"
        exit 1
    }
}

Write-Host ""
Write-Host "Linking stockfish.exe..."
$objFiles = @(
    "benchmark.o", "bitbase.o", "bitboard.o", "endgame.o", "evaluate.o",
    "main.o", "material.o", "misc.o", "movegen.o", "movepick.o",
    "pawns.o", "position.o", "psqt.o", "search.o", "thread.o",
    "timeman.o", "tt.o", "uci.o", "ucioption.o", "tune.o",
    "partner.o", "parser.o", "piece.o", "variant.o", "xboard.o",
    "tbprobe.o", "evaluate_nnue.o", "half_ka_v2.o", "half_ka_v2_variants.o"
)
$linkArgs = @("-o", "stockfish.exe") + $objFiles + $LDFLAGS
$proc = Start-Process -FilePath $CXX -ArgumentList $linkArgs -NoNewWindow -Wait -PassThru -RedirectStandardError "link_err.txt"
if ($proc.ExitCode -ne 0) {
    Write-Host "FAILED linking"
    Get-Content "link_err.txt"
    exit 1
}

Write-Host ""
Write-Host "Build successful!"
Get-Item stockfish.exe | Select-Object Name, Length, LastWriteTime

# Clean up temp files
Remove-Item "compile_err_*.txt" -ErrorAction SilentlyContinue
Remove-Item "link_err.txt" -ErrorAction SilentlyContinue
