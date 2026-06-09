# Duffish Xiangqi Engine


<div align="center">
<img width="450" height="450" alt="logo已修改" src="https://github.com/user-attachments/assets/ef279ca5-92ac-4b8e-88ed-aee491ebe7fd" />
</div>



**Duffish** is a highly aggressive chess variant engine derived from [Fairy‑Stockfish](https://github.com/fairy-stockfish/Fairy-Stockfish). It inherits full support for Xiangqi while introducing dedicated evaluation features that promote sharp, sacrificial, and uncompromising play.


---

## Aggressiveness

### Measuring Aggressiveness
Aggressiveness in Duffish is quantified using the **Xiangqi Engine Aggressiveness Score (XEAS)**, a metric developed for Xiangqi engines.  
Typical engines score between **30,000** and **80,000** XEAS. Duffish 3 targets a comparable level of aggression -around **134,597** -in the Xiangqi domain while remaining highly competitive in playing strength.  
On the **Kaka's rating list**, Duffish 1 reaches approximately **3060 Elo**, well above human grandmaster level.

### XEAS Comparison

| Engine | Games | XEAS | Elo |
|---|---|---|---|
| **Duffish 3** | 500 | **134,597** | 3060.3 |
| Pikafish HCE 20240522 | 500 | 72,639 | 3107.8 |

---

## Customization Options

Duffish supports **all standard UCI options** from Fairy‑Stockfish, plus the following custom parameters that fine‑tune its aggressive behavior.

| Option | Range | Default | Description |
|---|---:|---:|---|
| **Aggressiveness** | 0-300 | 165 | Controls the engine's overall attacking style.<br>**>100** = more aggressive: tighter pruning margins, stronger attacking move preference, easier attack/sacrifice continuation handling, higher king-attack pressure weight, and amplified evaluation.<br>**<100** = more conservative.<br>**100** = neutral. |
| **DrawValue** | -100-100 | -39 | Sets the draw score for the side to move at the root only; the opponent's draw score is unchanged. Under the AXF chasing rule in Xiangqi, this effect may be partially suppressed, but it remains active in other supported variants. |
| **SacBonus** | 0-200 | 45 | Adds an evaluation bonus when the root side has sacrificed material and the position shows enough attacking compensation. Higher values encourage sacrificial play. |
| **DynamicComp** | 0-150 | 63 | Adds dynamic compensation based on attack quality and evaluation trends, helping the engine avoid overly passive play in promising attacking positions. |
| **AdvisorBreakBonus** | 0-150 | 35 | Adds a bonus when the opponent has lost advisors, encouraging attacks that damage the king's defensive structure. |
| **BishopBreakBonus** | 0-150 | 16 | Adds a bonus when the opponent has lost bishops/elephants, encouraging attacks that weaken defensive coverage and open the board. |
| **MatScale** | 0-150 | 10 | Scales positive evaluations upward based on remaining material and sacrifice size, making favorable attacking/material-compensation positions score more clearly. |
| **SacDetect** | 0-150 | 12 | Enables and weights Patricia-style sacrifice detection from the root side's perspective. Higher values increase bonuses for sacrifice sequences with sufficient attack pressure, trace, and compensation quality. |
| **DrawMatBias** | -100-100 | 25 | Biases draw evaluations according to root-side material imbalance, making drawn lines slightly more or less attractive depending on the material situation. |
| **EvalDecay** | 0-100 | 35 | Gradually reduces non-zero evaluations as the move-rule counter increases, dampening optimistic scores in positions approaching draw-rule territory. |
---

## Compilation

Duffish builds exactly like Fairy‑Stockfish. Please refer to the [Fairy‑Stockfish build instructions](https://github.com/fairy-stockfish/Fairy-Stockfish#compilation) for your platform.  
A quick start:

```bash
git clone https://github.com/lxsgx23/Duffish.git
cd Duffish/src
make -j profile-build
```

Replace `profile-build` with the appropriate target if necessary (e.g., `build` for a debug version).

---

## Usage

Duffish is a standard **UCI engine** and works with any UCI‑compatible GUI (e.g. Sharkchess).  

---

## Credits

Duffish is built on [Fairy‑Stockfish](https://github.com/fairy-stockfish/Fairy-Stockfish) by Fabian Fichter, which in turn derives from [Stockfish](https://stockfishchess.org/).  

The aggressiveness scoring methodology is inspired by the **Patricia** and the **Engine Aggressiveness Score (EAS)** concept.

Thanks x , rui for testing my engine!

---

## License

Duffish is distributed under the **GNU General Public License v3.0**.  

