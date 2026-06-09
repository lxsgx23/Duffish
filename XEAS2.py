#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import re
import os
import glob
import time
import math
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from typing import Dict, List, Optional

# ================= 全局配置区 =================
IGNORE_OPENING_MOVES = 0      
SAC_HORIZON = 9  # 保持9步，过滤短线战术，捕捉Patricia式的投机/鲁莽弃子

# ==============================================
PIECE_VALUES = {
    'K': 1000000, 'A': 104, 'B': 167, 'N': 561, 'R': 1373, 'C': 768, 'P': 127,
    'k': 1000000, 'a': 104, 'b': 167, 'n': 561, 'r': 1373, 'c': 768, 'p': 127
}
PAWN_UNIT = 127.0 
# ==============================================

@dataclass
class EngineStats:
    total_games: int = 0
    wins: int = 0
    losses: int = 0
    draws: int = 0
    win_moves: List[float] = field(default_factory=list) 
    sac_plies: List[int] = field(default_factory=list)   
    sac_values: List[float] = field(default_factory=list) 
    
    shorts: int = 0              
    bad_draws: int = 0
    bloody_draws: int = 0        
    sacs: int = 0                
    early_sacs: int = 0          
    sac_score: float = 0.0       
    total_sac_value: float = 0.0

    advisor_breaks: int = 0      
    elephant_breaks: int = 0     
    early_river_crosses: int = 0 
    king_hunts: int = 0          
    palace_pawns: int = 0        
    center_cannons: int = 0      
    empty_cannons: int = 0       
    rook_sacs: int = 0           

class XiangqiBoard:
    NUM_MAP = {'一': 1, '二': 2, '三': 3, '四': 4, '五': 5, '六': 6, '七': 7, '八': 8, '九': 9,
               '1': 1, '2': 2, '3': 3, '4': 4, '5': 5, '6': 6, '7': 7, '8': 8, '9': 9,
               '１': 1, '２': 2, '３': 3, '４': 4, '５': 5, '６': 6, '７': 7, '８': 8, '９': 9}

    def __init__(self, start_fen: Optional[str] = None):
        self.board = [['' for _ in range(9)] for _ in range(10)]
        if start_fen: self.set_fen(start_fen)
        else: self._init_standard_board()
    
    def _init_standard_board(self):
        self.board[0] =['R', 'N', 'B', 'A', 'K', 'A', 'B', 'N', 'R']
        self.board[2][1] = 'C'; self.board[2][7] = 'C'
        self.board[3][0] = 'P'; self.board[3][2] = 'P'; self.board[3][4] = 'P'; self.board[3][6] = 'P'; self.board[3][8] = 'P'
        self.board[9] =['r', 'n', 'b', 'a', 'k', 'a', 'b', 'n', 'r']
        self.board[7][1] = 'c'; self.board[7][7] = 'c'
        self.board[6][0] = 'p'; self.board[6][2] = 'p'; self.board[6][4] = 'p'; self.board[6][6] = 'p'; self.board[6][8] = 'p'

    def set_fen(self, fen: str):
        board_part = fen.split(' ')[0]
        for y, row_str in enumerate(board_part.split('/')):
            actual_y = 9 - y; x = 0
            for char in row_str:
                if char.isdigit(): x += int(char)
                else: self.board[actual_y][x] = char; x += 1
    
    def get_material(self, is_red: bool) -> float:
        return sum(PIECE_VALUES.get(p, 0) for row in self.board for p in row if p and ((is_red and p.isupper()) or (not is_red and p.islower())))

    def to_fen(self, is_red_turn: bool) -> str:
        rows =[]
        for y in range(9, -1, -1):
            empty = 0; row_str = ""
            for x in range(9):
                p = self.board[y][x]
                if p == '': empty += 1
                else:
                    if empty > 0: row_str += str(empty); empty = 0
                    row_str += p
            if empty > 0: row_str += str(empty)
            rows.append(row_str)
        return "/".join(rows) + f" {'w' if is_red_turn else 'b'} - - 0 1"

    def parse_zh_to_iccs(self, move_str: str, is_red: bool) -> Optional[str]:
        if re.match(r'^[A-Ia-i][0-9]-[A-Ia-i][0-9]$', move_str): return move_str.upper()
        if len(move_str) != 4: return None
        piece_char, file_char, action, target_char = move_str[0], move_str[1], move_str[2], move_str[3]
        modifier = None
        if piece_char in "前中后": modifier = piece_char; piece_char = file_char
        pt = {'车':'R','車':'R','马':'N','馬':'N','炮':'C','砲':'C','相':'B','象':'B','仕':'A','士':'A','帅':'K','将':'K','兵':'P','卒':'P'}.get(piece_char)
        if not pt: return None
        if not is_red: pt = pt.lower()
        
        candidates = [(x, y) for y in range(10) for x in range(9) if self.board[y][x] == pt]
        if not candidates: return None
        
        fx, fy = -1, -1
        if modifier:
            candidates.sort(key=lambda pos: pos[1], reverse=is_red)
            if modifier == '前': fx, fy = candidates[0]
            elif modifier == '后': fx, fy = candidates[-1]
            elif modifier == '中': fx, fy = candidates[1] if len(candidates) >= 3 else candidates[0]
        else:
            num = self.NUM_MAP.get(file_char)
            if num is None: return None
            expected_x = (9 - num) if is_red else (num - 1)
            for c in candidates:
                if c[0] == expected_x: fx, fy = c; break
        if fx == -1: return None
        
        target_num = self.NUM_MAP.get(target_char)
        if target_num is None: return None
        
        tx, ty = fx, fy
        is_step = pt.upper() in ['R', 'C', 'P', 'K']
        
        if action == '平': tx = (9 - target_num) if is_red else (target_num - 1)
        elif action == '进':
            if is_step: ty = fy + target_num if is_red else fy - target_num
            else:
                tx = (9 - target_num) if is_red else (target_num - 1); dx = abs(tx - fx)
                if pt.upper() == 'N': ty = fy + (2 if dx == 1 else 1) if is_red else fy - (2 if dx == 1 else 1)
                elif pt.upper() == 'B': ty = fy + 2 if is_red else fy - 2
                elif pt.upper() == 'A': ty = fy + 1 if is_red else fy - 1
        elif action == '退':
            if is_step: ty = fy - target_num if is_red else fy + target_num
            else:
                tx = (9 - target_num) if is_red else (target_num - 1); dx = abs(tx - fx)
                if pt.upper() == 'N': ty = fy - (2 if dx == 1 else 1) if is_red else fy + (2 if dx == 1 else 1)
                elif pt.upper() == 'B': ty = fy - 2 if is_red else fy + 2
                elif pt.upper() == 'A': ty = fy - 1 if is_red else fy + 1

        if 0 <= fx <= 8 and 0 <= fy <= 9 and 0 <= tx <= 8 and 0 <= ty <= 9:
            return f"{chr(fx + ord('A'))}{fy}-{chr(tx + ord('A'))}{ty}"
        return None

    def make_move(self, move_str: str, is_red: bool) -> tuple:
        iccs_move = self.parse_zh_to_iccs(move_str, is_red)
        if not iccs_move: return None, False
        match = re.match(r'([A-I])([0-9])-([A-I])([0-9])', iccs_move)
        if not match: return None, False
        
        fx, fy = ord(match.group(1)) - ord('A'), int(match.group(2))
        tx, ty = ord(match.group(3)) - ord('A'), int(match.group(4))
        
        piece = self.board[fy][fx]
        captured = self.board[ty][tx]
        
        is_river_cross = False
        if piece == 'P' and fy <= 4 and ty >= 5: is_river_cross = True
        if piece == 'p' and fy >= 5 and ty <= 4: is_river_cross = True
            
        self.board[ty][tx] = piece
        self.board[fy][fx] = ''
        return captured, is_river_cross

    def is_king_alive(self) -> bool:
        return any('K' in row for row in self.board) and any('k' in row for row in self.board)

class SimplePGNParser:
    @staticmethod
    def parse(pgn_path: str) -> Optional[Dict]:
        try:
            content = ""
            for enc in ['gb18030', 'utf-8', 'big5']:
                try:
                    with open(pgn_path, 'r', encoding=enc) as f: content = f.read(); break
                except UnicodeDecodeError: continue
            if not content:
                with open(pgn_path, 'r', encoding='gb18030', errors='ignore') as f: content = f.read()
            
            result_match = re.search(r'\[Result\s+"([^"]+)"\]', content)
            red_match = re.search(r'\[Red\s+"([^"]+)"\]', content)
            black_match = re.search(r'\[Black\s+"([^"]+)"\]', content)
            red_name = red_match.group(1) if red_match else "UnknownRed"
            black_name = black_match.group(1) if black_match else "UnknownBlack"
            fen_match = re.search(r'\[FEN\s+"([^"]+)"\]', content)
            
            text_clean = re.sub(r'\$\d+', '', re.sub(r'\(.*?\)', '', re.sub(r'\{.*?\}', '', re.sub(r'\[.*?\]', '', content), flags=re.DOTALL), flags=re.DOTALL))
            moves = re.findall(r'[A-Ia-i][0-9]-[A-Ia-i][0-9]|(?:[前中后][车車马馬炮砲相象仕士帅将兵卒]|[车車马馬炮砲相象仕士帅将兵卒][一二三四五六七八九123456789１２３４５６７８９])[进退平][一二三四五六七八九123456789１２３４５６７８９]', text_clean)

            res_str = result_match.group(1) if result_match else "*"
            if "1/2" in res_str: res_str = "1/2-1/2"
            return {'red': red_name, 'black': black_name, 'result': res_str, 'moves': moves, 'start_fen': fen_match.group(1) if fen_match else None}
        except: return None

class SacrificeAnalyzer:
    @staticmethod
    def analyze(game_data: Dict) -> Dict[str, EngineStats]:
        red_name, black_name = game_data['red'], game_data['black']
        stats = {red_name: EngineStats(), black_name: EngineStats()}
        stats[red_name].total_games = stats[black_name].total_games = 1
        
        board = XiangqiBoard(game_data.get('start_fen'))
        is_red_turn = not (game_data.get('start_fen') and game_data['start_fen'].split()[1] == 'b')
        history = []
        
        game_flags = {
            red_name: {'kh': False, 'pp': False, 'cc': False, 'ec': False},
            black_name: {'kh': False, 'pp': False, 'cc': False, 'ec': False}
        }
        
        for ply, move_str in enumerate(game_data['moves']):
            history.append({
                'fen': board.to_fen(is_red_turn),
                'mat_red': board.get_material(True),
                'mat_black': board.get_material(False),
                'is_red_turn': is_red_turn
            })
            
            captured, is_river_cross = board.make_move(move_str, is_red_turn)
            player_name = red_name if is_red_turn else black_name
            
            if is_river_cross and ply < 80:
                stats[player_name].early_river_crosses += 1

            red_k_pos, black_k_pos = None, None
            red_pp, black_pp = 0, 0
            red_cc, black_cc = 0, 0

            for y in range(10):
                for x in range(9):
                    piece = board.board[y][x]
                    if piece == 'K': red_k_pos = (x, y)
                    elif piece == 'k': black_k_pos = (x, y)
                    elif piece == 'C' and x == 4 and y >= 5: red_cc += 1
                    elif piece == 'c' and x == 4 and y <= 4: black_cc += 1
                    elif piece == 'P' and 7 <= y <= 9 and 3 <= x <= 5: red_pp += 1
                    elif piece == 'p' and 0 <= y <= 2 and 3 <= x <= 5: black_pp += 1
            
            red_empty_cc, black_empty_cc = 0, 0
            if black_k_pos and black_k_pos[0] == 4:
                for cy in range(black_k_pos[1] - 1, -1, -1):
                    p = board.board[cy][4]
                    if p != '':
                        if p == 'C' and cy >= 5: red_empty_cc += 1
                        break
            if red_k_pos and red_k_pos[0] == 4:
                for cy in range(red_k_pos[1] + 1, 10):
                    p = board.board[cy][4]
                    if p != '':
                        if p == 'c' and cy <= 4: black_empty_cc += 1
                        break
            
            active_mat_red = board.get_material(True) - 1000000
            active_mat_black = board.get_material(False) - 1000000
            total_active = active_mat_red + active_mat_black
            
            if total_active >= 4000:
                if black_k_pos and (black_k_pos[0] != 4 or black_k_pos[1] == 7): game_flags[red_name]['kh'] = True
                if red_k_pos and (red_k_pos[0] != 4 or red_k_pos[1] == 2): game_flags[black_name]['kh'] = True
                    
                if red_pp > 0: game_flags[red_name]['pp'] = True
                if black_pp > 0: game_flags[black_name]['pp'] = True
                
                if red_cc > 0: game_flags[red_name]['cc'] = True
                if black_cc > 0: game_flags[black_name]['cc'] = True
                
                if red_empty_cc > 0: game_flags[red_name]['ec'] = True
                if black_empty_cc > 0: game_flags[black_name]['ec'] = True
                
            if captured in ['A', 'a'] and ply < 70: 
                stats[player_name].advisor_breaks += 1
            elif captured in ['B', 'b'] and ply < 70:
                stats[player_name].elephant_breaks += 1
                
            if captured in ['K', 'k'] or not board.is_king_alive():
                is_red_turn = not is_red_turn
                break
            is_red_turn = not is_red_turn
        
        for pname in [red_name, black_name]:
            if game_flags[pname]['kh']: stats[pname].king_hunts += 1
            if game_flags[pname]['pp']: stats[pname].palace_pawns += 1
            if game_flags[pname]['cc']: stats[pname].center_cannons += 1
            if game_flags[pname]['ec']: stats[pname].empty_cannons += 1

        history.append({
            'fen': board.to_fen(is_red_turn),
            'mat_red': board.get_material(True),
            'mat_black': board.get_material(False),
            'is_red_turn': is_red_turn
        })

        skip_until = 0
        start_ply = IGNORE_OPENING_MOVES * 2 
        
        # 【修改核心】单局暂存变量：不急着加给全局，以防输掉这局后把白给当成了弃子
        red_sacs, red_rook_sacs = 0, 0
        red_sac_plies, red_sac_values = [], []
        
        black_sacs, black_rook_sacs = 0, 0
        black_sac_plies, black_sac_values = [], []
        
        for i in range(start_ply, len(history) - SAC_HORIZON):
            if i < skip_until: continue
            
            prev, future = history[i], history[i + SAC_HORIZON]
            is_red = prev['is_red_turn']
            
            mb_prev = prev['mat_red'] - prev['mat_black'] if is_red else prev['mat_black'] - prev['mat_red']
            mb_future = future['mat_red'] - future['mat_black'] if is_red else future['mat_black'] - future['mat_red']
            
            net_mat = mb_future - mb_prev
            
            if net_mat <= -PAWN_UNIT:
                material_lost = abs(net_mat)
                my_rook_char = 'R' if is_red else 'r'
                rooks_before = prev['fen'].split(' ')[0].count(my_rook_char)
                rooks_after = future['fen'].split(' ')[0].count(my_rook_char)
                
                is_rook_sac = (rooks_before > rooks_after and material_lost >= 500.0)
                
                if is_red:
                    red_sacs += 1
                    red_sac_plies.append(i)
                    red_sac_values.append(material_lost)
                    if is_rook_sac: red_rook_sacs += 1
                else:
                    black_sacs += 1
                    black_sac_plies.append(i)
                    black_sac_values.append(material_lost)
                    if is_rook_sac: black_rook_sacs += 1
                    
                skip_until = i + SAC_HORIZON 
        
        actual_moves = (len(history) - 1) / 2.0 
        
        if game_data['result'] == '1-0':
            stats[red_name].wins = stats[black_name].losses = 1
            stats[red_name].win_moves.append(actual_moves)
            # 【修改核心】红方赢了，才真正认定红方这局的战损是精彩的“弃子进攻”
            stats[red_name].sacs += red_sacs
            stats[red_name].rook_sacs += red_rook_sacs
            stats[red_name].sac_plies.extend(red_sac_plies)
            stats[red_name].sac_values.extend(red_sac_values)
            stats[red_name].total_sac_value += sum(red_sac_values)
            # 黑方的战损(丢子)就真的是纯漏算失误，直接丢弃，不计入黑方的激进分！
        elif game_data['result'] == '0-1':
            stats[black_name].wins = stats[red_name].losses = 1
            stats[black_name].win_moves.append(actual_moves)
            # 黑方赢了，计入黑方的弃子进攻
            stats[black_name].sacs += black_sacs
            stats[black_name].rook_sacs += black_rook_sacs
            stats[black_name].sac_plies.extend(black_sac_plies)
            stats[black_name].sac_values.extend(black_sac_values)
            stats[black_name].total_sac_value += sum(black_sac_values)
        else:
            stats[red_name].draws = stats[black_name].draws = 1
            
            BAD_DRAW_THRESHOLD = PAWN_UNIT * 4 
            max_red_adv = 0.0
            max_black_adv = 0.0
            for i, snap in enumerate(history):
                if i < 20: 
                    continue
                red_adv = (snap['mat_red'] - 1000000) - (snap['mat_black'] - 1000000)
                if red_adv > max_red_adv: max_red_adv = red_adv
                if -red_adv > max_black_adv: max_black_adv = -red_adv
            if max_red_adv > BAD_DRAW_THRESHOLD:
                stats[red_name].bad_draws += 1
            if max_black_adv > BAD_DRAW_THRESHOLD:
                stats[black_name].bad_draws += 1
            
            # 血腥和棋利用本局临时战损，但不向各自引擎档案里累加弃子数，防止弱引擎熬平白嫖激进分
            red_major_sacs = sum(1 for val in red_sac_values if val >= 400.0)
            black_major_sacs = sum(1 for val in black_sac_values if val >= 400.0)
            total_sacs = red_sacs + black_sacs
            
            is_bloody = (red_rook_sacs > 0 or 
                         black_rook_sacs > 0 or
                         (red_major_sacs + black_major_sacs) >= 3 or
                         total_sacs >= 12)
            
            if is_bloody:
                stats[red_name].bloody_draws += 1
                stats[black_name].bloody_draws += 1
                
        return stats

class XEASCalculator:
    @staticmethod
    def calculate(stats: EngineStats) -> float:
        if stats.total_games == 0: return 0.0
            
        # 【修改核心】弃子率等数据均按"获胜局数"为分母归一化
        # 这确保了无论引擎本身的Elo多高/多低，只要它赢的时候经常使用弃子，那它就是激进的
        wins = stats.wins if stats.wins > 0 else 1
            
        shorts_per_win = stats.shorts / wins 
        bad_draws_per_draw = stats.bad_draws / stats.draws if stats.draws > 0 else 0.0
        bloody_draws_per_draw = stats.bloody_draws / stats.draws if stats.draws > 0 else 0.0
        
        score = 0.0 
        
        sacs_avg = stats.sacs / wins
        score += sacs_avg * 20000.0  
        
        # 阵地进攻指标相对不易被被动漏算影响，保留总局数分母
        plcpwn_rate = stats.palace_pawns / stats.total_games
        score += plcpwn_rate * 2500.0  
        
        khunt_rate = stats.king_hunts / stats.total_games
        score += khunt_rate * 4000.0
        
        cc_rate = stats.center_cannons / stats.total_games
        score += cc_rate * 1500.0    
        
        ec_rate = stats.empty_cannons / stats.total_games
        score += ec_rate * 8000.0  
        
        score += bloody_draws_per_draw * 15000.0
        
        score += (stats.advisor_breaks / stats.total_games) * 3000.0   
        score += (stats.elephant_breaks / stats.total_games) * 3000.0  
        
        score += (stats.early_river_crosses / stats.total_games) * 1000.0

        score += (stats.rook_sacs / wins) * 25000.0
        
        score += (stats.sac_score / wins) * 1.5  
        
        if shorts_per_win > 0:
            score += shorts_per_win * 15000.0  
        
        score -= bad_draws_per_draw * 10000.0      
            
        return max(score, 0.0)

class MultiThreadXEAS:
    def __init__(self, num_threads: int = 14):
        self.num_threads = num_threads
        self.engine_stats = defaultdict(EngineStats)

    def analyze_directory(self, pattern: str):
        pgn_files = glob.glob(pattern, recursive=True)
        if not pgn_files: 
            print("未找到PGN文件。")
            return
        
        start = time.time()
        completed = 0
        
        print(f"开始分析，使用 {self.num_threads} 个线程，纯静态极速解析...")
        with ThreadPoolExecutor(max_workers=self.num_threads) as executor:
            futures = [executor.submit(self._process_file, f) for f in pgn_files]
            for future in as_completed(futures):
                res = future.result()
                if res:
                    for eng, st in res.items():
                        total = self.engine_stats[eng]
                        total.total_games += st.total_games; total.wins += st.wins
                        total.losses += st.losses; total.draws += st.draws
                        total.win_moves.extend(st.win_moves)
                        total.sac_plies.extend(st.sac_plies); total.sac_values.extend(st.sac_values)
                        total.bad_draws += st.bad_draws; total.bloody_draws += st.bloody_draws
                        total.sacs += st.sacs; 
                        total.advisor_breaks += st.advisor_breaks; total.elephant_breaks += st.elephant_breaks
                        total.early_river_crosses += st.early_river_crosses; total.total_sac_value += st.total_sac_value
                        
                        total.king_hunts += st.king_hunts
                        total.palace_pawns += st.palace_pawns
                        total.center_cannons += st.center_cannons
                        total.empty_cannons += st.empty_cannons
                        total.rook_sacs += st.rook_sacs 

                completed += 1
                if completed % 100 == 0: 
                    print(f"进度: {completed}/{len(pgn_files)}...")

        global_wins = sum(st.wins for st in self.engine_stats.values())
        global_win_moves_total = sum(sum(st.win_moves) for st in self.engine_stats.values())
        global_avg_win_moves = (global_win_moves_total / global_wins) if global_wins > 0 else 80.0
        
        decay_constant_moves = max(global_avg_win_moves / 2.0, 20.0) 
        short_win_threshold = global_avg_win_moves * 0.75  
        
        for st in self.engine_stats.values():
            st.early_sacs = sum(1 for ply in st.sac_plies if (ply/2.0) <= decay_constant_moves)
            score_sum = 0.0
            for ply, val in zip(st.sac_plies, st.sac_values):
                val_in_pawns = val / PAWN_UNIT
                score_sum += (5000.0 + val_in_pawns * 4000.0) * math.exp(-(ply / 2.0) / decay_constant_moves)
            st.sac_score = score_sum
            st.shorts = sum(1 for moves in st.win_moves if moves <= short_win_threshold)

        print(f"分析完毕！耗时: {time.time() - start:.2f} 秒")

    def _process_file(self, pgn_file: str):
        game = SimplePGNParser.parse(pgn_file)
        if not game: return None
        return SacrificeAnalyzer.analyze(game)

    def print_report(self):
        print("\n" + "="*115)
        print("★ 中国象棋 XEAS (Engine Aggression Score) 引擎激进评分报告 ★")
        print("="*115)
        
        results = [(XEASCalculator.calculate(st), name, st) for name, st in self.engine_stats.items()]
        results.sort(reverse=True, key=lambda x: x[0])
        
        # 【修改核心】将报表头端的展示口径改为均局胜场 /W (per Win)
        print(f"{'Engine':<20} | {'Games':<5} | {'Win%':<5} | {'XEAS-Score':<10} | {'RookSac/W':<9} | {'Brks/G':<7} | {'Ec/Cc(%)':<9} | {'RivCrs/G':<8} | {'Sacs/W':<7} | {'Bloody/D':<8}")
        print("-" * 115)
        
        for score, name, st in results:
            tg = st.total_games if st.total_games > 0 else 1
            wins = st.wins if st.wins > 0 else 1
            win_pct = (st.wins / tg) * 100
            
            rooksac_per_w = st.rook_sacs / wins
            breaks_per_g = (st.advisor_breaks + st.elephant_breaks) / tg
            
            ec_cc_str = f"{int(st.empty_cannons/tg*100)}/{int(st.center_cannons/tg*100)}"
            
            rivcrs_per_g = st.early_river_crosses / tg
            sacs_per_w = st.sacs / wins
            
            bloody_draws_pct = (st.bloody_draws / st.draws * 100) if st.draws > 0 else 0.0
            
            display_name = name[:18] + ".." if len(name) > 20 else name
            print(f"{display_name:<20} | {st.total_games:<5} | {win_pct:<5.1f} | {int(score):<10} | {rooksac_per_w:<9.2f} | {breaks_per_g:<7.1f} | {ec_cc_str:<9} | {rivcrs_per_g:<8.1f} | {sacs_per_w:<7.2f} | {bloody_draws_pct:>7.1f}%")
            
        print("-" * 115)

def main():
    target_dir = r".\**\*.pgn"
    analyzer = MultiThreadXEAS(num_threads=14)
    analyzer.analyze_directory(target_dir)
    analyzer.print_report()

if __name__ == '__main__':
    main()