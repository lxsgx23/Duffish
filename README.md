# Duffish Xiangqi Engine


<div align="center">
  <img width="300" height="300" alt="Duffish_logo" src="https://github.com/user-attachments/assets/3372feb1-2b44-46e3-8bac-15013a5657bd" />
</div>



**Duffish** is a highly aggressive chess variant engine derived from [Fairy‑Stockfish](https://github.com/fairy-stockfish/Fairy-Stockfish). It inherits full support for Xiangqi and dozens of other variants while introducing dedicated evaluation features that promote sharp, sacrificial, and uncompromising play.


---

## Aggressiveness

### Measuring Aggressiveness
Aggressiveness in Duffish is quantified using the **Xiangqi Engine Aggressiveness Score (XEAS)**, a metric developed for Xiangqi engines.  
Typical engines score between **40,000** and **80,000** XEAS. Duffish 1.1 targets a comparable level of aggression -around **153,867** -in the Xiangqi domain while remaining highly competitive in playing strength.  
On the **Kaka's rating list**, Duffish 1 reaches approximately **2933.9 Elo**, well above human grandmaster level.

### XEAS Comparison

| Engine | Elo | XEAS | Discription |
|---|---|---|---|
| Duffish 1 | 2933.9 | 334,250 | against sachess 1.6 |
| Duffish 1.1 | 3404.0 | 153,867 | against FSF |

| Engine | Games | XEAS | Sacs% | E.Sacs% | Shorts% | BadDraw% | AvgWinMvs |
|---|---|---|---|---|---|---|---|
| **Duffish 1.1** | 304 | **134,180** | 27.96 | 23.36 | 1.97 | 27.30 | 155.5 |
| Fairy‑Stockfish 14 NNUE | 304 | 67,594 | 15.79 | 13.49 | 1.32 | 47.37 | 163.8 |

> **Average winning moves:** 159.7 moves 

> **Quick‑win threshold:** 116.6 moves

*using same xiangqi NNUE eval file

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

*Since Duffish 1.2,some specialized UCI option for Xiangqi has been added,please disable them to play other variants.*

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

## More Information

| Engine | Games | Elo | XEAS | Sacs% | E.Sacs% | Shorts% | BadDraw% | AvgWinMvs |
|---|---|---|---|---|---|---|---|---|
| Pikafish 2026-01-31 | 2000 | 4006.0 | 55,406 | 12.95 | 6.35 | 6.05 | 16.75 | 156.1 |
| Cyclone 2026-04-20 [Ulitimate] | 6000 | 3988.6 | 54,108 | 11.82 | 6.07 | 7.93 | 19.90 | 149.9 |
| Bugchess 2026-01-18 | 2000 | 3929.3 | 49,496 | 13.60 | 6.45 | 5.05 | 22.40 | 157.7 |
| Cyclone 2026-01-01 [Ulitimate] | 2000 | 3952.9 | 45,018 | 11.25 | 5.75 | 6.35 | 24.25 | 153.1 |

> **Average winning moves:** 152.4 moves 

> **Quick‑win threshold:** 111.3 moves

## Credits

Duffish is built on [Fairy‑Stockfish](https://github.com/fairy-stockfish/Fairy-Stockfish) by Fabian Fichter, which in turn derives from [Stockfish](https://stockfishchess.org/).  

The aggressiveness scoring methodology is inspired by the **Patricia** and the **Engine Aggressiveness Score (EAS)** concept.

Thanks x for testing my engine,and making the first XEAS version!

---

## License

Duffish is distributed under the **GNU General Public License v3.0**.  

