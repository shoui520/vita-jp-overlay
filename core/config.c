#include "config.h"

#include <stddef.h>
#include <string.h>

#include "port.h"
#include "local_dict.h"
#include "textfilter.h"
#include "utf.h"

static const char *const trigger_names[VJO_TRIGGER_COUNT] = {
    "select", "start", "l+r", "select+l", "select+r", "rear_double_tap",
};

const char *vjo_trigger_name(int trigger)
{
    if (trigger < 0 || trigger >= VJO_TRIGGER_COUNT)
        return "?";
    return trigger_names[trigger];
}

static const VjoDictInfo dicts[VJO_DICT_COUNT] = {
    [VJO_DICT_JPDB] = {"jpdb", "jpdb.io", "jpdb_api_key", "VJO_JPDB_KEY"},
    [VJO_DICT_JITEN] = {"jiten", "jiten.moe", "jiten_api_key", "VJO_JITEN_KEY"},
    [VJO_DICT_HACHIDORI] = {"hachidori", "Hachidori relay", NULL, NULL},
    [VJO_DICT_LOCAL] = {"local", "Local dictionaries", NULL, NULL},
};

const VjoDictInfo *vjo_dict_info(int dictionary)
{
    return &dicts[dictionary >= 0 && dictionary < VJO_DICT_COUNT ? dictionary : VJO_DICT_JPDB];
}

const char *vjo_dict_name(int dictionary)
{
    return vjo_dict_info(dictionary)->name;
}

const char *vjo_config_api_key(const VjoConfig *c)
{
    return c->dictionary == VJO_DICT_HACHIDORI || c->dictionary == VJO_DICT_LOCAL ? "" : c->api_key[c->dictionary];
}

int vjo_config_dict_ready(const VjoConfig *c)
{
    /* File availability is checked by the worker, without blocking the UI thread. */
    if (c->dictionary == VJO_DICT_LOCAL)
        return 1;
    if (c->dictionary == VJO_DICT_HACHIDORI) {
        char host[64];
        int port;
        return vjo_hachidori_endpoint(c->hachidori_host, host, sizeof(host), &port) == 0;
    }
    return vjo_config_api_key(c)[0] != '\0';
}

static const struct {
    const char *key, *def; /* defaults: the Lapis note type */
} anki_fields[VJO_ANKI_FIELD_COUNT] = {
    [VJO_ANKI_WORD] = {"anki_field_word", "Expression"},
    [VJO_ANKI_READING] = {"anki_field_reading", "ExpressionReading"},
    [VJO_ANKI_FURIGANA] = {"anki_field_furigana", "ExpressionFurigana"},
    [VJO_ANKI_DEFINITION] = {"anki_field_definition", "MainDefinition"},
    [VJO_ANKI_SENTENCE] = {"anki_field_sentence", "Sentence"},
    [VJO_ANKI_PICTURE] = {"anki_field_picture", "Picture"},
    [VJO_ANKI_FREQUENCY] = {"anki_field_frequency", "FreqSort"},
    [VJO_ANKI_AUDIO] = {"anki_field_audio", "ExpressionAudio"},
    [VJO_ANKI_SENTENCE_FURIGANA] = {"anki_field_sentence_furigana", "SentenceFurigana"},
    [VJO_ANKI_PITCH_POSITION] = {"anki_field_pitch_position", "PitchPosition"},
    [VJO_ANKI_PITCH_CATEGORIES] = {"anki_field_pitch_categories", "PitchCategories"},
    [VJO_ANKI_FREQUENCY_LIST] = {"anki_field_frequency_list", "Frequency"},
    [VJO_ANKI_MISC_INFO] = {"anki_field_misc_info", "MiscInfo"},
};

void vjo_config_defaults(VjoConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->dictionary = VJO_DICT_LOCAL;
    vjo_snprintf(c->local_dictionary_dir, sizeof(c->local_dictionary_dir), "ux0:data/VitaJPOverlay/dictionaries");
    vjo_snprintf(c->local_dictionaries, sizeof(c->local_dictionaries), "plaintext.vjdict");
    c->local_dictionary_results = 1;
    c->non_japanese_filter = VJO_FILTER_LINES;
    c->japanese_only_selection = 1;
    c->highlight_mode = VJO_HIGHLIGHT_WORD;
    c->font_size_ja = 18;
    c->font_size_en = 14;
    c->toggle_button = VJO_TRIGGER_L_R;
    c->subtitle_button = VJO_TRIGGER_SELECT_R;
    c->ocr_mode = VJO_OCR_AUTO;
    c->ocr_backend = VJO_OCR_VOCR;
    c->text_source = VJO_SOURCE_OCR;
    vjo_snprintf(c->ocr_model_dir, sizeof(c->ocr_model_dir), "ux0:data/VitaJPOverlay/ocr");
    /* VOCR3-H15 is the default where the game's memory allows; VOCR3-FL10 runs otherwise. */
    vjo_snprintf(c->vocr_model, sizeof(c->vocr_model), "VOCR3_H15_w8.vocr");
    vjo_snprintf(c->vocr_fallback_model, sizeof(c->vocr_fallback_model), "VOCR3_FL10_w8.vocr");
    c->ocr_settle_ms = 150;
    c->ocr_settle_max_ms = 2000;
    c->ocr_skip_blank_lines = 0;
    c->ocr_threads = 3;
    c->ocr_boost_clock = 0;
    c->log_file = 1; /* log.txt is on unless log_file = off */
    vjo_snprintf(c->anki_deck, sizeof(c->anki_deck), "Default");
    vjo_snprintf(c->anki_note_type, sizeof(c->anki_note_type), "Lapis");
    vjo_snprintf(c->anki_tags, sizeof(c->anki_tags), "vita-jp-overlay");
    for (int i = 0; i < VJO_ANKI_FIELD_COUNT; i++)
        vjo_snprintf(c->anki_field[i], sizeof(c->anki_field[i]), "%s", anki_fields[i].def);
}

const char *vjo_config_default_text(void)
{
    return "; Vita JP Overlay settings. Changes apply the next time the overlay opens.\n"
           "\n"
           "; Dictionary used for word lookups: hachidori | local | jpdb | jiten\n"
           "dictionary = local\n"
           "\n"
           "; Hachidori relay computer: host[:port], default port 19633 (no API key)\n"
           "; Plain HTTP: use only on a trusted LAN. Anki and a sharing Hachidori host\n"
           "; with dictionaries must be running; enable Sharing > Also with my other computers.\n"
           "hachidori_host =\n"
           "\n"
           "; Local Yomitan dictionaries: convert on a computer, then copy .vjdict files here.\n"
           "local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries\n"
           "; Up to 8 filenames, separated by commas, in priority order.\n"
           "local_dictionaries = plaintext.vjdict\n"
           "; Dictionaries shown per selected word, 1-8: the best match first, then each\n"
           "; other dictionary's entry in the order above. Up/Down pages; one is in memory.\n"
           "local_dictionary_results = 1\n"
           "\n"
           "; jpdb.io API key (needed for dictionary = jpdb): jpdb.io -> Settings -> API key\n"
           "jpdb_api_key =\n"
           "\n"
           "; jiten.moe API key (needed for dictionary = jiten): jiten.moe -> Settings -> API key\n"
           "jiten_api_key =\n"
           "\n"
           "; Drop OCR lines without Japanese characters: none | lines\n"
           "non_japanese_filter = lines\n"
           "\n"
           "; Left/right character lookup skips Latin text, numbers and punctuation: on | off.\n"
           "; Select Japanese kana and Han characters; this does not tokenize words.\n"
           "japanese_only_selection = on\n"
           "\n"
           "; Highlight the dictionary match or only the cursor character: word | character.\n"
           "; Word skips the matched source span; character steps one character.\n"
           "highlight_mode = word\n"
           "\n"
           "; Text size, 8-40: Japanese (sentence, words, readings) and English (meanings, messages)\n"
           "font_size_ja = 18\n"
           "font_size_en = 14\n"
           "\n"
           "; Opens/closes the overlay: a button combination, or a double tap on the rear\n"
           "; touchpad (buttons are hidden from the game; rear taps are not):\n"
           "; l+r | select | start | select+l | select+r | rear_double_tap\n"
           "toggle_button = l+r\n"
           "\n"
           "; Turns subtitles on/off: the recognized text in a strip at the top of the\n"
           "; screen while the game runs, with furigana once its words are looked up\n"
           "; (jpdb or jiten only). Same values as toggle_button, but not the same one.\n"
           "; When one is part of the other (select and select+r), the shorter one acts\n"
           "; on release.\n"
           "subtitle_button = select+r\n"
           "\n"
           "; Text is read from screenshots using OCR.\n"
           "text_source = ocr\n"
           "\n"
           "; OCR: lens (online) | vocr (on the Vita: vita-vn-ocr, needs a build with it\n"
           "; and its model file; see docs/vita-vn-ocr.md) | ncnn (PP-OCRv5, host tests only)\n"
           "; Local OCR never falls back to uploading the screen.\n"
           "ocr_backend = vocr\n"
           "ocr_model_dir = ux0:data/VitaJPOverlay/ocr\n"
           "; vocr weights in ocr_model_dir: VOCR3_H15_w8.vocr (default) | VOCR3_FL10_w8.vocr\n"
           "; (smaller) | v0.2.0's H15_w8.vocr, FL10_w8.vocr | F20_w8.vocr (larger)\n"
           "vocr_model = VOCR3_H15_w8.vocr\n"
           "; Smaller weights used automatically for a job when the game has too little free\n"
           "; memory for vocr_model: VOCR3_FL10_w8.vocr | FL10_w8.vocr | none\n"
           "vocr_fallback_model = VOCR3_FL10_w8.vocr\n"
           "; vocr: read the region once its rows have held still this long, in ms (0-1000,\n"
           "; 0 = off; sent in 10 ms steps), so text that types out is read whole; waiting at\n"
           "; most ocr_settle_max_ms (100-10000, in 50 ms steps) per recognition, then it reads\n"
           "; anyway (set it above ocr_settle_ms, or every wait runs out before the text holds still).\n"
           "ocr_settle_ms = 150\n"
           "ocr_settle_max_ms = 2000\n"
           "; vocr: skip found lines with no ink (an even area, a smooth gradient): on | off\n"
           "; (off: it finds next to none at native resolution; see docs/vita-vn-ocr.md)\n"
           "ocr_skip_blank_lines = off\n"
           "; vocr: lines recognized at once (1-3), and 444 MHz while recognizing (on | off)\n"
           "ocr_threads = 3\n"
           "ocr_boost_clock = off\n"
           "\n"
           "; auto: recognize text in the background when the region changes (lens, vocr)\n"
           "; on_press: recognize only when the overlay is opened\n"
           "ocr_mode = auto\n"
           "\n"
           "; Debugging: IP of a computer running tools/udp_log_listener.py (empty = off)\n"
           "log_host =\n"
           "\n"
           "; Debugging: write ux0:data/VitaJPOverlay/log.txt (max 1 MB + one rotated file): on | off\n"
           "log_file = on\n"
           "\n"
           "; ---- Anki card mining (optional, see docs/anki.md) ----\n"
           "; on: × in the overlay saves the shown word as a card in a queue on the memory\n"
           "; card (ux0:data/VitaJPOverlay/anki_queue), even with Wi-Fi off or Anki closed.\n"
           "; △ sends the queue to AnkiConnect on a computer on your local network (plain\n"
           "; HTTP). No AnkiWeb sync. off (the default): × and △ do nothing.\n"
           "anki_mining = off\n"
           "\n"
           "; The computer running Anki with AnkiConnect, for △: its IP address[:port]\n"
           "; (default 8765), or auto = search the local network once. Empty: cards stay queued.\n"
           "anki_host =\n"
           "\n"
           "; Deck (created if missing), note type, and tags separated by spaces. These and\n"
           "; the fields below are read when △ sends, so queued cards use the latest ones.\n"
           "anki_deck = Default\n"
           "anki_note_type = Lapis\n"
           "anki_tags = vita-jp-overlay\n"
           "\n"
           "; The note type's field for each item; empty = leave it out\n"
           "anki_field_word = Expression\n"
           "anki_field_reading = ExpressionReading\n"
           "anki_field_furigana = ExpressionFurigana\n"
           "anki_field_definition = MainDefinition\n"
           "anki_field_sentence = Sentence\n"
           "anki_field_picture = Picture\n"
           "anki_field_frequency = FreqSort\n"
           "anki_field_audio = ExpressionAudio\n"
           "anki_field_sentence_furigana = SentenceFurigana\n"
           "anki_field_pitch_position = PitchPosition\n"
           "anki_field_pitch_categories = PitchCategories\n"
           "anki_field_frequency_list = Frequency\n"
           "anki_field_misc_info = MiscInfo\n"
           "\n"
           "; Word audio, looked up when △ sends: a Yomitan audio source URL with {term}\n"
           "; and {reading} (empty = off)\n"
           "anki_audio_url =\n";
}

/* host[:port], the n bytes at s: a name or IP address, and a port
 * (default_port when there is none). Returns 0, or -1 when invalid. */
static int host_port(const char *s, size_t n, char *host, size_t cap, int default_port, int *port)
{
    const char *colon = memchr(s, ':', n);
    size_t hn = colon ? (size_t)(colon - s) : n;
    int v = 0;
    if (hn == 0 || hn >= cap)
        return -1;
    for (size_t i = 0; i < hn; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '.' ||
              ch == '-'))
            return -1;
    }
    if (colon) {
        const char *p = colon + 1, *end = s + n;
        if (p == end)
            return -1;
        for (; p < end; p++) {
            if (*p < '0' || *p > '9')
                return -1;
            v = v * 10 + (*p - '0');
            if (v > 65535)
                return -1;
        }
        if (v == 0)
            return -1;
    }
    *port = colon ? v : default_port;
    memcpy(host, s, hn);
    host[hn] = '\0';
    return 0;
}

int vjo_anki_endpoint(const char *setting, char *host, size_t cap, int *port)
{
    if (!*setting)
        return VJO_ANKI_OFF;
    if (vjo_ieq(setting, "auto"))
        return VJO_ANKI_AUTO;
    if (host_port(setting, strlen(setting), host, cap, VJO_ANKI_PORT, port) < 0)
        return -1;
    return VJO_ANKI_MANUAL;
}

int vjo_hachidori_endpoint(const char *setting, char *host, size_t cap, int *port)
{
    int rc;
    size_t label = 0;
    if (!setting || vjo_ieq(setting, "auto"))
        return -1;
    rc = vjo_anki_endpoint(setting, host, cap, port);
    if (rc != VJO_ANKI_MANUAL)
        return -1;
    /* DNS labels must not be empty or start/end with a hyphen. */
    for (size_t i = 0; ; i++) {
        char ch = host[i];
        if (ch == '.' || ch == '\0') {
            if (!label || host[i - 1] == '-')
                return -1;
            label = 0;
            if (!ch)
                break;
        } else {
            if (!label && ch == '-')
                return -1;
            label++;
        }
    }
    if (!strchr(setting, ':'))
        *port = VJO_HACHIDORI_PORT;
    return 0;
}

int vjo_anki_audio_endpoint(const char *url, char *host, int *port, int *tls, const char **path)
{
    size_t n = 0;
    int term = 0;
    if (!strncmp(url, "https://", 8)) {
        *tls = 1;
        url += 8;
    } else if (!strncmp(url, "http://", 7)) {
        *tls = 0;
        url += 7;
    } else {
        return -1;
    }
    while (url[n] && url[n] != '/' && url[n] != '?')
        n++;
    for (const char *s = url + n; *s; s++) {
        if ((unsigned char)*s <= ' ' || (unsigned char)*s >= 0x7F || *s == '#')
            return -1; /* not one URL (a space, a fragment), or not ASCII */
        term |= !strncmp(s, "{term}", 6);
    }
    if (!term || host_port(url, n, host, VJO_HOST_MAX, *tls ? 443 : 80, port) < 0)
        return -1;
    *path = url + n;
    return 0;
}

static void warn(VjoConfig *c, const char *fmt, const char *a, const char *b)
{
    if (c->n_warnings >= VJO_CONFIG_MAX_WARNINGS)
        return;
    vjo_snprintf(c->warnings[c->n_warnings++], sizeof(c->warnings[0]), fmt, a, b);
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r';
}

static int parse_int(const char *s, int *out)
{
    long v = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000)
            return -1;
        v = v * 10 + (*s - '0');
    }
    *out = (int)v;
    return 0;
}

int vjo_dict_find(const char *id)
{
    for (int i = 0; i < VJO_DICT_COUNT; i++)
        if (vjo_ieq(id, dicts[i].id))
            return i;
    return -1;
}

/* VJO_DICT_* whose API key `key` sets, or -1. */
static int api_key_dict(const char *key)
{
    if (vjo_ieq(key, "api_key")) /* the old name of jpdb_api_key (tools/migrate_config.py RENAMED) */
        return VJO_DICT_JPDB;
    for (int i = 0; i < VJO_DICT_COUNT; i++)
        if (dicts[i].key_setting && vjo_ieq(key, dicts[i].key_setting))
            return i;
    return -1;
}

static int valid_hachidori_host(const char *v)
{
    char host[64];
    int port;
    return !*v || vjo_hachidori_endpoint(v, host, sizeof(host), &port) == 0;
}

static int valid_anki_host(const char *v)
{
    char host[64];
    int port;
    return vjo_anki_endpoint(v, host, sizeof(host), &port) >= 0;
}

static int valid_audio_url(const char *v)
{
    char host[VJO_HOST_MAX];
    const char *path;
    int port, tls;
    return !*v || vjo_anki_audio_endpoint(v, host, &port, &tls, &path) == 0;
}

/* vocr_model: a plain file name in ocr_model_dir, ending in .vocr (which
 * models a build accepts is checked when it loads one). */
static int valid_vocr_model(const char *v)
{
    size_t n = strlen(v);
    if (n <= 5 || n >= sizeof(((VjoConfig *)0)->vocr_model) || strcmp(v + n - 5, ".vocr"))
        return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = v[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              ch == '.' || ch == '_' || ch == '-'))
            return 0;
    }
    return 1;
}

/* Plain string settings. Deck, note type, tags, field names and the
 * audio URL may contain ';' and '#' (raw: no inline comment). */
typedef struct {
    const char *key;
    size_t offset, size;
    int required; /* an empty value keeps the default, with a warning */
    int raw;
    int (*valid)(const char *v); /* NULL = anything; an invalid value turns the setting off ("") */
    const char *expected;        /* the warning's description of a valid value */
    int quiet;                   /* the warning leaves out the value (long, or secret) */
} StrSetting;

#define STR_SETTING(name, required, raw) \
    {#name, offsetof(VjoConfig, name), sizeof(((VjoConfig *)0)->name), required, raw, NULL, NULL, 0}
#define CHECKED_SETTING(name, raw, valid, expected, quiet) \
    {#name, offsetof(VjoConfig, name), sizeof(((VjoConfig *)0)->name), 0, raw, valid, expected, quiet}
static const StrSetting str_settings[] = {
    CHECKED_SETTING(hachidori_host, 0, valid_hachidori_host, "host[:port], not a URL", 0),
    STR_SETTING(local_dictionary_dir, 1, 1),
    STR_SETTING(ocr_model_dir, 1, 1),
    CHECKED_SETTING(local_dictionaries, 0, vjo_local_names_valid, "1-8 relative .vjdict filenames separated by commas", 1),
    STR_SETTING(log_host, 0, 0),
    CHECKED_SETTING(anki_host, 0, valid_anki_host, "empty, auto, or IP[:port]", 0),
    STR_SETTING(anki_deck, 1, 1),
    STR_SETTING(anki_note_type, 1, 1),
    STR_SETTING(anki_tags, 0, 1),
    CHECKED_SETTING(anki_audio_url, 1, valid_audio_url, "empty, or http(s)://host[:port]/path with {term}", 1),
};
#define N_STR_SETTINGS ((int)(sizeof(str_settings) / sizeof(str_settings[0])))

static const StrSetting *find_str_setting(const char *key)
{
    for (int i = 0; i < N_STR_SETTINGS; i++)
        if (vjo_ieq(key, str_settings[i].key))
            return &str_settings[i];
    return NULL;
}

static int anki_field_index(const char *key)
{
    for (int i = 0; i < VJO_ANKI_FIELD_COUNT; i++)
        if (vjo_ieq(key, anki_fields[i].key))
            return i;
    return -1;
}

static int keeps_comment_chars(const char *key)
{
    const StrSetting *ss = find_str_setting(key);
    return (ss && ss->raw) || anki_field_index(key) >= 0;
}

static void set_str(VjoConfig *c, const char *key, const char *val, char *dst, size_t size)
{
    if (strlen(val) >= size)
        warn(c, "%s: value too long%s", key, "");
    else
        memcpy(dst, val, strlen(val) + 1);
}

static void set_font(VjoConfig *c, const char *key, const char *val, int *dst)
{
    int v;
    if (parse_int(val, &v) == 0 && v >= 8 && v <= 40)
        *dst = v;
    else
        warn(c, "%s: invalid value '%s' (8-40)", key, val);
}

/* A time in ms within [lo, hi]. */
static void set_ms(VjoConfig *c, const char *key, const char *val, int *dst, int lo, int hi)
{
    int v;
    if (parse_int(val, &v) == 0 && v >= lo && v <= hi)
        *dst = v;
    else
        warn(c, "%s: invalid value '%s' (ms, out of range)", key, val);
}

static void set_trigger(VjoConfig *c, const char *key, const char *val, int *dst)
{
    for (int i = 0; i < VJO_TRIGGER_COUNT; i++) {
        if (vjo_ieq(val, trigger_names[i])) {
            *dst = i;
            return;
        }
    }
    warn(c, "%s: invalid value '%s'", key, val);
}

static void set_kv(VjoConfig *c, const char *key, const char *val)
{
    const StrSetting *ss;
    int d;
    if (vjo_ieq(key, "dictionary")) {
        d = vjo_dict_find(val);
        if (d >= 0)
            c->dictionary = d;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if ((d = api_key_dict(key)) >= 0) {
        set_str(c, key, val, c->api_key[d], sizeof(c->api_key[d]));
    } else if (vjo_ieq(key, "frequency_filter") || vjo_ieq(key, "font_size") || vjo_ieq(key, "hw_jpeg") ||
               vjo_ieq(key, "combo_delay_ms")) {
        /* removed settings (tools/migrate_config.py REMOVED): no warning */
    } else if (vjo_ieq(key, "non_japanese_filter")) {
        if (vjo_ieq(val, "lines"))
            c->non_japanese_filter = VJO_FILTER_LINES;
        else if (vjo_ieq(val, "none"))
            c->non_japanese_filter = VJO_FILTER_NONE;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "japanese_only_selection")) {
        if (vjo_ieq(val, "on")) c->japanese_only_selection = 1;
        else if (vjo_ieq(val, "off")) c->japanese_only_selection = 0;
        else warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "highlight_mode")) {
        if (vjo_ieq(val, "word")) c->highlight_mode = VJO_HIGHLIGHT_WORD;
        else if (vjo_ieq(val, "character")) c->highlight_mode = VJO_HIGHLIGHT_CHARACTER;
        else warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "local_dictionary_results")) {
        int v;
        if (parse_int(val, &v) == 0 && v >= 1 && v <= VJO_LOCAL_MAX_DICTS)
            c->local_dictionary_results = v;
        else
            warn(c, "%s: invalid value '%s' (1-8)", key, val);
    } else if (vjo_ieq(key, "font_size_ja")) {
        set_font(c, key, val, &c->font_size_ja);
    } else if (vjo_ieq(key, "font_size_en")) {
        set_font(c, key, val, &c->font_size_en);
    } else if (vjo_ieq(key, "toggle_button")) {
        set_trigger(c, key, val, &c->toggle_button);
    } else if (vjo_ieq(key, "subtitle_button")) {
        set_trigger(c, key, val, &c->subtitle_button);
    } else if (vjo_ieq(key, "text_source")) {
        if (vjo_ieq(val, "auto")) c->text_source = VJO_SOURCE_OCR;
        else if (vjo_ieq(val, "hooks")) {
            c->text_source = VJO_SOURCE_OCR;
            warn(c, "%s: hooks are unavailable in this OCR build", key, "");
        }
        else if (vjo_ieq(val, "ocr")) c->text_source = VJO_SOURCE_OCR;
        else warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "ocr_backend")) {
        if (vjo_ieq(val, "lens"))
            c->ocr_backend = VJO_OCR_LENS;
        else if (vjo_ieq(val, "ncnn"))
            c->ocr_backend = VJO_OCR_NCNN;
        else if (vjo_ieq(val, "vocr"))
            c->ocr_backend = VJO_OCR_VOCR;
        else if (vjo_ieq(val, "meiki")) {
            /* Saved by v0.7.6-v0.7.12; the trial engine has been removed. */
            c->ocr_backend = VJO_OCR_VOCR;
            warn(c, "%s: Meiki was removed; using %s", key, "vocr");
        }
        else if (vjo_ieq(val, "vocr2"))
            c->ocr_backend = VJO_OCR_VOCR2;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "vocr_model")) {
        if (valid_vocr_model(val))
            set_str(c, key, val, c->vocr_model, sizeof(c->vocr_model));
        else
            warn(c, "%s: invalid value '%s' (a .vocr file name in ocr_model_dir)", key, val);
    } else if (vjo_ieq(key, "vocr_fallback_model")) {
        if (!val[0] || vjo_ieq(val, "none"))
            c->vocr_fallback_model[0] = '\0';
        else if (valid_vocr_model(val))
            set_str(c, key, val, c->vocr_fallback_model, sizeof(c->vocr_fallback_model));
        else
            warn(c, "%s: invalid value '%s' (a .vocr file name in ocr_model_dir, or none)", key, val);
    } else if (vjo_ieq(key, "ocr_settle_ms")) {
        set_ms(c, key, val, &c->ocr_settle_ms, 0, 1000);
    } else if (vjo_ieq(key, "ocr_settle_max_ms")) {
        set_ms(c, key, val, &c->ocr_settle_max_ms, 100, 10000);
    } else if (vjo_ieq(key, "ocr_skip_blank_lines")) {
        if (vjo_ieq(val, "on")) c->ocr_skip_blank_lines = 1;
        else if (vjo_ieq(val, "off")) c->ocr_skip_blank_lines = 0;
        else warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "ocr_threads")) {
        if (!strcmp(val, "1") || !strcmp(val, "2") || !strcmp(val, "3")) c->ocr_threads = val[0] - '0';
        else warn(c, "%s: invalid value '%s' (1-3)", key, val);
    } else if (vjo_ieq(key, "ocr_boost_clock")) {
        if (vjo_ieq(val, "on")) c->ocr_boost_clock = 1;
        else if (vjo_ieq(val, "off")) c->ocr_boost_clock = 0;
        else warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "ocr_mode")) {
        if (vjo_ieq(val, "auto"))
            c->ocr_mode = VJO_OCR_AUTO;
        else if (vjo_ieq(val, "on_press"))
            c->ocr_mode = VJO_OCR_ON_PRESS;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if ((ss = find_str_setting(key)) != NULL) {
        char *dst = (char *)c + ss->offset;
        if (!*val && ss->required)
            warn(c, "%s: empty (keeping %s)", key, dst);
        else
            set_str(c, key, val, dst, ss->size);
        if (ss->valid && !ss->valid(dst)) {
            if (ss->quiet) {
                warn(c, "%s: invalid value (%s)", key, ss->expected);
            } else {
                char what[96];
                vjo_snprintf(what, sizeof(what), "'%s' (%s)", val, ss->expected);
                warn(c, "%s: invalid value %s", key, what);
            }
            dst[0] = '\0';
        }
    } else if ((d = anki_field_index(key)) >= 0) {
        set_str(c, key, val, c->anki_field[d], sizeof(c->anki_field[d]));
    } else if (vjo_ieq(key, "log_file")) {
        if (vjo_ieq(val, "on"))
            c->log_file = 1;
        else if (vjo_ieq(val, "off"))
            c->log_file = 0;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "anki_mining")) {
        if (vjo_ieq(val, "on"))
            c->anki_mining = 1;
        else if (vjo_ieq(val, "off"))
            c->anki_mining = 0;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else {
        warn(c, "unknown setting '%s'%s", key, "");
    }
}

static int preference_key(const char *line, size_t n)
{
    size_t begin = 0, end;
    char key[40];
    while (begin < n && is_space(line[begin])) begin++;
    end = begin;
    while (end < n && line[end] != '=' && !is_space(line[end])) end++;
    size_t key_len = end - begin;
    if (!key_len || key_len >= sizeof(key)) return -1;
    while (end < n && is_space(line[end])) end++;
    if (end == n || line[end] != '=') return -1;
    memcpy(key, line + begin, key_len);
    key[key_len] = 0;
    return vjo_ieq(key, "ocr_backend") ? 0 :
           vjo_ieq(key, "japanese_only_selection") ? 1 :
           vjo_ieq(key, "highlight_mode") ? 2 :
           vjo_ieq(key, "local_dictionary_results") ? 3 : -1;
}

#define OWNED_PREFERENCES 4

int vjo_config_patch_preferences(const char *text, size_t len, int backend,
                                 int japanese_only, int highlight_mode,
                                 int dictionary_results, char *out, size_t cap)
{
    const char *values[OWNED_PREFERENCES];
    char results_line[40];
    size_t i = 0, used = 0;
    unsigned seen = 0;
    static const char *const backend_values[] = {
        [VJO_OCR_LENS] = "ocr_backend = lens\n", [VJO_OCR_NCNN] = "ocr_backend = ncnn\n",
        [VJO_OCR_VOCR] = "ocr_backend = vocr\n", [VJO_OCR_VOCR2] = "ocr_backend = vocr2\n"
    };
    if ((!text && len) || len > VJO_CONFIG_TEXT_MAX || !out || !cap ||
        (backend < VJO_OCR_LENS || backend > VJO_OCR_VOCR2 || !backend_values[backend]) ||
        (japanese_only != 0 && japanese_only != 1) ||
        (highlight_mode != VJO_HIGHLIGHT_WORD && highlight_mode != VJO_HIGHLIGHT_CHARACTER) ||
        dictionary_results < 1 || dictionary_results > VJO_LOCAL_MAX_DICTS ||
        (len && memchr(text, 0, len)))
        return -1;
    values[0] = backend_values[backend];
    values[1] = japanese_only ? "japanese_only_selection = on\n" : "japanese_only_selection = off\n";
    values[2] = highlight_mode == VJO_HIGHLIGHT_WORD
        ? "highlight_mode = word\n" : "highlight_mode = character\n";
    vjo_snprintf(results_line, sizeof(results_line), "local_dictionary_results = %d\n", dictionary_results);
    values[3] = results_line;
    /* Keep a BOM even when the first line is an owned preference. */
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) {
        if (cap <= 3) return -1;
        memcpy(out, text, 3); used = i = 3;
    }
    while (i < len) {
        size_t begin = i;
        while (i < len && text[i] != '\n') i++;
        int key = preference_key(text + begin, i - begin);
        if (i < len) i++;
        const char *line = text + begin;
        size_t bytes = i - begin;
        if (key >= 0) {
            if (seen & (1u << key)) continue;
            seen |= 1u << key;
            line = values[key]; bytes = strlen(line);
        }
        if (bytes >= cap - used) return -1;
        memcpy(out + used, line, bytes); used += bytes;
    }
    for (int key = 0; key < OWNED_PREFERENCES; key++) {
        if (seen & (1u << key)) continue;
        if (used && out[used - 1] != '\n') {
            if (used + 1 >= cap) return -1;
            out[used++] = '\n';
        }
        size_t bytes = strlen(values[key]);
        if (bytes >= cap - used) return -1;
        memcpy(out + used, values[key], bytes); used += bytes;
    }
    if (used > VJO_CONFIG_TEXT_MAX) return -1;
    out[used] = 0;
    return (int)used;
}

void vjo_config_parse(VjoConfig *c, const char *text, size_t len)
{
    size_t i = 0;
    /* Skip a UTF-8 BOM (Notepad). */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        i = 3;
    while (i < len) {
        char line[1024], key[64], val[sizeof(((VjoConfig *)0)->local_dictionaries)]; /* the longest value */
        size_t n = 0, s, e, eq;
        int cut = 0;
        while (i < len && text[i] != '\n') {
            if (n < sizeof(line) - 1)
                line[n++] = text[i];
            else
                cut = 1;
            i++;
        }
        i++;
        line[n] = '\0';

        s = 0;
        while (s < n && is_space(line[s]))
            s++;
        if (s == n || line[s] == ';' || line[s] == '#')
            continue;
        {
            char *p = strchr(line + s, '=');
            if (!p) {
                warn(c, "ignored line '%s'%s", line + s, "");
                continue;
            }
            eq = (size_t)(p - line);
        }
        e = eq;
        while (e > s && is_space(line[e - 1]))
            e--;
        if (e - s >= sizeof(key))
            e = s + sizeof(key) - 1;
        memcpy(key, line + s, e - s);
        key[e - s] = '\0';

        /* Inline comment in the value: ';' or '#' preceded by whitespace
         * (then a cut only shortened the comment). */
        if (!keeps_comment_chars(key)) {
            for (e = eq + 1; e < n; e++) {
                if ((line[e] == ';' || line[e] == '#') && is_space(line[e - 1])) {
                    n = e;
                    cut = 0;
                    break;
                }
            }
        }
        s = eq + 1;
        while (s < n && is_space(line[s]))
            s++;
        e = n;
        while (e > s && is_space(line[e - 1]))
            e--;
        if (cut || e - s >= sizeof(val)) {
            warn(c, "%s: value too long%s", key, "");
            continue;
        }
        memcpy(val, line + s, e - s);
        val[e - s] = '\0';
        set_kv(c, key, val);
    }
    /* The toggle keeps its button (it was there first). */
    if (c->toggle_button == c->subtitle_button) {
        c->subtitle_button = c->toggle_button == VJO_TRIGGER_SELECT_R ? VJO_TRIGGER_SELECT_L : VJO_TRIGGER_SELECT_R;
        warn(c, "toggle_button and subtitle_button are both %s: subtitle_button is %s",
             trigger_names[c->toggle_button], trigger_names[c->subtitle_button]);
    }
}
