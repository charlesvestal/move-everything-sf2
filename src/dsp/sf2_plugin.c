/*
 * SF2 Synth DSP Plugin
 *
 * Uses FluidLite to render SoundFont (.sf2) files.
 * Provides polyphonic synthesis with preset selection.
 *
 * V2 API only - instance-based for multi-instance support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>

/* Include plugin API - inline definitions to avoid path issues */
#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION_2 2
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

/* FluidLite */
#include "fluidlite.h"

/* Shared host API */
static const host_api_v1_t *g_host = NULL;

/* Constants */
#define MAX_SOUNDFONTS 64
#define MAX_PRESETS 1024

typedef struct {
    char path[512];
    char name[128];
} soundfont_entry_t;

typedef struct {
    char name[128];
    int bank;
    int program;
} preset_entry_t;

/* Per-Instance State */
typedef struct {
    fluid_settings_t *settings;
    fluid_synth_t *synth;
    int sfont_id;
    int current_preset;
    int preset_count;
    int octave_transpose;
    float gain;
    char soundfont_path[512];
    char soundfont_name[128];
    char preset_name[128];
    int soundfont_index;
    int soundfont_count;
    soundfont_entry_t soundfonts[MAX_SOUNDFONTS];
    preset_entry_t presets[MAX_PRESETS];
    float left_buf[MOVE_FRAMES_PER_BLOCK];
    float right_buf[MOVE_FRAMES_PER_BLOCK];
    char module_dir[512];
    char load_error[256];
} sf2_instance_t;

/* Helper: log via host */
static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[sf2] %s", msg);
        g_host->log(buf);
    }
}

/* Helper: extract number from JSON */
static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

/* Helper: extract string from JSON */
static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    const char *end = strchr(pos, '"');
    if (!end) return -1;
    int len = end - pos;
    if (len >= out_len) len = out_len - 1;
    strncpy(out, pos, len);
    out[len] = '\0';
    return len;
}

/* Soundfont Management */

static int soundfont_entry_cmp(const void *a, const void *b) {
    const soundfont_entry_t *sa = (const soundfont_entry_t *)a;
    const soundfont_entry_t *sb = (const soundfont_entry_t *)b;
    return strcasecmp(sa->name, sb->name);
}

/* Find soundfont index by name, returns -1 if not found */
static int find_soundfont_by_name(sf2_instance_t *inst, const char *name) {
    for (int i = 0; i < inst->soundfont_count; i++) {
        if (strcmp(inst->soundfonts[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void scan_soundfonts(sf2_instance_t *inst, const char *module_dir) {
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/soundfonts", module_dir);

    inst->soundfont_count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcasecmp(ext, ".sf2") != 0) continue;
        if (inst->soundfont_count >= MAX_SOUNDFONTS) {
            plugin_log("soundfont list full, skipping extras");
            break;
        }

        soundfont_entry_t *sf = &inst->soundfonts[inst->soundfont_count++];
        snprintf(sf->path, sizeof(sf->path), "%s/%s", dir_path, entry->d_name);
        strncpy(sf->name, entry->d_name, sizeof(sf->name) - 1);
        sf->name[sizeof(sf->name) - 1] = '\0';
    }

    closedir(dir);

    if (inst->soundfont_count > 1) {
        qsort(inst->soundfonts, inst->soundfont_count, sizeof(soundfont_entry_t), soundfont_entry_cmp);
    }
}

/* Build preset list from loaded soundfont */
static void build_preset_list(sf2_instance_t *inst) {
    char msg[256];
    inst->preset_count = 0;

    if (!inst->synth) {
        plugin_log("build_preset_list: synth is NULL");
        return;
    }
    if (inst->sfont_id < 0) {
        plugin_log("build_preset_list: sfont_id < 0");
        return;
    }

    snprintf(msg, sizeof(msg), "build_preset_list: sfont_id=%d", inst->sfont_id);
    plugin_log(msg);

    /* Try getting soundfont by ID first */
    fluid_sfont_t *sfont = fluid_synth_get_sfont_by_id(inst->synth, inst->sfont_id);
    if (!sfont) {
        plugin_log("build_preset_list: get_sfont_by_id returned NULL, trying index 0");
        /* Fallback: try getting by index */
        sfont = fluid_synth_get_sfont(inst->synth, 0);
    }

    if (!sfont) {
        plugin_log("build_preset_list: sfont is NULL");
        return;
    }

    plugin_log("build_preset_list: got sfont, starting iteration");

    /* Iterate through all presets */
    sfont->iteration_start(sfont);
    fluid_preset_t preset;
    memset(&preset, 0, sizeof(preset));

    int iterations = 0;
    while (sfont->iteration_next(sfont, &preset) && inst->preset_count < MAX_PRESETS) {
        iterations++;
        preset_entry_t *p = &inst->presets[inst->preset_count];

        const char *name = NULL;
        if (preset.get_name) {
            name = preset.get_name(&preset);
        }
        if (name) {
            strncpy(p->name, name, sizeof(p->name) - 1);
            p->name[sizeof(p->name) - 1] = '\0';
        } else {
            snprintf(p->name, sizeof(p->name), "Preset %d", inst->preset_count);
        }

        if (preset.get_banknum && preset.get_num) {
            p->bank = preset.get_banknum(&preset);
            p->program = preset.get_num(&preset);
        } else {
            p->bank = 0;
            p->program = inst->preset_count;
        }

        inst->preset_count++;
    }

    snprintf(msg, sizeof(msg), "Found %d presets after %d iterations", inst->preset_count, iterations);
    plugin_log(msg);
}

static int load_soundfont(sf2_instance_t *inst, const char *path) {
    char msg[256];

    /* Unload previous soundfont */
    if (inst->sfont_id >= 0 && inst->synth) {
        fluid_synth_sfunload(inst->synth, inst->sfont_id, 1);
        inst->sfont_id = -1;
    }

    inst->preset_count = 0;
    inst->current_preset = 0;

    snprintf(msg, sizeof(msg), "Loading SF2: %s", path);
    plugin_log(msg);

    inst->sfont_id = fluid_synth_sfload(inst->synth, path, 1);
    snprintf(msg, sizeof(msg), "fluid_synth_sfload returned: %d", inst->sfont_id);
    plugin_log(msg);
    if (inst->sfont_id < 0) {
        snprintf(msg, sizeof(msg), "Failed to load SF2: %s", path);
        plugin_log(msg);
        strcpy(inst->soundfont_name, "Load failed");
        snprintf(inst->load_error, sizeof(inst->load_error),
                 "SF2: failed to load soundfont");
        return -1;
    }

    /* Clear any previous load error on success */
    inst->load_error[0] = '\0';

    /* Build preset list */
    build_preset_list(inst);

    const char *fname = strrchr(path, '/');
    if (fname) {
        strncpy(inst->soundfont_name, fname + 1, sizeof(inst->soundfont_name) - 1);
    } else {
        strncpy(inst->soundfont_name, path, sizeof(inst->soundfont_name) - 1);
    }

    strncpy(inst->soundfont_path, path, sizeof(inst->soundfont_path) - 1);
    inst->soundfont_path[sizeof(inst->soundfont_path) - 1] = '\0';

    snprintf(msg, sizeof(msg), "SF2 loaded: %d presets", inst->preset_count);
    plugin_log(msg);

    /* Select first preset */
    if (inst->preset_count > 0) {
        strncpy(inst->preset_name, inst->presets[0].name, sizeof(inst->preset_name) - 1);
        fluid_synth_program_select(inst->synth, 0, inst->sfont_id,
                                   inst->presets[0].bank, inst->presets[0].program);
    }

    return 0;
}

static void set_soundfont_index(sf2_instance_t *inst, int index) {
    if (inst->soundfont_count <= 0) return;

    if (index < 0) index = inst->soundfont_count - 1;
    if (index >= inst->soundfont_count) index = 0;

    inst->soundfont_index = index;
    load_soundfont(inst, inst->soundfonts[inst->soundfont_index].path);
}

static void select_preset(sf2_instance_t *inst, int index) {
    if (!inst->synth || inst->preset_count == 0) return;

    if (index < 0) index = inst->preset_count - 1;
    if (index >= inst->preset_count) index = 0;

    /* Send all notes off before changing preset */
    if (inst->current_preset != index) {
        fluid_synth_all_notes_off(inst->synth, -1);
    }

    inst->current_preset = index;

    preset_entry_t *p = &inst->presets[index];
    strncpy(inst->preset_name, p->name, sizeof(inst->preset_name) - 1);

    fluid_synth_program_select(inst->synth, 0, inst->sfont_id, p->bank, p->program);

    char msg[128];
    snprintf(msg, sizeof(msg), "Preset %d: %s (bank %d, prog %d)",
             index, inst->preset_name, p->bank, p->program);
    plugin_log(msg);
}

/* V2 API Implementation */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Creating instance from: %s", module_dir);
    plugin_log(msg);

    sf2_instance_t *inst = calloc(1, sizeof(sf2_instance_t));
    if (!inst) return NULL;

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    strcpy(inst->soundfont_name, "No SF2 loaded");
    inst->load_error[0] = '\0';
    inst->sfont_id = -1;

    /* Create FluidLite settings and synth */
    inst->settings = new_fluid_settings();
    if (!inst->settings) {
        plugin_log("Failed to create FluidLite settings");
        free(inst);
        return NULL;
    }

    /* Use host's sample rate for proper tuning */
    int sample_rate = g_host ? g_host->sample_rate : MOVE_SAMPLE_RATE;

    fluid_settings_setnum(inst->settings, "synth.sample-rate", (double)sample_rate);
    fluid_settings_setnum(inst->settings, "synth.gain", 1.0);
    fluid_settings_setint(inst->settings, "synth.polyphony", 64);
    inst->gain = 1.0f;

    inst->synth = new_fluid_synth(inst->settings);
    if (!inst->synth) {
        plugin_log("Failed to create FluidLite synth");
        delete_fluid_settings(inst->settings);
        free(inst);
        return NULL;
    }

    /* Explicitly set sample rate on synth (belt and suspenders) */
    fluid_synth_set_sample_rate(inst->synth, (float)sample_rate);

    /* Verify and log sample rate */
    double actual_rate = 0;
    fluid_settings_getnum(inst->settings, "synth.sample-rate", &actual_rate);
    char rate_msg[128];
    snprintf(rate_msg, sizeof(rate_msg), "FluidLite sample rate: host=%d, actual=%.1f",
             sample_rate, actual_rate);
    plugin_log(rate_msg);
    /* Also log to stderr for debugging */
    fprintf(stderr, "[sf2] %s\n", rate_msg);
    fflush(stderr);

    /* Set 4th order interpolation for better pitch accuracy (-1 = all channels) */
    fluid_synth_set_interp_method(inst->synth, -1, FLUID_INTERP_4THORDER);
    plugin_log("Set interpolation to FLUID_INTERP_4THORDER (4)");

    /* Parse default soundfont path from JSON */
    char default_sf[512] = {0};
    if (json_defaults) {
        const char *pos = strstr(json_defaults, "\"soundfont_path\"");
        if (pos) {
            pos = strchr(pos, ':');
            if (pos) {
                pos = strchr(pos, '"');
                if (pos) {
                    pos++;
                    int i = 0;
                    while (*pos && *pos != '"' && i < (int)sizeof(default_sf) - 1) {
                        default_sf[i++] = *pos++;
                    }
                    default_sf[i] = '\0';
                }
            }
        }
    }

    scan_soundfonts(inst, module_dir);

    if (inst->soundfont_count > 0) {
        inst->soundfont_index = 0;
        if (default_sf[0]) {
            const char *default_name = strrchr(default_sf, '/');
            default_name = default_name ? default_name + 1 : default_sf;
            for (int i = 0; i < inst->soundfont_count; i++) {
                if (strcmp(inst->soundfonts[i].path, default_sf) == 0 ||
                    strcmp(inst->soundfonts[i].name, default_name) == 0) {
                    inst->soundfont_index = i;
                    break;
                }
            }
        }
        load_soundfont(inst, inst->soundfonts[inst->soundfont_index].path);
    } else if (default_sf[0]) {
        load_soundfont(inst, default_sf);
    } else {
        char sf_path[512];
        snprintf(sf_path, sizeof(sf_path), "%s/instrument.sf2", module_dir);
        load_soundfont(inst, sf_path);
    }

    plugin_log("Instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    sf2_instance_t *inst = (sf2_instance_t *)instance;
    if (!inst) return;

    plugin_log("Instance destroying");

    if (inst->synth) {
        delete_fluid_synth(inst->synth);
        inst->synth = NULL;
    }

    if (inst->settings) {
        delete_fluid_settings(inst->settings);
        inst->settings = NULL;
    }

    free(inst);
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    sf2_instance_t *inst = (sf2_instance_t *)instance;
    if (!inst || !inst->synth || len < 2) return;
    (void)source;

    uint8_t status = msg[0] & 0xF0;
    uint8_t channel = msg[0] & 0x0F;
    uint8_t data1 = msg[1];
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    int is_note = (status == 0x90 || status == 0x80);
    int note = data1;
    if (is_note) {
        note += inst->octave_transpose * 12;
        if (note < 0) note = 0;
        if (note > 127) note = 127;
    }

    switch (status) {
        case 0x90:  /* Note on */
            if (data2 > 0) {
                fluid_synth_noteon(inst->synth, channel, note, data2);
            } else {
                fluid_synth_noteoff(inst->synth, channel, note);
            }
            break;
        case 0x80:  /* Note off */
            fluid_synth_noteoff(inst->synth, channel, note);
            break;
        case 0xB0:  /* Control change */
            if (data1 == 123) {  /* All notes off */
                fluid_synth_all_notes_off(inst->synth, -1);
            } else {
                fluid_synth_cc(inst->synth, channel, data1, data2);
            }
            break;
        case 0xE0:  /* Pitch bend */
            {
                int bend = ((int)data2 << 7) | data1;
                fluid_synth_pitch_bend(inst->synth, channel, bend);
            }
            break;
        case 0xC0:  /* Program change - map to our preset list */
            if (data1 < inst->preset_count) {
                select_preset(inst, data1);
            }
            break;
        case 0xD0:  /* Channel pressure (aftertouch) */
            fluid_synth_channel_pressure(inst->synth, channel, data1);
            break;
    }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    sf2_instance_t *inst = (sf2_instance_t *)instance;
    if (!inst) return;

    if (strcmp(key, "soundfont_path") == 0) {
        load_soundfont(inst, val);
        if (inst->soundfont_count > 0) {
            const char *name = strrchr(val, '/');
            name = name ? name + 1 : val;
            for (int i = 0; i < inst->soundfont_count; i++) {
                if (strcmp(inst->soundfonts[i].path, val) == 0 ||
                    strcmp(inst->soundfonts[i].name, name) == 0) {
                    inst->soundfont_index = i;
                    break;
                }
            }
        }
    } else if (strcmp(key, "soundfont_index") == 0) {
        set_soundfont_index(inst, atoi(val));
    } else if (strcmp(key, "next_soundfont") == 0) {
        set_soundfont_index(inst, inst->soundfont_index + 1);
    } else if (strcmp(key, "prev_soundfont") == 0) {
        set_soundfont_index(inst, inst->soundfont_index - 1);
    } else if (strcmp(key, "preset") == 0) {
        select_preset(inst, atoi(val));
    } else if (strcmp(key, "octave_transpose") == 0) {
        inst->octave_transpose = atoi(val);
        if (inst->octave_transpose < -4) inst->octave_transpose = -4;
        if (inst->octave_transpose > 4) inst->octave_transpose = 4;
    } else if (strcmp(key, "gain") == 0) {
        inst->gain = atof(val);
        if (inst->gain < 0.0f) inst->gain = 0.0f;
        if (inst->gain > 2.0f) inst->gain = 2.0f;
        if (inst->synth) {
            fluid_synth_set_gain(inst->synth, inst->gain);
        }
    } else if (strcmp(key, "all_notes_off") == 0 || strcmp(key, "panic") == 0) {
        if (inst->synth) {
            fluid_synth_all_notes_off(inst->synth, -1);
        }
    } else if (strcmp(key, "state") == 0) {
        /* Restore state from JSON */
        float f;
        /* Restore soundfont - try by name first, fall back to index */
        char sf_name[128];
        int sf_idx = -1;
        if (json_get_string(val, "soundfont_name", sf_name, sizeof(sf_name)) > 0) {
            sf_idx = find_soundfont_by_name(inst, sf_name);
        }
        if (sf_idx < 0 && json_get_number(val, "soundfont_index", &f) == 0) {
            int idx = (int)f;
            if (idx >= 0 && idx < inst->soundfont_count) {
                sf_idx = idx;
            }
        }
        if (sf_idx >= 0) {
            set_soundfont_index(inst, sf_idx);
        }
        if (json_get_number(val, "preset", &f) == 0) {
            select_preset(inst, (int)f);
        }
        if (json_get_number(val, "octave_transpose", &f) == 0) {
            inst->octave_transpose = (int)f;
            if (inst->octave_transpose < -4) inst->octave_transpose = -4;
            if (inst->octave_transpose > 4) inst->octave_transpose = 4;
        }
        if (json_get_number(val, "gain", &f) == 0) {
            inst->gain = f;
            if (inst->gain < 0.0f) inst->gain = 0.0f;
            if (inst->gain > 2.0f) inst->gain = 2.0f;
            if (inst->synth) {
                fluid_synth_set_gain(inst->synth, inst->gain);
            }
        }
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    sf2_instance_t *inst = (sf2_instance_t *)instance;
    if (!inst) return -1;

    if (strcmp(key, "load_error") == 0) {
        if (inst->load_error[0]) {
            return snprintf(buf, buf_len, "%s", inst->load_error);
        }
        return 0;  /* No error */
    } else if (strcmp(key, "soundfont_name") == 0) {
        strncpy(buf, inst->soundfont_name, buf_len - 1);
        return strlen(buf);
    } else if (strcmp(key, "soundfont_path") == 0) {
        strncpy(buf, inst->soundfont_path, buf_len - 1);
        return strlen(buf);
    } else if (strcmp(key, "soundfont_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->soundfont_count);
    } else if (strcmp(key, "soundfont_index") == 0) {
        return snprintf(buf, buf_len, "%d", inst->soundfont_index);
    } else if (strcmp(key, "preset") == 0 || strcmp(key, "current_patch") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_preset);
    } else if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_name") == 0 || strcmp(key, "name") == 0) {
        strncpy(buf, inst->preset_name, buf_len - 1);
        return strlen(buf);
    } else if (strcmp(key, "preset_count") == 0 || strcmp(key, "total_patches") == 0) {
        return snprintf(buf, buf_len, "%d", inst->preset_count);
    } else if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    } else if (strcmp(key, "gain") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->gain);
    }
    /* Unified bank/preset parameters for Chain compatibility */
    else if (strcmp(key, "bank_name") == 0) {
        strncpy(buf, inst->soundfont_name, buf_len - 1);
        return strlen(buf);
    } else if (strcmp(key, "patch_in_bank") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_preset + 1);
    } else if (strcmp(key, "bank_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->soundfont_count);
    }
    /* Dynamic soundfont list for Shadow UI menu - rescan each time */
    else if (strcmp(key, "soundfont_list") == 0) {
        /* Rescan soundfonts directory to pick up any new files */
        scan_soundfonts(inst, inst->module_dir);

        int written = 0;
        written += snprintf(buf + written, buf_len - written, "[");
        for (int i = 0; i < inst->soundfont_count && written < buf_len - 50; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"label\":\"%s\",\"index\":%d}",
                inst->soundfonts[i].name, i);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* State serialization for save/load */
    else if (strcmp(key, "state") == 0) {
        /* Save soundfont by name for robustness (index can change if files added/removed) */
        const char *sf_name = "";
        if (inst->soundfont_count > 0 && inst->soundfont_index < inst->soundfont_count) {
            sf_name = inst->soundfonts[inst->soundfont_index].name;
        }
        return snprintf(buf, buf_len,
            "{\"soundfont_name\":\"%s\",\"soundfont_index\":%d,\"preset\":%d,\"octave_transpose\":%d,\"gain\":%.2f}",
            sf_name, inst->soundfont_index, inst->current_preset, inst->octave_transpose, inst->gain);
    }
    /* UI hierarchy for shadow parameter editor */
    else if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"label\":\"SF2\","
                    "\"list_param\":\"preset\","
                    "\"count_param\":\"preset_count\","
                    "\"name_param\":\"preset_name\","
                    "\"children\":null,"
                    "\"knobs\":[\"octave_transpose\",\"gain\"],"
                    "\"params\":["
                        "{\"key\":\"octave_transpose\",\"label\":\"Octave\"},"
                        "{\"key\":\"gain\",\"label\":\"Gain\"},"
                        "{\"level\":\"soundfont\",\"label\":\"Choose Soundfont\"}"
                    "]"
                "},"
                "\"soundfont\":{"
                    "\"label\":\"Soundfont\","
                    "\"items_param\":\"soundfont_list\","
                    "\"select_param\":\"soundfont_index\","
                    "\"children\":null,"
                    "\"knobs\":[],"
                    "\"params\":[]"
                "}"
            "}"
        "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }

    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    sf2_instance_t *inst = (sf2_instance_t *)instance;
    if (!inst || !inst->load_error[0]) return 0;  /* No error */

    int len = strlen(inst->load_error);
    if (len >= buf_len) len = buf_len - 1;
    memcpy(buf, inst->load_error, len);
    buf[len] = '\0';
    return len;
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    sf2_instance_t *inst = (sf2_instance_t *)instance;
    if (!inst || !inst->synth) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Render to separate left/right float buffers */
    fluid_synth_write_float(inst->synth, frames,
                            inst->left_buf, 0, 1,
                            inst->right_buf, 0, 1);

    /* Interleave and convert to int16 */
    for (int i = 0; i < frames; i++) {
        float left = inst->left_buf[i];
        float right = inst->right_buf[i];

        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;

        out_interleaved_lr[i * 2] = (int16_t)(left * 32767.0f);
        out_interleaved_lr[i * 2 + 1] = (int16_t)(right * 32767.0f);
    }
}

/* V2 API struct */
static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi = v2_on_midi,
    .set_param = v2_set_param,
    .get_param = v2_get_param,
    .get_error = v2_get_error,
    .render_block = v2_render_block
};

/* V2 Entry Point */
plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    plugin_log("V2 API initialized (FluidLite)");
    return &g_plugin_api_v2;
}
