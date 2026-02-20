# SF2 Synth Module

Polyphonic SoundFont synthesizer for Move Everything using [TinySoundFont](https://github.com/schellingb/TinySoundFont) by Bernhard Schelling.

## Features

- Load any .sf2 SoundFont file
- Preset selection via UI
- Multiple soundfont support (switch with Shift + Left/Right)
- Octave transpose (-4 to +4)
- Full polyphony
- Velocity sensitivity
- Pitch bend support
- Signal Chain compatible

## Prerequisites

- [Move Everything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Installation

### Via Module Store (Recommended)

1. Launch Move Everything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Sound Generators** → **SF2 Synth**
4. Select **Install**

### Manual Installation

```bash
./scripts/install.sh
```

## Loading SoundFonts

### Quick Setup

1. Download a .sf2 file (see "SoundFont Sources" below)
2. Copy to Move:
   ```bash
   scp YourSound.sf2 ableton@move.local:/data/UserData/move-anything/modules/sound_generators/sf2/soundfonts/
   ```
3. Restart the SF2 module or use Shift + Left/Right to switch soundfonts

The module loads the first `.sf2` file in `soundfonts/` by default. If the folder is empty, it falls back to `instrument.sf2` in the module root.

## Controls

| Control | Action |
|---------|--------|
| **Jog wheel** | Navigate presets |
| **Left/Right** | Previous/next preset |
| **Shift + Left/Right** | Switch soundfonts |
| **Up/Down** | Octave transpose |
| **Pads** | Play notes (velocity sensitive) |

Switching soundfonts resets to preset 1 in the new file.

## SoundFont Sources

Free SoundFonts are available from:
- https://musical-artifacts.com/artifacts?formats=sf2
- https://www.philscomputerlab.com/general-midi-soundfonts.html
- FluidSynth's FluidR3_GM.sf2

**Recommended starter SoundFonts:**
- FluidR3_GM.sf2 - Full General MIDI set
- Timbres of Heaven - High quality orchestral
- Arachno SoundFont - Good all-around GM set

## Troubleshooting

**No sound:**
- Ensure a .sf2 file exists in the soundfonts folder
- Check that the soundfont loaded (name shows on display)
- Try a different preset - some may be silent or very quiet

**Wrong preset:**
- Large GM SoundFonts can have 100+ presets - keep scrolling
- Bank 0 typically has melodic instruments, Bank 128 has drums

**Clicking/glitching:**
- Large SoundFonts may exceed available memory
- Try a smaller file or one with fewer samples
- FluidR3_GM (~150MB) works well; larger ones may have issues

**Can't find soundfont:**
- Ensure file is in `modules/sound_generators/sf2/soundfonts/` (not the module root)
- File must have `.sf2` extension (case sensitive)

## Technical Details

- Sample rate: 44100 Hz
- Block size: 128 frames
- Output: Stereo interleaved int16

## Building from Source

```bash
./scripts/build.sh
```

Requires Docker or ARM64 cross-compiler.

## Credits

- [TinySoundFont](https://github.com/schellingb/TinySoundFont) by Bernhard Schelling (MIT license)

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.  
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
