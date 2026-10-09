#include <pspkernel.h>
#include <pspctrl.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspiofilemgr.h>
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define SCREEN_WIDTH  480
#define SCREEN_HEIGHT 272
#define BARS_COUNT    160
#define M_PI_F        3.14159265358979323846f

int exit_request = 0;
int wave_picos[BARS_COUNT];

// Fade 0→1 com soma constante: fade_out = 1 - fade_in.
// Na emenda do loop a amplitude não pulsa.
#define FADE_LUT_SIZE 1024
float fade_lut[FADE_LUT_SIZE];
float pitch_lut[49];
int sample_via_sdl = 0;

#define PARAM_SLEW 0.0022f
#define TEXTURE_SLEW 0.0012f

void InitFadeLUT(void) {
    for (int i = 0; i < FADE_LUT_SIZE; i++) {
        float t = (float)i / (float)(FADE_LUT_SIZE - 1);
        fade_lut[i] = 0.5f * (1.0f - cosf(M_PI_F * t));
    }
    for (int i = 0; i < 49; i++) {
        pitch_lut[i] = powf(2.0f, (float)(i - 24) / 12.0f);
    }
}

// --- ÁUDIO PCM ---
Uint8  *audio_buffer = NULL;  
Uint32  audio_length = 0;     
Sint16 *samples_16 = NULL;    
Uint32  total_samples = 0;    

const char* sample_files[8] = {"sample.wav", "sample2.wav", "sample3.wav", "sample4.wav", "sample5.wav", "sample6.wav", "sample7.wav", "sample8.wav"};
const char* sample_names[8] = {"AMOSTRA 1", "AMOSTRA 2", "AMOSTRA 3", "AMOSTRA 4", "AMOSTRA 5", "AMOSTRA 6", "AMOSTRA 7", "AMOSTRA 8"};
int loaded_sample_index = 0;

#define SAMPLE_LIST_MAX 48
#define SAMPLE_NAME_MAX 64
static char sample_dir[256] = ".";
static char sample_dir_files[SAMPLE_LIST_MAX][SAMPLE_NAME_MAX];
static int sample_dir_count = 0;
static int sample_browser_open = 0;
static int sample_browser_index = 0;
static char sample_ativo_nome[SAMPLE_NAME_MAX] = "sample.wav"; 

// --- PARÂMETROS GLOBAIS ---
float grain_center    = 0.30f;   
float grain_width     = 0.04f;   
float playback_speed  = 1.00f;   
int   is_audio_paused = 0;     
int   auto_motion_active = 1;  
int   bpm = 60;                 
Uint32 motion_start_tick = 0;   
int   scan_mode = 0;
float tape_angle = 0.0f; 

// --- DSP EFEITOS (Filtros Simples 1-Polo + Vol/Pan) ---
int hpf_enabled = 1; float hpf_cutoff = 0.05f; float hpf_vol = 1.0f; float hpf_pan = 0.5f;
int lpf_enabled = 1; float lpf_cutoff = 1.00f; float lpf_vol = 1.0f; float lpf_pan = 0.5f;

float delay_time_ms = 420.0f, delay_feedback = 0.40f, delay_wet = 0.35f; int delay_enabled = 1; 
float reverb_time = 0.50f, reverb_damp = 0.40f, reverb_wet = 0.30f; int reverb_enabled = 1;

int active_tab = 0; 
int selected_param_index = 0; 

#define MAX_DELAY_BUFFER 70560
float delay_buffer_L[MAX_DELAY_BUFFER], delay_buffer_R[MAX_DELAY_BUFFER];
int delay_write_ptr = 0;
float delay_tone_L = 0.0f, delay_tone_R = 0.0f;
float tape_phase = 0.0f;
float audio_cpu = 0.0f;

#define REVERB_BUFFER_SIZE 8820
float reverb_buffer_L[REVERB_BUFFER_SIZE], reverb_buffer_R[REVERB_BUFFER_SIZE];
int reverb_ptr = 0;

// Variáveis de Estado (Filtros)
float lpf_last_L = 0.0f, lpf_last_R = 0.0f;
float hpf_last_L = 0.0f, hpf_last_R = 0.0f;
float rev_damp_L = 0.0f, rev_damp_R = 0.0f;

// Parâmetros suavizados no callback (evita zipper e pente móvel ao girar o knob)
float sm_center = 0.30f, sm_width = 0.04f, sm_speed = 1.00f;
float sm_hpf_cut = 0.05f, sm_lpf_cut = 1.00f;
float sm_hpf_vol = 1.0f, sm_lpf_vol = 1.0f, sm_hpf_pan = 0.5f, sm_lpf_pan = 0.5f;
float sm_hpf_amt = 1.0f, sm_lpf_amt = 1.0f, sm_delay_amt = 1.0f, sm_rev_amt = 1.0f;
float sm_delay_fb = 0.40f, sm_delay_wet = 0.35f;
float sm_rev_fb = 0.50f, sm_rev_damp = 0.40f, sm_rev_wet = 0.30f;
float sm_sustain = 0.50f;
float delay_from = 18522.0f, delay_to = 18522.0f, delay_applied = 18522.0f, delay_fade = 1.0f;
int dsp_ready = 0;

#define REVERB_TAP2 5791

// --- MOTOR POLIFÔNICO E ADSR ---
typedef enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE } EnvState;

typedef struct {
    EnvState state;
    float level;
    float pitch_mult;
    float read_pos;
} Voice;

Voice voices[4];
Voice texture_voice; 

float adsr_attack  = 0.05f, adsr_decay = 0.20f, adsr_sustain = 0.50f, adsr_release = 0.40f;  

// --- SEQUENCIADOR ---
int seq_active[16] = {1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0};
int seq_pitch[16]  = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0}; 
int current_seq_step = 0;
int seq_note_on = 0;
Uint32 last_seq_tick = 0;
int last_triggered_voice = 0; 

int seq_res_idx = 4; 
float res_mults[5] = {4.0f, 2.0f, 1.0f, 0.5f, 0.25f}; 
const char* res_texts[5] = {"1", "1/2", "1/4", "1/8", "1/16"};

const unsigned char font8x8_basic[128][8] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['+'] = {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00},
    ['-'] = {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00},
    ['.'] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00},
    [':'] = {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00},
    ['%'] = {0x00, 0x63, 0x66, 0x0C, 0x18, 0x33, 0x63, 0x00},
    ['['] = {0x1C, 0x10, 0x10, 0x10, 0x10, 0x1C, 0x00, 0x00},
    [']'] = {0x38, 0x08, 0x08, 0x08, 0x08, 0x38, 0x00, 0x00},
    ['/'] = {0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x00, 0x00},
    ['0'] = {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x3C, 0x00, 0x00},
    ['1'] = {0x18, 0x38, 0x18, 0x18, 0x18, 0x7E, 0x00, 0x00},
    ['2'] = {0x3C, 0x66, 0x0C, 0x18, 0x30, 0x7E, 0x00, 0x00},
    ['3'] = {0x3C, 0x66, 0x1C, 0x06, 0x66, 0x3C, 0x00, 0x00},
    ['4'] = {0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x00, 0x00},
    ['5'] = {0x7E, 0x60, 0x7C, 0x06, 0x66, 0x3C, 0x00, 0x00},
    ['6'] = {0x3C, 0x66, 0x7C, 0x66, 0x66, 0x3C, 0x00, 0x00},
    ['7'] = {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x00, 0x00},
    ['8'] = {0x3C, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00, 0x00},
    ['9'] = {0x3C, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00, 0x00},
    ['A'] = {0x18, 0x3C, 0x66, 0x7E, 0x66, 0x66, 0x00, 0x00},
    ['B'] = {0x7C, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00, 0x00},
    ['C'] = {0x3C, 0x66, 0x60, 0x60, 0x66, 0x3C, 0x00, 0x00},
    ['D'] = {0x78, 0x6C, 0x66, 0x66, 0x6C, 0x78, 0x00, 0x00},
    ['E'] = {0x7E, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00, 0x00},
    ['F'] = {0x7E, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00, 0x00},
    ['G'] = {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x3C, 0x00, 0x00},
    ['H'] = {0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00, 0x00},
    ['I'] = {0x3E, 0x1C, 0x1C, 0x1C, 0x1C, 0x3E, 0x00, 0x00},
    ['K'] = {0x66, 0x6C, 0x78, 0x78, 0x6C, 0x66, 0x00, 0x00},
    ['L'] = {0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00, 0x00},
    ['M'] = {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x00, 0x00},
    ['N'] = {0x66, 0x76, 0x7E, 0x6E, 0x66, 0x66, 0x00, 0x00},
    ['O'] = {0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00},
    ['P'] = {0x7C, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00, 0x00},
    ['Q'] = {0x3C, 0x66, 0x66, 0x66, 0x6A, 0x3C, 0x0E, 0x00},
    ['R'] = {0x7C, 0x66, 0x7C, 0x6C, 0x66, 0x63, 0x00, 0x00},
    ['S'] = {0x3C, 0x66, 0x30, 0x0E, 0x66, 0x3C, 0x00, 0x00},
    ['T'] = {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00},
    ['U'] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00},
    ['V'] = {0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00, 0x00},
    ['W'] = {0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00, 0x00},
    ['X'] = {0x66, 0x3C, 0x18, 0x18, 0x3C, 0x66, 0x00, 0x00},
    ['Y'] = {0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00, 0x00},
    ['Z'] = {0x7E, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00, 0x00}
};

void DrawTextCustom(SDL_Renderer *r, int x, int y, const char *str, Uint8 red, Uint8 grn, Uint8 blu, int scale) {
    SDL_SetRenderDrawColor(r, red, grn, blu, 255);
    int len = strlen(str);
    for (int i = 0; i < len; i++) {
        char c = str[i];
        if (c >= 0 && c < 128) {
            for (int row = 0; row < 8; row++) {
                unsigned char b = font8x8_basic[(int)c][row];
                for (int col = 0; col < 8; col++) {
                    if (b & (1 << (7 - col))) {
                        if (scale == 1) SDL_RenderDrawPoint(r, x + (i * 7) + col, y + row);
                        else {
                            SDL_Rect px = { x + (i * 7 * scale) + (col * scale), y + (row * scale), scale, scale };
                            SDL_RenderFillRect(r, &px);
                        }
                    }
                }
            }
        }
    }
}

void DrawTriangle(SDL_Renderer *r, int cx, int cy, int size, int dir) {
    for(int w = 0; w <= size; w++) {
        int px = cx + (dir * (w - size / 2));
        int h = size - w;
        if (h < 0) h = 0;
        SDL_RenderDrawLine(r, px, cy - h, px, cy + h);
    }
}

int exit_callback(int arg1, int arg2, void *common) { exit_request = 1; return 0; }
int CallbackThread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}
void SetupCallbacks(void) {
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if(thid >= 0) sceKernelStartThread(thid, 0, 0);
}

void ExtrairWaveformDoWAV(void) {
    if (!samples_16 || total_samples == 0) return;
    Uint32 samples_per_bar = total_samples / BARS_COUNT;
    if (samples_per_bar == 0) samples_per_bar = 1;

    for (int i = 0; i < BARS_COUNT; i++) {
        Uint32 start_idx = i * samples_per_bar;
        Uint32 end_idx = start_idx + samples_per_bar;
        if (end_idx > total_samples) end_idx = total_samples;

        int max_val = 0;
        for (Uint32 j = start_idx; j < end_idx; j += 2) { 
            int val = abs(samples_16[j]);
            if (val > max_val) max_val = val;
        }

        int height = (max_val * 34) / 32768; 
        if (height > 36) height = 36;
        if (height < 2)  height = 2;
        wave_picos[i] = height;
    }
}

static void LiberarSample(void) {
    if (audio_buffer) {
        if (sample_via_sdl) SDL_FreeWAV(audio_buffer);
        else free(audio_buffer);
    }
    audio_buffer = NULL;
    samples_16 = NULL;
    audio_length = 0;
    total_samples = 0;
    sample_via_sdl = 0;
}

static void InstalarSilencio(void) {
    LiberarSample();
    audio_length = 44100 * 2 * sizeof(Sint16);
    audio_buffer = (Uint8 *)calloc(audio_length, 1);
    samples_16 = (Sint16 *)audio_buffer;
    total_samples = audio_length / sizeof(Sint16);
}

// Sempre entrega estéreo intercalado a 44100 Hz. WAV mono lido como L/R
// deslocava os canais em 1 sample e criava um pente fixo entre eles.
static int InstalarPCM(SDL_AudioSpec spec, Uint8 *src, Uint32 src_len) {
    int is_s16 = (spec.format == AUDIO_S16LSB || spec.format == AUDIO_S16SYS);
    int channels = spec.channels;
    int freq = spec.freq;
    if (!is_s16 || channels < 1 || freq < 8000 || src == NULL || src_len < 4) {
        if (src) SDL_FreeWAV(src);
        return 0;
    }

    Sint16 *src16 = (Sint16 *)src;
    Uint32 src_frames = src_len / (sizeof(Sint16) * (Uint32)channels);
    if (src_frames < 2) {
        SDL_FreeWAV(src);
        return 0;
    }

    if (freq == 44100 && channels == 2) {
        LiberarSample();
        audio_buffer = src;
        samples_16 = src16;
        sample_via_sdl = 1;
        total_samples = (src_len / (Uint32)sizeof(Sint16)) & ~1u;
        audio_length = total_samples * (Uint32)sizeof(Sint16);
        return 1;
    }

    Uint32 dst_frames = src_frames;
    if (freq != 44100) {
        dst_frames = (Uint32)((float)src_frames * (44100.0f / (float)freq));
        if (dst_frames < 2) dst_frames = 2;
    }

    Sint16 *dst = (Sint16 *)malloc(dst_frames * 2 * sizeof(Sint16));
    if (!dst) {
        SDL_FreeWAV(src);
        return 0;
    }

    if (freq == 44100 && channels > 2) {
        for (Uint32 i = 0; i < dst_frames; i++) {
            dst[i * 2]     = src16[i * channels];
            dst[i * 2 + 1] = src16[i * channels + 1];
        }
    } else {
        float step = (float)freq / 44100.0f;
        for (Uint32 i = 0; i < dst_frames; i++) {
            float src_pos = (float)i * step;
            Uint32 i0 = (Uint32)src_pos;
            float frac = src_pos - (float)i0;
            if (i0 >= src_frames) i0 = src_frames - 1;
            Uint32 i1 = i0 + 1;
            if (i1 >= src_frames) i1 = i0;
            float l0, r0, l1, r1;
            if (channels == 1) {
                l0 = r0 = (float)src16[i0];
                l1 = r1 = (float)src16[i1];
            } else {
                l0 = (float)src16[i0 * channels];
                r0 = (float)src16[i0 * channels + 1];
                l1 = (float)src16[i1 * channels];
                r1 = (float)src16[i1 * channels + 1];
            }
            float l = l0 + (l1 - l0) * frac;
            float r = r0 + (r1 - r0) * frac;
            if (l > 32767.0f) l = 32767.0f; else if (l < -32768.0f) l = -32768.0f;
            if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
            dst[i * 2] = (Sint16)l;
            dst[i * 2 + 1] = (Sint16)r;
        }
    }

    SDL_FreeWAV(src);
    LiberarSample();
    audio_buffer = (Uint8 *)dst;
    samples_16 = dst;
    total_samples = dst_frames * 2;
    audio_length = total_samples * sizeof(Sint16);
    return 1;
}

static int ExtensaoWav(const char *name) {
    int n = (int)strlen(name);
    if (n < 5) return 0;
    const char *e = name + n - 4;
    char c1 = e[1], c2 = e[2], c3 = e[3];
    if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 - 'A' + 'a');
    if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 - 'A' + 'a');
    if (c3 >= 'A' && c3 <= 'Z') c3 = (char)(c3 - 'A' + 'a');
    return e[0] == '.' && c1 == 'w' && c2 == 'a' && c3 == 'v';
}

static int NomeIgual(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int NomeAntes(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return ca < cb;
        a++;
        b++;
    }
    return *a == 0 && *b != 0;
}

static void DefinirPastaSamples(const char *argv0) {
    sample_dir[0] = '.';
    sample_dir[1] = 0;
    if (argv0 == NULL || argv0[0] == 0) return;
    strncpy(sample_dir, argv0, sizeof(sample_dir) - 1);
    sample_dir[sizeof(sample_dir) - 1] = 0;
    char *slash = NULL;
    for (char *p = sample_dir; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (slash == NULL || slash == sample_dir) {
        sample_dir[0] = '.';
        sample_dir[1] = 0;
        return;
    }
    *slash = 0;
}

static void ListarSamplesPasta(void) {
    sample_dir_count = 0;
    int fd = sceIoDopen(sample_dir);
    if (fd < 0) {
        fd = sceIoDopen(".");
        if (fd >= 0) {
            sample_dir[0] = '.';
            sample_dir[1] = 0;
        }
    }
    if (fd < 0) return;
    sceIoChdir(sample_dir);

    SceIoDirent ent;
    while (sample_dir_count < SAMPLE_LIST_MAX) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(fd, &ent) <= 0) break;
        if (ent.d_name[0] == 0 || ent.d_name[0] == '.') continue;
        if ((ent.d_stat.st_mode & FIO_S_IFMT) == FIO_S_IFDIR) continue;
        if (!ExtensaoWav(ent.d_name)) continue;
        strncpy(sample_dir_files[sample_dir_count], ent.d_name, SAMPLE_NAME_MAX - 1);
        sample_dir_files[sample_dir_count][SAMPLE_NAME_MAX - 1] = 0;
        sample_dir_count++;
    }
    sceIoDclose(fd);

    for (int i = 1; i < sample_dir_count; i++) {
        char tmp[SAMPLE_NAME_MAX];
        strncpy(tmp, sample_dir_files[i], SAMPLE_NAME_MAX - 1);
        tmp[SAMPLE_NAME_MAX - 1] = 0;
        int j = i;
        while (j > 0 && NomeAntes(tmp, sample_dir_files[j - 1])) {
            strncpy(sample_dir_files[j], sample_dir_files[j - 1], SAMPLE_NAME_MAX - 1);
            sample_dir_files[j][SAMPLE_NAME_MAX - 1] = 0;
            j--;
        }
        strncpy(sample_dir_files[j], tmp, SAMPLE_NAME_MAX - 1);
        sample_dir_files[j][SAMPLE_NAME_MAX - 1] = 0;
    }
}

static void CaminhoSample(char *dst, int dst_n, const char *name) {
    if (sample_dir[0] == '.' && sample_dir[1] == 0) {
        strncpy(dst, name, dst_n - 1);
        dst[dst_n - 1] = 0;
        return;
    }
    snprintf(dst, dst_n, "%s/%s", sample_dir, name);
}

static void *LerArquivoInteiro(const char *path, int *out_len) {
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) return NULL;

    int cap = 65536;
    int got = 0;
    unsigned char *buf = (unsigned char *)malloc((size_t)cap);
    if (!buf) {
        sceIoClose(fd);
        return NULL;
    }

    while (got < 18 * 1024 * 1024) {
        if (got + 4096 > cap) {
            int ncap = cap * 2;
            if (ncap > 18 * 1024 * 1024) ncap = 18 * 1024 * 1024;
            if (ncap <= cap) break;
            unsigned char *nbuf = (unsigned char *)realloc(buf, (size_t)ncap);
            if (!nbuf) {
                free(buf);
                sceIoClose(fd);
                return NULL;
            }
            buf = nbuf;
            cap = ncap;
        }
        int n = sceIoRead(fd, buf + got, (SceSize)(cap - got));
        if (n < 0) {
            free(buf);
            sceIoClose(fd);
            return NULL;
        }
        if (n == 0) break;
        got += n;
    }
    sceIoClose(fd);
    if (got < 44) {
        free(buf);
        return NULL;
    }
    *out_len = got;
    return buf;
}

static int LerU16(const unsigned char *p) {
    return p[0] | (p[1] << 8);
}

static Uint32 LerU32(const unsigned char *p) {
    return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16) | ((Uint32)p[3] << 24);
}

static int AmostraWav(const unsigned char *p, int bits, int format) {
    if (format == 3) {
        float f;
        memcpy(&f, p, 4);
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        return (int)(f * 32767.0f);
    }
    if (bits == 8) return ((int)p[0] - 128) << 8;
    if (bits == 16) return (int)(short)(p[0] | (p[1] << 8));
    int s = p[0] | (p[1] << 8) | (p[2] << 16);
    if (s & 0x800000) s |= ~0xFFFFFF;
    return s >> 8;
}

static int InstalarWavMem(const unsigned char *b, int len) {
    if (len < 44) return 0;
    if (memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) return 0;

    int channels = 0, rate = 0, bits = 0, format = 0;
    const unsigned char *data = NULL;
    int data_len = 0;
    int pos = 12;
    while (pos + 8 <= len) {
        Uint32 csz = LerU32(b + pos + 4);
        int payload = pos + 8;
        if (payload > len) break;
        if (csz > (Uint32)(len - payload)) csz = (Uint32)(len - payload);
        if (memcmp(b + pos, "fmt ", 4) == 0 && csz >= 16) {
            format = LerU16(b + payload);
            channels = LerU16(b + payload + 2);
            rate = (int)LerU32(b + payload + 4);
            bits = LerU16(b + payload + 14);
            if (format == 0xFFFE && csz >= 40) format = LerU16(b + payload + 24);
        } else if (memcmp(b + pos, "data", 4) == 0 && csz > 0) {
            data = b + payload;
            data_len = (int)csz;
        }
        pos = payload + (int)csz;
        if (csz & 1) pos++;
    }

    if (!data || channels < 1 || channels > 8 || rate < 8000 || rate > 192000) return 0;
    if (format != 1 && format != 3) return 0;
    if (format == 1 && bits != 8 && bits != 16 && bits != 24) return 0;
    if (format == 3 && bits != 32) return 0;

    int bpf = channels * (bits / 8);
    if (bpf < 1) return 0;
    int src_frames = data_len / bpf;
    if (src_frames < 2) return 0;

    Uint32 dst_frames = (Uint32)src_frames;
    if (rate == 44100 && channels == 2 && bits == 16 && format == 1) {
        Sint16 *dst = (Sint16 *)malloc((size_t)src_frames * 2 * sizeof(Sint16));
        if (!dst) return 0;
        memcpy(dst, data, (size_t)src_frames * 2 * sizeof(Sint16));
        LiberarSample();
        audio_buffer = (Uint8 *)dst;
        samples_16 = dst;
        sample_via_sdl = 0;
        total_samples = (Uint32)src_frames * 2;
        audio_length = total_samples * sizeof(Sint16);
        return 1;
    }
    if (rate != 44100) {
        dst_frames = (Uint32)((float)src_frames * (44100.0f / (float)rate));
        if (dst_frames < 2) dst_frames = 2;
    }

    Sint16 *dst = (Sint16 *)malloc(dst_frames * 2 * sizeof(Sint16));
    if (!dst) return 0;

    float step = (float)rate / 44100.0f;
    for (Uint32 i = 0; i < dst_frames; i++) {
        float src_pos = (rate == 44100) ? (float)i : (float)i * step;
        int i0 = (int)src_pos;
        float frac = src_pos - (float)i0;
        if (i0 < 0) i0 = 0;
        if (i0 >= src_frames) i0 = src_frames - 1;
        int i1 = i0 + 1;
        if (i1 >= src_frames) i1 = i0;
        int b0 = i0 * bpf;
        int b1 = i1 * bpf;
        float l0 = (float)AmostraWav(data + b0, bits, format);
        float r0 = (channels > 1) ? (float)AmostraWav(data + b0 + (bits / 8), bits, format) : l0;
        float l1 = (float)AmostraWav(data + b1, bits, format);
        float r1 = (channels > 1) ? (float)AmostraWav(data + b1 + (bits / 8), bits, format) : l1;
        float l = l0 + (l1 - l0) * frac;
        float r = r0 + (r1 - r0) * frac;
        if (l > 32767.0f) l = 32767.0f; else if (l < -32768.0f) l = -32768.0f;
        if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
        dst[i * 2] = (Sint16)l;
        dst[i * 2 + 1] = (Sint16)r;
    }

    LiberarSample();
    audio_buffer = (Uint8 *)dst;
    samples_16 = dst;
    sample_via_sdl = 0;
    total_samples = dst_frames * 2;
    audio_length = total_samples * sizeof(Sint16);
    return 1;
}

static int DecodificarWavMem(void *mem, int len) {
    if (InstalarWavMem((const unsigned char *)mem, len)) return 1;
    SDL_RWops *rw = SDL_RWFromMem(mem, len);
    if (!rw) return 0;
    SDL_AudioSpec spec;
    Uint8 *wav_buf = NULL;
    Uint32 wav_len = 0;
    int parsed = SDL_LoadWAV_RW(rw, 0, &spec, &wav_buf, &wav_len) != NULL;
    SDL_RWclose(rw);
    if (!parsed || wav_buf == NULL || wav_len < 4) return 0;
    return InstalarPCM(spec, wav_buf, wav_len);
}

static int AbrirWavNoDisco(const char *path) {
    int len = 0;
    void *mem = LerArquivoInteiro(path, &len);
    if (mem) {
        int ok = DecodificarWavMem(mem, len);
        free(mem);
        if (ok) return 1;
    }

    SDL_AudioSpec spec;
    Uint8 *wav_buf = NULL;
    Uint32 wav_len = 0;
    if (SDL_LoadWAV(path, &spec, &wav_buf, &wav_len) == NULL) return 0;
    return InstalarPCM(spec, wav_buf, wav_len);
}

static void MarcarSampleCarregado(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    strncpy(sample_ativo_nome, base, sizeof(sample_ativo_nome) - 1);
    sample_ativo_nome[sizeof(sample_ativo_nome) - 1] = 0;
    loaded_sample_index = -1;
    for (int i = 0; i < 8; i++) {
        if (NomeIgual(sample_files[i], base)) {
            loaded_sample_index = i;
            break;
        }
    }
}

static void CopiarTextoVisivel(char *dst, int dst_n, const char *src, int max_chars) {
    int n = 0;
    int limit = dst_n - 1;
    if (max_chars < limit) limit = max_chars;
    for (int i = 0; src[i] && n < limit; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c == '_') c = '-';
        int ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.' || c == '/' || c == ':';
        dst[n++] = ok ? (char)c : ' ';
    }
    dst[n] = 0;
}

void CarregarSampleArquivo(const char *path) {
    SDL_PauseAudio(1);
    if (sample_dir[0] != '.' || sample_dir[1] != 0) sceIoChdir(sample_dir);

    int ok = AbrirWavNoDisco(path);
    if (!ok) {
        const char *base = path;
        for (const char *p = path; *p; p++) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
        if (base != path) ok = AbrirWavNoDisco(base);
    }

    if (ok) {
        memset(delay_buffer_L, 0, sizeof(delay_buffer_L));
        memset(delay_buffer_R, 0, sizeof(delay_buffer_R));
        memset(reverb_buffer_L, 0, sizeof(reverb_buffer_L));
        memset(reverb_buffer_R, 0, sizeof(reverb_buffer_R));
        delay_write_ptr = 0;
        delay_tone_L = delay_tone_R = 0.0f;
        tape_phase = 0.0f;
        reverb_ptr = 0;
        lpf_last_L = lpf_last_R = hpf_last_L = hpf_last_R = 0.0f;
        rev_damp_L = rev_damp_R = 0.0f;
        dsp_ready = 0;

        texture_voice.read_pos = 0.0f;
        texture_voice.level = 0.0f;
        texture_voice.pitch_mult = 1.0f;
        texture_voice.state = ENV_IDLE;
        for (int i = 0; i < 4; i++) {
            voices[i].read_pos = 0.0f;
            voices[i].level = 0.0f;
            voices[i].state = ENV_IDLE;
        }

        ExtrairWaveformDoWAV();
        MarcarSampleCarregado(path);
    } else {
        strncpy(sample_ativo_nome, "NAO ABRIU", sizeof(sample_ativo_nome) - 1);
        sample_ativo_nome[sizeof(sample_ativo_nome) - 1] = 0;
        if (samples_16 == NULL) InstalarSilencio();
    }

    if (!is_audio_paused) SDL_PauseAudio(0);
}

void CarregarSample(int index) {
    if (index < 0 || index > 7) return;
    CarregarSampleArquivo(sample_files[index]);
    loaded_sample_index = index;
}

void NoteOn(int voice_idx) {
    voices[voice_idx].read_pos = texture_voice.read_pos;
    voices[voice_idx].level = 0.0f;
    voices[voice_idx].state = ENV_ATTACK;
}

void NoteOff(int voice_idx) {
    if (voices[voice_idx].state != ENV_IDLE) voices[voice_idx].state = ENV_RELEASE;
}

// =======================================================
// Um playhead contínuo. A segunda leitura só existe na emenda
// do loop (crossfade curto). Dois grãos o tempo todo, com janela
// que não soma 1, viravam tremolo e pente — sobretudo ao mover
// posição, tamanho ou o tempo do delay.
// =======================================================

float sm_texture = 1.0f;

static inline float Slew(float state, float target) {
    return state + (target - state) * PARAM_SLEW;
}

static inline float Undenorm(float x) {
    return (x > -1.0e-12f && x < 1.0e-12f) ? 0.0f : x;
}

static inline void ReadStereo(float pos, float *l, float *r) {
    int n = (int)(total_samples & ~1u);
    if (n < 2 || samples_16 == NULL) { *l = *r = 0.0f; return; }
    if (pos < 0.0f) pos += (float)n;
    if (pos < 0.0f) pos = 0.0f;
    if (pos >= (float)n) pos -= (float)n;
    if (pos >= (float)n) pos = 0.0f;

    int i0 = ((int)pos) & ~1;
    if (i0 < 0) i0 = 0;
    if (i0 >= n) i0 = 0;
    float frac = (pos - (float)i0) * 0.5f;
    if (frac < 0.0f) frac = 0.0f;
    else if (frac > 1.0f) frac = 1.0f;
    int i1 = i0 + 2;
    if (i1 >= n) i1 = 0;

    float aL = (float)samples_16[i0];
    float aR = (float)samples_16[i0 + 1];
    float bL = (float)samples_16[i1];
    float bR = (float)samples_16[i1 + 1];
    *l = aL + (bL - aL) * frac;
    *r = aR + (bR - aR) * frac;
}

static inline float TapeSat(float x) {
    float ax = (x < 0.0f) ? -x : x;
    return x / (1.0f + ax * (1.0f / 26000.0f));
}

static inline void ReadDelay(float frames_back, float *l, float *r) {
    float rp = (float)delay_write_ptr - frames_back;
    if (rp < 0.0f) rp += (float)MAX_DELAY_BUFFER;
    if (rp < 0.0f) rp += (float)MAX_DELAY_BUFFER;
    if (rp >= (float)MAX_DELAY_BUFFER) rp -= (float)MAX_DELAY_BUFFER;
    int i0 = (int)rp;
    if (i0 < 0) i0 = 0;
    if (i0 >= MAX_DELAY_BUFFER) i0 = MAX_DELAY_BUFFER - 1;
    int i1 = i0 + 1;
    if (i1 >= MAX_DELAY_BUFFER) i1 = 0;
    float f = rp - (float)i0;
    *l = delay_buffer_L[i0] + (delay_buffer_L[i1] - delay_buffer_L[i0]) * f;
    *r = delay_buffer_R[i0] + (delay_buffer_R[i1] - delay_buffer_R[i0]) * f;
}

static inline void AdvanceGrain(Voice *v, float step_samples, float region_start, float region_len, float xf_samples, float level, float *mixL, float *mixR) {
    float len = region_len;
    float rel = v->read_pos - region_start;
    if (rel < 0.0f) {
        if (rel > -8.0f) rel = 0.0f;
        else {
            int wraps = (int)(rel / len);
            rel -= len * (float)wraps;
            if (rel < 0.0f) rel += len;
            if (rel >= len) rel = 0.0f;
        }
    } else if (rel >= len) {
        if (rel < len + 64.0f) rel -= len;
        else {
            int wraps = (int)(rel / len);
            rel -= len * (float)wraps;
            if (rel < 0.0f) rel += len;
            if (rel >= len) rel = 0.0f;
        }
    }

    if (level > 0.0008f) {
        float pos = region_start + rel;
        float l, r;
        ReadStereo(pos, &l, &r);
        if (rel >= len - xf_samples) {
            float t = (rel - (len - xf_samples)) / xf_samples;
            if (t < 0.0f) t = 0.0f;
            else if (t > 1.0f) t = 1.0f;
            int li = (int)(t * (float)(FADE_LUT_SIZE - 1));
            float fade_in = fade_lut[li];
            float fade_out = 1.0f - fade_in;
            float l2, r2;
            ReadStereo(pos - len, &l2, &r2);
            l = l * fade_out + l2 * fade_in;
            r = r * fade_out + r2 * fade_in;
        }
        *mixL += l * level;
        *mixR += r * level;
    }

    rel += step_samples;
    if (rel >= len) {
        if (rel < len + 64.0f) rel -= len;
        else {
            int wraps = (int)(rel / len);
            rel -= len * (float)wraps;
            if (rel >= len || rel < 0.0f) rel = 0.0f;
        }
    }
    v->read_pos = region_start + rel;
}

static inline void PanGains(float pan, float *gL, float *gR) {
    float pL = (1.0f - pan) * 2.0f;
    float pR = pan * 2.0f;
    if (pL > 1.0f) pL = 1.0f;
    if (pR > 1.0f) pR = 1.0f;
    if (pL < 0.0f) pL = 0.0f;
    if (pR < 0.0f) pR = 0.0f;
    *gL = pL;
    *gR = pR;
}

static void PublicarCpu(u64 t0, int nframes) {
    if (nframes <= 0) return;
    u64 us = sceKernelGetSystemTimeWide() - t0;
    float budget = (float)nframes * (1000000.0f / 44100.0f);
    float load = (budget > 0.0f) ? ((float)us / budget) : 0.0f;
    if (load < 0.0f) load = 0.0f;
    if (load > 1.5f) load = 1.5f;
    audio_cpu += (load - audio_cpu) * 0.2f;
}

void GranularAudioCallback(void *userdata, Uint8 *stream, int len) {
    u64 cpu_t0 = sceKernelGetSystemTimeWide();
    Sint16 *out_buffer = (Sint16 *)stream;
    int nframes = len / (int)(sizeof(Sint16) * 2);
    (void)userdata;

    if (nframes <= 0) return;

    if (total_samples < 4 || samples_16 == NULL || is_audio_paused) {
        SDL_memset(stream, 0, len);
        PublicarCpu(cpu_t0, nframes);
        return;
    }

    int seq_mode = (active_tab == 3);

    float tgt_center = grain_center;
    float tgt_width = grain_width;
    float tgt_speed = playback_speed;
    if (tgt_center < 0.0f) tgt_center = 0.0f; else if (tgt_center > 1.0f) tgt_center = 1.0f;
    if (tgt_width < 0.0005f) tgt_width = 0.0005f; else if (tgt_width > 0.20f) tgt_width = 0.20f;
    if (tgt_speed < 0.2f) tgt_speed = 0.2f; else if (tgt_speed > 2.5f) tgt_speed = 2.5f;

    float tgt_hpf_cut = hpf_cutoff;
    float tgt_lpf_cut = lpf_cutoff;
    if (tgt_hpf_cut < 0.01f) tgt_hpf_cut = 0.01f; else if (tgt_hpf_cut > 1.0f) tgt_hpf_cut = 1.0f;
    if (tgt_lpf_cut < 0.01f) tgt_lpf_cut = 0.01f; else if (tgt_lpf_cut > 1.0f) tgt_lpf_cut = 1.0f;

    float tgt_hpf_vol = hpf_vol;
    float tgt_lpf_vol = lpf_vol;
    float tgt_hpf_pan = hpf_pan;
    float tgt_lpf_pan = lpf_pan;
    float tgt_hpf_amt = hpf_enabled ? 1.0f : 0.0f;
    float tgt_lpf_amt = lpf_enabled ? 1.0f : 0.0f;
    float tgt_delay_amt = delay_enabled ? 1.0f : 0.0f;
    float tgt_rev_amt = reverb_enabled ? 1.0f : 0.0f;
    float tgt_delay_fb = delay_enabled ? delay_feedback : 0.0f;
    float tgt_delay_wet = delay_wet;
    float tgt_rev_fb = reverb_enabled ? reverb_time : 0.0f;
    float tgt_rev_damp = reverb_damp;
    float tgt_rev_wet = reverb_wet;
    float tgt_sustain = adsr_sustain;

    if (tgt_delay_fb < 0.0f) tgt_delay_fb = 0.0f; else if (tgt_delay_fb > 0.99f) tgt_delay_fb = 0.99f;
    if (tgt_rev_fb < 0.0f) tgt_rev_fb = 0.0f; else if (tgt_rev_fb > 0.95f) tgt_rev_fb = 0.95f;
    if (tgt_delay_wet < 0.0f) tgt_delay_wet = 0.0f; else if (tgt_delay_wet > 1.0f) tgt_delay_wet = 1.0f;
    if (tgt_rev_wet < 0.0f) tgt_rev_wet = 0.0f; else if (tgt_rev_wet > 1.0f) tgt_rev_wet = 1.0f;
    if (tgt_rev_damp < 0.0f) tgt_rev_damp = 0.0f; else if (tgt_rev_damp > 0.90f) tgt_rev_damp = 0.90f;
    if (tgt_sustain < 0.0f) tgt_sustain = 0.0f; else if (tgt_sustain > 1.0f) tgt_sustain = 1.0f;

    float tgt_delay = (delay_time_ms * 0.001f) * 44100.0f;
    if (tgt_delay < 1.0f) tgt_delay = 1.0f;
    if (tgt_delay > (float)(MAX_DELAY_BUFFER - 2)) tgt_delay = (float)(MAX_DELAY_BUFFER - 2);

    if (!dsp_ready) {
        sm_center = tgt_center;
        sm_width = tgt_width;
        sm_speed = tgt_speed;
        sm_hpf_cut = tgt_hpf_cut;
        sm_lpf_cut = tgt_lpf_cut;
        sm_hpf_vol = tgt_hpf_vol;
        sm_lpf_vol = tgt_lpf_vol;
        sm_hpf_pan = tgt_hpf_pan;
        sm_lpf_pan = tgt_lpf_pan;
        sm_hpf_amt = tgt_hpf_amt;
        sm_lpf_amt = tgt_lpf_amt;
        sm_delay_amt = tgt_delay_amt;
        sm_rev_amt = tgt_rev_amt;
        sm_delay_fb = tgt_delay_fb;
        sm_delay_wet = tgt_delay_wet;
        sm_rev_fb = tgt_rev_fb;
        sm_rev_damp = tgt_rev_damp;
        sm_rev_wet = tgt_rev_wet;
        sm_sustain = tgt_sustain;
        sm_texture = seq_mode ? 0.0f : 1.0f;
        delay_from = tgt_delay;
        delay_to = tgt_delay;
        delay_applied = tgt_delay;
        delay_fade = 1.0f;
        dsp_ready = 1;
    } else if (delay_fade >= 1.0f && (tgt_delay - delay_applied > 2.0f || delay_applied - tgt_delay > 2.0f)) {
        delay_from = delay_applied;
        delay_to = tgt_delay;
        delay_fade = 0.0f;
    }

    float attack_s  = (adsr_attack * 4.0f)  < 0.01f ? 0.01f : (adsr_attack * 4.0f);
    float decay_s   = (adsr_decay * 4.0f)   < 0.01f ? 0.01f : (adsr_decay * 4.0f);
    float release_s = (adsr_release * 4.0f) < 0.01f ? 0.01f : (adsr_release * 4.0f);
    float attack_rate  = 1.0f / (attack_s * 44100.0f);
    float decay_rate   = (1.0f - tgt_sustain) / (decay_s * 44100.0f);
    float release_rate = 1.0f / (release_s * 44100.0f);
    if (decay_rate < 0.0f) decay_rate = 0.0f;

    float file_len = (float)(total_samples & ~1u);
    float texture_target = seq_mode ? 0.0f : 1.0f;
    const float delay_fade_step = 1.0f / 2048.0f;

    for (int f = 0; f < nframes; f++) {
        float cdiff = tgt_center - sm_center;
        if (cdiff > 0.5f) cdiff -= 1.0f;
        else if (cdiff < -0.5f) cdiff += 1.0f;
        sm_center += cdiff * PARAM_SLEW;
        if (sm_center >= 1.0f) sm_center -= 1.0f;
        else if (sm_center < 0.0f) sm_center += 1.0f;

        sm_width = Slew(sm_width, tgt_width);
        sm_speed = Slew(sm_speed, tgt_speed);
        sm_hpf_cut = Slew(sm_hpf_cut, tgt_hpf_cut);
        sm_lpf_cut = Slew(sm_lpf_cut, tgt_lpf_cut);
        sm_hpf_vol = Slew(sm_hpf_vol, tgt_hpf_vol);
        sm_lpf_vol = Slew(sm_lpf_vol, tgt_lpf_vol);
        sm_hpf_pan = Slew(sm_hpf_pan, tgt_hpf_pan);
        sm_lpf_pan = Slew(sm_lpf_pan, tgt_lpf_pan);
        sm_hpf_amt = Slew(sm_hpf_amt, tgt_hpf_amt);
        sm_lpf_amt = Slew(sm_lpf_amt, tgt_lpf_amt);
        sm_delay_amt = Slew(sm_delay_amt, tgt_delay_amt);
        sm_rev_amt = Slew(sm_rev_amt, tgt_rev_amt);
        sm_delay_fb = Slew(sm_delay_fb, tgt_delay_fb);
        sm_delay_wet = Slew(sm_delay_wet, tgt_delay_wet);
        sm_rev_fb = Slew(sm_rev_fb, tgt_rev_fb);
        sm_rev_damp = Slew(sm_rev_damp, tgt_rev_damp);
        sm_rev_wet = Slew(sm_rev_wet, tgt_rev_wet);
        sm_sustain = Slew(sm_sustain, tgt_sustain);
        sm_texture += (texture_target - sm_texture) * TEXTURE_SLEW;

        float region_len = sm_width * file_len;
        if (region_len > file_len) region_len = file_len;
        if (region_len < 128.0f) region_len = (file_len < 128.0f) ? file_len : 128.0f;
        if (region_len < 2.0f) region_len = 2.0f;
        region_len = (float)((int)region_len & ~1);
        if (region_len < 2.0f) region_len = 2.0f;

        float region_start = sm_center * file_len - region_len * 0.5f;
        float xf_samples = region_len * 0.08f;
        if (xf_samples < 32.0f) xf_samples = 32.0f;
        if (xf_samples > 256.0f) xf_samples = 256.0f;
        if (xf_samples > region_len * 0.30f) xf_samples = region_len * 0.30f;
        float mix_L = 0.0f;
        float mix_R = 0.0f;
        float step_1x = sm_speed * 2.0f;

        AdvanceGrain(&texture_voice, step_1x, region_start, region_len, xf_samples, sm_texture, &mix_L, &mix_R);

        for (int v = 0; v < 4; v++) {
            if (voices[v].state == ENV_IDLE) continue;
            switch (voices[v].state) {
                case ENV_ATTACK:
                    voices[v].level += attack_rate;
                    if (voices[v].level >= 1.0f) { voices[v].level = 1.0f; voices[v].state = ENV_DECAY; }
                    break;
                case ENV_DECAY:
                    voices[v].level -= decay_rate;
                    if (voices[v].level <= tgt_sustain) { voices[v].level = tgt_sustain; voices[v].state = ENV_SUSTAIN; }
                    break;
                case ENV_SUSTAIN:
                    voices[v].level += (sm_sustain - voices[v].level) * PARAM_SLEW;
                    break;
                case ENV_RELEASE:
                    voices[v].level -= release_rate;
                    if (voices[v].level <= 0.0f) { voices[v].level = 0.0f; voices[v].state = ENV_IDLE; }
                    break;
                default: break;
            }
            if (voices[v].state == ENV_IDLE) continue;
            AdvanceGrain(&voices[v], step_1x * voices[v].pitch_mult, region_start, region_len, xf_samples, voices[v].level, &mix_L, &mix_R);
        }

        mix_L *= 0.90f;
        mix_R *= 0.90f;

        {
            float preL = mix_L;
            float preR = mix_R;
            hpf_last_L = Undenorm(hpf_last_L + sm_hpf_cut * (preL - hpf_last_L));
            hpf_last_R = Undenorm(hpf_last_R + sm_hpf_cut * (preR - hpf_last_R));
            float hpL = (preL - hpf_last_L) * sm_hpf_vol;
            float hpR = (preR - hpf_last_R) * sm_hpf_vol;
            float gL, gR;
            PanGains(sm_hpf_pan, &gL, &gR);
            hpL *= gL;
            hpR *= gR;
            mix_L = preL + (hpL - preL) * sm_hpf_amt;
            mix_R = preR + (hpR - preR) * sm_hpf_amt;
        }

        {
            float preL = mix_L;
            float preR = mix_R;
            lpf_last_L = Undenorm(lpf_last_L + sm_lpf_cut * (preL - lpf_last_L));
            lpf_last_R = Undenorm(lpf_last_R + sm_lpf_cut * (preR - lpf_last_R));
            float lpL = lpf_last_L * sm_lpf_vol;
            float lpR = lpf_last_R * sm_lpf_vol;
            float gL, gR;
            PanGains(sm_lpf_pan, &gL, &gR);
            lpL *= gL;
            lpR *= gR;
            mix_L = preL + (lpL - preL) * sm_lpf_amt;
            mix_R = preR + (lpR - preR) * sm_lpf_amt;
        }

        float dL, dR;
        float wobble = sinf(tape_phase) * 36.0f;
        tape_phase += 0.0000855f;
        if (tape_phase > 6.2831853f) tape_phase -= 6.2831853f;
        ReadDelay(delay_from + wobble, &dL, &dR);
        if (delay_fade < 1.0f) {
            float dL2, dR2;
            ReadDelay(delay_to + wobble, &dL2, &dR2);
            dL += (dL2 - dL) * delay_fade;
            dR += (dR2 - dR) * delay_fade;
            delay_fade += delay_fade_step;
            if (delay_fade >= 1.0f) {
                delay_fade = 1.0f;
                delay_applied = delay_to;
                delay_from = delay_to;
            }
        }

        {
            dL = TapeSat(dL);
            dR = TapeSat(dR);
            delay_tone_L = Undenorm(delay_tone_L + 0.20f * (dL - delay_tone_L));
            delay_tone_R = Undenorm(delay_tone_R + 0.20f * (dR - delay_tone_R));
            float heardL = dL * 0.35f + delay_tone_L * 0.65f;
            float heardR = dR * 0.35f + delay_tone_R * 0.65f;
            float wL = mix_L * sm_delay_amt + delay_tone_L * sm_delay_fb;
            float wR = mix_R * sm_delay_amt + delay_tone_R * sm_delay_fb;
            if (!(wL <= 30000.0f && wL >= -30000.0f)) wL = (wL > 0.0f) ? 30000.0f : (wL < 0.0f) ? -30000.0f : 0.0f;
            if (!(wR <= 30000.0f && wR >= -30000.0f)) wR = (wR > 0.0f) ? 30000.0f : (wR < 0.0f) ? -30000.0f : 0.0f;
            delay_buffer_L[delay_write_ptr] = Undenorm(wL);
            delay_buffer_R[delay_write_ptr] = Undenorm(wR);
            delay_write_ptr++;
            if (delay_write_ptr >= MAX_DELAY_BUFFER) delay_write_ptr = 0;

            float delL = mix_L + (heardL - mix_L) * sm_delay_wet;
            float delR = mix_R + (heardR - mix_R) * sm_delay_wet;
            mix_L = mix_L + (delL - mix_L) * sm_delay_amt;
            mix_R = mix_R + (delR - mix_R) * sm_delay_amt;
        }

        {
            int tap2 = reverb_ptr + REVERB_TAP2;
            if (tap2 >= REVERB_BUFFER_SIZE) tap2 -= REVERB_BUFFER_SIZE;
            float revL = (reverb_buffer_L[reverb_ptr] + reverb_buffer_L[tap2]) * 0.5f;
            float revR = (reverb_buffer_R[reverb_ptr] + reverb_buffer_R[tap2]) * 0.5f;
            float damp_coef = 1.0f - sm_rev_damp;
            if (damp_coef < 0.05f) damp_coef = 0.05f;
            rev_damp_L = Undenorm(rev_damp_L + damp_coef * (revL - rev_damp_L));
            rev_damp_R = Undenorm(rev_damp_R + damp_coef * (revR - rev_damp_R));
            float rwL = mix_L * sm_rev_amt + rev_damp_L * sm_rev_fb;
            float rwR = mix_R * sm_rev_amt + rev_damp_R * sm_rev_fb;
            if (!(rwL <= 30000.0f && rwL >= -30000.0f)) rwL = (rwL > 0.0f) ? 30000.0f : (rwL < 0.0f) ? -30000.0f : 0.0f;
            if (!(rwR <= 30000.0f && rwR >= -30000.0f)) rwR = (rwR > 0.0f) ? 30000.0f : (rwR < 0.0f) ? -30000.0f : 0.0f;
            reverb_buffer_L[reverb_ptr] = Undenorm(rwL);
            reverb_buffer_R[reverb_ptr] = Undenorm(rwR);
            reverb_ptr++;
            if (reverb_ptr >= REVERB_BUFFER_SIZE) reverb_ptr = 0;

            float wetL = mix_L + (rev_damp_L - mix_L) * sm_rev_wet;
            float wetR = mix_R + (rev_damp_R - mix_R) * sm_rev_wet;
            mix_L = mix_L + (wetL - mix_L) * sm_rev_amt;
            mix_R = mix_R + (wetR - mix_R) * sm_rev_amt;
        }

        {
            float drive = 1.55f;
            float norm_L = (mix_L * (1.0f / 32768.0f)) * drive;
            float norm_R = (mix_R * (1.0f / 32768.0f)) * drive;
            float abs_L = norm_L < 0.0f ? -norm_L : norm_L;
            float abs_R = norm_R < 0.0f ? -norm_R : norm_R;
            float final_L = (norm_L / (1.0f + abs_L)) * 32000.0f;
            float final_R = (norm_R / (1.0f + abs_R)) * 32000.0f;
            if (final_L > 32767.0f) final_L = 32767.0f; else if (final_L < -32768.0f) final_L = -32768.0f;
            if (final_R > 32767.0f) final_R = 32767.0f; else if (final_R < -32768.0f) final_R = -32768.0f;
            out_buffer[f * 2] = (Sint16)final_L;
            out_buffer[f * 2 + 1] = (Sint16)final_R;
        }
    }
    PublicarCpu(cpu_t0, nframes);
}

// =======================================================
// UI: PALETA UBUNTU TERMINAL E LAYOUT 4 LINHAS
// =======================================================
void RenderCardParam(SDL_Renderer *r, int x, int y, int w, int h, 
                    const char *title, const char *val_str, int is_focused, int is_enabled) {
    SDL_Rect box = { x, y, w, h };
    
    if (!is_enabled) {
        SDL_SetRenderDrawColor(r, 60, 25, 45, 255); SDL_RenderFillRect(r, &box);
        if (is_focused) { SDL_SetRenderDrawColor(r, 222, 72, 20, 255); SDL_RenderDrawRect(r, &box); }
    } else if (is_focused) {
        SDL_SetRenderDrawColor(r, 226, 164, 35, 255); SDL_RenderFillRect(r, &box);
    } else {
        SDL_SetRenderDrawColor(r, 58, 139, 132, 255); SDL_RenderFillRect(r, &box);
    }

    int t_y = (h <= 24) ? y + 3 : y + 6;
    int v_y = (h <= 24) ? y + 13 : y + 20;

    if (!is_enabled) {
        DrawTextCustom(r, x + 6, t_y, title, 130, 110, 120, 1);
        if (active_tab == 2) DrawTextCustom(r, x + 6, v_y, "PRONTO", 150, 130, 140, 1);
        else DrawTextCustom(r, x + 6, v_y, val_str, 150, 130, 140, 1); 
    } else if (is_focused) {
        DrawTextCustom(r, x + 6, t_y, title, 255, 255, 255, 1);
        DrawTextCustom(r, x + 6, v_y, val_str, 255, 255, 255, 1);
    } else {
        DrawTextCustom(r, x + 6, t_y, title, 255, 255, 255, 1);
        DrawTextCustom(r, x + 6, v_y, val_str, 220, 255, 255, 1);
    }
}

void RenderTransportButton(SDL_Renderer *r, int x, int y, int w, int h, int mode_id, int is_focused, int is_active) {
    SDL_Rect box = { x, y, w, h };
    if (is_active && is_focused) { SDL_SetRenderDrawColor(r, 226, 164, 35, 255); SDL_RenderFillRect(r, &box); } 
    else if (is_active) { SDL_SetRenderDrawColor(r, 58, 139, 132, 255); SDL_RenderFillRect(r, &box); } 
    else if (is_focused) { SDL_SetRenderDrawColor(r, 222, 72, 20, 255); SDL_RenderFillRect(r, &box); } 
    else { SDL_SetRenderDrawColor(r, 60, 25, 45, 255); SDL_RenderFillRect(r, &box); }

    if (is_active || is_focused) SDL_SetRenderDrawColor(r, 255, 255, 255, 255); else SDL_SetRenderDrawColor(r, 140, 110, 130, 255);
    int cx = x + w / 2, cy = y + h / 2, s = 6; 
    if (mode_id == 0) DrawTriangle(r, cx, cy, s, 1);
    else if (mode_id == 1) DrawTriangle(r, cx, cy, s, -1);
    else if (mode_id == 2) { DrawTriangle(r, cx - 4, cy, s, 1); DrawTriangle(r, cx + 4, cy, s, 1); }
    else if (mode_id == 3) { DrawTriangle(r, cx - 4, cy, s, -1); DrawTriangle(r, cx + 4, cy, s, -1); }
}

void RenderSeqStep(SDL_Renderer *r, int x, int y, int w, int h, int pitch, int is_active, int is_focused, int is_current) {
    SDL_Rect box = {x, y, w, h};
    if (is_active && is_focused) SDL_SetRenderDrawColor(r, 226, 164, 35, 255); 
    else if (is_active) SDL_SetRenderDrawColor(r, 58, 139, 132, 255); 
    else SDL_SetRenderDrawColor(r, 60, 25, 45, 255); 
    SDL_RenderFillRect(r, &box);

    if (is_current) { SDL_SetRenderDrawColor(r, 255, 255, 255, 120); SDL_RenderFillRect(r, &box); }
    if (is_focused && !is_active) { SDL_SetRenderDrawColor(r, 222, 72, 20, 255); SDL_RenderDrawRect(r, &box); }

    if (is_active) {
        char buf[8]; snprintf(buf, sizeof(buf), "%+d", pitch);
        int offset = (pitch >= 0) ? 14 : 10; if (pitch == 0) offset = 20; 
        DrawTextCustom(r, x + offset, y + 6, buf, 255, 255, 255, 1);
    }
}

static float Clamp01(float n) {
    if (n < 0.0f) return 0.0f;
    if (n > 1.0f) return 1.0f;
    return n;
}

static float NormRange(float v, float a, float b) {
    return Clamp01((v - a) / (b - a));
}

static void DrawCentered(SDL_Renderer *r, int cx, int y, const char *str, Uint8 red, Uint8 grn, Uint8 blu) {
    int tw = (int)strlen(str) * 7;
    DrawTextCustom(r, cx - tw / 2, y, str, red, grn, blu, 1);
}

static void DrawScreenArc(SDL_Renderer *r, int cx, int cy, int rad, float a0, float sweep, int segs) {
    if (segs < 1 || rad < 1) return;
    float step = sweep / (float)segs;
    int px = cx + (int)(cosf(a0) * (float)rad);
    int py = cy - (int)(sinf(a0) * (float)rad);
    for (int i = 1; i <= segs; i++) {
        float a = a0 + step * (float)i;
        int x = cx + (int)(cosf(a) * (float)rad);
        int y = cy - (int)(sinf(a) * (float)rad);
        SDL_RenderDrawLine(r, px, py, x, y);
        px = x;
        py = y;
    }
}

static void RenderKnob(SDL_Renderer *r, int cx, int cy, const char *name, const char *val, float norm, int focused, int enabled) {
    norm = Clamp01(norm);
    const float a0 = 225.0f * M_PI_F / 180.0f;
    const float sweep = -285.0f * M_PI_F / 180.0f;
    const int rad = 10;
    const int track_segs = 16;
    int val_segs = (int)(norm * (float)track_segs + 0.5f);
    if (val_segs < 0) val_segs = 0;
    if (val_segs > track_segs) val_segs = track_segs;

    if (focused) {
        if (enabled) SDL_SetRenderDrawColor(r, 226, 164, 35, 255);
        else SDL_SetRenderDrawColor(r, 222, 72, 20, 255);
        DrawScreenArc(r, cx, cy, rad + 3, a0, sweep, track_segs);
    }

    if (enabled) SDL_SetRenderDrawColor(r, 130, 90, 110, 255);
    else SDL_SetRenderDrawColor(r, 80, 55, 70, 255);
    DrawScreenArc(r, cx, cy, rad, a0, sweep, track_segs);
    DrawScreenArc(r, cx, cy, rad - 1, a0, sweep, track_segs);

    if (val_segs > 0) {
        if (focused && enabled) SDL_SetRenderDrawColor(r, 226, 164, 35, 255);
        else if (enabled) SDL_SetRenderDrawColor(r, 58, 139, 132, 255);
        else SDL_SetRenderDrawColor(r, 150, 90, 80, 255);
        float shown = sweep * ((float)val_segs / (float)track_segs);
        DrawScreenArc(r, cx, cy, rad, a0, shown, val_segs);
        DrawScreenArc(r, cx, cy, rad - 1, a0, shown, val_segs);
    }

    float ang = a0 + sweep * norm;
    int nx = cx + (int)(cosf(ang) * (float)(rad - 3));
    int ny = cy - (int)(sinf(ang) * (float)(rad - 3));
    if (enabled) SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    else SDL_SetRenderDrawColor(r, 160, 130, 140, 255);
    SDL_RenderDrawLine(r, cx, cy, nx, ny);

    Uint8 cr = enabled ? 255 : 150;
    Uint8 cg = enabled ? 255 : 130;
    Uint8 cb = enabled ? 255 : 140;
    DrawCentered(r, cx, cy - rad - 12, name, cr, cg, cb);
    DrawCentered(r, cx, cy + rad + 2, val, cr, cg, cb);
}

static void RenderFxSwitch(SDL_Renderer *r, int x, int y, int w, int h, const char *name, const char *info, int on, int focused) {
    SDL_Rect box = { x, y, w, h };
    if (on && focused) SDL_SetRenderDrawColor(r, 226, 164, 35, 255);
    else if (on) SDL_SetRenderDrawColor(r, 58, 139, 132, 255);
    else SDL_SetRenderDrawColor(r, 60, 25, 45, 255);
    SDL_RenderFillRect(r, &box);

    if (focused && !on) {
        SDL_SetRenderDrawColor(r, 222, 72, 20, 255);
        SDL_RenderDrawRect(r, &box);
    } else if (focused && on) {
        SDL_SetRenderDrawColor(r, 255, 236, 180, 255);
        SDL_RenderDrawRect(r, &box);
    }

    Uint8 sr = on ? 255 : 170;
    Uint8 sg = on ? 255 : 140;
    Uint8 sb = on ? 255 : 150;
    if (focused && on) { sr = 60; sg = 30; sb = 10; }

    DrawTextCustom(r, x + 6, y + 5, "FX", sr, sg, sb, 1);
    DrawTextCustom(r, x + 6, y + 18, name, sr, sg, sb, 1);
    DrawTextCustom(r, x + 6, y + 32, info, sr, sg, sb, 1);

    int pw = 26, ph = 12;
    int px = x + w - pw - 6;
    int py = y + (h - ph) / 2;
    SDL_Rect track = { px, py, pw, ph };
    if (on) SDL_SetRenderDrawColor(r, focused ? 255 : 220, focused ? 236 : 255, focused ? 180 : 230, 255);
    else SDL_SetRenderDrawColor(r, 36, 12, 28, 255);
    SDL_RenderFillRect(r, &track);

    int thumb_r = 4;
    int tcx = on ? (px + pw - 6) : (px + 6);
    int tcy = py + ph / 2;
    if (on) SDL_SetRenderDrawColor(r, 44, 90, 86, 255);
    else SDL_SetRenderDrawColor(r, 180, 140, 150, 255);
    for (int dy = -thumb_r; dy <= thumb_r; dy++) {
        for (int dx = -thumb_r; dx <= thumb_r; dx++) {
            if (dx * dx + dy * dy <= thumb_r * thumb_r) SDL_RenderDrawPoint(r, tcx + dx, tcy + dy);
        }
    }
}

static void RenderFxSwitchCompact(SDL_Renderer *r, int x, int y, int w, int h, const char *name, int on, int focused) {
    SDL_Rect box = { x, y, w, h };
    if (on && focused) SDL_SetRenderDrawColor(r, 226, 164, 35, 255);
    else if (on) SDL_SetRenderDrawColor(r, 58, 139, 132, 255);
    else SDL_SetRenderDrawColor(r, 60, 25, 45, 255);
    SDL_RenderFillRect(r, &box);

    if (focused && !on) {
        SDL_SetRenderDrawColor(r, 222, 72, 20, 255);
        SDL_RenderDrawRect(r, &box);
    } else if (focused && on) {
        SDL_SetRenderDrawColor(r, 255, 236, 180, 255);
        SDL_RenderDrawRect(r, &box);
    }

    Uint8 sr = on ? 255 : 170;
    Uint8 sg = on ? 255 : 140;
    Uint8 sb = on ? 255 : 150;
    if (focused && on) { sr = 60; sg = 30; sb = 10; }
    DrawTextCustom(r, x + 6, y + (h - 8) / 2, name, sr, sg, sb, 1);

    int pw = 20, ph = 10;
    int px = x + w - pw - 5;
    int py = y + (h - ph) / 2;
    SDL_Rect track = { px, py, pw, ph };
    if (on) SDL_SetRenderDrawColor(r, focused ? 255 : 220, focused ? 236 : 255, focused ? 180 : 230, 255);
    else SDL_SetRenderDrawColor(r, 36, 12, 28, 255);
    SDL_RenderFillRect(r, &track);

    int thumb_r = 3;
    int tcx = on ? (px + pw - 5) : (px + 5);
    int tcy = py + ph / 2;
    if (on) SDL_SetRenderDrawColor(r, 44, 90, 86, 255);
    else SDL_SetRenderDrawColor(r, 180, 140, 150, 255);
    for (int dy = -thumb_r; dy <= thumb_r; dy++) {
        for (int dx = -thumb_r; dx <= thumb_r; dx++) {
            if (dx * dx + dy * dy <= thumb_r * thumb_r) SDL_RenderDrawPoint(r, tcx + dx, tcy + dy);
        }
    }
}

static void FillDisc(SDL_Renderer *r, int cx, int cy, int rad) {
    if (rad < 1) return;
    for (int dy = -rad; dy <= rad; dy++) {
        int half = (int)sqrtf((float)(rad * rad - dy * dy));
        SDL_RenderDrawLine(r, cx - half, cy + dy, cx + half, cy + dy);
    }
}

#define CASS_W 200
#define CASS_H 133
static const unsigned char cass_bits[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0x00,0x03,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC0,0x07,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x60,0x06,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0E,0x30,
    0x0C,0xFC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0x30,0x0C,0xFC,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x3F,0x30,0x0C,0xFC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0x30,0x0C,0xFC,0x1F,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xF8,0x3F,0x30,0x0C,0xFC,0x3F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFC,0x3F,0x30,0x0C,0x78,0x70,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x0E,0x1E,0x30,0x0C,0x00,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x30,0x0C,
    0x01,0xC0,0x00,0x00,0x00,0x00,0x03,0xFF,0x83,0xF0,0xF0,0x79,0xFF,0x9E,0x3C,0x3F,
    0x00,0x00,0x00,0x00,0x00,0x03,0x80,0x30,0x0C,0x03,0x80,0x00,0x00,0x00,0x00,0x03,
    0xFF,0x83,0xF0,0xF8,0x79,0xFF,0x9E,0x3C,0x3F,0x00,0x00,0x00,0x00,0x00,0x01,0xC0,
    0x30,0x0C,0x07,0x00,0x00,0x00,0x00,0x00,0x03,0xFF,0xC3,0xF8,0xFC,0xFB,0xFF,0xDF,
    0x38,0x3F,0x80,0x00,0x00,0x00,0x00,0x00,0xE0,0x30,0x0C,0x0E,0x00,0x00,0x00,0x00,
    0x00,0x01,0xE7,0xC7,0xF8,0x7F,0xFB,0xE7,0xDF,0xB8,0x7F,0x80,0x00,0x00,0x00,0x00,
    0x00,0x70,0x30,0x0C,0x1C,0x00,0x00,0x00,0x00,0x00,0x01,0xE3,0xC7,0xF8,0x7F,0xFB,
    0xC3,0xDF,0xB8,0x7F,0x80,0x00,0x00,0x00,0x00,0x00,0x38,0x30,0x0C,0x18,0x00,0x00,
    0x00,0x00,0x00,0x01,0xEF,0xC7,0xBC,0x7F,0xFB,0xC3,0xDF,0xF8,0xFF,0xC0,0x00,0x00,
    0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x01,0xFF,0x87,0x3C,
    0x7F,0xFB,0xC3,0xDF,0xF8,0xF3,0xC0,0x00,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,
    0x00,0x00,0x00,0x00,0x00,0x03,0xFF,0x0F,0xFE,0x7F,0xFB,0xC3,0xDF,0xFC,0xFF,0xE0,
    0x00,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0xFF,0xFF,0xFF,0xFF,0xFC,0x03,0xFF,
    0x8F,0xFE,0xF7,0xFB,0xC3,0xDD,0xFD,0xFF,0xE0,0x1F,0xFF,0xFF,0xFF,0xFF,0x18,0x30,
    0x0C,0x18,0xFF,0xFF,0xFF,0xFF,0xFC,0x01,0xE7,0x8F,0xFE,0x73,0x3B,0xE7,0xDC,0xF9,
    0xFF,0xE0,0x1F,0xFF,0xFF,0xFF,0xFF,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,
    0x01,0xE3,0xDE,0x1F,0x70,0x7B,0xFF,0xDE,0xFD,0xE1,0xE0,0x00,0x00,0x00,0x00,0x00,
    0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x03,0xE3,0xFE,0x0F,0x70,0x79,0xFF,
    0x9E,0x7D,0xE1,0xF0,0x00,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0xFF,0xFF,0xFF,
    0xFF,0xE0,0x01,0xE3,0xFE,0x0F,0x70,0x79,0xFF,0x9E,0x7D,0xC1,0xF0,0x03,0xFF,0xFF,
    0xFF,0xFF,0x18,0x30,0x0C,0x18,0xFF,0xFF,0xFF,0xFF,0xE0,0x01,0xE1,0xDC,0x0F,0x70,
    0x38,0x7E,0x1C,0x39,0xC0,0xE0,0x03,0xFF,0xFF,0xFF,0xFF,0x18,0x30,0x0C,0x18,0xFF,
    0xFF,0xFF,0xFF,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,
    0xFF,0xFF,0xFF,0xFF,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x03,0xFE,0x7F,0xE0,
    0x7C,0x3C,0x7B,0xC7,0xBC,0x00,0x7C,0x3F,0xF0,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,
    0x18,0xFF,0xFF,0xFF,0xFF,0x07,0xFE,0xFF,0xE0,0x7C,0x3E,0x7B,0xC7,0xBC,0x00,0xFC,
    0x3F,0xF8,0xFF,0xFF,0xFF,0xFF,0x18,0x30,0x0C,0x18,0xFF,0xFF,0xFF,0xFF,0x0F,0xFE,
    0x7F,0xE0,0xFE,0x3E,0x7B,0xC7,0xBC,0x00,0xFE,0x1F,0xF8,0xFF,0xFF,0xFF,0xFF,0x18,
    0x30,0x0C,0x18,0xFF,0xFF,0xFF,0xFF,0x1F,0x9E,0x79,0xF1,0xFE,0x1F,0x7B,0xC3,0xBC,
    0x01,0xFE,0x1E,0x7C,0xFF,0xFF,0xFF,0xFF,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,
    0x1E,0x0E,0x78,0xF1,0xFE,0x1F,0xFB,0xC3,0xBC,0x01,0xFE,0x1E,0x3C,0x00,0x00,0x00,
    0x00,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x1E,0x00,0x79,0xF1,0xEF,0x1F,0xFB,
    0xC3,0xBC,0x01,0xEE,0x1C,0x7C,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x00,0x00,
    0x00,0x00,0x3C,0x3F,0x7F,0xE1,0xCF,0x1F,0xFB,0xC3,0x9C,0x01,0xC7,0x1F,0xF8,0x00,
    0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x3C,0x7F,0x7F,0xC3,0xFF,
    0xBF,0xFB,0xC7,0xBC,0x03,0xFF,0x9F,0xF0,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,
    0x00,0x00,0x00,0x00,0x3C,0x7F,0x7F,0xC3,0xFF,0xBF,0xFB,0xC7,0xBC,0x03,0xFF,0xBF,
    0xF8,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x3E,0x3F,0x7B,
    0xE3,0xFF,0x9D,0xFB,0xEF,0xBC,0x03,0xFF,0x9C,0xF8,0x00,0x00,0x00,0x00,0x18,0x30,
    0x0C,0x18,0x00,0x00,0x00,0x00,0x3F,0xFE,0x7B,0xE7,0x87,0xDD,0xFB,0xFF,0xBF,0xF7,
    0x87,0xDE,0x7E,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x1F,
    0xFE,0x79,0xF7,0x83,0xFC,0xF9,0xFF,0x3F,0xF7,0x83,0xDE,0x3E,0x00,0x00,0x00,0x00,
    0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x0F,0xE6,0x70,0x77,0x03,0xDC,0x78,0x7C,
    0x3F,0xF7,0x03,0xDC,0x0F,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,
    0x00,0x07,0xE0,0x70,0x77,0x03,0x9C,0x30,0x7C,0x00,0x07,0x03,0xDC,0x0F,0x00,0x00,
    0x00,0x00,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x03,
    0x7F,0xF6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x03,0xFD,0xFF,0x80,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,
    0x18,0x03,0xE0,0x1F,0xC0,0x00,0x0F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xF0,0x00,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x1F,0xC0,0x1D,0xE0,0x00,0x3F,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFC,0x00,0x00,0x00,0x00,0x00,0x18,
    0x30,0x0C,0x18,0x0F,0x00,0x00,0xF8,0x00,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x0F,0x00,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x0F,0x00,0x00,0x78,
    0x01,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x80,0x00,0x00,0x00,
    0x00,0x18,0x30,0x0C,0x18,0x3E,0x00,0x00,0x0E,0x03,0x87,0xF8,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x1F,0xE1,0xC0,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x78,0x38,
    0x30,0x06,0x07,0x1F,0xFE,0x00,0x4F,0xFF,0xFF,0xFF,0xEA,0x00,0x7F,0xF8,0xE0,0x00,
    0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x70,0x3C,0x78,0x07,0x0E,0x3F,0xFF,0x80,0x48,
    0x00,0x1F,0xFF,0xFA,0x00,0xFF,0xFC,0x70,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,
    0xE0,0x3C,0x78,0x03,0x9C,0xF9,0xE7,0xC0,0x6C,0x00,0x1F,0xFF,0xDA,0x01,0xF7,0x9F,
    0x30,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x19,0xE0,0x3C,0x78,0x01,0x98,0xE0,0x03,
    0xC0,0xEC,0x00,0x0F,0xFF,0xD2,0x03,0xC0,0x07,0x18,0x00,0x00,0x00,0x00,0x18,0x30,
    0x0C,0x19,0xC0,0x00,0x00,0x07,0xB9,0xF0,0x03,0xE0,0xF4,0x00,0x0F,0xFF,0xD6,0x07,
    0xE0,0x0F,0x98,0x1F,0xFF,0xFF,0xFF,0x98,0x30,0x0C,0x19,0xC0,0x00,0x00,0x07,0xB1,
    0xF0,0x03,0xE0,0xFC,0x00,0x0F,0xFF,0xD6,0x07,0xC0,0x0F,0x8C,0x1F,0xFF,0xFF,0xFF,
    0x98,0x30,0x0C,0x19,0x80,0x03,0xE0,0x3F,0xB3,0xA0,0x01,0x70,0xFE,0x00,0x0F,0xFF,
    0xF6,0x0E,0xC0,0x05,0xCC,0x18,0x00,0x7D,0xF9,0x98,0x30,0x0C,0x1B,0xE0,0x03,0xE0,
    0x3F,0xB3,0x80,0x00,0x70,0xFE,0x00,0x0F,0xFF,0xF6,0x0E,0x00,0x01,0xCC,0x1B,0xFD,
    0xFD,0xFD,0x98,0x30,0x0C,0x1B,0xF8,0x03,0xE0,0x1E,0x63,0x80,0x00,0x70,0xFE,0x00,
    0x0F,0xFF,0xFE,0x0E,0x00,0x01,0xCC,0x1B,0xFD,0xFD,0xFF,0x98,0x30,0x0C,0x1B,0xFE,
    0x03,0xC0,0x1E,0x63,0x00,0x00,0x70,0xFE,0x00,0x0F,0xFF,0xFE,0x0E,0x00,0x01,0xCC,
    0x1B,0xDC,0x3D,0xDF,0x98,0x30,0x0C,0x1B,0xFE,0x00,0x00,0x3F,0x63,0x40,0x00,0xF0,
    0xFE,0x00,0x0F,0xFF,0xBE,0x0F,0x00,0x03,0xCC,0x1B,0xDE,0x79,0xFF,0x98,0x30,0x0C,
    0x18,0xFE,0x00,0x00,0x3F,0xE3,0xC0,0x01,0xF0,0xFE,0x00,0x0F,0xFF,0xFE,0x0F,0x80,
    0x07,0xCC,0x19,0xCE,0xF9,0xFD,0x98,0x30,0x0C,0x18,0xFC,0x00,0x00,0x1F,0xB3,0xE0,
    0x01,0xF0,0xFF,0x00,0x8F,0xFF,0xFE,0x0F,0x80,0x07,0xCC,0x1B,0xDF,0xE1,0xFF,0x98,
    0x30,0x0C,0x18,0x7C,0x00,0x00,0x3F,0x33,0xC0,0x00,0xE0,0xFE,0x00,0x0F,0xFF,0xFE,
    0x0F,0x80,0x03,0x8C,0x19,0xDD,0xE1,0xDF,0x98,0x30,0x0C,0x18,0x3B,0x00,0x00,0xFE,
    0x33,0xC0,0x00,0xE0,0xFC,0x00,0x0F,0xFF,0xFE,0x07,0x00,0x03,0x98,0x1B,0xFD,0xFD,
    0xFF,0x98,0x30,0x0C,0x18,0x3F,0x0D,0x00,0xFE,0x39,0xE0,0x01,0xE0,0xFC,0x00,0x0F,
    0xFF,0xD6,0x07,0x80,0x07,0x98,0x1B,0xF9,0xFD,0xFD,0x98,0x30,0x0C,0x18,0x7F,0x3F,
    0xEF,0xF8,0x18,0xFE,0x1F,0xC0,0xF4,0x00,0x0F,0xFF,0xD2,0x03,0xDC,0x7F,0x38,0x1B,
    0xF1,0xFD,0xF9,0x98,0x30,0x0C,0x18,0x7F,0xFF,0xFF,0xF8,0x1C,0xFF,0x1F,0x80,0xFC,
    0x00,0x0F,0xFF,0xDA,0x01,0xFC,0x7E,0x70,0x18,0x00,0x00,0x01,0x98,0x30,0x0C,0x18,
    0x6F,0xFF,0xFF,0xF8,0x0E,0x7F,0xFF,0x00,0x7F,0xFF,0xFF,0xFF,0xFA,0x00,0xFF,0xFC,
    0xE0,0x1F,0xFF,0xFF,0xFF,0x98,0x30,0x0C,0x18,0x07,0xFF,0xDE,0xF0,0x07,0x1F,0xFC,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0xF1,0xC0,0x00,0x00,0x00,0x00,0x18,0x30,
    0x0C,0x18,0x03,0x3F,0xC6,0x00,0x03,0xC3,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0F,0xC3,0x80,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x00,0x38,0xC6,0x00,0x01,
    0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,0x00,0x00,0x00,0x00,
    0x18,0x30,0x0C,0x18,0x00,0x18,0x00,0x00,0x00,0x7F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFC,0x00,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,
    0x00,0x00,0x0F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xF0,0x00,0x00,0x00,
    0x00,0x00,0x18,0x30,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x18,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x18,0x30,0x0C,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x38,0x30,0x0C,
    0x0E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x70,0x30,0x0C,0x07,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xE0,
    0x30,0x0C,0x03,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC0,0x30,0x0C,0x01,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0x80,0x30,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x0C,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,
    0x0C,0x00,0x00,0x00,0x03,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xC0,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x02,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60,0x00,0x00,
    0x00,0x30,0x0C,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x06,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,
    0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,
    0x00,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x30,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x0C,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x30,0x0C,
    0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x18,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x18,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x00,
    0x30,0x0C,0x00,0x00,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x10,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0C,0x00,
    0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0C,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,
    0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x04,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x30,0x0C,0x00,
    0x00,0x00,0x20,0x00,0x00,0x00,0x07,0xE0,0x00,0x00,0x00,0x00,0x00,0x07,0xE0,0x00,
    0x00,0x00,0x06,0x00,0x00,0x00,0x30,0x0C,0x00,0x00,0x00,0x60,0x00,0x07,0x00,0x06,
    0x30,0x00,0x00,0x00,0x00,0x00,0x06,0x30,0x00,0x70,0x00,0x06,0x00,0x00,0x00,0x30,
    0x0C,0x78,0x00,0x00,0x60,0x00,0x1F,0xC0,0x04,0x30,0x00,0x00,0x00,0x00,0x00,0x04,
    0x10,0x01,0xFC,0x00,0x02,0x00,0x00,0x1E,0x30,0x0C,0xFC,0x00,0x00,0x40,0x00,0x30,
    0x60,0x04,0x30,0x00,0x00,0x00,0x00,0x00,0x04,0x10,0x03,0x0C,0x00,0x02,0x00,0x00,
    0x3F,0x30,0x0C,0xFC,0x00,0x00,0x40,0x00,0x20,0x60,0x04,0x30,0x00,0x00,0x00,0x00,
    0x00,0x04,0x10,0x02,0x06,0x00,0x03,0x00,0x00,0x3F,0x30,0x0C,0xFC,0x00,0x00,0xC0,
    0x00,0x20,0x20,0x06,0x30,0x00,0x00,0x00,0x00,0x00,0x06,0x30,0x02,0x06,0x00,0x03,
    0x00,0x00,0x3F,0x30,0x0C,0xFC,0x00,0x00,0xC0,0x00,0x20,0x60,0x07,0xE0,0x00,0x00,
    0x00,0x00,0x00,0x03,0xE0,0x02,0x06,0x00,0x01,0x00,0x00,0x3F,0x30,0x0C,0xFC,0x00,
    0x00,0x80,0x00,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x04,
    0x00,0x01,0x80,0x00,0x3F,0x30,0x06,0x00,0x00,0x01,0x80,0x00,0x39,0xC0,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x9C,0x00,0x01,0x80,0x00,0x00,0x60,0x07,
    0x00,0x00,0x01,0x80,0x00,0x0F,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0xF8,0x00,0x00,0x80,0x00,0x00,0xE0,0x03,0xFF,0xFF,0xFF,0x80,0x00,0x00,0x00,
    0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xC0,0x00,0x60,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static void RenderMarcaFita(SDL_Renderer *r, int x, int y) {
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    const int stride = (CASS_W + 7) >> 3;
    for (int row = 0; row < CASS_H; row++) {
        const unsigned char *src = cass_bits + row * stride;
        int run = -1;
        for (int col = 0; col <= CASS_W; col++) {
            int on = 0;
            if (col < CASS_W) on = (src[col >> 3] & (unsigned char)(0x80 >> (col & 7))) != 0;
            if (on) {
                if (run < 0) run = col;
            } else if (run >= 0) {
                SDL_RenderDrawLine(r, x + run, y + row, x + col - 1, y + row);
                run = -1;
            }
        }
    }

    for (int hub = 0; hub < 2; hub++) {
        float fx = (hub == 0) ? 0.324f : 0.674f;
        int cx = x + (int)(fx * CASS_W);
        int cy = y + (int)(0.525f * CASS_H);
        int rad = 8;
        SDL_SetRenderDrawColor(r, 44, 0, 30, 255);
        FillDisc(r, cx, cy, rad);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        for (int s = 0; s < 6; s++) {
            float a = tape_angle + (float)s * (2.0f * M_PI_F / 6.0f);
            int x2 = cx + (int)(cosf(a) * (float)(rad - 2));
            int y2 = cy - (int)(sinf(a) * (float)(rad - 2));
            SDL_RenderDrawLine(r, cx, cy, x2, y2);
        }
        SDL_SetRenderDrawColor(r, 44, 0, 30, 255);
        FillDisc(r, cx, cy, 2);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        DrawScreenArc(r, cx, cy, rad + 1, 0.0f, 6.2831853f, 16);
    }

    int tx = x + (int)(0.43f * CASS_W);
    int tw = (int)(0.14f * CASS_W);
    int ty = y + (int)(0.485f * CASS_H);
    int th = (int)(0.09f * CASS_H);
    if (th < 4) th = 4;
    SDL_Rect roll = { tx, ty, tw, th };
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderFillRect(r, &roll);
    int period = 5;
    int offset = (int)(tape_angle * 8.0f) % period;
    if (offset < 0) offset += period;
    SDL_SetRenderDrawColor(r, 44, 0, 30, 255);
    for (int sx = tx - offset; sx < tx + tw; sx += period) {
        if (sx >= tx) SDL_RenderDrawLine(r, sx, ty, sx, ty + th - 1);
    }
}

static void RenderFaixaFx(SDL_Renderer *r, int focus_base) {
    int enabled_fx[4] = {hpf_enabled, lpf_enabled, delay_enabled, reverb_enabled};
    char info[4][16];
    const char *fx_names[4] = {"HPF", "LPF", "DELAY", "REVERB"};

    snprintf(info[0], 16, "CORTE %.0f", hpf_cutoff * 100.0f);
    snprintf(info[1], 16, "CORTE %.0f", lpf_cutoff * 100.0f);
    snprintf(info[2], 16, "%.0fMS", delay_time_ms);
    snprintf(info[3], 16, "TAM %.0f", reverb_time * 100.0f);

    SDL_Rect fx_bar = { 0, 208, SCREEN_WIDTH, 64 };
    SDL_SetRenderDrawColor(r, 30, 5, 20, 255);
    SDL_RenderFillRect(r, &fx_bar);

    for (int e = 0; e < 4; e++) {
        int x = 8 + e * 118;
        RenderFxSwitch(r, x, 214, 112, 52, fx_names[e], info[e], enabled_fx[e], selected_param_index == (focus_base + e));
    }
}

static void RenderModalSamples(SDL_Renderer *r) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 16, 0, 10, 170);
    SDL_Rect shade = { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
    SDL_RenderFillRect(r, &shade);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    SDL_Rect panel = { 64, 46, 352, 180 };
    SDL_SetRenderDrawColor(r, 44, 0, 30, 255);
    SDL_RenderFillRect(r, &panel);
    SDL_SetRenderDrawColor(r, 226, 164, 35, 255);
    SDL_RenderDrawRect(r, &panel);

    DrawTextCustom(r, 76, 54, "ARQUIVOS", 226, 164, 35, 1);

    if (sample_dir_count <= 0) {
        DrawTextCustom(r, 76, 84, "NENHUM WAV NA PASTA", 255, 255, 255, 1);
    } else {
        int visible = 6;
        int start = 0;
        if (sample_browser_index >= visible) start = sample_browser_index - visible + 1;
        if (start + visible > sample_dir_count) start = sample_dir_count - visible;
        if (start < 0) start = 0;
        int shown = sample_dir_count - start;
        if (shown > visible) shown = visible;
        for (int i = 0; i < shown; i++) {
            int idx = start + i;
            int y = 74 + i * 16;
            int focused = (idx == sample_browser_index);
            if (focused) {
                SDL_Rect row = { 72, y - 2, 336, 14 };
                SDL_SetRenderDrawColor(r, 226, 164, 35, 255);
                SDL_RenderFillRect(r, &row);
            }
            char line[40];
            CopiarTextoVisivel(line, sizeof(line), sample_dir_files[idx], 36);
            DrawTextCustom(r, 78, y, line, 255, 255, 255, 1);
        }
    }

    DrawTextCustom(r, 76, 202, "X CARREGA   O FECHA", 180, 150, 165, 1);
}

void RenderizarInterfaceFORM(SDL_Renderer *r) {
    SDL_SetRenderDrawColor(r, 44, 0, 30, 255); SDL_RenderClear(r);

    SDL_Rect header = { 0, 0, SCREEN_WIDTH, 28 };
    SDL_SetRenderDrawColor(r, 60, 20, 45, 255); SDL_RenderFillRect(r, &header);

    const char *tab_name = "SEQUENCIADOR";
    if (active_tab == 0) tab_name = "TEXTURA";
    else if (active_tab == 1) tab_name = "EFEITOS";
    else if (active_tab == 2) tab_name = "SAMPLES";
    DrawTextCustom(r, 12, 8, tab_name, 226, 164, 35, 1);

    if (active_tab != 0) {
        int tab_w = (int)strlen(tab_name) * 7;
        DrawTextCustom(r, 12 + tab_w + 12, 8, "RAMONA GRANULAR", 255, 255, 255, 1);
    }
    {
        int cpu_pct = (int)(audio_cpu * 100.0f + 0.5f);
        if (cpu_pct < 0) cpu_pct = 0;
        if (cpu_pct > 150) cpu_pct = 150;
        char cpu_buf[12];
        snprintf(cpu_buf, sizeof(cpu_buf), "CPU %d%%", cpu_pct);
        Uint8 cr = 58, cg = 139, cb = 132;
        if (cpu_pct >= 80) { cr = 222; cg = 72; cb = 20; }
        else if (cpu_pct >= 50) { cr = 226; cg = 164; cb = 35; }
        const char *ver = "VER 1.0";
        int bar_slot = 40;
        int ver_w = (int)strlen(ver) * 7;
        int cpu_w = (int)strlen(cpu_buf) * 7;
        int bar_x = SCREEN_WIDTH - 10 - bar_slot;
        int cpu_x = bar_x - 6 - cpu_w;
        int ver_x = cpu_x - 14 - ver_w;
        DrawTextCustom(r, ver_x, 8, ver, 130, 96, 112, 1);
        DrawTextCustom(r, cpu_x, 8, cpu_buf, cr, cg, cb, 1);
        int bar_w = (cpu_pct > 100) ? bar_slot : (cpu_pct * bar_slot) / 100;
        SDL_Rect cpu_bg = { bar_x, 11, bar_slot, 6 };
        SDL_SetRenderDrawColor(r, 30, 8, 20, 255);
        SDL_RenderFillRect(r, &cpu_bg);
        if (bar_w > 0) {
            SDL_Rect cpu_fg = { bar_x, 11, bar_w, 6 };
            SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
            SDL_RenderFillRect(r, &cpu_fg);
        }
    }

    int wave_center_y = 66, bar_width = 2, bar_gap = 1;   
    int total_width = BARS_COUNT * (bar_width + bar_gap);
    int start_x = (SCREEN_WIDTH - total_width) / 2;

    SDL_Rect wave_bg = { start_x - 6, 32, total_width + 12, 68 };
    SDL_SetRenderDrawColor(r, 30, 5, 20, 255); SDL_RenderFillRect(r, &wave_bg);

    int center_pixel_x = start_x + (int)(grain_center * total_width);
    int width_pixels   = (int)(grain_width * total_width);
    int region_left    = center_pixel_x - (width_pixels / 2);
    int region_right   = center_pixel_x + (width_pixels / 2);

    if (region_left < start_x) region_left = start_x; if (region_right > start_x + total_width) region_right = start_x + total_width;

    for (int i = 0; i < BARS_COUNT; i++) {
        int x = start_x + i * (bar_width + bar_gap);
        int h = wave_picos[i];
        if (x >= region_left && x <= region_right) SDL_SetRenderDrawColor(r, 222, 72, 20, 255); 
        else SDL_SetRenderDrawColor(r, 130, 90, 110, 255); 
        SDL_Rect bar = { x, wave_center_y - h, bar_width, h * 2 }; SDL_RenderFillRect(r, &bar);
    }

    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderDrawLine(r, region_left, 32, region_left, 100); SDL_RenderDrawLine(r, region_right, 32, region_right, 100);
    SDL_SetRenderDrawColor(r, 58, 139, 132, 255); 
    SDL_RenderDrawLine(r, center_pixel_x, 32, center_pixel_x, 100);

    char val_buf[32];
    int card_w = 110;

    if (active_tab == 0) {
        int gap = 4;
        int cass_x = SCREEN_WIDTH - 8 - CASS_W;
        int right = cass_x - gap;
        int x0 = 8;
        int bw = (right - x0 - gap) / 2;
        int x1 = x0 + bw + gap;
        int bh = 30;
        int y0 = 104;
        int y1 = y0 + bh + gap;
        snprintf(val_buf, sizeof(val_buf), "%.1fMS", grain_width * 1000.0f); RenderCardParam(r, x0, y0, bw, bh, "TAMANHO", val_buf, (selected_param_index == 0), 1);
        snprintf(val_buf, sizeof(val_buf), "%.1f%%", grain_center * 100.0f); RenderCardParam(r, x1, y0, right - x1, bh, "POSICAO", val_buf, (selected_param_index == 1), 1);
        snprintf(val_buf, sizeof(val_buf), "%dBPM", bpm); RenderCardParam(r, x0, y1, bw, bh, "BPM/VELOC", val_buf, (selected_param_index == 2), 1);
        snprintf(val_buf, sizeof(val_buf), "%.2fX", playback_speed); RenderCardParam(r, x1, y1, right - x1, bh, "TOM/VELOC", val_buf, (selected_param_index == 3), 1);

        int tw = (right - x0 - 3 * gap) / 4;
        int ty = y1 + bh + 8;
        int th = 26;
        RenderTransportButton(r, x0, ty, tw, th, 3, (selected_param_index == 4), (scan_mode == 3));
        RenderTransportButton(r, x0 + (tw + gap), ty, tw, th, 1, (selected_param_index == 5), (scan_mode == 1));
        RenderTransportButton(r, x0 + 2 * (tw + gap), ty, tw, th, 0, (selected_param_index == 6), (scan_mode == 0));
        RenderTransportButton(r, x0 + 3 * (tw + gap), ty, right - (x0 + 3 * (tw + gap)), th, 2, (selected_param_index == 7), (scan_mode == 2));
        RenderMarcaFita(r, cass_x, y0);
        {
            const char *fx_short[4] = {"HPF", "LPF", "DELAY", "REVERB"};
            int enabled_fx[4] = {hpf_enabled, lpf_enabled, delay_enabled, reverb_enabled};
            for (int e = 0; e < 4; e++) {
                RenderFxSwitchCompact(r, 8 + e * 116, 248, card_w, 20, fx_short[e], enabled_fx[e], selected_param_index == (8 + e));
            }
        }
    } else if (active_tab == 1) {
        const char *titles[4] = {"HPF", "LPF", "DLY", "REV"};
        const char *knames[4][3] = {
            {"CORTE", "VOL", "PAN"},
            {"CORTE", "VOL", "PAN"},
            {"TEMPO", "ECO", "MIX"},
            {"TAM", "DMP", "MIX"}
        };
        int enabled_fx[4] = {hpf_enabled, lpf_enabled, delay_enabled, reverb_enabled};
        float norms[4][3];
        char vals[4][3][12];

        norms[0][0] = NormRange(hpf_cutoff, 0.01f, 1.0f);
        norms[0][1] = NormRange(hpf_vol, 0.0f, 2.0f);
        norms[0][2] = hpf_pan;
        snprintf(vals[0][0], 12, "%.0f%%", hpf_cutoff * 100.0f);
        snprintf(vals[0][1], 12, "%.0f%%", hpf_vol * 100.0f);
        {
            int p = (int)((hpf_pan - 0.5f) * 200.0f);
            if (p == 0) snprintf(vals[0][2], 12, "C");
            else if (p < 0) snprintf(vals[0][2], 12, "E%d", -p);
            else snprintf(vals[0][2], 12, "D%d", p);
        }
        norms[1][0] = NormRange(lpf_cutoff, 0.01f, 1.0f);
        norms[1][1] = NormRange(lpf_vol, 0.0f, 2.0f);
        norms[1][2] = lpf_pan;
        snprintf(vals[1][0], 12, "%.0f%%", lpf_cutoff * 100.0f);
        snprintf(vals[1][1], 12, "%.0f%%", lpf_vol * 100.0f);
        {
            int p = (int)((lpf_pan - 0.5f) * 200.0f);
            if (p == 0) snprintf(vals[1][2], 12, "C");
            else if (p < 0) snprintf(vals[1][2], 12, "E%d", -p);
            else snprintf(vals[1][2], 12, "D%d", p);
        }
        norms[2][0] = NormRange(delay_time_ms, 60.0f, 1500.0f);
        norms[2][1] = NormRange(delay_feedback, 0.0f, 0.99f);
        norms[2][2] = delay_wet;
        snprintf(vals[2][0], 12, "%.0fMS", delay_time_ms);
        snprintf(vals[2][1], 12, "%.0f%%", delay_feedback * 100.0f);
        snprintf(vals[2][2], 12, "%.0f%%", delay_wet * 100.0f);

        norms[3][0] = NormRange(reverb_time, 0.10f, 0.95f);
        norms[3][1] = NormRange(reverb_damp, 0.0f, 0.90f);
        norms[3][2] = reverb_wet;
        snprintf(vals[3][0], 12, "%.0f%%", reverb_time * 100.0f);
        snprintf(vals[3][1], 12, "%.0f%%", reverb_damp * 100.0f);
        snprintf(vals[3][2], 12, "%.0f%%", reverb_wet * 100.0f);
        for (int e = 0; e < 4; e++) {
            int col = e % 2;
            int row = e / 2;
            int bx = col ? 244 : 8;
            int by = 104 + row * 52;
            Uint8 tr = enabled_fx[e] ? 226 : 140;
            Uint8 tg = enabled_fx[e] ? 164 : 110;
            Uint8 tb = enabled_fx[e] ? 35 : 120;
            DrawTextCustom(r, bx, by + 16, titles[e], tr, tg, tb, 1);
            for (int k = 0; k < 3; k++) {
                int cx = bx + 70 + k * 56;
                int cy = by + 22;
                int idx = e * 3 + k;
                RenderKnob(r, cx, cy, knames[e][k], vals[e][k], norms[e][k], selected_param_index == idx, enabled_fx[e]);
            }
        }

        RenderFaixaFx(r, 12);
    } else if (active_tab == 2) {
        int card_h = 36, row1_y = 110, row2_y = 160;
        for (int i = 0; i < 8; i++) RenderCardParam(r, 8+(i%4)*116, (i<4)?row1_y:row2_y, card_w, card_h, sample_names[i], (loaded_sample_index == i)?"ATIVO":"PRONTO", (selected_param_index == i), (loaded_sample_index == i));
        char shown[40];
        CopiarTextoVisivel(shown, sizeof(shown), sample_ativo_nome, 36);
        RenderCardParam(r, 8, row2_y + card_h + 10, 464, 36, "CARREGAR", shown, (selected_param_index == 8), 1);
    } else if (active_tab == 3) {
        int card_h = 36, row1_y = 110;
        snprintf(val_buf, sizeof(val_buf), "%.0f%%", adsr_attack * 100.0f); RenderCardParam(r, 8, row1_y, card_w, card_h, "ATTACK", val_buf, (selected_param_index == 0), 1);
        snprintf(val_buf, sizeof(val_buf), "%.0f%%", adsr_decay * 100.0f); RenderCardParam(r, 124, row1_y, card_w, card_h, "DECAY", val_buf, (selected_param_index == 1), 1);
        snprintf(val_buf, sizeof(val_buf), "%.0f%%", adsr_sustain * 100.0f); RenderCardParam(r, 240, row1_y, card_w, card_h, "SUSTAIN", val_buf, (selected_param_index == 2), 1);
        snprintf(val_buf, sizeof(val_buf), "%.0f%%", adsr_release * 100.0f); RenderCardParam(r, 356, row1_y, card_w, card_h, "RELEASE", val_buf, (selected_param_index == 3), 1);
        
        int seq_w = 48, spacing = 6;
        int start_seq_x = (SCREEN_WIDTH - (8 * seq_w + 7 * spacing)) / 2;
        int seq_y1 = 166, seq_y2 = 188;
        
        char res_buf[32]; snprintf(res_buf, sizeof(res_buf), "DURACAO DA NOTA: %s", res_texts[seq_res_idx]);
        DrawTextCustom(r, start_seq_x + 80, seq_y1 - 10, res_buf, 226, 164, 35, 1);

        for (int i = 0; i < 16; i++) {
            int row = i / 8, col = i % 8;
            int x = start_seq_x + col * (seq_w + spacing);
            int y = (row == 0) ? seq_y1 : seq_y2;
            RenderSeqStep(r, x, y, seq_w, 18, seq_pitch[i], seq_active[i], (selected_param_index == i + 4), (current_seq_step == i && active_tab == 3));
        }

        int grain_y = seq_y2 + 18 + 8;
        int grain_h = 36;
        snprintf(val_buf, sizeof(val_buf), "%.1fMS", grain_width * 1000.0f); RenderCardParam(r, 8, grain_y, card_w, grain_h, "TAMANHO", val_buf, (selected_param_index == 20), 1);
        snprintf(val_buf, sizeof(val_buf), "%.1f%%", grain_center * 100.0f); RenderCardParam(r, 124, grain_y, card_w, grain_h, "POSICAO", val_buf, (selected_param_index == 21), 1);
        snprintf(val_buf, sizeof(val_buf), "%dBPM", bpm); RenderCardParam(r, 240, grain_y, card_w, grain_h, "BPM/VELOC", val_buf, (selected_param_index == 22), 1);
        snprintf(val_buf, sizeof(val_buf), "%.2fX", playback_speed); RenderCardParam(r, 356, grain_y, card_w, grain_h, "TOM/VELOC", val_buf, (selected_param_index == 23), 1);
    }

    if (sample_browser_open) RenderModalSamples(r);

    SDL_RenderPresent(r);
}

static void MudarAba(int dir) {
    int old_tab = active_tab;
    int next = active_tab + dir;
    if (next < 0) next = 3;
    if (next > 3) next = 0;
    active_tab = next;
    selected_param_index = 0;
    sample_browser_open = 0;

    if (active_tab == 3) {
        last_seq_tick = SDL_GetTicks();
    } else if (old_tab == 3) {
        SDL_LockAudio();
        for (int i = 0; i < 4; i++) NoteOff(i);
        SDL_UnlockAudio();
    }
}

static void AlternarEfeito(int fx) {
    if (fx == 0) hpf_enabled = !hpf_enabled;
    else if (fx == 1) lpf_enabled = !lpf_enabled;
    else if (fx == 2) delay_enabled = !delay_enabled;
    else if (fx == 3) reverb_enabled = !reverb_enabled;
}

int main(int argc, char* argv[]) {
    SetupCallbacks();
    scePowerSetClockFrequency(333, 333, 166);
    InitFadeLUT();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    texture_voice.pitch_mult = 1.0f;
    texture_voice.read_pos = 0.0f;
    texture_voice.state = ENV_IDLE;
    texture_voice.level = 0.0f;
    for (int i = 0; i < 4; i++) {
        voices[i].pitch_mult = 1.0f;
        voices[i].state = ENV_IDLE;
        voices[i].level = 0.0f;
        voices[i].read_pos = 0.0f;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) sceKernelSleepThread();
    DefinirPastaSamples(argc > 0 ? argv[0] : NULL);
    SDL_Window *window = SDL_CreateWindow("Ramona Granular", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    SDL_AudioSpec wanted_spec; SDL_zero(wanted_spec); wanted_spec.freq = 44100; wanted_spec.format = AUDIO_S16SYS; wanted_spec.channels = 2; wanted_spec.samples = 2048; wanted_spec.callback = GranularAudioCallback;
    SDL_OpenAudio(&wanted_spec, NULL);
    CarregarSample(0);

    SceCtrlData pad;
    int start_pr = 0, tri_pr = 0, select_pr = 0, cross_pr = 0;
    int left_pr = 0, right_pr = 0, up_pr = 0, down_pr = 0;
    int l_pr = 0, r_pr = 0, circle_pr = 0;
    float pitch_accum = 0.0f;

    motion_start_tick = SDL_GetTicks();
    last_seq_tick = SDL_GetTicks();

    while (!exit_request) {
        sceCtrlReadBufferPositive(&pad, 1);
        Uint32 current_tick = SDL_GetTicks();
        float dt = (current_tick - motion_start_tick) / 1000.0f; 
        motion_start_tick = current_tick; 

        if (sample_browser_open) {
            if (pad.Buttons & PSP_CTRL_CROSS) {
                if (!cross_pr) {
                    cross_pr = 1;
                    if (sample_dir_count > 0 && sample_browser_index >= 0 && sample_browser_index < sample_dir_count) {
                        char path[320];
                        CaminhoSample(path, sizeof(path), sample_dir_files[sample_browser_index]);
                        CarregarSampleArquivo(path);
                        sample_browser_open = 0;
                    }
                }
            } else { cross_pr = 0; }

            if (pad.Buttons & PSP_CTRL_CIRCLE) {
                if (!circle_pr) { circle_pr = 1; sample_browser_open = 0; }
            } else { circle_pr = 0; }

            if (pad.Buttons & PSP_CTRL_UP) {
                if (!up_pr) {
                    up_pr = 1;
                    if (sample_browser_index > 0) sample_browser_index--;
                }
            } else { up_pr = 0; }

            if (pad.Buttons & PSP_CTRL_DOWN) {
                if (!down_pr) {
                    down_pr = 1;
                    if (sample_browser_index + 1 < sample_dir_count) sample_browser_index++;
                }
            } else { down_pr = 0; }

            if (!(pad.Buttons & PSP_CTRL_START)) start_pr = 0;
            if (!(pad.Buttons & PSP_CTRL_SELECT)) select_pr = 0;
            if (!(pad.Buttons & PSP_CTRL_TRIANGLE)) tri_pr = 0;
            if (!(pad.Buttons & PSP_CTRL_LEFT)) left_pr = 0;
            if (!(pad.Buttons & PSP_CTRL_RIGHT)) right_pr = 0;
            if (!(pad.Buttons & PSP_CTRL_LTRIGGER)) l_pr = 0;
            if (!(pad.Buttons & PSP_CTRL_RTRIGGER)) r_pr = 0;
        } else {

        if (pad.Buttons & PSP_CTRL_START) {
            if (!start_pr) { start_pr = 1; is_audio_paused = !is_audio_paused; SDL_PauseAudio(is_audio_paused); }
        } else { start_pr = 0; }

        if (pad.Buttons & PSP_CTRL_SELECT) {
            if (!select_pr) {
                select_pr = 1;
                if (active_tab == 3) {
                    seq_res_idx++;
                    if (seq_res_idx > 4) seq_res_idx = 0;
                }
            }
        } else { select_pr = 0; }

        if (pad.Buttons & PSP_CTRL_CROSS) {
            if (!cross_pr) {
                cross_pr = 1;
                if (active_tab == 0 && selected_param_index >= 8) {
                    AlternarEfeito(selected_param_index - 8);
                } else if (active_tab == 0 && selected_param_index >= 4) {
                    if (selected_param_index == 4) scan_mode = 3; 
                    if (selected_param_index == 5) scan_mode = 1; 
                    if (selected_param_index == 6) scan_mode = 0; 
                    if (selected_param_index == 7) scan_mode = 2; 
                } else if (active_tab == 1) {
                    int fx = (selected_param_index >= 12) ? (selected_param_index - 12) : (selected_param_index / 3);
                    AlternarEfeito(fx);
                } else if (active_tab == 2) {
                    if (selected_param_index == 8) {
                        ListarSamplesPasta();
                        sample_browser_index = 0;
                        sample_browser_open = 1;
                    } else if (selected_param_index >= 0 && selected_param_index < 8) {
                        CarregarSample(selected_param_index);
                    }
                } else if (active_tab == 3 && selected_param_index >= 4 && selected_param_index < 20) {
                    int step_idx = selected_param_index - 4;
                    seq_active[step_idx] = !seq_active[step_idx];
                }
            }
        } else { cross_pr = 0; }

        if (pad.Buttons & PSP_CTRL_TRIANGLE) {
            if (!tri_pr) { tri_pr = 1; auto_motion_active = !auto_motion_active; }
        } else { tri_pr = 0; }

        if (pad.Buttons & PSP_CTRL_LEFT) { 
            if (!left_pr) { 
                left_pr = 1;
                if (active_tab == 0 && selected_param_index < 4) {
                    if (selected_param_index & 1) selected_param_index--;
                } else if (active_tab == 0 && selected_param_index < 8) {
                    if (selected_param_index > 4) selected_param_index--;
                } else if (active_tab == 0) {
                    if (selected_param_index == 8) selected_param_index = 11;
                    else selected_param_index--;
                } else if (active_tab == 2 && selected_param_index == 8) {
                } else {
                    selected_param_index--; 
                    int max_idx = (active_tab==3)?23:(active_tab==1?15:7); 
                    if (selected_param_index < 0) selected_param_index = max_idx; 
                }
            } 
        } else { left_pr = 0; }

        if (pad.Buttons & PSP_CTRL_RIGHT) { 
            if (!right_pr) { 
                right_pr = 1;
                if (active_tab == 0 && selected_param_index < 4) {
                    if ((selected_param_index & 1) == 0) selected_param_index++;
                } else if (active_tab == 0 && selected_param_index < 8) {
                    if (selected_param_index < 7) selected_param_index++;
                } else if (active_tab == 0) {
                    if (selected_param_index == 11) selected_param_index = 8;
                    else selected_param_index++;
                } else if (active_tab == 2 && selected_param_index == 8) {
                } else {
                    selected_param_index++; 
                    int max_idx = (active_tab==3)?23:(active_tab==1?15:7); 
                    if (selected_param_index > max_idx) selected_param_index = 0; 
                }
            } 
        } else { right_pr = 0; }

        if (pad.Buttons & PSP_CTRL_UP) { 
            if (!up_pr) { 
                up_pr = 1; 
                if (active_tab == 1) {
                    if (selected_param_index >= 12) {
                        if (selected_param_index == 12) selected_param_index = 1;
                        else if (selected_param_index == 13) selected_param_index = 4;
                        else if (selected_param_index == 14) selected_param_index = 7;
                        else selected_param_index = 10;
                    } else if (selected_param_index >= 6) {
                        selected_param_index -= 6;
                    }
                } else if (active_tab == 0) {
                    if (selected_param_index == 2 || selected_param_index == 3) selected_param_index -= 2;
                    else if (selected_param_index == 4 || selected_param_index == 5) selected_param_index = 2;
                    else if (selected_param_index == 6 || selected_param_index == 7) selected_param_index = 3;
                    else if (selected_param_index >= 8) selected_param_index -= 4;
                } else if (active_tab == 2) {
                    if (selected_param_index == 8) selected_param_index = 4;
                    else if (selected_param_index >= 4) selected_param_index -= 4;
                } else if (active_tab != 3 && selected_param_index >= 4) selected_param_index -= 4; 
                else if (active_tab == 3) {
                    if (selected_param_index >= 20) selected_param_index = 12 + (selected_param_index - 20) * 2;
                    else if (selected_param_index >= 12) selected_param_index -= 8;
                    else if (selected_param_index >= 4) selected_param_index = (selected_param_index - 4) / 2;
                }
            } 
        } else { up_pr = 0; }

        if (pad.Buttons & PSP_CTRL_DOWN) { 
            if (!down_pr) { 
                down_pr = 1; 
                if (active_tab == 1) {
                    if (selected_param_index < 6) selected_param_index += 6;
                    else if (selected_param_index < 9) selected_param_index = 14;
                    else if (selected_param_index < 12) selected_param_index = 15;
                } else if (active_tab == 0) {
                    if (selected_param_index <= 1) selected_param_index += 2;
                    else if (selected_param_index == 2) selected_param_index = 4;
                    else if (selected_param_index == 3) selected_param_index = 6;
                    else if (selected_param_index >= 4 && selected_param_index <= 7) selected_param_index += 4;
                } else if (active_tab == 2) {
                    if (selected_param_index < 4) selected_param_index += 4;
                    else if (selected_param_index < 8) selected_param_index = 8;
                } else if (active_tab != 3 && selected_param_index < 4) selected_param_index += 4; 
                else if (active_tab == 3) {
                    if (selected_param_index < 4) selected_param_index = 4 + (selected_param_index * 2);
                    else if (selected_param_index < 12) selected_param_index += 8;
                    else if (selected_param_index < 20) selected_param_index = 20 + ((selected_param_index - 12) / 2);
                }
            } 
        } else { down_pr = 0; }

        if (pad.Ly < 80 || pad.Ly > 170) {
            float delta = (pad.Ly < 80) ? 0.02f : -0.02f;
            if (active_tab == 3 && selected_param_index < 4) delta = (pad.Ly < 80) ? 0.05f : -0.05f;

            if (active_tab == 0 && selected_param_index < 4) {
                switch (selected_param_index) {
                    case 0: grain_width += delta * 0.02f; if (grain_width < 0.0005f) grain_width = 0.0005f; if (grain_width > 0.20f) grain_width = 0.20f; break;
                    case 1: grain_center += delta * 0.05f; if (grain_center < 0.00f) grain_center = 0.00f; if (grain_center > 1.00f) grain_center = 1.00f; break;
                    case 2: bpm += (delta > 0) ? 1 : -1; if (bpm < 1) bpm = 1; if (bpm > 200) bpm = 200; break;
                    case 3: playback_speed += delta * 0.5f; if (playback_speed < 0.2f) playback_speed = 0.2f; if (playback_speed > 2.5f) playback_speed = 2.5f; break;
                }
            } else if (active_tab == 1) {
                int knob = selected_param_index;
                if (knob == 12) knob = 0;
                else if (knob == 13) knob = 3;
                else if (knob == 14) knob = 6;
                else if (knob == 15) knob = 9;
                switch (knob) {
                    case 0: hpf_cutoff += delta; if (hpf_cutoff < 0.01f) hpf_cutoff = 0.01f; if (hpf_cutoff > 1.0f) hpf_cutoff = 1.0f; break;
                    case 1: hpf_vol += delta * 2.0f; if (hpf_vol < 0.0f) hpf_vol = 0.0f; if (hpf_vol > 2.0f) hpf_vol = 2.0f; break;
                    case 2: hpf_pan += delta; if (hpf_pan < 0.0f) hpf_pan = 0.0f; if (hpf_pan > 1.0f) hpf_pan = 1.0f; break;
                    case 3: lpf_cutoff += delta; if (lpf_cutoff < 0.01f) lpf_cutoff = 0.01f; if (lpf_cutoff > 1.0f) lpf_cutoff = 1.0f; break;
                    case 4: lpf_vol += delta * 2.0f; if (lpf_vol < 0.0f) lpf_vol = 0.0f; if (lpf_vol > 2.0f) lpf_vol = 2.0f; break;
                    case 5: lpf_pan += delta; if (lpf_pan < 0.0f) lpf_pan = 0.0f; if (lpf_pan > 1.0f) lpf_pan = 1.0f; break;
                    case 6: delay_time_ms += delta * 700.0f; if (delay_time_ms < 60.0f) delay_time_ms = 60.0f; if (delay_time_ms > 1500.0f) delay_time_ms = 1500.0f; break;
                    case 7: delay_feedback += delta; if (delay_feedback < 0.0f) delay_feedback = 0.0f; if (delay_feedback > 0.99f) delay_feedback = 0.99f; break;
                    case 8: delay_wet += delta; if (delay_wet < 0.0f) delay_wet = 0.0f; if (delay_wet > 1.0f) delay_wet = 1.0f; break;
                    case 9: reverb_time += delta; if (reverb_time < 0.10f) reverb_time = 0.10f; if (reverb_time > 0.95f) reverb_time = 0.95f; break;
                    case 10: reverb_damp += delta; if (reverb_damp < 0.0f) reverb_damp = 0.0f; if (reverb_damp > 0.90f) reverb_damp = 0.90f; break;
                    case 11: reverb_wet += delta; if (reverb_wet < 0.0f) reverb_wet = 0.0f; if (reverb_wet > 1.0f) reverb_wet = 1.0f; break;
                }
            } else if (active_tab == 3) {
                if (selected_param_index < 4) {
                    switch (selected_param_index) {
                        case 0: adsr_attack += delta; if (adsr_attack < 0.0f) adsr_attack = 0.0f; if (adsr_attack > 1.0f) adsr_attack = 1.0f; break;
                        case 1: adsr_decay += delta; if (adsr_decay < 0.0f) adsr_decay = 0.0f; if (adsr_decay > 1.0f) adsr_decay = 1.0f; break;
                        case 2: adsr_sustain += delta; if (adsr_sustain < 0.0f) adsr_sustain = 0.0f; if (adsr_sustain > 1.0f) adsr_sustain = 1.0f; break;
                        case 3: adsr_release += delta; if (adsr_release < 0.0f) adsr_release = 0.0f; if (adsr_release > 1.0f) adsr_release = 1.0f; break;
                    }
                } else if (selected_param_index >= 20) {
                    switch (selected_param_index) {
                        case 20: grain_width += delta * 0.02f; if (grain_width < 0.0005f) grain_width = 0.0005f; if (grain_width > 0.20f) grain_width = 0.20f; break;
                        case 21: grain_center += delta * 0.05f; if (grain_center < 0.00f) grain_center = 0.00f; if (grain_center > 1.00f) grain_center = 1.00f; break;
                        case 22: bpm += (delta > 0) ? 1 : -1; if (bpm < 1) bpm = 1; if (bpm > 200) bpm = 200; break;
                        case 23: playback_speed += delta * 0.5f; if (playback_speed < 0.2f) playback_speed = 0.2f; if (playback_speed > 2.5f) playback_speed = 2.5f; break;
                    }
                } else {
                    float delta_pitch = (pad.Ly < 80) ? 1.0f : -1.0f;
                    pitch_accum += delta_pitch;
                    if (pitch_accum > 8.0f) {
                        seq_pitch[selected_param_index - 4]++;
                        if (seq_pitch[selected_param_index - 4] > 24) seq_pitch[selected_param_index - 4] = 24;
                        pitch_accum = 0.0f;
                    } else if (pitch_accum < -8.0f) {
                        seq_pitch[selected_param_index - 4]--;
                        if (seq_pitch[selected_param_index - 4] < -24) seq_pitch[selected_param_index - 4] = -24;
                        pitch_accum = 0.0f;
                    }
                }
            }
        } else { pitch_accum = 0.0f; }

        if (pad.Buttons & PSP_CTRL_LTRIGGER) {
            if (!l_pr) { l_pr = 1; MudarAba(-1); }
        } else { l_pr = 0; }
        if (pad.Buttons & PSP_CTRL_RTRIGGER) {
            if (!r_pr) { r_pr = 1; MudarAba(1); }
        } else { r_pr = 0; }

        }

        if (active_tab == 3 && !is_audio_paused) {
            Uint32 step_time_ms = (Uint32)((60000.0f / (float)bpm) * res_mults[seq_res_idx]); 
            Uint32 elapsed = current_tick - last_seq_tick;

            if (elapsed >= step_time_ms) {
                last_seq_tick = current_tick;
                current_seq_step = (current_seq_step + 1) % 16;
                
                if (seq_active[current_seq_step]) {
                    int semis = seq_pitch[current_seq_step] + 24;
                    if (semis < 0) semis = 0;
                    if (semis > 48) semis = 48;
                    float pmul = pitch_lut[semis];
                    int v = (last_triggered_voice + 1) & 3;
                    SDL_LockAudio();
                    last_triggered_voice = v;
                    voices[v].pitch_mult = pmul;
                    NoteOn(v);
                    SDL_UnlockAudio();
                    seq_note_on = 1;
                }
            } else if (seq_note_on && elapsed > (step_time_ms * 0.75f)) {
                SDL_LockAudio();
                NoteOff(last_triggered_voice);
                SDL_UnlockAudio();
                seq_note_on = 0;
            }
        } 
        
        float scan_step = 0.0f;
        if (auto_motion_active && !is_audio_paused) {
            float base_speed = 0.15f; 
            float move_amount = base_speed * dt * ((float)bpm / 120.0f); 

            if (scan_mode == 0) scan_step = move_amount;
            else if (scan_mode == 1) scan_step = -move_amount;
            else if (scan_mode == 2) scan_step = move_amount * 2.0f;
            else if (scan_mode == 3) scan_step = -move_amount * 2.0f;

            grain_center += scan_step;
            if (grain_center > 1.0f) grain_center -= 1.0f;
            if (grain_center < 0.0f) grain_center += 1.0f;
        }
        tape_angle += scan_step * 48.0f;
        tape_angle = fmodf(tape_angle, 6.2831853f);
        if (tape_angle < 0.0f) tape_angle += 6.2831853f;

        RenderizarInterfaceFORM(renderer);
        SDL_Delay(33); 
    }

    SDL_CloseAudio();
    LiberarSample();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    sceKernelExitGame();
    return 0;
}
