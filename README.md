# SenseRing

**Open-source biosensing and motion tracking, packed into a ring you can build, program, and improve.**

SenseRing is a compact, finger-worn research and development platform built around the Nordic Semiconductor [nRF52832](https://www.nordicsemi.com/Products/nRF52832) Bluetooth Low Energy SoC. Its custom 14.9 × 10 mm PCB combines optical pulse sensing, motion sensing, wireless connectivity, battery charging, and power management in a wearable form factor.

Use it to explore photoplethysmography (PPG), heart-rate and SpO₂ algorithm development, activity and gesture recognition, low-power Bluetooth applications, and new wearable interfaces. The repository contains the editable electronics and enclosure sources—not just renders—so the design can be studied, reproduced, and extended.

<p align="center">
  <img src="Images/Hand.jpg" alt="SenseRing 1.0 prototype worn on a finger" width="500" />
</p>

> [!IMPORTANT]
> SenseRing is an experimental research platform, not a certified medical device. Do not use it for diagnosis, treatment, or safety-critical monitoring.

## Hardware features

| Function | Implementation |
| --- | --- |
| Processing and wireless | [nRF52832](https://www.nordicsemi.com/Products/nRF52832), 64 MHz Arm Cortex-M4F with Bluetooth Low Energy and a 2.4 GHz radio |
| Optical sensing | [MAX30102](https://www.analog.com/en/products/max30102.html) integrated red/IR PPG sensor for pulse-oximetry and heart-rate experiments |
| Motion sensing | [BMA530](https://www.bosch-sensortec.com/en/products/motion-sensors/accelerometers/bma530) ultra-compact, low-power 3-axis accelerometer with motion interrupts and step-counting support |
| Battery charging | [BQ25180](https://www.ti.com/product/BQ25180) single-cell Li-ion/LiPo charger with I²C control, power path, and ship mode |
| Power and protection | AP6683 battery protection, XC6504A1819R-G 1.8 V LDO, and two WS4622C load switches for the PPG LED supply and battery-measurement circuit |
| RF front end | 2450AT18B100E 2.4 GHz chip antenna and 32 MHz crystal |
| Debugging and test | TC2030 SWD footprint plus reset, I²C, UART-labelled, battery, and power test points |
| PCB | 14.9 × 10 mm, two copper layers, fine-pitch WLCSP and 0201 components |

<p align="center">
  <img src="Images/Ring.jpg" alt="SenseRing optical sensor operating inside the ring" width="46%" />
  <img src="Images/Exploded%20View.png" alt="Exploded rendering of the SenseRing enclosure, battery, and circuit board" width="46%" />
</p>

## Architecture

The nRF52832 communicates with the PPG sensor, accelerometer, and charger over a shared I²C bus. Dedicated interrupt lines allow the sensors and PMIC to wake the processor. The PPG LED supply and battery-measurement divider are independently switchable, limiting leakage when those functions are idle.

| Signal | nRF52832 pin | Connected function |
| --- | --- | --- |
| `SDA` | `P0.29 / AIN5` | MAX30102, BMA530, and BQ25180 data |
| `SCL` | `P0.28 / AIN4` | MAX30102, BMA530, and BQ25180 clock |
| `PPG_INT` | `P0.06` | MAX30102 interrupt |
| `IMU_INT` | `P0.18` | BMA530 interrupt |
| `PMIC_INT` | `P0.15` | BQ25180 interrupt |
| `LED_EN` | `P0.01` | PPG LED-rail load switch |
| `VBAT_EN` | `P0.00` | Battery-divider load switch |
| `VBAT_MEAS` | `P0.05 / AIN3` | Battery-voltage ADC input |
| `TX` test pad | `P0.30 / AIN6` | Firmware-assignable test/UART signal |
| `RX` test pad | `P0.25` | Firmware-assignable test/UART signal |

## Repository contents

| Path | Contents |
| --- | --- |
| [`Hardware/`](Hardware/) | Autodesk Fusion Electronics/EAGLE schematic and board sources (`.fsch`, `.fbrd`, `.sch`, `.brd`) and a ready-to-view [schematic PDF](Hardware/SenseRing_schematic.pdf) |
| [`Mechanical/`](Mechanical/) | Complete Fusion 360 archive, STEP and OBJ assemblies, printable shell STLs, board and battery models, and two programming-fixture STLs |
| [`Images/`](Images/) | Prototype photographs and the exploded assembly render used in this README |
| [`Software/`](Software/) | Reserved for firmware; no firmware application is included in the current release |

## Working with the design

### 1. Clone the repository

```bash
git clone https://github.com/Sense-Ring/Sense-Ring-1.0.git
cd Sense-Ring-1.0
```

### 2. Inspect or modify the electronics

- Open [`SenseRing.fsch`](Hardware/SenseRing.fsch) and [`SenseRing.fbrd`](Hardware/SenseRing.fbrd) in Autodesk Fusion Electronics.
- The XML [`SenseRing.sch`](Hardware/SenseRing.sch) and [`SenseRing.brd`](Hardware/SenseRing.brd) files are also included for EAGLE-compatible workflows.
- For a quick review without CAD software, open [`SenseRing_schematic.pdf`](Hardware/SenseRing_schematic.pdf).

### 3. Inspect or print the enclosure

- Open [`RingCase.f3z`](Mechanical/RingCase.f3z) for the editable Fusion 360 assembly.
- Use [`RingCase.step`](Mechanical/RingCase.step) or [`RingCase.obj`](Mechanical/RingCase.obj) for interchange with other CAD tools.
- Print [`InnerShell.stl`](Mechanical/InnerShell.stl) and [`OuterShell.stl`](Mechanical/OuterShell.stl) for the enclosure.
- The repository also provides board and battery reference models and two-piece programming-fixture STLs.

### 4. Program and debug

The PCB exposes `SWDIO`, `SWDCLK`, `RESET`, 1.8 V, +5 V, and GND through its TC2030 programming footprint. Use a compatible SWD debugger and verify the target-voltage and power configuration before connecting it to the board.


## Manufacturing notes

This is a miniaturized wearable design using WLCSP/BGA packages and 0201 passives. Professional PCB assembly and appropriate inspection equipment are strongly recommended.

The current repository does **not** include production exports such as Gerbers, drill files, a bill of materials, or pick-and-place data. Generate and verify those outputs from the hardware sources before ordering boards. Battery selection, charge settings, clearances, enclosure material, skin contact, RF performance, and optical performance must all be validated for your implementation.


## Contributing

Issues, design reviews, firmware ports, enclosure variants, measurements, and reproducibility notes are welcome. When reporting a hardware problem, please include the board revision, power source, programmer/debugger, and enough measurements or logs to reproduce it.

## License

SenseRing 1.0 is open source and distributed under the [GNU General Public License v3.0](LICENSE). It is provided as-is, without warranty.

