@echo off
setlocal

set CXX=g++
set CXXFLAGS=-std=c++17 -O3 -fno-strict-aliasing -Wall -Wcast-qual -fno-exceptions -Wextra -Wshadow -m64 -msse -msse2 -mssse3 -msse4.1 -mavx2 -mbmi -mpopcnt -DIS_64BIT -DUSE_PREFETCH -DUSE_POPCNT -DUSE_SSE41 -DUSE_SSSE3 -DUSE_SSE2 -DUSE_AVX2 -DNDEBUG -DLARGEBOARDS -DPRECOMPUTED_MAGICS -DNNUE_EMBEDDING_OFF
set INCLUDES=-I. -Innue
set LDFLAGS=-static -m64

echo Compiling Duffish (xiangqi + avx2 + static)...
echo.

echo [1/18] benchmark.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c benchmark.cpp -o benchmark.o
if errorlevel 1 goto :error

echo [2/18] bitboard.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c bitboard.cpp -o bitboard.o
if errorlevel 1 goto :error

echo [3/18] evaluate.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c evaluate.cpp -o evaluate.o
if errorlevel 1 goto :error

echo [4/18] main.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c main.cpp -o main.o
if errorlevel 1 goto :error

echo [5/18] misc.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c misc.cpp -o misc.o
if errorlevel 1 goto :error

echo [6/18] movegen.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c movegen.cpp -o movegen.o
if errorlevel 1 goto :error

echo [7/18] movepick.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c movepick.cpp -o movepick.o
if errorlevel 1 goto :error

echo [8/18] position.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c position.cpp -o position.o
if errorlevel 1 goto :error

echo [9/18] psqt.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c psqt.cpp -o psqt.o
if errorlevel 1 goto :error

echo [10/18] search.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c search.cpp -o search.o
if errorlevel 1 goto :error

echo [11/18] thread.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c thread.cpp -o thread.o
if errorlevel 1 goto :error

echo [12/18] timeman.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c timeman.cpp -o timeman.o
if errorlevel 1 goto :error

echo [13/18] tt.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c tt.cpp -o tt.o
if errorlevel 1 goto :error

echo [14/18] uci.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c uci.cpp -o uci.o
if errorlevel 1 goto :error

echo [15/18] ucioption.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c ucioption.cpp -o ucioption.o
if errorlevel 1 goto :error

echo [16/18] tune.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c tune.cpp -o tune.o
if errorlevel 1 goto :error

echo [17/18] piece.cpp + variant.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c piece.cpp -o piece.o
if errorlevel 1 goto :error
%CXX% %CXXFLAGS% %INCLUDES% -c variant.cpp -o variant.o
if errorlevel 1 goto :error

echo [18/18] nnue/evaluate_nnue.cpp + nnue/features/half_ka_v2.cpp + nnue/features/half_ka_v2_variants.cpp
%CXX% %CXXFLAGS% %INCLUDES% -c nnue/evaluate_nnue.cpp -o evaluate_nnue.o
if errorlevel 1 goto :error
%CXX% %CXXFLAGS% %INCLUDES% -c nnue/features/half_ka_v2.cpp -o half_ka_v2.o
if errorlevel 1 goto :error
%CXX% %CXXFLAGS% %INCLUDES% -c nnue/features/half_ka_v2_variants.cpp -o half_ka_v2_variants.o
if errorlevel 1 goto :error

echo.
echo Linking stockfish.exe...
%CXX% -o stockfish.exe benchmark.o bitboard.o evaluate.o main.o misc.o movegen.o movepick.o position.o psqt.o search.o thread.o timeman.o tt.o uci.o ucioption.o tune.o piece.o variant.o evaluate_nnue.o half_ka_v2.o half_ka_v2_variants.o %LDFLAGS%
if errorlevel 1 goto :error

echo.
echo Build successful!
goto :end

:error
echo.
echo Build FAILED!
exit /b 1

:end
endlocal
