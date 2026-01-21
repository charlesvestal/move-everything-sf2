/*
 * SF2 Synth DSP Plugin
 *
 * Uses TinySoundFont to render SoundFont (.sf2) files.
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
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

/* TinySoundFont implementation */
#define TSF_IMPLEMENTATION
#include "third_party/tsf.h"

/* Shared host API */
static const host_api_v1_t *g_host = NULL;

/* Constants */
#define MAX_SOUNDFONTS 64

typedef struct {
    char path[512];
    char name[128];
} soundfont_entry_t;

/* Per-Instance State */
typedef struct {
    tsf *tsf;
    int current_preset;
    int preset_count;
    int octave_transpose;
    char soundfont_path[512];
    char soundfont_name[128];
    char preset_name[128];
    int active_voices;
    int soundfont_index;
    int soundfont_count;
    soundfont_entry_t soundfonts[MAX_SOUNDFONTS];
    float render_buffer[MOVE_FRAMES_PER_BLOCK * 2];
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

/* Soundfont Management */

static int soundfont_entry_cmp(const void *a, const void *b) {
    const soundfont_entry_t *sa = (const soundfont_entry_t *)a;
    const soundfont_entry_t *sb = (const soundfont_entry_t *)b;
    return strcasecmp(sa->name, sb->name);
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

static int load_soundfont(sf2_instance_t *inst, const char *path) {
    char msg[256];

    if (inst->tsf) {
        tsf_close(inst->tsf);
        inst->tsf = NULL;
    }

    snprintf(msg, sizeof(msg), "Loading SF2: %s", path);
    plugin_log(msg);

    inst->tsf = tsf_load_filename(path);
    if (!inst->tsf) {
        snprintf(msg, sizeof(msg), "Failed to load SF2: %s", path);
        plugin_log(msg);
        strcpy(inst->soundfont_name, "Load failed");
        snprintf(inst->load_error, sizeof(inst->load_error),
                 "SF2: no soundfont files found");
        inst->preset_count = 0;
        return -1;
    }

    /* Clear any previous load error on success */
    inst->load_error[0] = '\0';

    tsf_set_output(inst->tsf, TSF_STEREO_INTERLEAVED, MOVE_SAMPLE_RATE, -12.0f);
    inst->preset_count = tsf_get_presetcount(inst->tsf);

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

    inst->current_preset = 0;
    if (inst->preset_count > 0) {
        const char *name = tsf_get_presetname(inst->tsf, inst->current_preset);
        if (name) {
            strncpy(inst->preset_name, name, sizeof(inst->preset_name) - 1);
        }
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
    if (!inst->tsf || inst->preset_count == 0) return;

    if (index < 0) index = inst->preset_count - 1;
    if (index >= inst->preset_count) index = 0;

    inst->current_preset = index;

    const char *name = tsf_get_presetname(inst->tsf, inst->current_preset);
    if (name) {
        strncpy(inst->preset_name, name, sizeof(inst->preset_name) - 1);
    } else {
        snprintf(inst->preset_name, sizeof(inst->preset_name), "Preset %d", inst->current_preset);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Preset %d: %s", inst->current_preset, inst->preset_name);
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

    if (inst->tsf) {
        tsf_close(inst->tsf);
        inst->tsf = NULL;
    }

    free(inst);
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    sf2_instance_t *inst = (sf2_instance_t *)instance;
    if (!inst || !inst->tsf || len < 2) return;
    (void)source;

    uint8_t status = msg[0] & 0xF0;
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
        case 0x90:
            if (data2 > 0) {
                tsf_note_on(inst->tsf, inst->current_preset, note, data2 / 127.0f);
            } else {
                tsf_note_off(inst->tsf, inst->current_preset, note);
            }
            break;
        case 0x80:
            tsf_note_off(inst->tsf, inst->current_preset, note);
            break;
        case 0xB0:
            if (data1 == 123) {
                tsf_note_off_all(inst->tsf);
            }
            break;
        case 0xE0:
            {
                int bend = ((int)data2 << 7) | data1;
                tsf_channel_set_pitchwheel(inst->tsf, 0, bend);
            }
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
    } else if (strcmp(key, "all_notes_off") == 0 || strcmp(key, "panic") == 0) {
        if (inst->tsf) {
            tsf_note_off_all(inst->tsf);
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
    } else if (strcmp(key, "polyphony") == 0) {
        return snprintf(buf, buf_len, "%d", inst->active_voices);
    } else if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
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
    /* UI hierarchy for shadow parameter editor */
    else if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"list_param\":\"preset\","
                    "\"count_param\":\"preset_count\","
                    "\"name_param\":\"preset_name\","
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

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    sf2_instance_t *inst = (sf2_instance_t *)instance;
    if (!inst || !inst->tsf) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    tsf_render_float(inst->tsf, inst->render_buffer, frames, 0);
    inst->active_voices = tsf_active_voice_count(inst->tsf);

    for (int i = 0; i < frames * 2; i++) {
        float sample = inst->render_buffer[i];
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        out_interleaved_lr[i] = (int16_t)(sample * 32767.0f);
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
    .render_block = v2_render_block
};

/* V2 Entry Point */
plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    plugin_log("V2 API initialized");
    return &g_plugin_api_v2;
}
