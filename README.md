# Mogue

A VCV Rack 2 plugin containing one module, **Mogue**: a semi-modular, Moog-style synth voice in the
spirit of the Mother-32, without the sequencer. It is a 44 HP module, and it is polyphonic: it runs
up to 16 voices, one for each channel on the V/OCT and GATE inputs.

The panel is laid out in columns, with the patch bay on the right edge: each section's controls run top to bottom. VCO 1, 2 and 3, VCO
MOD and the mixer sit side by side at the top left; below them are the two contours, as
horizontal strips (FILTER EG above AMP EG, each attack → release left to right), and the output. FILTER, LFO and PERFORM fill the
remaining full-height columns.

## Signal path

```
VCO 1 ─┐
VCO 2 ─┤            ┌─ drive ─► 4-pole ladder VCF ─► VCA ─► volume ─► OUT
VCO 3 ─┼─► MIXER ───┘             ▲ filter contour     ▲ loudness contour
NOISE ─┤                          ▲ LFO / KB track     ▲ VEL
EXT  ──┘
```

| Section | Controls |
|---|---|
| VCO 1–3 | Range (32'–2'), FREQ (±7 semitones), waveform (tri / saw / square / pulse), LEVEL into the mixer. |
| VCO MOD | PW (for pulse), FM input depth, SYNC (hard-syncs VCO 2 to VCO 1). |
| MIXER | Levels for noise (with a white/PINK switch) and the external input. (Each VCO's level is in its own section.) |
| FILTER | Cutoff, resonance (self-oscillates at the top of the range), EG amount (bipolar), keyboard tracking, drive into the ladder, LP/HP. |
| FILTER EG / AMP EG | Two analog-style ADSRs, each 1 ms – 10 s. |
| OUTPUT | DRONE (holds the VCA open), volume. |
| LFO | Rate (0.02–40 Hz); waveform (sine / triangle / saw / square / S&H); depth to pitch, cutoff and PW. |
| PERFORM | Glide, master tune (±12 semitones), analog drift, manual GATE button. |

### Patch bay

The patch bay is on the right edge. Inputs: the upper, grey block. Outputs: the lower, lighter grey block.

The VCF IN and VCA IN inputs work like normalled jacks on hardware. When nothing is patched, the
mixer feeds the filter and the filter feeds the VCA. Patching VCF IN or VCA IN replaces that
internal connection, so you can re-route the signal path. Patching VCA CV replaces the loudness
contour.

## Patches

The display in the header shows the current patch name, with an `*` once you have changed a setting
since it was loaded or saved. The arrows step through the factory bank and then your own patches;
clicking the name opens the patch browser (it is also in the module's right-click menu under
**Patches**). Loading a patch can be undone with Ctrl+Z.

- **Factory patches** (64, in Bass, Lead, Keys, Pad, Brass, Drums, FX and Pluck) are in
  [res/patches.json](res/patches.json). A patch lists only the settings that differ from the panel
  defaults; anything it leaves out is reset to its default when the patch loads.
- **Save as new patch** stores the current settings under a name you type (press Enter to save).
  **Save** overwrites the current user patch, and **Delete** removes it.
- User patches are kept as one JSON file each in `<Rack user folder>/Mogue/patches/`, in the same
  format as the factory entries. **Open user patch folder** opens it.

A patch holds every knob and switch except the manual GATE button. Cables are not part of a patch.
The patch name is saved with the Rack patch.

## Building

Requires the [VCV Rack 2 SDK](https://vcvrack.com/manual/Building#Building-Rack-plugins). On
Windows, build from an MSYS2 MinGW 64-bit shell. The Makefile expects the SDK at `../Rack-SDK`
(override with `make RACK_DIR=/path/to/Rack-SDK`):

```sh
make -j8      # builds plugin.dll / .so / .dylib
make dist     # packages dist/Mogue-2.0.0-<arch>.vcvplugin
make install RACK_USER_DIR="C:/Users/utmos/AppData/Local/Rack2"  # copies the package into your Rack user folder
```

Rack extracts and loads the `.vcvplugin` on its next start.

## Implementation notes

- **Oscillators:** PolyBLEP saw and pulse waves. The audio path runs at 2× oversampling and is
  decimated with `dsp::Decimator`.
- **Filter:** a zero-delay-feedback 4-pole ladder with a tanh-saturating input stage. It stays in
  tune when it self-oscillates. Feedback reduces bass, as on the hardware; this loss is only
  partly compensated.
- **Drift:** each oscillator has its own slow random pitch drift, of up to about ±24 cents.
