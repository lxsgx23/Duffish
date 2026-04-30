#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import re
import os
import glob
import time
import math
import queue
import subprocess
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from typing import Dict, List, Tuple, Optional

# 引擎路径配置
ENGINE_PATH = "pikafish-avxvnni.exe"  # 请确保皮卡鱼引擎在这个路径
ENGINE_DEPTH = 15             # 分析深度10-15（建议10-15）

PIECE_VALUES = {
    'K': 10000, 'A': 1, 'B': 1.5, 'N': 4.5, 'R': 9, 'C': 4.5, 'P': 1,
    'k': 10000, 'a': 1, 'b': 1.5, 'n': 4.5, 'r': 9, 'c': 4.5, 'p': 1
}

@dataclass
class EngineStats:
    total_games: int = 0
    wins: int = 0
    losses: int = 0
    draws: int = 0
    win_lengths: List[int] = field(default_factory=list)
    sac_plies: List[int] = field(default_factory=list)
    
    shorts: int = 0              
    bad_draws: int = 0           
    sacs: int = 0                
    early_sacs: int = 0          
    sac_score: float = 0.0       
    
    blunders: int = 0            
    defense_breaks: int = 0      
    total_sac_value: float = 0.0

class PikafishWrapper:
    """与皮卡鱼引擎进行UCI通信的包装类"""
    def __init__(self, executable_path: str):
        self.executable_path = executable_path
        self.process = None
        self._start_engine()

    def _start_engine(self):
        """启动或重启引擎子进程"""
        self.close()

        kwargs = {
            'stdin': subprocess.PIPE,
            'stdout': subprocess.PIPE,
            'stderr': subprocess.DEVNULL,
            'universal_newlines': True,
            'encoding': 'utf-8',
            'errors': 'ignore',
            'bufsize': 1
        }
        
        if os.name == 'nt':
            kwargs['creationflags'] = subprocess.CREATE_NO_WINDOW

        self.process = subprocess.Popen([self.executable_path], **kwargs)
        try:
            self.process.stdin.write("uci\n")
            self.process.stdin.flush()
            while True:
                line = self.process.stdout.readline()
                if "uciok" in line or "ucciok" in line or not line:
                    break
            
            self.process.stdin.write("setoption name Hash value 32\n")
            self.process.stdin.write("isready\n")
            self.process.stdin.flush()
            while True:
                line = self.process.stdout.readline()
                if "readyok" in line or not line:
                    break
        except Exception:
            pass

    def evaluate_fen(self, fen: str, depth: int = ENGINE_DEPTH) -> float:
        """评估局面并返回分数"""
        score = 0.0
        try:
            if self.process is None or self.process.poll() is not None:
                self._start_engine()
                
            self.process.stdin.write(f"position fen {fen}\n")
            self.process.stdin.write(f"go depth {depth}\n")
            self.process.stdin.flush()
            
            while True:
                line = self.process.stdout.readline()
                if not line or "bestmove" in line:
                    break
                if "info depth" in line and "score cp" in line:
                    match = re.search(r"score cp (-?\d+)", line)
                    if match:
                        score = int(match.group(1)) / 100.0 
                elif "info depth" in line and "score mate" in line:
                    match = re.search(r"score mate (-?\d+)", line)
                    if match:
                        mate_in = int(match.group(1))
                        score = 100.0 if mate_in > 0 else -100.0
                        
        except OSError:
            self._start_engine()
        except Exception:
            pass
            
        return score

    def close(self):
        if self.process is not None:
            try:
                if self.process.poll() is None:
                    try:
                        self.process.stdin.write("quit\n")
                        self.process.stdin.flush()
                    except:
                        pass
                    self.process.terminate()
                    self.process.wait(timeout=1)
            except:
                pass
            finally:
                if self.process.stdin:
                    try: self.process.stdin.close()
                    except: pass
                if self.process.stdout:
                    try: self.process.stdout.close()
                    except: pass
            self.process = None

class XiangqiBoard:
    def __init__(self, start_fen: Optional[str] = None):
        self.board = [['' for _ in range(9)] for _ in range(10)]
        if start_fen:
            self.set_fen(start_fen)
        else:
            self._init_standard_board()
    
    def _init_standard_board(self):
        self.board[0] =['R', 'N', 'B', 'A', 'K', 'A', 'B', 'N', 'R']
        self.board[2][1] = 'C'
        self.board[2][7] = 'C'
        self.board[3][0] = 'P'
        self.board[3][2] = 'P'
        self.board[3][4] = 'P'
        self.board[3][6] = 'P'
        self.board[3][8] = 'P'
        self.board[9] =['r', 'n', 'b', 'a', 'k', 'a', 'b', 'n', 'r']
        self.board[7][1] = 'c'
        self.board[7][7] = 'c'
        self.board[6][0] = 'p'
        self.board[6][2] = 'p'
        self.board[6][4] = 'p'
        self.board[6][6] = 'p'
        self.board[6][8] = 'p'

    def set_fen(self, fen: str):
        """根据给定的 FEN 字符串布置棋盘"""
        board_part = fen.split(' ')[0]
        rows = board_part.split('/')
        for y, row_str in enumerate(rows):
            actual_y = 9 - y
            x = 0
            for char in row_str:
                if char.isdigit():
                    x += int(char)
                else:
                    self.board[actual_y][x] = char
                    x += 1
    
    def get_material(self, is_red: bool) -> float:
        total = 0.0
        for row in self.board:
            for piece in row:
                if piece:
                    if (is_red and piece.isupper()) or (not is_red and piece.islower()):
                        total += PIECE_VALUES.get(piece, 0)
        return total

    def to_fen(self, is_red_turn: bool) -> str:
        rows =[]
        for y in range(9, -1, -1):
            empty = 0
            row_str = ""
            for x in range(9):
                p = self.board[y][x]
                if p == '':
                    empty += 1
                else:
                    if empty > 0:
                        row_str += str(empty)
                        empty = 0
                    row_str += p
            if empty > 0:
                row_str += str(empty)
            rows.append(row_str)
        turn = 'w' if is_red_turn else 'b'
        return "/".join(rows) + f" {turn} - - 0 1"

    def make_move(self, move_str: str) -> Optional[str]:
        match = re.match(r'([A-Ia-i])([0-9])-([A-Ia-i])([0-9])', move_str)
        if not match: return None
        fx = ord(match.group(1).upper()) - ord('A')
        fy = int(match.group(2))
        tx = ord(match.group(3).upper()) - ord('A')
        ty = int(match.group(4))
        
        # 边界与异常保护
        if not (0 <= fx <= 8 and 0 <= fy <= 9 and 0 <= tx <= 8 and 0 <= ty <= 9):
            return None

        piece = self.board[fy][fx]
        captured = self.board[ty][tx]
        self.board[ty][tx] = piece
        self.board[fy][fx] = ''
        return captured

    def is_king_alive(self) -> bool:
        """检查双方老将是否在场，防止传给引擎非法FEN"""
        has_red_king = any('K' in row for row in self.board)
        has_black_king = any('k' in row for row in self.board)
        return has_red_king and has_black_king

class SimplePGNParser:
    @staticmethod
    def parse(pgn_path: str) -> Optional[Dict]:
        try:
            with open(pgn_path, 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()
            
            # ========== 提取标签信息 ==========
            # 提取结果
            result_match = re.search(r'\[Result\s+"([^"]+)"\]', content)
            
            # 提取双方名称
            red_match = re.search(r'\[Red\s+"([^"]+)"\]', content)
            black_match = re.search(r'\[Black\s+"([^"]+)"\]', content)
            red_name = red_match.group(1) if red_match else "UnknownRed"
            black_name = black_match.group(1) if black_match else "UnknownBlack"

            # 提取可能存在的非标准 FEN（用于继续残局或指定开局库）
            fen_match = re.search(r'\[FEN\s+"([^"]+)"\]', content)
            start_fen = fen_match.group(1) if fen_match else None
            
            # ========== 纯净化棋谱解析 ==========
            # 1. 剔除所有头部标签如 [Event "EAS"]
            text_no_tags = re.sub(r'\[.*?\]', '', content)
            # 2. 剔除所有大括号引擎分析或中文多行注释（例如：{#1654,0,18#和棋...}）
            text_no_comments = re.sub(r'\{.*?\}', '', text_no_tags, flags=re.DOTALL)
            # 3. 剔除所有圆括号变例（防止误走入变例分支）
            text_no_vars = re.sub(r'\(.*?\)', '', text_no_comments, flags=re.DOTALL)
            # 4. 剔除残余的评注如 $1, $2
            text_clean = re.sub(r'\$\d+', '', text_no_vars)
            
            # 提取极其纯净的主线走法
            moves = re.findall(r'[A-Ia-i][0-9]-[A-Ia-i][0-9]', text_clean)

            # 如果解析结果为平局的 1/2-1/2，映射到规范状态
            res_str = result_match.group(1) if result_match else "*"
            if "1/2" in res_str: res_str = "1/2-1/2"

            return {
                'red': red_name,
                'black': black_name,
                'result': res_str,
                'moves': moves,
                'start_fen': start_fen
            }
        except Exception:
            return None

class SacrificeAnalyzer:
    @staticmethod
    def analyze(game_data: Dict, engine: Optional[PikafishWrapper]) -> Dict[str, EngineStats]:
        red_name, black_name = game_data['red'], game_data['black']
        stats = {red_name: EngineStats(), black_name: EngineStats()}
        stats[red_name].total_games = stats[black_name].total_games = 1
        
        # 支持从指定 FEN 初始化棋盘
        board = XiangqiBoard(game_data.get('start_fen'))
        
        # 识别先手方
        is_red_turn = True
        if game_data.get('start_fen'):
            parts = game_data['start_fen'].split()
            if len(parts) > 1 and parts[1] == 'b':
                is_red_turn = False
                
        history =[]
        
        for move_str in game_data['moves']:
            history.append({
                'fen': board.to_fen(is_red_turn),
                'mat_red': board.get_material(True),
                'mat_black': board.get_material(False),
                'is_red_turn': is_red_turn
            })
            captured = board.make_move(move_str)
            
            if not board.is_king_alive():
                break
            
            if captured in['A', 'B', 'a', 'b']:
                killer = red_name if is_red_turn else black_name
                stats[killer].defense_breaks += 1
                
            if captured in['K', 'k']:
                is_red_turn = not is_red_turn
                break

            is_red_turn = not is_red_turn
            
        history.append({
            'fen': board.to_fen(is_red_turn),
            'mat_red': board.get_material(True),
            'mat_black': board.get_material(False),
            'is_red_turn': is_red_turn
        })

        if engine:
            skip_until = 0
            for i in range(16, len(history) - 5):
                if i < skip_until:
                    continue
                
                prev = history[i]       
                future = history[i+5]   
                is_red = prev['is_red_turn']
                player_name = red_name if is_red else black_name
                
                if is_red:
                    mb_prev = prev['mat_red'] - prev['mat_black']
                    mb_future = future['mat_red'] - future['mat_black']
                else:
                    mb_prev = prev['mat_black'] - prev['mat_red']
                    mb_future = future['mat_black'] - future['mat_red']
                    
                net_mat = mb_future - mb_prev
                
                if net_mat <= -2.0:
                    eval_before = engine.evaluate_fen(prev['fen'])
                    if not is_red: eval_before = -eval_before
                    
                    if eval_before < mb_prev - 3.0:
                        continue 
                        
                    eval_after = engine.evaluate_fen(future['fen'])
                    if not is_red: eval_after = -eval_after
                    
                    if eval_after >= eval_before - 1.5:
                        stats[player_name].sacs += 1
                        stats[player_name].total_sac_value += abs(net_mat)
                        stats[player_name].sac_plies.append(i)
                        skip_until = i + 5 
                    else:
                        stats[player_name].blunders += 1
                        skip_until = i + 5
        
        num_moves = len(game_data['moves'])
        if game_data['result'] == '1-0':
            stats[red_name].wins = stats[black_name].losses = 1
            stats[red_name].win_lengths.append(num_moves)
        elif game_data['result'] == '0-1':
            stats[black_name].wins = stats[red_name].losses = 1
            stats[black_name].win_lengths.append(num_moves)
        else:
            stats[red_name].draws = stats[black_name].draws = 1
            
            last_state = history[-1]
            nk_red = max(0.0, last_state['mat_red'] - 10000.0)
            nk_black = max(0.0, last_state['mat_black'] - 10000.0)
            
            if (nk_red + nk_black) >= 46.0:
                stats[red_name].bad_draws += 1
                stats[black_name].bad_draws += 1
            else:
                if nk_red - nk_black >= 1.5:
                    stats[red_name].bad_draws += 1 
                elif nk_black - nk_red >= 1.5:
                    stats[black_name].bad_draws += 1
                
        return stats

class XEASCalculator:
    @staticmethod
    def calculate(stats: EngineStats) -> float:
        if stats.total_games == 0:
            return 0.0
            
        sac_score_per_game = stats.sac_score / stats.total_games
        shorts_pct = stats.shorts / stats.total_games
        bad_draws_pct = stats.bad_draws / stats.total_games
        defense_breaks_pct = stats.defense_breaks / stats.total_games
        blunders_pct = stats.blunders / stats.total_games
        avg_win_moves = sum(stats.win_lengths) / stats.wins if stats.wins > 0 else 0
        
        score = 100000.0 
        score += sac_score_per_game
        score += shorts_pct * 150000.0
        score += defense_breaks_pct * 10000.0
        
        if shorts_pct > 0.20:
            score += 500000.0
        
        score -= bad_draws_pct * 50000.0
        score -= avg_win_moves * 500.0
        score -= blunders_pct * 50000.0
            
        return max(score, 0.0)

class MultiThreadXEAS:
    def __init__(self, num_threads: int = 14, use_engine: bool = True):
        self.num_threads = num_threads
        self.use_engine = use_engine
        self.engine_pool = queue.Queue()
        self.engine_stats = defaultdict(EngineStats)
        
        if use_engine:
            if not os.path.exists(ENGINE_PATH):
                print(f"[警告] 找不到引擎 {ENGINE_PATH}，将降级为无引擎启发式模式。")
                self.use_engine = False
            else:
                print(f"正在初始化 {num_threads} 个皮卡鱼引擎实例，请稍候...")
                for _ in range(num_threads):
                    self.engine_pool.put(PikafishWrapper(ENGINE_PATH))
                print("引擎初始化完成！")

    def __del__(self):
        self.close()

    def close(self):
        """安全释放引擎池"""
        while not self.engine_pool.empty():
            try:
                eng = self.engine_pool.get_nowait()
                eng.close()
            except queue.Empty:
                break

    def analyze_directory(self, pattern: str):
        pgn_files = glob.glob(pattern, recursive=True)
        if not pgn_files: return print("未找到PGN文件。")
        
        start = time.time()
        completed = 0
        
        with ThreadPoolExecutor(max_workers=self.num_threads) as executor:
            futures =[executor.submit(self._process_file, f) for f in pgn_files]
            for future in as_completed(futures):
                res = future.result()
                if res:
                    for eng, st in res.items():
                        total = self.engine_stats[eng]
                        total.total_games += st.total_games
                        total.wins += st.wins
                        total.losses += st.losses
                        total.draws += st.draws
                        total.win_lengths.extend(st.win_lengths)
                        total.sac_plies.extend(st.sac_plies)
                        total.bad_draws += st.bad_draws
                        total.sacs += st.sacs
                        total.blunders += st.blunders
                        total.defense_breaks += st.defense_breaks
                        total.total_sac_value += st.total_sac_value
                completed += 1
                if completed % 10 == 0:
                    print(f"进度: {completed}/{len(pgn_files)}...")

        global_wins = sum(st.wins for st in self.engine_stats.values())
        global_win_lengths = sum(sum(st.win_lengths) for st in self.engine_stats.values())
        global_avg_win_moves = (global_win_lengths / global_wins) if global_wins > 0 else 100.0
        
        decay_constant = max(global_avg_win_moves / 2.0, 10.0)
        short_win_threshold = global_avg_win_moves * 0.73 
        
        for st in self.engine_stats.values():
            st.early_sacs = sum(1 for ply in st.sac_plies if ply <= decay_constant)
            st.sac_score = sum(
                200000.0 + 450000.0 * math.exp(-ply / decay_constant)
                for ply in st.sac_plies
            )
            st.shorts = sum(1 for length in st.win_lengths if length <= short_win_threshold)

        print(f"分析完毕！耗时: {time.time() - start:.1f} 秒")
        print(f"全局平均赢棋回合数: {global_avg_win_moves:.1f}，速胜判定阈值: {short_win_threshold:.1f} 回合")

    def _process_file(self, pgn_file: str):
        game = SimplePGNParser.parse(pgn_file)
        if not game: return None
        
        engine = None
        if self.use_engine:
            engine = self.engine_pool.get()
        try:
            return SacrificeAnalyzer.analyze(game, engine)
        finally:
            if self.use_engine:
                self.engine_pool.put(engine)

    def print_report(self):
        print("\n" + "="*95)
        print("★ 中国象棋 XEAS 引擎联赛评分报告 ★")
        print("="*95)
        
        results =[]
        for name, st in self.engine_stats.items():
            score = XEASCalculator.calculate(st)
            results.append((score, name, st))
        
        results.sort(reverse=True, key=lambda x: x[0])
        
        print(f"{'Engine':<25} | {'Games':<5} | {'EAS-Score':<10} | {'Sacs%':<7} | {'E.Sacs%':<7} | {'Shorts%':<7} | {'BadDraw%':<8} | {'AvgWinMvs':<9}")
        print("-" * 95)
        
        for score, name, st in results:
            tg = st.total_games if st.total_games > 0 else 1
            sacs_pct = (st.sacs / tg) * 100
            early_sacs_pct = (st.early_sacs / tg) * 100
            shorts_pct = (st.shorts / tg) * 100 
            bad_draws_pct = (st.bad_draws / tg) * 100  
            avg_win_moves = sum(st.win_lengths) / st.wins if st.wins > 0 else 0
            
            display_name = name[:23] + ".." if len(name) > 25 else name
            print(f"{display_name:<25} | {st.total_games:<5} | {int(score):<10} | {sacs_pct:<7.2f} | {early_sacs_pct:<7.2f} | {shorts_pct:<7.2f} | {bad_draws_pct:<8.2f} | {avg_win_moves:<9.1f}")
            
        print("-" * 95)

def main():
    target_dir = r".\**\*.pgn"
    
    analyzer = MultiThreadXEAS(num_threads=14, use_engine=True)
    try:
        analyzer.analyze_directory(target_dir)
        analyzer.print_report()
    finally:
        analyzer.close()

if __name__ == '__main__':
    main()