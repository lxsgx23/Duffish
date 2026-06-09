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

| Option               | Range       | Default | Description                                                                                                                                                                                                                                                                                                                                 |
|----------------------|-------------|---------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Aggressiveness**   | 0 – 200     | 100     | Controls the overall playing style.<br> **>100** → **more aggressive:** futility margins tighten, null‑move reduction increases, ProbCut threshold lowers, check extensions trigger more easily, LMR reduction decreases for attacking moves, king‑attack contribution increases, and overall evaluation is amplified.<br> **<100** → **more conservative** (reverse effects).<br> **=100** → neutral (no effect). |
| **DrawValue**        | −100 – 100  | −39     | Affects only the draw evaluation of the **side to move at the root**; the opponent’s draw score is left unchanged.Under the AXF chasing rule in Xiangqi, the effect of Contempt may be partially suppressed, but it remains active in all other supported variants.                                                                                                                                                                                                                         |
| **SacBonus**         | 0 – 100     | 30      | Extra evaluation bonus added when the side to move trails in material, rewarding sacrificial play.                                                                                                                                                                                                                                          |
| **DynamicComp**      | 0 – 100     | 43      | When positive, injects a dynamic compensation term proportional to the evaluation advantage, discouraging the engine from becoming excessively passive in favorable positions.                                                                                                                                                              |
| **AdvisorBreakBonus**| 0 – 100     | 10      | Awarded when the side to move captures or breaks an opponent’s advisor, encouraging attacks that dismantle the king’s defensive structure.                                                                                                                                                                                                 |
| **BishopBreakBonus** | 0 – 100     | 15      | Awarded when the side to move captures or breaks an opponent’s bishop (elephant), promoting positional sacrifices to open the board or weaken the defence.                                                                                                                                                                                 |
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

