# Duffish Xiangqi Engine
<img width="1254" height="1254" alt="Duffish_logo" src="https://github.com/user-attachments/assets/3372feb1-2b44-46e3-8bac-15013a5657bd" />

**Duffish** is a highly aggressive chess variant engine derived from [Fairy‑Stockfish](https://github.com/fairy-stockfish/Fairy-Stockfish). It inherits full support for Xiangqi and dozens of other variants while introducing dedicated evaluation features that promote sharp, sacrificial, and uncompromising play.


---

## Aggressiveness

### Measuring Aggressiveness
Aggressiveness in Duffish is quantified using the **Xiangqi Engine Aggressiveness Score (XEAS)**, a metric developed for Xiangqi engines.  
Typical engines score between **50,000** and **165,000** XEAS. Duffish targets a comparable level of aggression -around **334,250** -in the Xiangqi domain while remaining highly competitive in playing strength.  
On the **Kaka's rating list**, Duffish 1 reaches approximately **2933.9 Elo**, well above human grandmaster level.

### Where Does the Aggressiveness Come From?

- **Default Contempt of −39 centipawns**  
  Negative Contempt makes the engine avoid draws and actively seek winning chances.  
  *Note:* Under the AXF chasing rule in Xiangqi, the effect of Contempt may be partially suppressed, but it remains active in all other supported variants.

- **Partially Asymmetrical Evaluation**  
  Sacrifice‑related evaluation bonuses are **only** applied to the side to move at the root. The opponent never receives an aggressiveness bonus for giving up material. This design ensures that every sacrifice bonus directly benefits the engine’s own attacking ambitions.

- **Sacrifice Bonus (`SacBonus`)**  
  A dedicated evaluation term that grants an extra score when the side to move is materially behind (i.e., has sacrificed material), actively encouraging sacrificial attacks.

- **Dynamic Compensation (`DynamicComp`)**  
  A positive compensation factor that makes the engine more confident when it holds an advantage. It dynamically scales the evaluation to prevent overly conservative play in winning positions.

---

## Customization Options

Duffish supports **all standard UCI options** from Fairy‑Stockfish, plus the following custom parameters that fine‑tune its aggressive behavior.

| Option             | Range       |  Default               | Description                                                                                                                                                                                                                                                                                                                                 |
|--------------------|-------------|------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Aggressiveness** | 0 – 200     | 100                    | Controls the overall playing style.<br> **>100** → **more aggressive:** futility margins tighten, null‑move reduction increases, ProbCut threshold lowers, check extensions trigger more easily, LMR reduction decreases for attacking moves, king‑attack contribution increases, and overall evaluation is amplified.<br> **<100** → **more conservative** (reverse effects).<br> **=100** → neutral (no effect). |
| **Contempt**       | −100 – 100  | −39                    | Positive values encourage avoiding draws (contempt for the opponent); negative values increase willingness to accept draws. Affects only the draw evaluation of the **side to move at the root**; the opponent’s draw score is left unchanged.                                                                                                       |
| **SacBonus**       | 0 – 100     | 30                     | Extra evaluation bonus added when the side to move trails in material, rewarding sacrificial play.                                                                                                                                                                                                                                          |
| **DynamicComp**    | 0 – 100     | 43                     | When positive, injects a dynamic compensation term proportional to the evaluation advantage, discouraging the engine from becoming excessively passive in favorable positions.                                                                                                                                                              |

*Values given as “Default” hasn't benn tuned for the best XEAS performance,because I don't have enough computing power.*

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

Duffish is a standard **UCI engine** and works with any UCI‑compatible GUI (e.g., Sharkchess).  

---

## Credits

Duffish is built on [Fairy‑Stockfish](https://github.com/fairy-stockfish/Fairy-Stockfish) by Fabian Fichter, which in turn derives from [Stockfish](https://stockfishchess.org/).  
The aggressiveness scoring methodology is inspired by the **Patricia** and the **Engine Aggressiveness Score (EAS)** concept.

---

## License

Duffish is distributed under the **GNU General Public License v3.0**.  

