# Switch 2 controllers and Joy-Cons (Windows)

## Switch 2 controllers over Bluetooth

SDL drives Switch 2 controllers over USB only, so SuperWonders has its own Bluetooth LE driver for them
(`runtime/src/switch2/`, from [mkw-recomp](https://github.com/adriadam10/mkw-recomp), MIT): Pro Controller 2 and
Joy-Con 2, left and right.

- **Connecting one the first time:** start the game, then hold the controller's small Sync button until its lights
  run. It pairs with this PC: from then on any button press connects it while the game runs. It is not paired in
  Windows' Bluetooth settings, and it does not need to be.
- **Rumble:** the game's own HD rumble (amplitude and frequency of both bands, left and right actuator) is played as it
  is. The rumble strength setting still applies.

## DualSense (Windows)

A DualSense plays the game's HD rumble through its voice coils, over its USB cable (the controller's own four-channel sound card; over Bluetooth it keeps the plain rumble), with
[RenHDrum](https://github.com/trzz1214/RenHDrum) (`renhdrum.dll` beside the program, loaded at run time; `NX_DUALSENSE_HAPTICS=0` turns it off). Without
the DLL it plays the plain SDL rumble as before. The rumble strength setting still applies.
- **Player lights** show the player each controller plays as.
- `NX_SWITCH2=0` turns the driver off; `NX_SWITCH2=2` also logs each Bluetooth LE device it sees once (for a
  controller that does not connect: send the log, `NX_LOG=nx.log`).

It needs Windows 10 or 11 with a Bluetooth adapter that does Bluetooth LE, and a build made with LLVM-MinGW (the
release toolchain). Switch 1 controllers keep going through SDL, over Bluetooth or USB, as before.

## Joy-Cons: pairs and sideways

Joy-Cons, Switch 1 or Switch 2, come in one by one and are made into controllers as on a Switch's Change Grip/Order
screen:

| What | How |
|---|---|
| Two Joy-Cons as one controller | Automatic: a left and a right Joy-Con, both upright, pair in the order they connected. A Switch 1 and a Switch 2 Joy-Con pair too. |
| One Joy-Con held sideways, as its own player | Hold **SL and SR** on it for one second. It leaves its pair (the other Joy-Con waits for a new partner) and plays alone: the stick is the stick, the four buttons on the side are A/B/X/Y by where they are, SL/SR are L/R, and − or + pauses. |
| Back to upright | Hold SL and SR again for one second. |
| Two sideways Joy-Cons into one controller again | Hold **L** on the left one and **R** on the right one for one second, together. |

A short buzz and a notice confirm each change. The player lights show who plays as which player. Up to eight devices
(for example four pairs of Joy-Cons) make up to four controllers; the Controls section of the menu assigns them to
players as before.
