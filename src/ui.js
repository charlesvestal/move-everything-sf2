/*
 * SF2 Synth Module UI
 *
 * Uses shared sound generator UI base.
 * Bank switching (Shift+L/R) handled by base using 'soundfont_index' param.
 */

/* Shared utilities - absolute path for module location independence */
import { createSoundGeneratorUI } from '/data/UserData/move-anything/shared/sound_generator_ui.mjs';

/* Create the UI - SF2 uses 'soundfont_index' for bank switching */
const ui = createSoundGeneratorUI({
    moduleName: 'SF2',
    bankParamName: 'soundfont_index',
    showPolyphony: true,
    showOctave: true,
});

/* Export required callbacks */
globalThis.init = ui.init;
globalThis.tick = ui.tick;
globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
globalThis.onMidiMessageExternal = ui.onMidiMessageExternal;
