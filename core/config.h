/* config.ini parsing (ux0:data/VitaJPOverlay/config.ini). */
#ifndef VJO_CONFIG_H
#define VJO_CONFIG_H

#include <stddef.h>

#include "../include/vjo_api.h"

enum { VJO_OCR_AUTO = 0, VJO_OCR_ON_PRESS = 1 };
/* ocr_backend. VJO_OCR_VOCR and VJO_OCR_VOCR2 share bee-san/vita-vn-ocr on
 * the Vita CPU (VJO_WITH_VOCR); VJO_OCR_NCNN is for host experiments only.
 * Values are persisted by the settings app; keep existing IDs unchanged.
 * 3 belonged to the removed Meiki trial and stays unused. */
enum { VJO_OCR_LENS = 0, VJO_OCR_NCNN = 1, VJO_OCR_VOCR = 2, VJO_OCR_VOCR2 = 4 };
enum { VJO_SOURCE_OCR = 0, VJO_SOURCE_AUTO = 1, VJO_SOURCE_HOOKS = 2 };
enum { VJO_HIGHLIGHT_WORD = 0, VJO_HIGHLIGHT_CHARACTER = 1 };
enum { VJO_DICT_JPDB = 0, VJO_DICT_JITEN = 1, VJO_DICT_HACHIDORI = 2, VJO_DICT_LOCAL = 3, VJO_DICT_COUNT };

/* Data put on an Anki note, each into the field named by its
 * anki_field_* setting ("" = not added). */
enum {
    VJO_ANKI_WORD = 0,   /* spelling */
    VJO_ANKI_READING,    /* kana */
    VJO_ANKI_FURIGANA,   /* 言葉[ことば] */
    VJO_ANKI_DEFINITION, /* <ol><li>meaning</li>...</ol> */
    VJO_ANKI_SENTENCE,   /* the recognized text, the word in <b> */
    VJO_ANKI_PICTURE,    /* screenshot */
    VJO_ANKI_FREQUENCY,  /* frequency rank */
    VJO_ANKI_AUDIO,      /* the word's pronunciation, downloaded by Anki */
    VJO_ANKI_SENTENCE_FURIGANA, /* the sentence in 漢字[かんじ] syntax, the word in <b> */
    VJO_ANKI_PITCH_POSITION,    /* [2], as Yomitan writes it */
    VJO_ANKI_PITCH_CATEGORIES,  /* heiban,kifuku */
    VJO_ANKI_FREQUENCY_LIST,    /* <ul><li>Jiten: 384</li></ul> */
    VJO_ANKI_MISC_INFO,         /* the game and the date */
    VJO_ANKI_FIELD_COUNT
};

#define VJO_CONFIG_MAX_WARNINGS 4
/* The Shell reads the text plus NUL into its existing 24 KiB scratch arena.
 * Settings must never publish a file larger than that reader can load. */
#define VJO_CONFIG_TEXT_MAX (24 * 1024 - 1)

typedef struct {
    int dictionary;           /* VJO_DICT_* */
    char api_key[VJO_DICT_COUNT][128]; /* indexed by VJO_DICT_* */
    char hachidori_host[64];   /* required host[:port], default port 19633 */
    char local_dictionary_dir[160];
    char local_dictionaries[512]; /* comma-separated relative .vjdict filenames */
    int local_dictionary_results; /* 1..8: dictionaries paged with Up/Down per selected word */
    int non_japanese_filter;  /* VJO_FILTER_NONE | VJO_FILTER_LINES */
    int japanese_only_selection; /* 0/1: local cursor skips non-Japanese characters */
    int highlight_mode;       /* VJO_HIGHLIGHT_*: match span or selected character */
    int font_size_ja;         /* 8..40: header, headwords, readings */
    int font_size_en;         /* 8..40: meanings, rank, messages */
    int toggle_button;        /* enum VjoTrigger */
    int subtitle_button;      /* enum VjoTrigger, != toggle_button */
    int ocr_mode;             /* VJO_OCR_* */
    int ocr_backend;          /* VJO_OCR_LENS | VJO_OCR_VOCR | VJO_OCR_VOCR2 | VJO_OCR_NCNN */
    int text_source;          /* compatibility setting, always screenshot OCR in this build */
    char ocr_model_dir[160];
    char vocr_model[64];      /* old VOCR weights in ocr_model_dir; VOCR2 uses its build pin */
    /* Smaller weights run instead when the game's free memory has no room
     * for vocr_model (the game worker decides per job); "" = none. */
    char vocr_fallback_model[64];
    /* Local OCR (vocr): the region's rows hold still this long before a job
     * reads its lines (0 = off), waiting at most ocr_settle_max_ms per job; and
     * found lines without text-like ink are skipped (core/vocr_ocr.h). */
    int ocr_settle_ms;        /* 0..1000 */
    int ocr_settle_max_ms;    /* 100..10000 */
    int ocr_skip_blank_lines; /* 0/1 */
    /* Local OCR (vocr, ocr-speed): threads that recognize a region's lines at
     * once (1 = one line after another), and the ARM clock at 444 MHz while a
     * job recognizes (off by default; core/vocr_ocr.h VjoVocrPool, busy). */
    int ocr_threads;          /* 1..3 */
    int ocr_boost_clock;      /* 0/1 */
    char log_host[64];        /* "" = UDP log off */
    int log_file;             /* 0/1 */
    int anki_mining;          /* 0/1: × saves cards to the offline queue, △ sends it */
    char anki_host[64];       /* see vjo_anki_endpoint */
    char anki_deck[128];
    char anki_note_type[64];
    char anki_tags[128];      /* separated by spaces */
    char anki_field[VJO_ANKI_FIELD_COUNT][64]; /* note field names, "" = skip */
    char anki_audio_url[256]; /* "" = no word audio; see vjo_anki_audio_endpoint */
    char warnings[VJO_CONFIG_MAX_WARNINGS][96];
    int n_warnings;
} VjoConfig;

void vjo_config_defaults(VjoConfig *c);
/* Parses INI text over the defaults; invalid values keep the default and
 * add a warning. */
void vjo_config_parse(VjoConfig *c, const char *text, size_t len);
/* Default file written on first run (with comments). */
const char *vjo_config_default_text(void);

/* Changes only the app's four OCR/lookup preferences, retaining every other INI
 * line verbatim. Duplicate owned keys are collapsed. Input/output must not
 * overlap. Returns output bytes (not including NUL), or -1 for invalid
 * choices, embedded NUL, or insufficient capacity. */
int vjo_config_patch_preferences(const char *text, size_t len, int backend,
                                 int japanese_only, int highlight_mode,
                                 int dictionary_results, char *out, size_t cap);

const char *vjo_trigger_name(int trigger);

/* A dictionary's config-level names (its network side: client.h). */
typedef struct {
    const char *id;          /* config.ini dictionary / CLI --dict value: "jpdb" | "jiten" */
    const char *name;        /* "jpdb.io" | "jiten.moe" */
    const char *key_setting; /* config.ini key of its API key, NULL for relay */
    const char *key_env;     /* host CLI key environment variable, NULL for relay */
} VjoDictInfo;

/* Info for a VJO_DICT_* value (jpdb for an out-of-range one). */
const VjoDictInfo *vjo_dict_info(int dictionary);
const char *vjo_dict_name(int dictionary); /* "jpdb.io" | "jiten.moe" */
/* VJO_DICT_* for an id ("jpdb" | "jiten", any case), or -1. */
int vjo_dict_find(const char *id);
/* API key of the selected dictionary ("" if unset). */
const char *vjo_config_api_key(const VjoConfig *c);
/* Local lookup is always schedulable (worker checks files); relay/cloud need their settings. */
int vjo_config_dict_ready(const VjoConfig *c);

/* anki_host (or the saved anki_host.txt), where △ sends the queue: "" =
 * nowhere (× still saves cards), "auto" = search the network, or
 * host[:port]. For VJO_ANKI_MANUAL, host (cap bytes) and port
 * are set. Returns -1 for an invalid value. */
#define VJO_ANKI_PORT 8765
enum { VJO_ANKI_OFF = 0, VJO_ANKI_AUTO = 1, VJO_ANKI_MANUAL = 2 };
int vjo_anki_endpoint(const char *setting, char *host, size_t cap, int *port);

/* Relay requires a hostname/IPv4 address, optionally :port (not a URL or auto).
 * Returns 0 on success, -1 if unset or invalid. */
#define VJO_HACHIDORI_PORT 19633
int vjo_hachidori_endpoint(const char *setting, char *host, size_t cap, int *port);

/* anki_audio_url: a Yomitan custom audio source,
 * http(s)://host[:port][/path][?query] with {term} (and usually {reading})
 * in it. Sets host (VJO_HOST_MAX bytes), port, tls and the path template
 * (in url, from the '/' or '?' after the host; "" for none). Returns 0, or
 * -1 for an invalid or empty value. */
#define VJO_HOST_MAX 128
int vjo_anki_audio_endpoint(const char *url, char *host, int *port, int *tls, const char **path);

#endif
