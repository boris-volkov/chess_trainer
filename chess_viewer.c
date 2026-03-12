// chess_viewer.c - A minimal PGN chess viewer using SDL2 for graphical display
// Displays games as an animated playback with per-move delays
// Dependencies: SDL2 and SDL2_image (for loading PNG piece images)
// Compile (Linux/Mac): gcc chess_viewer.c -o chess_viewer -lSDL2 -lSDL2_image
// Windows: Use a setup like MinGW or Visual Studio with SDL2 libs

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <strings.h>
#endif
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#define BOARD_SIZE 8
#define SCREEN_SIZE 800
#define DEFAULT_GAMES_DIR "games"
#define MAX_MOVES 8192
#define MOVE_TEXT_LEN 32
#define MOVE_DELAY_MS 5000
#define MOVE_DELAY_MIN_MS 500
#define MOVE_DELAY_MAX_MS 20000
#define MOVE_DELAY_STEP_MS 500
#define MOVE_ANIM_MS 450
#define NAME_LEN 128
#define YEAR_LEN 5
#define RESULT_LEN 16
#define GAME_OVER_PAUSE_MS 10000
#define KING_FLIP_MS 800
#define SPEED_MESSAGE_MS 1500
#define CURSOR_IDLE_MS 2500
#define FEN_SAVE_PATH "saved_positions.fen"
#define FEN_MESSAGE_MS 1500
#define GAME_NAV_PREV -1
#define GAME_NAV_NONE 0
#define GAME_NAV_NEXT 1
#define GAME_NAV_RESTART 2
#define GAME_NAV_SELECT 3

#ifdef _WIN32
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#else
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

typedef struct {
    char *name;
    int type;
} CatalogEntry;

char board[BOARD_SIZE][BOARD_SIZE];
SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
SDL_Texture *piece_textures[256] = {NULL};
char current_white_name[NAME_LEN] = "White";
char current_black_name[NAME_LEN] = "Black";
char current_game_year[YEAR_LEN] = "";
char current_white_elo[NAME_LEN] = "";
char current_black_elo[NAME_LEN] = "";
const char *games_dir_root = DEFAULT_GAMES_DIR;
int show_loser_king = 0;
int loser_is_white = 0;
float loser_king_angle = 180.0f;
int show_draw_kings = 0;
float draw_king_angle = 90.0f;
int view_from_white = 1;
int dim_board = 0;
int pause_buffered = 0;
int move_delay_ms = MOVE_DELAY_MS;
Uint32 speed_message_until = 0;
Uint32 fen_message_until = 0;
int analysis_mode = 0;
int show_help = 0;
int guess_mode = 0;
int guess_score = 0;
int turn_is_white = 1;
int game_nav_request = GAME_NAV_NONE;
int show_elos = 0;
int uncolored_mode = 0;
int show_defense_lines = 0;
int white_king_moved = 0;
int white_rook_a_moved = 0;
int white_rook_h_moved = 0;
int black_king_moved = 0;
int black_rook_a_moved = 0;
int black_rook_h_moved = 0;
int catalog_active = 0;
int catalog_selection_made = 0;
CatalogEntry *catalog_entries = NULL;
int catalog_entry_count = 0;
int catalog_index = 0;
int catalog_scroll = 0;
char *forced_pgn_path = NULL;
char catalog_dir[1024] = "";
int suppress_present = 0;
int analysis_saved_dim = 0;
int analysis_saved_show_loser_king = 0;
int analysis_saved_show_draw_kings = 0;
char analysis_saved_board[BOARD_SIZE][BOARD_SIZE];
unsigned char analysis_marks[BOARD_SIZE][BOARD_SIZE];
SDL_Cursor *analysis_cursor = NULL;
int mark_dragging = 0;
int mark_drag_value = 1;
int mark_last_r = -1;
int mark_last_f = -1;
int cursor_visible = 1;
Uint32 last_mouse_activity = 0;
char fen_message[64] = "FEN saved";

typedef struct {
    int square;
    int offset_x;
    int offset_y;
    int screen_w;
    int screen_h;
    int board_px;
} BoardView;

typedef struct {
    int active;
    char piece;
    float x;
    float y;
    int skip_r1, skip_f1;
} Overlay;

int is_in_check(int is_white);
void board_to_screen(const BoardView *view, int board_r, int board_f, int *out_x, int *out_y);
int screen_to_board(const BoardView *view, int x, int y, int *out_r, int *out_f);
void enter_analysis_mode(void);
void exit_analysis_mode(void);
SDL_Cursor *create_analysis_cursor(void);
void clear_analysis_marks(void);
int begin_mark_drag(const BoardView *view, int x, int y);
int update_mark_drag(const BoardView *view, int x, int y);
void end_mark_drag(void);
int adjust_move_delay(int delta_ms, Uint32 now);
void render_speed_label(const BoardView *view);
void render_fen_label(const BoardView *view);
void render_help_overlay(const BoardView *view);
void render_guess_score(const BoardView *view);
void render_catalog_overlay(const BoardView *view);
void catalog_free(void);
void catalog_open(const char *games_dir);
void catalog_select(const char *games_dir);
int handle_catalog_event(const SDL_Event *e, const char *games_dir);
int catalog_total_entries(void);
char *copy_string(const char *s);
int has_pgn_extension(const char *name);
void free_string_list(char **items, int count);
char *join_path(const char *dir, const char *name);
int list_pgn_files(const char *dir, char ***out_files);
static int list_pgn_files_recursive(const char *dir, const char *base,
                                    char ***out_files, int *count, int *cap);
void set_cursor_visible(int visible);
void note_mouse_activity(Uint32 now);
void update_cursor_auto_hide(Uint32 now);
void note_mouse_activity_event(const SDL_Event *e);
int piece_attacks_square(char piece, int from_r, int from_f, int to_r, int to_f, int is_white);
void draw_thick_line(int x1, int y1, int x2, int y2, int thickness, SDL_Color color);
void render_defense_lines(const BoardView *view);
void reset_castling_state(void);
void build_fen(char *out, size_t out_size);
void save_fen_snapshot(const char *path);

static int is_white_piece(char piece) {
    return (piece >= 'A' && piece <= 'Z');
}

int find_king_pos(char king, int *out_r, int *out_f) {
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            if (board[r][f] == king) {
                *out_r = r;
                *out_f = f;
                return 1;
            }
        }
    }
    return 0;
}

typedef struct {
    char c;
    unsigned char rows[7];
} Glyph;

static const Glyph font_glyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08}},
    {'\'',{0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'/', {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}},
    {'(', {0x04, 0x08, 0x10, 0x10, 0x10, 0x08, 0x04}},
    {')', {0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x04}},
    {':', {0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}}
};

static const unsigned char *get_glyph_rows(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof(font_glyphs) / sizeof(font_glyphs[0]); i++) {
        if (font_glyphs[i].c == c) return font_glyphs[i].rows;
    }
    return font_glyphs[7].rows;  // '?'
}

int text_width_px(const char *text, int scale) {
    int len = (int)strlen(text);
    if (len <= 0) return 0;
    return (len * 6 - 1) * scale;
}

void draw_text(int x, int y, int scale, const char *text, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    int pen_x = x;
    for (const char *p = text; *p; p++) {
        const unsigned char *rows = get_glyph_rows(*p);
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (rows[r] & (1 << (4 - c))) {
                    SDL_Rect rect = {pen_x + c * scale, y + r * scale, scale, scale};
                    SDL_RenderFillRect(renderer, &rect);
                }
            }
        }
        pen_x += 6 * scale;
    }
}

void draw_color_swatch(int x, int y, int size, SDL_Color fill, SDL_Color outline) {
    SDL_Rect rect = {x, y, size, size};
    SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, outline.r, outline.g, outline.b, outline.a);
    SDL_RenderDrawRect(renderer, &rect);
}

void render_year_label(const BoardView *view) {
    if (current_game_year[0] == '\0') return;

    int scale = (view->square >= 60) ? 3 : 2;
    int margin = (view->square >= 60) ? 16 : 8;
    int text_w = text_width_px(current_game_year, scale);
    int text_h = 7 * scale;

    int x = view->offset_x + margin;
    int y = view->offset_y + margin;
    if (view->offset_y >= text_h + 2 * margin) {
        y = view->offset_y - margin - text_h;
        x = view->offset_x + margin;
    } else if (view->offset_x >= text_w + 2 * margin) {
        x = view->offset_x - margin - text_w;
        y = view->offset_y + margin;
    }

    SDL_Color text_color = {255, 255, 255, 255};
    draw_text(x, y, scale, current_game_year, text_color);
}

int adjust_move_delay(int delta_ms, Uint32 now) {
    int new_delay = move_delay_ms + delta_ms;
    if (new_delay < MOVE_DELAY_MIN_MS) new_delay = MOVE_DELAY_MIN_MS;
    if (new_delay > MOVE_DELAY_MAX_MS) new_delay = MOVE_DELAY_MAX_MS;
    if (new_delay == move_delay_ms) return 0;
    move_delay_ms = new_delay;
    speed_message_until = now + SPEED_MESSAGE_MS;
    return 1;
}

void render_speed_label(const BoardView *view) {
    if (speed_message_until == 0) return;
    Uint32 now = SDL_GetTicks();
    if (now >= speed_message_until) {
        speed_message_until = 0;
        return;
    }

    char buf[32];
    int whole = move_delay_ms / 1000;
    int rem = move_delay_ms % 1000;
    if (rem == 0) {
        const char *unit = (whole == 1) ? "second" : "seconds";
        snprintf(buf, sizeof(buf), "%d %s/move", whole, unit);
    } else {
        snprintf(buf, sizeof(buf), "%d.%d seconds/move", whole, rem / 100);
    }

    int scale = (view->square >= 60) ? 3 : 2;
    int margin = (view->square >= 60) ? 16 : 8;
    int text_w = text_width_px(buf, scale);
    int text_h = 7 * scale;
    int x = view->offset_x + (view->board_px - text_w) / 2;
    if (x < view->offset_x + margin) x = view->offset_x + margin;
    int y = view->offset_y + margin;

    int pad = (scale >= 3) ? 4 : 3;
    SDL_Rect bg = {x - pad, y - pad, text_w + pad * 2, text_h + pad * 2};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 180);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color text_color = {255, 255, 255, 255};
    draw_text(x, y, scale, buf, text_color);
}

void render_fen_label(const BoardView *view) {
    if (fen_message_until == 0) return;
    Uint32 now = SDL_GetTicks();
    if (now >= fen_message_until) {
        fen_message_until = 0;
        return;
    }

    const char *msg = (fen_message[0] != '\0') ? fen_message : "FEN saved";
    int scale = (view->square >= 60) ? 3 : 2;
    int margin = (view->square >= 60) ? 16 : 8;
    int text_w = text_width_px(msg, scale);
    int text_h = 7 * scale;
    int x = view->offset_x + (view->board_px - text_w) / 2;
    if (x < view->offset_x + margin) x = view->offset_x + margin;
    int y = view->offset_y + margin;
    int pad = (scale >= 3) ? 4 : 3;
    if (speed_message_until != 0 && now < speed_message_until) {
        y += text_h + pad * 2 + 4;
    }

    SDL_Rect bg = {x - pad, y - pad, text_w + pad * 2, text_h + pad * 2};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 180);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color text_color = {255, 255, 255, 255};
    draw_text(x, y, scale, msg, text_color);
}

void render_guess_score(const BoardView *view) {
    if (!guess_mode) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "Score: %d", guess_score);

    int scale = (view->square >= 60) ? 3 : 2;
    int margin = (view->square >= 60) ? 16 : 8;
    int text_w = text_width_px(buf, scale);
    int text_h = 7 * scale;
    int swatch_size = text_h;
    if (swatch_size > 16) swatch_size = 16;
    int gap = (scale >= 3) ? 6 : 4;

    int x = view->offset_x + margin;
    int y = view->offset_y + view->board_px - margin - text_h;
    int swatch_x = x - gap - swatch_size;

    int left_space = view->offset_x;
    int right_space = view->screen_w - (view->offset_x + view->board_px);
    int need_w = text_w + swatch_size + gap + margin * 2;
    if (left_space >= need_w) {
        x = view->offset_x - margin - text_w;
        swatch_x = x - gap - swatch_size;
    } else if (right_space >= need_w) {
        swatch_x = view->offset_x + view->board_px + margin;
        x = swatch_x + swatch_size + gap;
    } else if (view->offset_y >= text_h + margin * 2) {
        swatch_x = view->offset_x + margin;
        x = swatch_x + swatch_size + gap;
        y = view->offset_y - margin - text_h;
    } else {
        swatch_x = view->offset_x + margin;
        x = swatch_x + swatch_size + gap;
    }
    if (y < view->offset_y + margin) {
        y = view->offset_y + margin;
    }

    SDL_Color fill = turn_is_white ? (SDL_Color){235, 235, 235, 255} : (SDL_Color){25, 25, 25, 255};
    SDL_Color outline = turn_is_white ? (SDL_Color){30, 30, 30, 255} : (SDL_Color){235, 235, 235, 255};
    draw_color_swatch(swatch_x, y + (text_h - swatch_size) / 2, swatch_size, fill, outline);

    SDL_Color text_color = {255, 255, 255, 255};
    draw_text(x, y, scale, buf, text_color);
}

void render_help_overlay(const BoardView *view) {
    if (!show_help) return;

    const char *lines[] = {
        "HELP",
        "ALL MODES:",
        "  Q: QUIT",
        "  N: NEXT GAME",
        "  P: PREV GAME",
        "  R: RESTART GAME",
        "  C: OPEN CATALOG",
        "  E: TOGGLE ELO",
        "  U: TOGGLE UNCOLORED",
        "  D: TOGGLE DEFENSE",
        "  S: SAVE FEN",
        "  ESC: TOGGLE HELP",
        "  F: FLIP VIEW",
        "  UP/DOWN: SPEED",
        "  RIGHT DRAG: MARK SQUARES",
        "  MIDDLE CLICK: CLEAR MARKS",
        "PLAYBACK:",
        "  SPACE: PAUSE/RESUME",
        "  A: TOGGLE ANALYSIS",
        "  G: TOGGLE GUESS MODE",
        "PAUSED (SPACE):",
        "  LEFT/RIGHT: STEP MOVES",
        "ANALYSIS (A):",
        "  LEFT DRAG: MOVE PIECE",
        "GUESS MODE (G):",
        "  LEFT DRAG: GUESS MOVE",
        "  SCORE: 1 POINT IF MATCH",
        "CATALOG (C):",
        "  UP/DOWN: SELECT FILE",
        "  ENTER: OPEN",
        "  ESC: CLOSE"
    };
    int line_count = (int)(sizeof(lines) / sizeof(lines[0]));

    int scale = (view->square >= 60) ? 3 : 2;
    int line_gap = (scale >= 3) ? 4 : 3;
    int text_h = 7 * scale;
    int max_w = 0;
    for (int i = 0; i < line_count; i++) {
        int w = text_width_px(lines[i], scale);
        if (w > max_w) max_w = w;
    }

    int total_h = line_count * text_h + (line_count - 1) * line_gap;
    int pad = (scale >= 3) ? 10 : 8;
    int box_w = max_w + pad * 2;
    int box_h = total_h + pad * 2;
    int x = (view->screen_w - box_w) / 2;
    int y = (view->screen_h - box_h) / 2;

    SDL_Rect bg = {x, y, box_w, box_h};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 180);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color text_color = {255, 255, 255, 255};
    int text_x = x + pad;
    int text_y = y + pad;
    for (int i = 0; i < line_count; i++) {
        draw_text(text_x, text_y, scale, lines[i], text_color);
        text_y += text_h + line_gap;
    }
}

static int filename_cmp(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
#ifdef _WIN32
    return _stricmp(sa, sb);
#else
    return strcasecmp(sa, sb);
#endif
}

static int catalog_entry_cmp(const void *a, const void *b) {
    const CatalogEntry *ea = (const CatalogEntry *)a;
    const CatalogEntry *eb = (const CatalogEntry *)b;
    if (ea->type != eb->type) return (ea->type < eb->type) ? -1 : 1;
#ifdef _WIN32
    return _stricmp(ea->name, eb->name);
#else
    return strcasecmp(ea->name, eb->name);
#endif
}

static int push_catalog_entry(CatalogEntry **items, int *count, int *cap, const char *name, int type) {
    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 16 : (*cap * 2);
        CatalogEntry *next = (CatalogEntry *)realloc(*items, (size_t)new_cap * sizeof(*next));
        if (!next) return 0;
        *items = next;
        *cap = new_cap;
    }
    (*items)[*count].name = copy_string(name);
    if (!(*items)[*count].name) return 0;
    (*items)[*count].type = type;
    (*count)++;
    return 1;
}

static void catalog_set_dir(const char *new_dir) {
    if (!new_dir) {
        catalog_dir[0] = '\0';
        return;
    }
    strncpy(catalog_dir, new_dir, sizeof(catalog_dir) - 1);
    catalog_dir[sizeof(catalog_dir) - 1] = '\0';
}

static void catalog_dir_up(void) {
    size_t len = strlen(catalog_dir);
    if (len == 0) return;
    for (size_t i = len; i > 0; i--) {
        if (catalog_dir[i - 1] == '/' || catalog_dir[i - 1] == '\\') {
            catalog_dir[i - 1] = '\0';
            return;
        }
    }
    catalog_dir[0] = '\0';
}

static int catalog_load_entries(const char *games_dir) {
    CatalogEntry *entries = NULL;
    int count = 0;
    int cap = 0;
    char *dir_path = NULL;
    if (catalog_dir[0] == '\0') {
        dir_path = copy_string(games_dir);
    } else {
        dir_path = join_path(games_dir, catalog_dir);
    }
    if (!dir_path) return 0;

#ifdef _WIN32
    char *search = join_path(dir_path, "*");
    if (!search) {
        free(dir_path);
        return 0;
    }
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(search, &data);
    free(search);
    if (h == INVALID_HANDLE_VALUE) {
        free(dir_path);
        return 0;
    }
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!push_catalog_entry(&entries, &count, &cap, data.cFileName, 1)) {
                FindClose(h);
                free(dir_path);
                catalog_entries = entries;
                catalog_entry_count = count;
                return 0;
            }
        } else if (has_pgn_extension(data.cFileName)) {
            if (!push_catalog_entry(&entries, &count, &cap, data.cFileName, 0)) {
                FindClose(h);
                free(dir_path);
                catalog_entries = entries;
                catalog_entry_count = count;
                return 0;
            }
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *d = opendir(dir_path);
    if (!d) {
        free(dir_path);
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char *full = join_path(dir_path, ent->d_name);
        if (!full) {
            closedir(d);
            free(dir_path);
            return 0;
        }
        int is_dir = 0;
#ifdef DT_DIR
        if (ent->d_type == DT_DIR) is_dir = 1;
        if (ent->d_type == DT_UNKNOWN)
#endif
        {
            DIR *probe = opendir(full);
            if (probe) {
                is_dir = 1;
                closedir(probe);
            }
        }
        if (is_dir) {
            if (!push_catalog_entry(&entries, &count, &cap, ent->d_name, 1)) {
                free(full);
                closedir(d);
                free(dir_path);
                return 0;
            }
        } else if (has_pgn_extension(ent->d_name)) {
            if (!push_catalog_entry(&entries, &count, &cap, ent->d_name, 0)) {
                free(full);
                closedir(d);
                free(dir_path);
                return 0;
            }
        }
        free(full);
    }
    closedir(d);
#endif

    if (catalog_dir[0] != '\0') {
        push_catalog_entry(&entries, &count, &cap, "..", 2);
    }

    if (count > 1) {
        qsort(entries, (size_t)count, sizeof(entries[0]), catalog_entry_cmp);
    }

    for (int i = 0; i < catalog_entry_count; i++) {
        free(catalog_entries[i].name);
    }
    free(catalog_entries);
    catalog_entries = entries;
    catalog_entry_count = count;
    free(dir_path);
    return 1;
}

void catalog_free(void) {
    if (catalog_entries) {
        for (int i = 0; i < catalog_entry_count; i++) {
            free(catalog_entries[i].name);
        }
        free(catalog_entries);
    }
    catalog_entries = NULL;
    catalog_entry_count = 0;
    catalog_index = 0;
    catalog_scroll = 0;
    catalog_active = 0;
}

void catalog_open(const char *games_dir) {
    if (catalog_active) return;
    catalog_free();
    catalog_set_dir("");
    if (!catalog_load_entries(games_dir)) {
        catalog_entries = NULL;
        catalog_entry_count = 0;
        return;
    }
    catalog_active = 1;
    catalog_selection_made = 0;
    catalog_index = 0;
    catalog_scroll = 0;
}

void catalog_select(const char *games_dir) {
    if (!catalog_active) return;
    if (catalog_index == 0) {
        free(forced_pgn_path);
        forced_pgn_path = NULL;
    } else {
        int entry_index = catalog_index - 1;
        if (entry_index >= 0 && entry_index < catalog_entry_count) {
            CatalogEntry *entry = &catalog_entries[entry_index];
            if (entry->type == 2) {
                catalog_dir_up();
                catalog_load_entries(games_dir);
                catalog_index = 0;
                catalog_scroll = 0;
                return;
            }
            if (entry->type == 1) {
                char next_dir[1024];
                if (catalog_dir[0] == '\0') {
                    snprintf(next_dir, sizeof(next_dir), "%s", entry->name);
                } else {
                    snprintf(next_dir, sizeof(next_dir), "%s%c%s", catalog_dir, PATH_SEP, entry->name);
                }
                catalog_set_dir(next_dir);
                catalog_load_entries(games_dir);
                catalog_index = 0;
                catalog_scroll = 0;
                return;
            }
            char *dir_path = NULL;
            if (catalog_dir[0] == '\0') {
                dir_path = copy_string(games_dir);
            } else {
                dir_path = join_path(games_dir, catalog_dir);
            }
            if (dir_path) {
                char *path = join_path(dir_path, entry->name);
                free(dir_path);
                if (path) {
                    free(forced_pgn_path);
                    forced_pgn_path = path;
                }
            }
        }
    }
    catalog_selection_made = 1;
    catalog_active = 0;
}

int catalog_total_entries(void) {
    return 1 + catalog_entry_count;
}

void render_catalog_overlay(const BoardView *view) {
    if (!catalog_active) return;

    const char *title = "CATALOG";
    const char *random_label = "[RANDOM FILE]";
    int total_entries = catalog_total_entries();

    int scale = (view->square >= 60) ? 3 : 2;
    int line_gap = (scale >= 3) ? 4 : 3;
    int text_h = 7 * scale;
    int pad = (scale >= 3) ? 10 : 8;
    int header_gap = line_gap + (scale >= 3 ? 4 : 2);

    int max_w = text_width_px(title, scale);
    int random_w = text_width_px(random_label, scale);
    if (random_w > max_w) max_w = random_w;
    for (int i = 0; i < catalog_entry_count; i++) {
        char label[1024];
        const CatalogEntry *entry = &catalog_entries[i];
        if (entry->type == 1) {
            snprintf(label, sizeof(label), "[DIR] %s", entry->name);
        } else if (entry->type == 2) {
            snprintf(label, sizeof(label), "[..]");
        } else {
            snprintf(label, sizeof(label), "%s", entry->name);
        }
        int w = text_width_px(label, scale);
        if (w > max_w) max_w = w;
    }

    int available_h = view->screen_h - pad * 4 - text_h - header_gap;
    int line_h = text_h + line_gap;
    int max_lines = (available_h > 0) ? (available_h / line_h) : 0;
    if (max_lines < 4) max_lines = 4;
    if (max_lines > total_entries) max_lines = total_entries;

    if (catalog_index < catalog_scroll) catalog_scroll = catalog_index;
    if (catalog_index >= catalog_scroll + max_lines) {
        catalog_scroll = catalog_index - max_lines + 1;
    }

    int list_h = max_lines * line_h - line_gap;
    int box_w = max_w + pad * 2;
    int box_h = text_h + header_gap + list_h + pad * 2;
    int x = (view->screen_w - box_w) / 2;
    int y = (view->screen_h - box_h) / 2;

    SDL_Rect bg = {x, y, box_w, box_h};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 190);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color text_color = {255, 255, 255, 255};
    int text_x = x + pad;
    int text_y = y + pad;
    draw_text(text_x, text_y, scale, title, text_color);
    text_y += text_h + header_gap;

    for (int i = 0; i < max_lines; i++) {
        int idx = catalog_scroll + i;
        if (idx >= total_entries) break;
        char label[1024];
        if (idx == 0) {
            strncpy(label, random_label, sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
        } else {
            const CatalogEntry *entry = &catalog_entries[idx - 1];
            if (entry->type == 1) {
                snprintf(label, sizeof(label), "[DIR] %s", entry->name);
            } else if (entry->type == 2) {
                snprintf(label, sizeof(label), "[..]");
            } else {
                snprintf(label, sizeof(label), "%s", entry->name);
            }
        }
        if (idx == catalog_index) {
            SDL_Rect hi = {text_x - 3, text_y - 3, max_w + 6, text_h + 6};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 40, 120, 255, 190);
            SDL_RenderFillRect(renderer, &hi);
        }
        draw_text(text_x, text_y, scale, label, text_color);
        text_y += line_h;
    }
}

int handle_catalog_event(const SDL_Event *e, const char *games_dir) {
    if (!catalog_active) return 0;
    if (e->type == SDL_KEYDOWN) {
        SDL_Keycode key = e->key.keysym.sym;
        if (key == SDLK_ESCAPE || key == SDLK_c) {
            catalog_active = 0;
            return 1;
        } else if (key == SDLK_UP) {
            if (catalog_index > 0) catalog_index--;
            return 1;
        } else if (key == SDLK_DOWN) {
            int total = catalog_total_entries();
            if (catalog_index < total - 1) catalog_index++;
            return 1;
        } else if (key == SDLK_PAGEUP) {
            int step = 6;
            catalog_index -= step;
            if (catalog_index < 0) catalog_index = 0;
            return 1;
        } else if (key == SDLK_PAGEDOWN) {
            int step = 6;
            int total = catalog_total_entries();
            catalog_index += step;
            if (catalog_index > total - 1) catalog_index = total - 1;
            return 1;
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            catalog_select(games_dir);
            game_nav_request = GAME_NAV_SELECT;
            return 1;
        }
    }
    return 1;
}

void render_player_labels(const BoardView *view) {
    int margin = (view->square >= 60) ? 16 : 8;
    int right_x0 = view->offset_x + view->board_px + margin;
    int right_x1 = view->screen_w - margin;
    if (right_x1 <= right_x0) return;

    int swatch_size = (view->square >= 60) ? 16 : 12;
    int gap = 6;
    int avail_text_w = right_x1 - right_x0 - swatch_size - gap;
    if (avail_text_w <= 0) return;

    const char *white_name = (current_white_name[0] != '\0') ? current_white_name : "White";
    const char *black_name = (current_black_name[0] != '\0') ? current_black_name : "Black";
    char white_label[NAME_LEN * 2] = "";
    char black_label[NAME_LEN * 2] = "";
    if (show_elos && current_white_elo[0] != '\0') {
        snprintf(white_label, sizeof(white_label), "%s (%s)", white_name, current_white_elo);
        white_name = white_label;
    }
    if (show_elos && current_black_elo[0] != '\0') {
        snprintf(black_label, sizeof(black_label), "%s (%s)", black_name, current_black_elo);
        black_name = black_label;
    }
    int top_is_white = view_from_white ? 0 : 1;
    const char *top_name = top_is_white ? white_name : black_name;
    const char *bottom_name = top_is_white ? black_name : white_name;
    int max_len = (int)strlen(white_name);
    int black_len = (int)strlen(black_name);
    if (black_len > max_len) max_len = black_len;

    int scale = 3;
    int need_w = (max_len > 0) ? text_width_px(white_name, scale) : 0;
    if (black_len > 0) {
        int black_w = text_width_px(black_name, scale);
        if (black_w > need_w) need_w = black_w;
    }
    if (need_w > avail_text_w && max_len > 0) {
        int denom = max_len * 6 - 1;
        scale = avail_text_w / denom;
        if (scale < 1) scale = 1;
    }

    int text_h = 7 * scale;
    if (swatch_size > text_h) swatch_size = text_h;
    int top_y = view->offset_y + margin;
    int bottom_y = view->offset_y + view->board_px - margin - text_h;
    if (bottom_y < top_y) bottom_y = top_y;

    SDL_Color text_color = {230, 230, 230, 255};
    SDL_Color black_fill = {20, 20, 20, 255};
    SDL_Color white_fill = {230, 230, 230, 255};
    SDL_Color outline = {30, 30, 30, 255};

    int swatch_y_top = top_y + (text_h - swatch_size) / 2;
    int swatch_y_bottom = bottom_y + (text_h - swatch_size) / 2;

    if (top_is_white) {
        draw_color_swatch(right_x0, swatch_y_top, swatch_size, white_fill, outline);
    } else {
        draw_color_swatch(right_x0, swatch_y_top, swatch_size, black_fill, white_fill);
    }
    draw_text(right_x0 + swatch_size + gap, top_y, scale, top_name, text_color);

    if (top_is_white) {
        draw_color_swatch(right_x0, swatch_y_bottom, swatch_size, black_fill, white_fill);
    } else {
        draw_color_swatch(right_x0, swatch_y_bottom, swatch_size, white_fill, outline);
    }
    draw_text(right_x0 + swatch_size + gap, bottom_y, scale, bottom_name, text_color);
}

void init_board() {
    const char *initial[] = {
        "rnbqkbnr",
        "pppppppp",
        "........",
        "........",
        "........",
        "........",
        "PPPPPPPP",
        "RNBQKBNR"
    };
    for (int i = 0; i < BOARD_SIZE; i++) {
        strcpy(board[i], initial[i]);
    }
    reset_castling_state();
}

void reset_castling_state(void) {
    white_king_moved = 0;
    white_rook_a_moved = 0;
    white_rook_h_moved = 0;
    black_king_moved = 0;
    black_rook_a_moved = 0;
    black_rook_h_moved = 0;
}

void build_fen(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t pos = 0;
    for (int r = 0; r < BOARD_SIZE; r++) {
        int empty = 0;
        for (int f = 0; f < BOARD_SIZE; f++) {
            char p = board[r][f];
            if (p == '.') {
                empty++;
            } else {
                if (empty > 0) {
                    if (pos + 1 < out_size) out[pos++] = (char)('0' + empty);
                    empty = 0;
                }
                if (pos + 1 < out_size) out[pos++] = p;
            }
        }
        if (empty > 0) {
            if (pos + 1 < out_size) out[pos++] = (char)('0' + empty);
        }
        if (r < BOARD_SIZE - 1) {
            if (pos + 1 < out_size) out[pos++] = '/';
        }
    }

    if (pos + 1 < out_size) out[pos++] = ' ';
    if (pos + 1 < out_size) out[pos++] = turn_is_white ? 'w' : 'b';
    if (pos + 1 < out_size) out[pos++] = ' ';

    char castle[5];
    int cpos = 0;
    if (!white_king_moved && !white_rook_h_moved && board[7][4] == 'K' && board[7][7] == 'R') {
        castle[cpos++] = 'K';
    }
    if (!white_king_moved && !white_rook_a_moved && board[7][4] == 'K' && board[7][0] == 'R') {
        castle[cpos++] = 'Q';
    }
    if (!black_king_moved && !black_rook_h_moved && board[0][4] == 'k' && board[0][7] == 'r') {
        castle[cpos++] = 'k';
    }
    if (!black_king_moved && !black_rook_a_moved && board[0][4] == 'k' && board[0][0] == 'r') {
        castle[cpos++] = 'q';
    }
    if (cpos == 0) {
        if (pos + 1 < out_size) out[pos++] = '-';
    } else {
        for (int i = 0; i < cpos; i++) {
            if (pos + 1 < out_size) out[pos++] = castle[i];
        }
    }

    const char *suffix = " - 0 1";
    for (const char *p = suffix; *p; p++) {
        if (pos + 1 < out_size) out[pos++] = *p;
    }
    if (pos < out_size) out[pos] = '\0';
    else out[out_size - 1] = '\0';
}

void save_fen_snapshot(const char *path) {
    char fen[128];
    build_fen(fen, sizeof(fen));
    FILE *f = fopen(path, "a");
    if (!f) {
        printf("Failed to open %s for FEN output\n", path);
        return;
    }
    fprintf(f, "%s\n", fen);
    fclose(f);
    printf("Saved FEN: %s\n", fen);
    snprintf(fen_message, sizeof(fen_message), "Saved to %s", path);
    fen_message_until = SDL_GetTicks() + FEN_MESSAGE_MS;
}

SDL_Texture *get_piece_texture(char piece) {
    if (piece == '.') return NULL;
    unsigned char idx = (unsigned char)piece;
    if (piece_textures[idx]) return piece_textures[idx];

    char letter = tolower(piece);
    const char *color = isupper(piece) ? "lt" : "dt";
    char path[64];
    snprintf(path, sizeof(path), "pieces/Chess_%c%s.png", letter, color);

    SDL_Texture *tex = IMG_LoadTexture(renderer, path);
    if (!tex) {
        printf("Failed to load %s: %s\n", path, IMG_GetError());
    }
    piece_textures[idx] = tex;
    return tex;
}

void render_rotated_king(const BoardView *view, char king, float angle) {
    int r = -1, f = -1;
    if (!find_king_pos(king, &r, &f)) return;
    SDL_Texture *tex = get_piece_texture(king);
    if (!tex) return;
    int x = 0;
    int y = 0;
    board_to_screen(view, r, f, &x, &y);
    SDL_Rect rect = {x, y, view->square, view->square};
    SDL_RenderCopyEx(renderer, tex, NULL, &rect, angle, NULL, SDL_FLIP_NONE);
}

void board_to_screen(const BoardView *view, int board_r, int board_f, int *out_x, int *out_y) {
    int draw_r = view_from_white ? board_r : (BOARD_SIZE - 1 - board_r);
    int draw_f = view_from_white ? board_f : (BOARD_SIZE - 1 - board_f);
    if (out_x) *out_x = view->offset_x + draw_f * view->square;
    if (out_y) *out_y = view->offset_y + draw_r * view->square;
}

int screen_to_board(const BoardView *view, int x, int y, int *out_r, int *out_f) {
    if (x < view->offset_x || y < view->offset_y) return 0;
    if (x >= view->offset_x + view->board_px || y >= view->offset_y + view->board_px) return 0;
    int rel_x = x - view->offset_x;
    int rel_y = y - view->offset_y;
    int draw_f = rel_x / view->square;
    int draw_r = rel_y / view->square;
    if (draw_r < 0 || draw_r >= BOARD_SIZE || draw_f < 0 || draw_f >= BOARD_SIZE) return 0;

    int inset = view->square / 8;  // center 75%
    int local_x = rel_x - draw_f * view->square;
    int local_y = rel_y - draw_r * view->square;
    if (local_x < inset || local_x >= view->square - inset) return 0;
    if (local_y < inset || local_y >= view->square - inset) return 0;

    if (out_r) *out_r = view_from_white ? draw_r : (BOARD_SIZE - 1 - draw_r);
    if (out_f) *out_f = view_from_white ? draw_f : (BOARD_SIZE - 1 - draw_f);
    return 1;
}

SDL_Cursor *create_analysis_cursor(void) {
    const int size = 25;
    const int center = size / 2;
    const int outer_thickness = 5;
    const int inner_thickness = 3;
    const int gap = 0;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return NULL;

    Uint32 transparent = SDL_MapRGBA(surface->format, 0, 0, 0, 0);
    Uint32 white = SDL_MapRGBA(surface->format, 255, 255, 255, 255);
    Uint32 black = SDL_MapRGBA(surface->format, 0, 0, 0, 255);

    if (SDL_LockSurface(surface) != 0) {
        SDL_FreeSurface(surface);
        return NULL;
    }

    Uint32 *pixels = (Uint32 *)surface->pixels;
    int pitch = surface->pitch / 4;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            pixels[y * pitch + x] = transparent;
        }
    }

    int outer_half = outer_thickness / 2;
    int inner_half = inner_thickness / 2;
    for (int y = 0; y < size; y++) {
        if (gap > 0 && abs(y - center) <= gap) continue;
        for (int dx = -outer_half; dx <= outer_half; dx++) {
            int x = center + dx;
            if (x >= 0 && x < size) pixels[y * pitch + x] = white;
        }
    }
    for (int x = 0; x < size; x++) {
        if (gap > 0 && abs(x - center) <= gap) continue;
        for (int dy = -outer_half; dy <= outer_half; dy++) {
            int y = center + dy;
            if (y >= 0 && y < size) pixels[y * pitch + x] = white;
        }
    }

    for (int y = 0; y < size; y++) {
        if (gap > 0 && abs(y - center) <= gap) continue;
        for (int dx = -inner_half; dx <= inner_half; dx++) {
            int x = center + dx;
            if (x >= 0 && x < size) pixels[y * pitch + x] = black;
        }
    }
    for (int x = 0; x < size; x++) {
        if (gap > 0 && abs(x - center) <= gap) continue;
        for (int dy = -inner_half; dy <= inner_half; dy++) {
            int y = center + dy;
            if (y >= 0 && y < size) pixels[y * pitch + x] = black;
        }
    }

    SDL_UnlockSurface(surface);
    SDL_Cursor *cursor = SDL_CreateColorCursor(surface, center, center);
    SDL_FreeSurface(surface);
    return cursor;
}

int begin_mark_drag(const BoardView *view, int x, int y) {
    int r = -1;
    int f = -1;
    if (!screen_to_board(view, x, y, &r, &f)) return 0;
    mark_dragging = 1;
    mark_drag_value = analysis_marks[r][f] ? 0 : 1;
    analysis_marks[r][f] = (unsigned char)mark_drag_value;
    mark_last_r = r;
    mark_last_f = f;
    return 1;
}

int update_mark_drag(const BoardView *view, int x, int y) {
    if (!mark_dragging) return 0;
    int r = -1;
    int f = -1;
    if (!screen_to_board(view, x, y, &r, &f)) return 0;
    if (r == mark_last_r && f == mark_last_f) return 0;
    mark_last_r = r;
    mark_last_f = f;
    if (analysis_marks[r][f] == (unsigned char)mark_drag_value) return 0;
    analysis_marks[r][f] = (unsigned char)mark_drag_value;
    return 1;
}

void end_mark_drag(void) {
    mark_dragging = 0;
    mark_last_r = -1;
    mark_last_f = -1;
}

void set_cursor_visible(int visible) {
    if (visible && analysis_cursor) {
        SDL_SetCursor(analysis_cursor);
    }
    SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
    cursor_visible = visible;
}

void note_mouse_activity(Uint32 now) {
    last_mouse_activity = now;
    if (!cursor_visible) set_cursor_visible(1);
}

void update_cursor_auto_hide(Uint32 now) {
    if (analysis_mode || guess_mode || mark_dragging) {
        if (!cursor_visible) set_cursor_visible(1);
        return;
    }
    if (cursor_visible && now - last_mouse_activity >= CURSOR_IDLE_MS) {
        set_cursor_visible(0);
    }
}

void note_mouse_activity_event(const SDL_Event *e) {
    if (e->type == SDL_MOUSEMOTION ||
        e->type == SDL_MOUSEBUTTONDOWN ||
        e->type == SDL_MOUSEBUTTONUP ||
        e->type == SDL_MOUSEWHEEL) {
        note_mouse_activity(SDL_GetTicks());
    }
}

void clear_analysis_marks(void) {
    memset(analysis_marks, 0, sizeof(analysis_marks));
    end_mark_drag();
}

void enter_analysis_mode(void) {
    if (analysis_mode) return;
    analysis_mode = 1;
    memcpy(analysis_saved_board, board, sizeof(board));
    analysis_saved_dim = dim_board;
    analysis_saved_show_loser_king = show_loser_king;
    analysis_saved_show_draw_kings = show_draw_kings;
    show_loser_king = 0;
    show_draw_kings = 0;
    dim_board = 0;
    if (!analysis_cursor) {
        analysis_cursor = create_analysis_cursor();
        if (!analysis_cursor) {
            analysis_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
        }
    }
    note_mouse_activity(SDL_GetTicks());
}

void exit_analysis_mode(void) {
    if (!analysis_mode) return;
    analysis_mode = 0;
    memcpy(board, analysis_saved_board, sizeof(board));
    dim_board = analysis_saved_dim;
    show_loser_king = analysis_saved_show_loser_king;
    show_draw_kings = analysis_saved_show_draw_kings;
    note_mouse_activity(SDL_GetTicks());
}

void get_board_view(BoardView *view) {
    int screen_w = SCREEN_SIZE;
    int screen_h = SCREEN_SIZE;
    if (SDL_GetRendererOutputSize(renderer, &screen_w, &screen_h) != 0) {
        screen_w = SCREEN_SIZE;
        screen_h = SCREEN_SIZE;
    }
    int min_dim = (screen_w < screen_h) ? screen_w : screen_h;
    view->square = min_dim / BOARD_SIZE;
    if (view->square < 1) view->square = 1;
    view->board_px = view->square * BOARD_SIZE;
    view->offset_x = (screen_w - view->board_px) / 2;
    view->offset_y = (screen_h - view->board_px) / 2;
    view->screen_w = screen_w;
    view->screen_h = screen_h;
}

void render_board(const BoardView *view, const Overlay *overlay) {
    SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
    SDL_RenderClear(renderer);

    SDL_Color light = analysis_mode ? (SDL_Color){140, 135, 125, 255} : (SDL_Color){125, 125, 125, 255};
    SDL_Color dark  = analysis_mode ? (SDL_Color){120, 115, 105, 255} : (SDL_Color){105, 105, 105, 255};
    if (guess_mode && !analysis_mode) {
        light = (SDL_Color){125, 140, 125, 255};
        dark = (SDL_Color){105, 120, 105, 255};
    }
    SDL_Color grid = {150, 150, 150, 255};
    if (uncolored_mode) {
        SDL_Color dot = light;
        light = dark;
        int lr = dot.r + 20;
        int lg = dot.g + 20;
        int lb = dot.b + 20;
        if (lr > 255) lr = 255;
        if (lg > 255) lg = 255;
        if (lb > 255) lb = 255;
        grid = (SDL_Color){(Uint8)lr, (Uint8)lg, (Uint8)lb, 255};
    }
    if (dim_board) {
        light.r = (Uint8)(light.r * 2 / 3);
        light.g = (Uint8)(light.g * 2 / 3);
        light.b = (Uint8)(light.b * 2 / 3);
        dark.r = (Uint8)(dark.r * 2 / 3);
        dark.g = (Uint8)(dark.g * 2 / 3);
        dark.b = (Uint8)(dark.b * 2 / 3);
    }

    for (int row = 0; row < BOARD_SIZE; row++) {  // row 0 = rank 8
        for (int col = 0; col < BOARD_SIZE; col++) {
            SDL_Color colr = ((row + col) % 2 == 0) ? light : dark;
            SDL_SetRenderDrawColor(renderer, colr.r, colr.g, colr.b, colr.a);
            int x = 0;
            int y = 0;
            board_to_screen(view, row, col, &x, &y);
            SDL_Rect rect = {x, y, view->square, view->square};
            SDL_RenderFillRect(renderer, &rect);
            if (uncolored_mode) {
                int dot = view->square / 6;
                if (dot < 2) dot = 2;
                if (dot > 5) dot = 5;
                int cx = rect.x + rect.w / 2;
                int cy = rect.y + rect.h / 2;
                SDL_Rect dot_rect = {cx - dot / 2, cy - dot / 2, dot, dot};
                SDL_SetRenderDrawColor(renderer, grid.r, grid.g, grid.b, grid.a);
                SDL_RenderFillRect(renderer, &dot_rect);
            }

            if (analysis_marks[row][col]) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 40, 120, 255, 110);
                SDL_RenderFillRect(renderer, &rect);
            }
        }
    }

    render_defense_lines(view);

    for (int row = 0; row < BOARD_SIZE; row++) {  // row 0 = rank 8
        for (int col = 0; col < BOARD_SIZE; col++) {
            int skip = 0;
            if (overlay && overlay->active) {
                if (row == overlay->skip_r1 && col == overlay->skip_f1) {
                    skip = 1;
                }
            }
            if (!skip && show_loser_king) {
                char losing_piece = loser_is_white ? 'K' : 'k';
                if (board[row][col] == losing_piece) {
                    skip = 1;
                }
            }
            if (!skip && show_draw_kings) {
                if (board[row][col] == 'K' || board[row][col] == 'k') {
                    skip = 1;
                }
            }
            char piece = skip ? '.' : board[row][col];
            SDL_Texture *tex = get_piece_texture(piece);
            if (tex) {
                int x = 0;
                int y = 0;
                board_to_screen(view, row, col, &x, &y);
                SDL_Rect rect = {x, y, view->square, view->square};
                SDL_RenderCopy(renderer, tex, NULL, &rect);
            }
        }
    }

    if (overlay && overlay->active) {
        SDL_Texture *tex = get_piece_texture(overlay->piece);
        if (tex) {
            SDL_Rect rect = {(int)(overlay->x + 0.5f), (int)(overlay->y + 0.5f),
                             view->square, view->square};
            SDL_RenderCopy(renderer, tex, NULL, &rect);
        }
    }

    if (show_draw_kings) {
        render_rotated_king(view, 'K', draw_king_angle);
        render_rotated_king(view, 'k', draw_king_angle);
    } else if (show_loser_king) {
        char losing_piece = loser_is_white ? 'K' : 'k';
        render_rotated_king(view, losing_piece, loser_king_angle);
    }

    int thickness = (view->square >= 60) ? 4 : 2;
    SDL_SetRenderDrawColor(renderer, 200, 20, 20, 255);
    if (is_in_check(1)) {
        int r = -1, f = -1;
        if (find_king_pos('K', &r, &f)) {
            int x = 0;
            int y = 0;
            board_to_screen(view, r, f, &x, &y);
            SDL_Rect rect = {x, y, view->square, view->square};
            for (int i = 0; i < thickness; i++) {
                SDL_Rect r2 = {rect.x + i, rect.y + i, rect.w - 2 * i, rect.h - 2 * i};
                if (r2.w <= 0 || r2.h <= 0) break;
                SDL_RenderDrawRect(renderer, &r2);
            }
        }
    }
    if (is_in_check(0)) {
        int r = -1, f = -1;
        if (find_king_pos('k', &r, &f)) {
            int x = 0;
            int y = 0;
            board_to_screen(view, r, f, &x, &y);
            SDL_Rect rect = {x, y, view->square, view->square};
            for (int i = 0; i < thickness; i++) {
                SDL_Rect r2 = {rect.x + i, rect.y + i, rect.w - 2 * i, rect.h - 2 * i};
                if (r2.w <= 0 || r2.h <= 0) break;
                SDL_RenderDrawRect(renderer, &r2);
            }
        }
    }

    render_year_label(view);
    render_speed_label(view);
    render_fen_label(view);
    render_player_labels(view);
    render_guess_score(view);
    render_help_overlay(view);
    render_catalog_overlay(view);

    if (!suppress_present) {
        SDL_RenderPresent(renderer);
    }
}

void draw_board() {
    BoardView view;
    get_board_view(&view);
    render_board(&view, NULL);
}

int sign(int x) { return (x > 0) ? 1 : (x < 0) ? -1 : 0; }

int is_path_clear(int from_r, int from_f, int to_r, int to_f) {
    int dr = sign(to_r - from_r);
    int df = sign(to_f - from_f);
    int steps = (dr == 0) ? abs(to_f - from_f) : abs(to_r - from_r);
    for (int i = 1; i < steps; i++) {
        if (board[from_r + i * dr][from_f + i * df] != '.') return 0;
    }
    return 1;
}

int is_valid_move(char piece, int from_r, int from_f, int to_r, int to_f, int is_white, int capture) {
    int dr = abs(to_r - from_r);
    int df = abs(to_f - from_f);
    char p = toupper(piece);
    int dir = is_white ? -1 : 1;  // row direction (board[0] = rank 8)
    char at_to = board[to_r][to_f];
    int is_empty = (at_to == '.');
    int is_enemy = !is_empty && (is_white_piece(at_to) != is_white);

    int movement_valid = 0;

    switch (p) {
        case 'P':
            if (df == 0) {  // advance
                if (capture) return 0;
                if (!is_empty) return 0;
                if (dr == 1 && (to_r - from_r) == dir) movement_valid = 1;
                else if (dr == 2 && ((is_white && from_r == 6) || (!is_white && from_r == 1)) && (to_r - from_r) == 2 * dir)
                    movement_valid = is_path_clear(from_r, from_f, to_r, to_f);
            } else if (df == 1 && dr == 1 && (to_r - from_r) == dir) {
                if (capture && is_enemy) movement_valid = 1;
                else if (capture && is_empty) {  // en passant
                    int ep_rank = is_white ? 3 : 4;  // board row for own pawn rank 5/4 (white/black)
                    if (from_r == ep_rank && board[from_r][to_f] == (is_white ? 'p' : 'P')) movement_valid = 1;  // Check if enemy pawn is there for ep
                }
            }
            break;
        case 'N':
            movement_valid = ((dr == 1 && df == 2) || (dr == 2 && df == 1));
            break;
        case 'B':
            movement_valid = (dr == df && dr > 0 && is_path_clear(from_r, from_f, to_r, to_f));
            break;
        case 'R':
            movement_valid = ((dr == 0 || df == 0) && (dr + df > 0) && is_path_clear(from_r, from_f, to_r, to_f));
            break;
        case 'Q':
            movement_valid = (((dr == df) || (dr == 0 || df == 0)) && (dr + df > 0) && is_path_clear(from_r, from_f, to_r, to_f));
            break;
        case 'K':
            movement_valid = (dr <= 1 && df <= 1 && (dr + df > 0));
            break;
    }

    if (movement_valid) {
        if (capture) return is_enemy || (p == 'P' && is_empty && movement_valid);  // for ep
        else return is_empty;
    }
    return 0;
}

int piece_attacks_square(char piece, int from_r, int from_f, int to_r, int to_f, int is_white) {
    if (from_r == to_r && from_f == to_f) return 0;
    int dr = to_r - from_r;
    int df = to_f - from_f;
    int adr = abs(dr);
    int adf = abs(df);
    char p = toupper(piece);
    int dir = is_white ? -1 : 1;

    switch (p) {
        case 'P':
            return (adr == 1 && adf == 1 && dr == dir);
        case 'N':
            return (adr == 1 && adf == 2) || (adr == 2 && adf == 1);
        case 'B':
            return (adr == adf && adr > 0 && is_path_clear(from_r, from_f, to_r, to_f));
        case 'R':
            return ((adr == 0 || adf == 0) && (adr + adf > 0) &&
                    is_path_clear(from_r, from_f, to_r, to_f));
        case 'Q':
            return (((adr == adf) || (adr == 0 || adf == 0)) && (adr + adf > 0) &&
                    is_path_clear(from_r, from_f, to_r, to_f));
        case 'K':
            return (adr <= 1 && adf <= 1 && (adr + adf > 0));
    }
    return 0;
}

void draw_thick_line(int x1, int y1, int x2, int y2, int thickness, SDL_Color color) {
    if (thickness < 1) thickness = 1;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    float dx = (float)(x2 - x1);
    float dy = (float)(y2 - y1);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) {
        SDL_RenderDrawPoint(renderer, x1, y1);
        return;
    }
    float nx = -dy / len;
    float ny = dx / len;
    float half = (float)thickness * 0.5f;
    float ox = nx * half;
    float oy = ny * half;
    SDL_Vertex verts[4];
    verts[0].position.x = (float)x1 + ox;
    verts[0].position.y = (float)y1 + oy;
    verts[1].position.x = (float)x1 - ox;
    verts[1].position.y = (float)y1 - oy;
    verts[2].position.x = (float)x2 - ox;
    verts[2].position.y = (float)y2 - oy;
    verts[3].position.x = (float)x2 + ox;
    verts[3].position.y = (float)y2 + oy;
    for (int i = 0; i < 4; i++) {
        verts[i].color = color;
        verts[i].tex_coord.x = 0.0f;
        verts[i].tex_coord.y = 0.0f;
    }
    int indices[6] = {0, 1, 2, 2, 3, 0};
    SDL_RenderGeometry(renderer, NULL, verts, 4, indices, 6);
}

void render_defense_lines(const BoardView *view) {
    if (!show_defense_lines) return;
    SDL_Color white_line = {230, 230, 230, 70};
    SDL_Color black_line = {20, 20, 20, 70};
    int thickness = (view->square >= 70) ? 12 : (view->square >= 50 ? 10 : 8);

    for (int r1 = 0; r1 < BOARD_SIZE; r1++) {
        for (int f1 = 0; f1 < BOARD_SIZE; f1++) {
            char p = board[r1][f1];
            if (p == '.') continue;
            int is_white = is_white_piece(p);
            for (int r2 = 0; r2 < BOARD_SIZE; r2++) {
                for (int f2 = 0; f2 < BOARD_SIZE; f2++) {
                    if (r1 == r2 && f1 == f2) continue;
                    char target = board[r2][f2];
                    if (target == '.') continue;
                    if (is_white_piece(target) != is_white) continue;
                    if (!piece_attacks_square(p, r1, f1, r2, f2, is_white)) continue;

                    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                    board_to_screen(view, r1, f1, &x1, &y1);
                    board_to_screen(view, r2, f2, &x2, &y2);
                    x1 += view->square / 2;
                    y1 += view->square / 2;
                    x2 += view->square / 2;
                    y2 += view->square / 2;
                    draw_thick_line(x1, y1, x2, y2, thickness,
                                    is_white ? white_line : black_line);
                }
            }
        }
    }
}

int is_in_check(int is_white) {
    int king_r = -1, king_f = -1;
    char king = is_white ? 'K' : 'k';
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            if (board[r][f] == king) {
                king_r = r;
                king_f = f;
                break;
            }
        }
        if (king_r != -1) break;
    }
    if (king_r == -1) return 0;  // No king? Unlikely

    int opponent_is_white = !is_white;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            char p = board[r][f];
            if (p == '.' || (is_white_piece(p) != opponent_is_white)) continue;  // Not opponent piece
            if (is_valid_move(p, r, f, king_r, king_f, opponent_is_white, 1)) {
                return 1;
            }
        }
    }
    return 0;
}

typedef struct {
    int from_r, from_f, to_r, to_f;
    char promo;
} Move;

// Forward declaration
void apply_move(const Move *m, int is_white);

int parse_san(const char *san, int is_white, Move *m) {
    char clean_san[16];
    strcpy(clean_san, san);
    int len = strlen(clean_san);
    if (clean_san[len - 1] == '+' || clean_san[len - 1] == '#') clean_san[--len] = '\0';

    m->promo = '\0';

    if (strcmp(clean_san, "O-O") == 0 || strcmp(clean_san, "0-0") == 0) {
        m->from_r = is_white ? 7 : 0;
        m->from_f = 4;
        m->to_r = m->from_r;
        m->to_f = 6;
        return 1;
    } else if (strcmp(clean_san, "O-O-O") == 0 || strcmp(clean_san, "0-0-0") == 0) {
        m->from_r = is_white ? 7 : 0;
        m->from_f = 4;
        m->to_r = m->from_r;
        m->to_f = 2;
        return 1;
    }

    // Promotion handling
    char *eq = strchr(clean_san, '=');
    if (eq) {
        m->promo = *(eq + 1);
        *eq = '\0';
        len = strlen(clean_san);
    }

    if (len < 2) return 0;
    int to_f = clean_san[len - 2] - 'a';
    int to_r = clean_san[len - 1] - '1';
    to_r = 7 - to_r;  // Invert to match board index (0 = rank 8, 7 = rank 1)
    if (to_r < 0 || to_r >= 8 || to_f < 0 || to_f >= 8) return 0;
    m->to_r = to_r;
    m->to_f = to_f;

    char piece = 'P';
    int hint_start = 0;
    if (isupper(clean_san[0]) && strchr("RNBQK", clean_san[0])) {
        piece = clean_san[0];
        hint_start = 1;
    }

    int capture_pos = 0;
    for (int i = hint_start; i < len - 2; i++) {
        if (clean_san[i] == 'x') {
            capture_pos = i;
            break;
        }
    }
    int capture = (capture_pos > 0);

    int hint_len = (capture_pos > 0 ? capture_pos : len - 2) - hint_start;
    char hint_str[4] = {0};
    if (hint_len > 0) {
        strncpy(hint_str, clean_san + hint_start, hint_len);
    }

    int hl = strlen(hint_str);
    int hint_f = -1, hint_r = -1;
    if (hl == 1) {
        if (islower(hint_str[0])) hint_f = hint_str[0] - 'a';
        else if (isdigit(hint_str[0])) hint_r = 7 - (hint_str[0] - '1');
    } else if (hl == 2) {
        hint_f = hint_str[0] - 'a';
        hint_r = 7 - (hint_str[1] - '1');
    }

    char target_piece = is_white ? piece : tolower(piece);

    struct Candidate {
        int r, f;
    };
    struct Candidate candidates[16];
    int num_candidates = 0;

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            if (board[r][f] == target_piece) {
                if ((hint_f >= 0 && f != hint_f) || (hint_r >= 0 && r != hint_r)) continue;
                if (is_valid_move(target_piece, r, f, to_r, to_f, is_white, capture)) {
                    candidates[num_candidates].r = r;
                    candidates[num_candidates].f = f;
                    num_candidates++;
                }
            }
        }
    }

    if (num_candidates == 0) return 0;
    if (num_candidates == 1) {
        m->from_r = candidates[0].r;
        m->from_f = candidates[0].f;
        return 1;
    }

    // Resolve ambiguity with legality check (no self-check)
    for (int i = 0; i < num_candidates; i++) {
        char temp_board[BOARD_SIZE][BOARD_SIZE];
        memcpy(temp_board, board, sizeof(board));

        Move temp_m = *m;
        temp_m.from_r = candidates[i].r;
        temp_m.from_f = candidates[i].f;
        int save_wk = white_king_moved;
        int save_wa = white_rook_a_moved;
        int save_wh = white_rook_h_moved;
        int save_bk = black_king_moved;
        int save_ba = black_rook_a_moved;
        int save_bh = black_rook_h_moved;
        apply_move(&temp_m, is_white);  // Apply on temp
        if (!is_in_check(is_white)) {
            memcpy(board, temp_board, sizeof(board));  // Restore original board
            white_king_moved = save_wk;
            white_rook_a_moved = save_wa;
            white_rook_h_moved = save_wh;
            black_king_moved = save_bk;
            black_rook_a_moved = save_ba;
            black_rook_h_moved = save_bh;
            m->from_r = candidates[i].r;
            m->from_f = candidates[i].f;
            return 1;
        }
        memcpy(board, temp_board, sizeof(board));  // Restore original board
        white_king_moved = save_wk;
        white_rook_a_moved = save_wa;
        white_rook_h_moved = save_wh;
        black_king_moved = save_bk;
        black_rook_a_moved = save_ba;
        black_rook_h_moved = save_bh;
    }

    return 0;  // No legal move found
}

void apply_move(const Move *m, int is_white) {
    char piece = board[m->from_r][m->from_f];
    char captured = board[m->to_r][m->to_f];

    if (toupper(piece) == 'K') {
        if (is_white) {
            white_king_moved = 1;
        } else {
            black_king_moved = 1;
        }
    } else if (toupper(piece) == 'R') {
        if (is_white) {
            if (m->from_r == 7 && m->from_f == 0) white_rook_a_moved = 1;
            if (m->from_r == 7 && m->from_f == 7) white_rook_h_moved = 1;
        } else {
            if (m->from_r == 0 && m->from_f == 0) black_rook_a_moved = 1;
            if (m->from_r == 0 && m->from_f == 7) black_rook_h_moved = 1;
        }
    }

    if (captured == 'R') {
        if (m->to_r == 7 && m->to_f == 0) white_rook_a_moved = 1;
        if (m->to_r == 7 && m->to_f == 7) white_rook_h_moved = 1;
    } else if (captured == 'r') {
        if (m->to_r == 0 && m->to_f == 0) black_rook_a_moved = 1;
        if (m->to_r == 0 && m->to_f == 7) black_rook_h_moved = 1;
    }

    // En passant capture
    int dir = is_white ? -1 : 1;
    if (toupper(piece) == 'P' && abs(m->from_f - m->to_f) == 1 && captured == '.') {
        board[m->to_r - dir][m->to_f] = '.';  // Remove captured pawn
    }

    // Place the piece (with promotion if applicable)
    board[m->to_r][m->to_f] = m->promo ? (is_white ? toupper(m->promo) : tolower(m->promo)) : piece;
    board[m->from_r][m->from_f] = '.';

    // Castling: move rook
    if (toupper(piece) == 'K' && abs(m->from_f - m->to_f) == 2) {
        int rook_from = (m->to_f > m->from_f) ? 7 : 0;
        int rook_to = (m->to_f > m->from_f) ? 5 : 3;
        char rook = is_white ? 'R' : 'r';
        board[m->from_r][rook_to] = rook;
        board[m->from_r][rook_from] = '.';
        if (is_white) {
            if (rook_from == 0) white_rook_a_moved = 1;
            if (rook_from == 7) white_rook_h_moved = 1;
        } else {
            if (rook_from == 0) black_rook_a_moved = 1;
            if (rook_from == 7) black_rook_h_moved = 1;
        }
    }
}

int animate_move(const Move *m, int is_white) {
    char piece = board[m->from_r][m->from_f];
    if (piece == '.') return 0;
    int castling = 0;
    int rook_r = -1;
    int rook_from_f = -1;
    int rook_to_f = -1;
    char rook_piece = '.';

    BoardView view;
    get_board_view(&view);

    int start_x = 0;
    int start_y = 0;
    int end_x = 0;
    int end_y = 0;
    board_to_screen(&view, m->from_r, m->from_f, &start_x, &start_y);
    board_to_screen(&view, m->to_r, m->to_f, &end_x, &end_y);
    int rook_start_x = 0;
    int rook_start_y = 0;
    int rook_end_x = 0;
    int rook_end_y = 0;
    if (toupper(piece) == 'K' && abs(m->from_f - m->to_f) == 2) {
        rook_r = m->from_r;
        rook_from_f = (m->to_f > m->from_f) ? 7 : 0;
        rook_to_f = (m->to_f > m->from_f) ? 5 : 3;
        rook_piece = board[rook_r][rook_from_f];
        if (rook_piece != '.') {
            castling = 1;
            board[rook_r][rook_from_f] = '.';
            board_to_screen(&view, rook_r, rook_from_f, &rook_start_x, &rook_start_y);
            board_to_screen(&view, rook_r, rook_to_f, &rook_end_x, &rook_end_y);
        }
    }
    Uint32 start = SDL_GetTicks();
    int quit = 0;

    for (;;) {
        Uint32 loop_now = SDL_GetTicks();
        update_cursor_auto_hide(loop_now);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            note_mouse_activity_event(&e);
            if (handle_catalog_event(&e, games_dir_root)) {
                draw_board();
                if (game_nav_request == GAME_NAV_SELECT && catalog_selection_made) {
                    catalog_selection_made = 0;
                    quit = 1;
                    break;
                }
                continue;
            }
            if (e.type == SDL_QUIT) {
                quit = 1;
                break;
            } else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode key = e.key.keysym.sym;
                if (key == SDLK_q) {
                    game_nav_request = GAME_NAV_NONE;
                    quit = 1;
                    break;
                } else if (key == SDLK_n) {
                    game_nav_request = GAME_NAV_NEXT;
                    quit = 1;
                    break;
                } else if (key == SDLK_p) {
                    game_nav_request = GAME_NAV_PREV;
                    quit = 1;
                    break;
                } else if (key == SDLK_r) {
                    game_nav_request = GAME_NAV_RESTART;
                    quit = 1;
                    break;
                } else if (key == SDLK_c) {
                    catalog_open(games_dir_root);
                    draw_board();
                } else if (key == SDLK_e) {
                    show_elos = !show_elos;
                    draw_board();
                } else if (key == SDLK_u) {
                    uncolored_mode = !uncolored_mode;
                    draw_board();
                } else if (key == SDLK_d) {
                    show_defense_lines = !show_defense_lines;
                    draw_board();
                } else if (key == SDLK_s) {
                    save_fen_snapshot(FEN_SAVE_PATH);
                    draw_board();
                } else if (key == SDLK_ESCAPE) {
                    show_help = !show_help;
                } else if (key == SDLK_SPACE) {
                    pause_buffered = 1;
                } else if (key == SDLK_UP || key == SDLK_DOWN) {
                    Uint32 now = SDL_GetTicks();
                    int delta = (key == SDLK_UP) ? MOVE_DELAY_STEP_MS : -MOVE_DELAY_STEP_MS;
                    adjust_move_delay(delta, now);
                } else if (key == SDLK_f) {
                    view_from_white = !view_from_white;
                    get_board_view(&view);
                    board_to_screen(&view, m->from_r, m->from_f, &start_x, &start_y);
                    board_to_screen(&view, m->to_r, m->to_f, &end_x, &end_y);
                    if (castling) {
                        board_to_screen(&view, rook_r, rook_from_f, &rook_start_x, &rook_start_y);
                        board_to_screen(&view, rook_r, rook_to_f, &rook_end_x, &rook_end_y);
                    }
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_MIDDLE) {
                clear_analysis_marks();
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
                begin_mark_drag(&view, e.button.x, e.button.y);
            } else if (e.type == SDL_MOUSEMOTION) {
                if (e.motion.state & SDL_BUTTON_RMASK) {
                    update_mark_drag(&view, e.motion.x, e.motion.y);
                }
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) {
                end_mark_drag();
            }
        }
        if (quit) break;

        Uint32 now = SDL_GetTicks();
        float t = (MOVE_ANIM_MS > 0) ? (float)(now - start) / (float)MOVE_ANIM_MS : 1.0f;
        if (t > 1.0f) t = 1.0f;
        float te = t * t * (3.0f - 2.0f * t);

        Overlay overlay;
        overlay.active = 1;
        overlay.piece = piece;
        overlay.x = start_x + (end_x - start_x) * te;
        overlay.y = start_y + (end_y - start_y) * te;
        overlay.skip_r1 = m->from_r;
        overlay.skip_f1 = m->from_f;
        if (castling) {
            Overlay rook_overlay = {0};
            rook_overlay.active = 1;
            rook_overlay.piece = rook_piece;
            rook_overlay.x = rook_start_x + (rook_end_x - rook_start_x) * te;
            rook_overlay.y = rook_start_y + (rook_end_y - rook_start_y) * te;
            rook_overlay.skip_r1 = rook_r;
            rook_overlay.skip_f1 = rook_from_f;
            suppress_present = 1;
            render_board(&view, &overlay);
            suppress_present = 0;
            SDL_Texture *rook_tex = get_piece_texture(rook_piece);
            if (rook_tex) {
                SDL_Rect rect = {(int)(rook_overlay.x + 0.5f), (int)(rook_overlay.y + 0.5f),
                                 view.square, view.square};
                SDL_RenderCopy(renderer, rook_tex, NULL, &rect);
            }
            SDL_RenderPresent(renderer);
        } else {
            render_board(&view, &overlay);
        }

        if (t >= 1.0f) break;
        SDL_Delay(10);
    }
    if (castling) {
        board[rook_r][rook_from_f] = rook_piece;
    }
    if (quit) return 1;
    return 0;
}

void clean_line(char *line) {
    char *out = line;
    int in_comment = 0;
    int in_var = 0;
    while (*line) {
        if (*line == '{') in_comment = 1;
        else if (*line == '}') in_comment = 0;
        else if (*line == '(') in_var = 1;
        else if (*line == ')') in_var = 0;
        else if (*line == ';') {
            while (*line && *line != '\n') line++;
            continue;
        } else if (!in_comment && !in_var) {
            *out++ = *line;
        }
        line++;
    }
    *out = '\0';
}

int extract_san_token(const char *token, char *out, size_t out_size) {
    const char *p = token;

    if (isdigit((unsigned char)*p)) {
        const char *q = p;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == '.') {
            p = q;
            while (*p == '.') p++;
        }
    } else {
        while (*p == '.') p++;
    }

    if (*p == '\0') return 0;

    size_t len = strcspn(p, "!?");
    if (len == 0) return 0;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

int is_result_token(const char *san) {
    return (strcmp(san, "1-0") == 0 || strcmp(san, "0-1") == 0 ||
            strcmp(san, "1/2-1/2") == 0 || strcmp(san, "*") == 0);
}

int build_move_list(const char *move_buffer, char moves[][MOVE_TEXT_LEN], int max_moves,
                    char *result_out, size_t result_out_size) {
    char temp_buffer[65536];
    strncpy(temp_buffer, move_buffer, sizeof(temp_buffer) - 1);
    temp_buffer[sizeof(temp_buffer) - 1] = '\0';

    if (result_out && result_out_size > 0) {
        result_out[0] = '\0';
    }

    int count = 0;
    char *token = strtok(temp_buffer, " \t\n\r");
    while (token) {
        char san_buf[MOVE_TEXT_LEN];
        if (extract_san_token(token, san_buf, sizeof(san_buf))) {
            if (is_result_token(san_buf)) {
                if (result_out && result_out_size > 0) {
                    strncpy(result_out, san_buf, result_out_size - 1);
                    result_out[result_out_size - 1] = '\0';
                }
                break;
            }
            if (count < max_moves) {
                strncpy(moves[count], san_buf, MOVE_TEXT_LEN - 1);
                moves[count][MOVE_TEXT_LEN - 1] = '\0';
                count++;
            }
        }
        token = strtok(NULL, " \t\n\r");
    }
    return count;
}

int loser_from_result(const char *result, int *out_loser_is_white) {
    if (!result) return 0;
    if (strcmp(result, "1-0") == 0) {
        if (out_loser_is_white) *out_loser_is_white = 0;
        return 1;
    }
    if (strcmp(result, "0-1") == 0) {
        if (out_loser_is_white) *out_loser_is_white = 1;
        return 1;
    }
    return 0;
}

int is_draw_result(const char *result) {
    return (result && strcmp(result, "1/2-1/2") == 0);
}

void replay_moves_to_index(char moves[][MOVE_TEXT_LEN], int move_count, int index) {
    init_board();
    int is_white = 1;
    int limit = (index < move_count) ? index : move_count;
    for (int i = 0; i < limit; i++) {
        Move m = {0};
        if (!parse_san(moves[i], is_white, &m)) {
            printf("Failed to parse move: %s\n", moves[i]);
            break;
        }
        apply_move(&m, is_white);
        is_white = !is_white;
    }
    draw_board();
}

typedef struct {
    char *moves;
    char white[NAME_LEN];
    char black[NAME_LEN];
    char white_elo[NAME_LEN];
    char black_elo[NAME_LEN];
    char year[YEAR_LEN];
    char result[RESULT_LEN];
} Game;

typedef struct {
    char *path;
    int game_index;
} GameSelection;

char *copy_string(const char *s) {
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len + 1);
    return out;
}

int push_string(char ***items, int *count, int *cap, const char *value) {
    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 16 : (*cap * 2);
        char **new_items = (char **)realloc(*items, (size_t)new_cap * sizeof(char *));
        if (!new_items) return 0;
        *items = new_items;
        *cap = new_cap;
    }
    (*items)[*count] = copy_string(value);
    if (!(*items)[*count]) return 0;
    (*count)++;
    return 1;
}

void free_string_list(char **items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

int has_pgn_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    if (dot[1] == '\0' || dot[2] == '\0' || dot[3] == '\0' || dot[4] != '\0') return 0;
    return (tolower((unsigned char)dot[1]) == 'p' &&
            tolower((unsigned char)dot[2]) == 'g' &&
            tolower((unsigned char)dot[3]) == 'n');
}

char *join_path(const char *dir, const char *name) {
    size_t dir_len = strlen(dir);
    int need_sep = (dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\');
    size_t len = dir_len + (need_sep ? 1 : 0) + strlen(name) + 1;
    char *out = (char *)malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s%s%s", dir, need_sep ? PATH_SEP_STR : "", name);
    return out;
}

int list_pgn_files(const char *dir, char ***out_files) {
    char **files = NULL;
    int count = 0;
    int cap = 0;
    if (list_pgn_files_recursive(dir, dir, &files, &count, &cap) < 0) {
        free_string_list(files, count);
        return -1;
    }
    *out_files = files;
    return count;
}

int push_selection(GameSelection **history, int *count, int *cap, GameSelection sel) {
    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 8 : (*cap * 2);
        GameSelection *next = (GameSelection *)realloc(*history, new_cap * sizeof(*next));
        if (!next) return 0;
        *history = next;
        *cap = new_cap;
    }
    (*history)[*count] = sel;
    (*count)++;
    return 1;
}

int choose_random_selection(const char *games_dir, GameSelection *out_sel) {
    char **files = NULL;
    int file_count = list_pgn_files(games_dir, &files);
    if (file_count <= 0) {
        if (files && file_count > 0) free_string_list(files, file_count);
        return 0;
    }
    int file_index = rand() % file_count;
    char *path = join_path(games_dir, files[file_index]);
    free_string_list(files, file_count);
    if (!path) return 0;
    out_sel->path = path;
    out_sel->game_index = -1;
    return 1;
}

static int relpath_from_base(const char *base, const char *path, char *out, size_t out_size) {
    size_t base_len = strlen(base);
    const char *p = path;
    if (strncmp(path, base, base_len) == 0) {
        p = path + base_len;
        if (*p == '\\' || *p == '/') p++;
    }
    if (strlen(p) + 1 > out_size) return 0;
    strcpy(out, p);
    return 1;
}

static int list_pgn_files_recursive(const char *dir, const char *base,
                                    char ***out_files, int *count, int *cap) {
#ifdef _WIN32
    char *search = join_path(dir, "*");
    if (!search) return -1;
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(search, &data);
    free(search);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        char *full = join_path(dir, data.cFileName);
        if (!full) {
            FindClose(h);
            return -1;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (list_pgn_files_recursive(full, base, out_files, count, cap) < 0) {
                free(full);
                FindClose(h);
                return -1;
            }
        } else if (has_pgn_extension(data.cFileName)) {
            char relbuf[1024];
            if (!relpath_from_base(base, full, relbuf, sizeof(relbuf))) {
                free(full);
                FindClose(h);
                return -1;
            }
            if (!push_string(out_files, count, cap, relbuf)) {
                free(full);
                FindClose(h);
                return -1;
            }
        }
        free(full);
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char *full = join_path(dir, ent->d_name);
        if (!full) {
            closedir(d);
            return -1;
        }
        int is_dir = 0;
#ifdef DT_DIR
        if (ent->d_type == DT_DIR) is_dir = 1;
        if (ent->d_type == DT_UNKNOWN)
#endif
        {
            DIR *probe = opendir(full);
            if (probe) {
                is_dir = 1;
                closedir(probe);
            }
        }
        if (is_dir) {
            if (list_pgn_files_recursive(full, base, out_files, count, cap) < 0) {
                free(full);
                closedir(d);
                return -1;
            }
        } else if (has_pgn_extension(ent->d_name)) {
            char relbuf[1024];
            if (!relpath_from_base(base, full, relbuf, sizeof(relbuf))) {
                free(full);
                closedir(d);
                return -1;
            }
            if (!push_string(out_files, count, cap, relbuf)) {
                free(full);
                closedir(d);
                return -1;
            }
        }
        free(full);
    }
    closedir(d);
#endif
    return 0;
}

int parse_tag_value(const char *line, const char *tag, char *out, size_t out_size) {
    size_t tag_len = strlen(tag);
    if (strncmp(line, "[", 1) != 0) return 0;
    if (strncmp(line + 1, tag, tag_len) != 0) return 0;
    if (!isspace((unsigned char)line[1 + tag_len])) return 0;
    const char *first_quote = strchr(line, '"');
    if (!first_quote) return 0;
    const char *second_quote = strchr(first_quote + 1, '"');
    if (!second_quote) return 0;
    size_t len = (size_t)(second_quote - first_quote - 1);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, first_quote + 1, len);
    out[len] = '\0';
    return 1;
}

void extract_year(char *out, size_t out_size, const char *date) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!date || strlen(date) < 4) return;
    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)date[i])) return;
    }
    if (out_size < 5) return;
    memcpy(out, date, 4);
    out[4] = '\0';
}

void set_last_name(char *out, size_t out_size, const char *full) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!full) return;

    const char *start = full;
    while (isspace((unsigned char)*start)) start++;
    if (*start == '\0') return;

    const char *comma = strchr(start, ',');
    const char *name_start = start;
    const char *name_end = NULL;

    if (comma) {
        name_end = comma;
        while (name_end > name_start && isspace((unsigned char)name_end[-1])) {
            name_end--;
        }
    } else {
        const char *p = start;
        const char *last_word = start;
        while (*p) {
            while (isspace((unsigned char)*p)) p++;
            if (*p == '\0') break;
            last_word = p;
            while (*p && !isspace((unsigned char)*p)) p++;
        }
        name_start = last_word;
        name_end = name_start;
        while (*name_end && !isspace((unsigned char)*name_end)) name_end++;
    }

    size_t len = (size_t)(name_end - name_start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, name_start, len);
    out[len] = '\0';
}

int push_game(Game **games, int *count, int *cap, const char *move_buffer,
              const char *white, const char *black,
              const char *white_elo, const char *black_elo,
              const char *year, const char *result) {
    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 16 : (*cap * 2);
        Game *new_games = (Game *)realloc(*games, (size_t)new_cap * sizeof(Game));
        if (!new_games) return 0;
        *games = new_games;
        *cap = new_cap;
    }
    (*games)[*count].moves = copy_string(move_buffer);
    if (!(*games)[*count].moves) return 0;
    strncpy((*games)[*count].white, (white && white[0]) ? white : "White", NAME_LEN - 1);
    (*games)[*count].white[NAME_LEN - 1] = '\0';
    strncpy((*games)[*count].black, (black && black[0]) ? black : "Black", NAME_LEN - 1);
    (*games)[*count].black[NAME_LEN - 1] = '\0';
    strncpy((*games)[*count].white_elo, (white_elo && white_elo[0]) ? white_elo : "", NAME_LEN - 1);
    (*games)[*count].white_elo[NAME_LEN - 1] = '\0';
    strncpy((*games)[*count].black_elo, (black_elo && black_elo[0]) ? black_elo : "", NAME_LEN - 1);
    (*games)[*count].black_elo[NAME_LEN - 1] = '\0';
    strncpy((*games)[*count].year, (year && year[0]) ? year : "", YEAR_LEN - 1);
    (*games)[*count].year[YEAR_LEN - 1] = '\0';
    strncpy((*games)[*count].result, (result && result[0]) ? result : "", RESULT_LEN - 1);
    (*games)[*count].result[RESULT_LEN - 1] = '\0';
    (*count)++;
    return 1;
}

void free_games(Game *games, int count) {
    if (!games) return;
    for (int i = 0; i < count; i++) {
        free(games[i].moves);
    }
    free(games);
}

int load_games(FILE *fp, Game **out_games) {
    Game *games = NULL;
    int count = 0;
    int cap = 0;
    char line_chunk[512];
    char line[65536];
    size_t line_len = 0;
    char move_buffer[65536] = {0};
    char current_white[NAME_LEN] = "";
    char current_black[NAME_LEN] = "";
    char current_date[NAME_LEN] = "";
    char current_white_elo[NAME_LEN] = "";
    char current_black_elo[NAME_LEN] = "";
    char current_year[YEAR_LEN] = "";
    char current_result[RESULT_LEN] = "";
    int in_game = 0;

    line[0] = '\0';
    while (fgets(line_chunk, sizeof(line_chunk), fp)) {
        size_t chunk_len = strlen(line_chunk);
        int chunk_has_newline = (strchr(line_chunk, '\n') != NULL);
        size_t space_left = sizeof(line) - 1 - line_len;
        if (chunk_len > space_left) {
            chunk_len = space_left;
        }
        if (chunk_len > 0) {
            memcpy(line + line_len, line_chunk, chunk_len);
            line_len += chunk_len;
            line[line_len] = '\0';
        }

        if (!chunk_has_newline && !feof(fp) && line_len < sizeof(line) - 1) {
            continue;
        }

        clean_line(line);
        const char *trim = line;
        while (isspace((unsigned char)*trim)) trim++;
        if (strncmp(trim, "[Event", 6) == 0) {  // New game starts
            if (in_game && move_buffer[0] != '\0') {
                if (!push_game(&games, &count, &cap, move_buffer,
                               current_white, current_black,
                               current_white_elo, current_black_elo,
                               current_year, current_result)) goto error;
                move_buffer[0] = '\0';
            }
            current_white[0] = '\0';
            current_black[0] = '\0';
            current_date[0] = '\0';
            current_white_elo[0] = '\0';
            current_black_elo[0] = '\0';
            current_year[0] = '\0';
            current_result[0] = '\0';
            in_game = 1;
            line[0] = '\0';
            line_len = 0;
            continue;  // Skip header lines
        }
        if (in_game && trim[0] == '[') {
            parse_tag_value(trim, "White", current_white, sizeof(current_white));
            parse_tag_value(trim, "Black", current_black, sizeof(current_black));
            parse_tag_value(trim, "WhiteElo", current_white_elo, sizeof(current_white_elo));
            parse_tag_value(trim, "BlackElo", current_black_elo, sizeof(current_black_elo));
            if (parse_tag_value(trim, "Date", current_date, sizeof(current_date))) {
                extract_year(current_year, sizeof(current_year), current_date);
            }
            parse_tag_value(trim, "Result", current_result, sizeof(current_result));
            line[0] = '\0';
            line_len = 0;
            continue;
        }
        if (in_game && trim[0] != '[' && trim[0] != '\0') {
            if (move_buffer[0] != '\0') {
                strncat(move_buffer, " ", sizeof(move_buffer) - strlen(move_buffer) - 1);
            }
            strncat(move_buffer, trim, sizeof(move_buffer) - strlen(move_buffer) - 1);
        }

        line[0] = '\0';
        line_len = 0;
    }

    if (in_game && move_buffer[0] != '\0') {
        if (!push_game(&games, &count, &cap, move_buffer,
                       current_white, current_black,
                       current_white_elo, current_black_elo,
                       current_year, current_result)) goto error;
    }

    *out_games = games;
    return count;

error:
    free_games(games, count);
    *out_games = NULL;
    return -1;
}

void shuffle_games(Game *games, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Game tmp = games[i];
        games[i] = games[j];
        games[j] = tmp;
    }
}

int play_game(const char *move_buffer, const char *header_result) {
    char moves[MAX_MOVES][MOVE_TEXT_LEN];
    char result_buf[RESULT_LEN];
    int move_count = build_move_list(move_buffer, moves, MAX_MOVES, result_buf, sizeof(result_buf));
    const char *result = (result_buf[0] != '\0') ? result_buf : header_result;
    int has_loser = 0;
    int loser_is_white_local = 0;
    int is_draw = 0;
    if (result && result[0] != '\0') {
        has_loser = loser_from_result(result, &loser_is_white_local);
        is_draw = is_draw_result(result);
    }

    init_board();
    clear_analysis_marks();
    draw_board();

    int index = 0;
    int paused = 0;
    int quit = 0;
    Uint32 last_move_tick = SDL_GetTicks();
    show_loser_king = 0;
    show_draw_kings = 0;
    dim_board = 0;
    pause_buffered = 0;
    game_nav_request = GAME_NAV_NONE;
    int analysis_dragging = 0;
    char analysis_piece = '.';
    int analysis_from_r = -1;
    int analysis_from_f = -1;
    int analysis_mouse_x = 0;
    int analysis_mouse_y = 0;
    int guess_dragging = 0;
    char guess_piece = '.';
    int guess_from_r = -1;
    int guess_from_f = -1;
    int guess_mouse_x = 0;
    int guess_mouse_y = 0;
    int guess_pending = 0;
    int guess_to_r = -1;
    int guess_to_f = -1;
    guess_score = 0;
    Uint32 force_redraw_until = 0;

    while (!quit) {
        Uint32 loop_now = SDL_GetTicks();
        update_cursor_auto_hide(loop_now);
        turn_is_white = (index % 2 == 0);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            note_mouse_activity_event(&e);
            if (handle_catalog_event(&e, games_dir_root)) {
                draw_board();
        if (game_nav_request == GAME_NAV_SELECT && catalog_selection_made) {
            catalog_selection_made = 0;
            quit = 1;
        }
                continue;
            }
            if (e.type == SDL_QUIT) {
                quit = 1;
            } else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode key = e.key.keysym.sym;
                if (key == SDLK_q) {
                    game_nav_request = GAME_NAV_NONE;
                    quit = 1;
                } else if (key == SDLK_n) {
                    game_nav_request = GAME_NAV_NEXT;
                    quit = 1;
                } else if (key == SDLK_p) {
                    game_nav_request = GAME_NAV_PREV;
                    quit = 1;
                } else if (key == SDLK_r) {
                    game_nav_request = GAME_NAV_RESTART;
                    quit = 1;
                } else if (key == SDLK_c) {
                    catalog_open(games_dir_root);
                    draw_board();
                } else if (key == SDLK_e) {
                    show_elos = !show_elos;
                    draw_board();
                } else if (key == SDLK_u) {
                    uncolored_mode = !uncolored_mode;
                    draw_board();
                } else if (key == SDLK_d) {
                    show_defense_lines = !show_defense_lines;
                    draw_board();
                } else if (key == SDLK_s) {
                    save_fen_snapshot(FEN_SAVE_PATH);
                    draw_board();
                } else if (key == SDLK_ESCAPE) {
                    show_help = !show_help;
                    draw_board();
                } else if (key == SDLK_UP || key == SDLK_DOWN) {
                    Uint32 now = SDL_GetTicks();
                    int prev = move_delay_ms;
                    int delta = (key == SDLK_UP) ? MOVE_DELAY_STEP_MS : -MOVE_DELAY_STEP_MS;
                    if (adjust_move_delay(delta, now)) {
                        if (!paused && move_delay_ms > prev) {
                            last_move_tick = now;
                        }
                        draw_board();
                    }
                } else if (key == SDLK_a) {
                    if (analysis_mode) {
                        exit_analysis_mode();
                        analysis_dragging = 0;
                        analysis_piece = '.';
                    } else if (guess_mode) {
                        guess_mode = 0;
                        guess_dragging = 0;
                        guess_pending = 0;
                        enter_analysis_mode();
                    } else {
                        enter_analysis_mode();
                        analysis_dragging = 0;
                        analysis_piece = '.';
                    }
                    draw_board();
                } else if (key == SDLK_g) {
                    if (guess_mode) {
                        guess_mode = 0;
                        guess_dragging = 0;
                        guess_pending = 0;
                    } else {
                        if (analysis_mode) {
                            exit_analysis_mode();
                            analysis_dragging = 0;
                            analysis_piece = '.';
                        }
                        guess_mode = 1;
                        guess_dragging = 0;
                        guess_pending = 0;
                    }
                    dim_board = 0;
                    paused = 0;
                    pause_buffered = 0;
                    draw_board();
                } else if (key == SDLK_SPACE) {
                    if (!analysis_mode && !guess_mode) {
                        paused = !paused;
                        dim_board = paused;
                        last_move_tick = SDL_GetTicks();
                        draw_board();
                    }
                } else if (key == SDLK_f) {
                    view_from_white = !view_from_white;
                    draw_board();
                } else if (!analysis_mode && !guess_mode && paused && key == SDLK_LEFT) {
                    if (index > 0) {
                        index--;
                        replay_moves_to_index(moves, move_count, index);
                    }
                } else if (!analysis_mode && !guess_mode && paused && key == SDLK_RIGHT) {
                    if (index < move_count) {
                        index++;
                        replay_moves_to_index(moves, move_count, index);
                    }
                }
            } else if (analysis_mode && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                BoardView view;
                get_board_view(&view);
                int r = -1;
                int f = -1;
                if (screen_to_board(&view, e.button.x, e.button.y, &r, &f)) {
                    if (board[r][f] != '.') {
                        analysis_dragging = 1;
                        analysis_piece = board[r][f];
                        analysis_from_r = r;
                        analysis_from_f = f;
                        analysis_mouse_x = e.button.x;
                        analysis_mouse_y = e.button.y;
                        board[r][f] = '.';
                    }
                }
            } else if (guess_mode && !analysis_mode && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                BoardView view;
                get_board_view(&view);
                int r = -1;
                int f = -1;
                if (screen_to_board(&view, e.button.x, e.button.y, &r, &f)) {
                    if (board[r][f] != '.') {
                        int is_white_turn = (index % 2 == 0);
                        if (is_white_piece(board[r][f]) == is_white_turn) {
                            guess_dragging = 1;
                            guess_piece = board[r][f];
                            guess_from_r = r;
                            guess_from_f = f;
                            guess_mouse_x = e.button.x;
                            guess_mouse_y = e.button.y;
                        }
                    }
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_MIDDLE) {
                clear_analysis_marks();
                if (!analysis_mode || !analysis_dragging) {
                    draw_board();
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
                BoardView view;
                get_board_view(&view);
                if (begin_mark_drag(&view, e.button.x, e.button.y) && !analysis_mode) {
                    draw_board();
                }
            } else if (e.type == SDL_MOUSEMOTION) {
                if (e.motion.state & SDL_BUTTON_RMASK) {
                    BoardView view;
                    get_board_view(&view);
                    if (update_mark_drag(&view, e.motion.x, e.motion.y) && !analysis_mode) {
                        draw_board();
                    }
                }
                if (analysis_mode && analysis_dragging) {
                    analysis_mouse_x = e.motion.x;
                    analysis_mouse_y = e.motion.y;
                }
                if (guess_mode && guess_dragging) {
                    guess_mouse_x = e.motion.x;
                    guess_mouse_y = e.motion.y;
                }
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) {
                end_mark_drag();
            } else if (analysis_mode && e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                if (analysis_dragging) {
                    analysis_mouse_x = e.button.x;
                    analysis_mouse_y = e.button.y;
                    BoardView view;
                    get_board_view(&view);
                    int r = -1;
                    int f = -1;
                    if (screen_to_board(&view, analysis_mouse_x, analysis_mouse_y, &r, &f)) {
                        board[r][f] = analysis_piece;
                    } else {
                        board[analysis_from_r][analysis_from_f] = analysis_piece;
                    }
                    analysis_dragging = 0;
                    analysis_piece = '.';
                }
            } else if (guess_mode && !analysis_mode && e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                if (guess_dragging) {
                    guess_mouse_x = e.button.x;
                    guess_mouse_y = e.button.y;
                    BoardView view;
                    get_board_view(&view);
                    if (screen_to_board(&view, guess_mouse_x, guess_mouse_y, &guess_to_r, &guess_to_f)) {
                        if (guess_to_r != guess_from_r || guess_to_f != guess_from_f) {
                            guess_pending = 1;
                        }
                    }
                    guess_dragging = 0;
                }
            }
        }
        if (quit) break;

        Uint32 speed_now = SDL_GetTicks();
        if (!analysis_mode && speed_message_until != 0 && speed_now >= speed_message_until) {
            speed_message_until = 0;
            draw_board();
        }
        if (!analysis_mode && fen_message_until != 0 && speed_now >= fen_message_until) {
            fen_message_until = 0;
            draw_board();
        }

        if (catalog_active) {
            BoardView view;
            get_board_view(&view);
            render_board(&view, NULL);
            SDL_Delay(10);
            continue;
        }

        if (!analysis_mode && !guess_mode && force_redraw_until != 0) {
            Uint32 now = SDL_GetTicks();
            if (now <= force_redraw_until) {
                draw_board();
            } else {
                force_redraw_until = 0;
            }
        }

        if (guess_pending && index < move_count) {
            int is_white = (index % 2 == 0);
            Move expected = {0};
            if (parse_san(moves[index], is_white, &expected)) {
                if (expected.from_r == guess_from_r && expected.from_f == guess_from_f &&
                    expected.to_r == guess_to_r && expected.to_f == guess_to_f) {
                    guess_score++;
                } else {
                    guess_score--;
                }
                if (animate_move(&expected, is_white)) {
                    quit = 1;
                    break;
                }
                apply_move(&expected, is_white);
                index++;
                turn_is_white = (index % 2 == 0);
                last_move_tick = SDL_GetTicks();
                force_redraw_until = last_move_tick + 120;
                draw_board();
            } else {
                printf("Failed to parse move: %s\n", moves[index]);
            }
            guess_pending = 0;
        }

        if (analysis_mode) {
            BoardView view;
            get_board_view(&view);
            Overlay overlay = {0};
            if (analysis_dragging) {
                overlay.active = 1;
                overlay.piece = analysis_piece;
                overlay.x = (float)analysis_mouse_x - (float)view.square * 0.5f;
                overlay.y = (float)analysis_mouse_y - (float)view.square * 0.5f;
                overlay.skip_r1 = analysis_from_r;
                overlay.skip_f1 = analysis_from_f;
            }
            render_board(&view, analysis_dragging ? &overlay : NULL);
            SDL_Delay(10);
            continue;
        }

        if (guess_mode) {
            BoardView view;
            get_board_view(&view);
            Overlay overlay = {0};
            if (guess_dragging) {
                overlay.active = 1;
                overlay.piece = guess_piece;
                overlay.x = (float)guess_mouse_x - (float)view.square * 0.5f;
                overlay.y = (float)guess_mouse_y - (float)view.square * 0.5f;
                overlay.skip_r1 = guess_from_r;
                overlay.skip_f1 = guess_from_f;
                render_board(&view, &overlay);
            } else {
                render_board(&view, NULL);
            }
            SDL_Delay(10);
            continue;
        }

        if (!paused && index < move_count) {
            Uint32 now = SDL_GetTicks();
            if (now - last_move_tick >= (Uint32)move_delay_ms) {
                int is_white = (index % 2 == 0);
                Move m = {0};
                if (parse_san(moves[index], is_white, &m)) {
                    if (animate_move(&m, is_white)) {
                        quit = 1;
                        break;
                    }
                    if (pause_buffered) {
                        paused = 1;
                        dim_board = 1;
                        pause_buffered = 0;
                        last_move_tick = SDL_GetTicks();
                    }
                    apply_move(&m, is_white);
                    draw_board();
                    force_redraw_until = SDL_GetTicks() + 120;
                } else {
                    printf("Failed to parse move: %s\n", moves[index]);
                }
                index++;
                turn_is_white = (index % 2 == 0);
                last_move_tick = now;
            }
        } else if (index >= move_count) {
            break;
        }

        SDL_Delay(10);
    }

    int pause_ms = 2000;
    Uint32 pause_start = SDL_GetTicks();
    dim_board = 0;
    if (!quit && index >= move_count) {
        pause_ms = GAME_OVER_PAUSE_MS;
        if (has_loser) {
            show_loser_king = 1;
            loser_is_white = loser_is_white_local;
            loser_king_angle = 0.0f;
            Uint32 flip_start = SDL_GetTicks();
            for (;;) {
                Uint32 loop_now = SDL_GetTicks();
                update_cursor_auto_hide(loop_now);
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    note_mouse_activity_event(&e);
                    if (handle_catalog_event(&e, games_dir_root)) {
                        draw_board();
                        if (game_nav_request == GAME_NAV_SELECT && catalog_selection_made) {
                            catalog_selection_made = 0;
                            quit = 1;
                        }
                        continue;
                    }
                    if (e.type == SDL_QUIT) {
                        quit = 1;
                    } else if (e.type == SDL_KEYDOWN) {
                        SDL_Keycode key = e.key.keysym.sym;
                        if (key == SDLK_q) {
                            game_nav_request = GAME_NAV_NONE;
                            quit = 1;
                        } else if (key == SDLK_n) {
                            game_nav_request = GAME_NAV_NEXT;
                            quit = 1;
                        } else if (key == SDLK_p) {
                            game_nav_request = GAME_NAV_PREV;
                            quit = 1;
                        } else if (key == SDLK_r) {
                            game_nav_request = GAME_NAV_RESTART;
                            quit = 1;
                        } else if (key == SDLK_c) {
                            catalog_open(games_dir_root);
                            draw_board();
                        } else if (key == SDLK_e) {
                            show_elos = !show_elos;
                            draw_board();
                        } else if (key == SDLK_u) {
                            uncolored_mode = !uncolored_mode;
                            draw_board();
                        } else if (key == SDLK_d) {
                            show_defense_lines = !show_defense_lines;
                            draw_board();
                        } else if (key == SDLK_s) {
                            save_fen_snapshot(FEN_SAVE_PATH);
                            draw_board();
                        } else if (key == SDLK_ESCAPE) {
                            show_help = !show_help;
                        } else if (key == SDLK_UP || key == SDLK_DOWN) {
                            Uint32 tick_now = SDL_GetTicks();
                            int delta = (key == SDLK_UP) ? MOVE_DELAY_STEP_MS : -MOVE_DELAY_STEP_MS;
                            adjust_move_delay(delta, tick_now);
                        } else if (key == SDLK_f) {
                            view_from_white = !view_from_white;
                            draw_board();
                        }
                    } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_MIDDLE) {
                        clear_analysis_marks();
                    }
                }
                if (quit) break;

                Uint32 now = SDL_GetTicks();
                float t = (KING_FLIP_MS > 0) ? (float)(now - flip_start) / (float)KING_FLIP_MS : 1.0f;
                if (t > 1.0f) t = 1.0f;
                loser_king_angle = 180.0f * t;
                draw_board();
                if (t >= 1.0f) break;
                SDL_Delay(10);
            }
        } else if (is_draw) {
            show_draw_kings = 1;
            draw_king_angle = 0.0f;
            Uint32 tilt_start = SDL_GetTicks();
            for (;;) {
                Uint32 loop_now = SDL_GetTicks();
                update_cursor_auto_hide(loop_now);
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    note_mouse_activity_event(&e);
                    if (handle_catalog_event(&e, games_dir_root)) {
                        draw_board();
                        if (game_nav_request == GAME_NAV_SELECT && catalog_selection_made) {
                            catalog_selection_made = 0;
                            quit = 1;
                        }
                        continue;
                    }
                    if (e.type == SDL_QUIT) {
                        quit = 1;
                    } else if (e.type == SDL_KEYDOWN) {
                        SDL_Keycode key = e.key.keysym.sym;
                        if (key == SDLK_q) {
                            game_nav_request = GAME_NAV_NONE;
                            quit = 1;
                        } else if (key == SDLK_n) {
                            game_nav_request = GAME_NAV_NEXT;
                            quit = 1;
                        } else if (key == SDLK_p) {
                            game_nav_request = GAME_NAV_PREV;
                            quit = 1;
                        } else if (key == SDLK_r) {
                            game_nav_request = GAME_NAV_RESTART;
                            quit = 1;
                        } else if (key == SDLK_c) {
                            catalog_open(games_dir_root);
                            draw_board();
                        } else if (key == SDLK_e) {
                            show_elos = !show_elos;
                            draw_board();
                        } else if (key == SDLK_u) {
                            uncolored_mode = !uncolored_mode;
                            draw_board();
                        } else if (key == SDLK_d) {
                            show_defense_lines = !show_defense_lines;
                            draw_board();
                        } else if (key == SDLK_s) {
                            save_fen_snapshot(FEN_SAVE_PATH);
                            draw_board();
                        } else if (key == SDLK_ESCAPE) {
                            show_help = !show_help;
                        } else if (key == SDLK_UP || key == SDLK_DOWN) {
                            Uint32 tick_now = SDL_GetTicks();
                            int delta = (key == SDLK_UP) ? MOVE_DELAY_STEP_MS : -MOVE_DELAY_STEP_MS;
                            adjust_move_delay(delta, tick_now);
                        } else if (key == SDLK_f) {
                            view_from_white = !view_from_white;
                            draw_board();
                        }
                    } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_MIDDLE) {
                        clear_analysis_marks();
                    }
                }
                if (quit) break;

                Uint32 now = SDL_GetTicks();
                float t = (KING_FLIP_MS > 0) ? (float)(now - tilt_start) / (float)KING_FLIP_MS : 1.0f;
                if (t > 1.0f) t = 1.0f;
                draw_king_angle = 90.0f * t;
                draw_board();
                if (t >= 1.0f) break;
                SDL_Delay(10);
            }
        }
    }

    int pause_hold = 0;
    Uint32 pause_hold_start = 0;
    Uint32 pause_hold_total = 0;
    int review_index = index;
    while (!quit) {
        Uint32 now = SDL_GetTicks();
        update_cursor_auto_hide(now);
        if (!analysis_mode && !pause_hold && now - pause_start - pause_hold_total >= (Uint32)pause_ms) {
            break;
        }
        if (!analysis_mode && speed_message_until != 0 && now >= speed_message_until) {
            speed_message_until = 0;
            draw_board();
        }
        if (!analysis_mode && fen_message_until != 0 && now >= fen_message_until) {
            fen_message_until = 0;
            draw_board();
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            note_mouse_activity_event(&e);
            if (handle_catalog_event(&e, games_dir_root)) {
                draw_board();
                if (game_nav_request == GAME_NAV_SELECT && catalog_selection_made) {
                    catalog_selection_made = 0;
                    quit = 1;
                }
                continue;
            }
            if (e.type == SDL_QUIT) {
                quit = 1;
            } else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode key = e.key.keysym.sym;
                if (key == SDLK_q) {
                    game_nav_request = GAME_NAV_NONE;
                    quit = 1;
                } else if (key == SDLK_n) {
                    game_nav_request = GAME_NAV_NEXT;
                    quit = 1;
                } else if (key == SDLK_p) {
                    game_nav_request = GAME_NAV_PREV;
                    quit = 1;
                } else if (key == SDLK_r) {
                    game_nav_request = GAME_NAV_RESTART;
                    quit = 1;
                } else if (key == SDLK_c) {
                    catalog_open(games_dir_root);
                    draw_board();
                } else if (key == SDLK_e) {
                    show_elos = !show_elos;
                    draw_board();
                } else if (key == SDLK_u) {
                    uncolored_mode = !uncolored_mode;
                    draw_board();
                } else if (key == SDLK_d) {
                    show_defense_lines = !show_defense_lines;
                    draw_board();
                } else if (key == SDLK_s) {
                    save_fen_snapshot(FEN_SAVE_PATH);
                    draw_board();
                } else if (key == SDLK_ESCAPE) {
                    show_help = !show_help;
                    draw_board();
                } else if (key == SDLK_UP || key == SDLK_DOWN) {
                    Uint32 tick_now = SDL_GetTicks();
                    int delta = (key == SDLK_UP) ? MOVE_DELAY_STEP_MS : -MOVE_DELAY_STEP_MS;
                    if (adjust_move_delay(delta, tick_now)) {
                        draw_board();
                    }
                } else if (key == SDLK_a) {
                    if (analysis_mode) {
                        exit_analysis_mode();
                        analysis_dragging = 0;
                        analysis_piece = '.';
                        pause_start = now;
                        pause_hold_total = 0;
                        if (pause_hold) {
                            pause_hold_start = now;
                        }
                    } else {
                        enter_analysis_mode();
                        analysis_dragging = 0;
                        analysis_piece = '.';
                    }
                    draw_board();
                } else if (!analysis_mode && key == SDLK_SPACE) {
                    if (!pause_hold) {
                        pause_hold = 1;
                        pause_hold_start = now;
                        dim_board = 1;
                        draw_board();
                    } else {
                        pause_hold = 0;
                        pause_hold_total += now - pause_hold_start;
                        dim_board = 0;
                        draw_board();
                    }
                } else if (key == SDLK_f) {
                    view_from_white = !view_from_white;
                    draw_board();
                } else if (!analysis_mode && key == SDLK_LEFT) {
                    if (review_index > 0) {
                        review_index--;
                        show_loser_king = 0;
                        show_draw_kings = 0;
                        replay_moves_to_index(moves, move_count, review_index);
                    }
                } else if (!analysis_mode && key == SDLK_RIGHT) {
                    if (review_index < move_count) {
                        review_index++;
                        show_loser_king = 0;
                        show_draw_kings = 0;
                        replay_moves_to_index(moves, move_count, review_index);
                    }
                }
            } else if (analysis_mode && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                BoardView view;
                get_board_view(&view);
                int r = -1;
                int f = -1;
                if (screen_to_board(&view, e.button.x, e.button.y, &r, &f)) {
                    if (board[r][f] != '.') {
                        analysis_dragging = 1;
                        analysis_piece = board[r][f];
                        analysis_from_r = r;
                        analysis_from_f = f;
                        analysis_mouse_x = e.button.x;
                        analysis_mouse_y = e.button.y;
                        board[r][f] = '.';
                    }
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_MIDDLE) {
                clear_analysis_marks();
                if (!analysis_mode || !analysis_dragging) {
                    draw_board();
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
                BoardView view;
                get_board_view(&view);
                if (begin_mark_drag(&view, e.button.x, e.button.y) && !analysis_mode) {
                    draw_board();
                }
            } else if (e.type == SDL_MOUSEMOTION) {
                if (e.motion.state & SDL_BUTTON_RMASK) {
                    BoardView view;
                    get_board_view(&view);
                    if (update_mark_drag(&view, e.motion.x, e.motion.y) && !analysis_mode) {
                        draw_board();
                    }
                }
                if (analysis_mode && analysis_dragging) {
                    analysis_mouse_x = e.motion.x;
                    analysis_mouse_y = e.motion.y;
                }
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) {
                end_mark_drag();
            } else if (analysis_mode && e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                if (analysis_dragging) {
                    analysis_mouse_x = e.button.x;
                    analysis_mouse_y = e.button.y;
                    BoardView view;
                    get_board_view(&view);
                    int r = -1;
                    int f = -1;
                    if (screen_to_board(&view, analysis_mouse_x, analysis_mouse_y, &r, &f)) {
                        board[r][f] = analysis_piece;
                    } else {
                        board[analysis_from_r][analysis_from_f] = analysis_piece;
                    }
                    analysis_dragging = 0;
                    analysis_piece = '.';
                }
            }
        }
        if (analysis_mode) {
            BoardView view;
            get_board_view(&view);
            Overlay overlay = {0};
            if (analysis_dragging) {
                overlay.active = 1;
                overlay.piece = analysis_piece;
                overlay.x = (float)analysis_mouse_x - (float)view.square * 0.5f;
                overlay.y = (float)analysis_mouse_y - (float)view.square * 0.5f;
                overlay.skip_r1 = analysis_from_r;
                overlay.skip_f1 = analysis_from_f;
            }
            render_board(&view, analysis_dragging ? &overlay : NULL);
            SDL_Delay(10);
            continue;
        }
        if (!quit && review_index == move_count) {
            if (has_loser) {
                show_loser_king = 1;
                show_draw_kings = 0;
                draw_board();
            } else if (is_draw) {
                show_draw_kings = 1;
                show_loser_king = 0;
                draw_board();
            }
        }
        SDL_Delay(10);
    }
    show_loser_king = 0;
    show_draw_kings = 0;
    dim_board = 0;
    return quit;
}

int main(int argc, char *argv[]) {
    const char *games_dir = DEFAULT_GAMES_DIR;
    (void)argc;
    (void)argv;
    games_dir_root = games_dir;

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || !(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
        printf("SDL init error: %s\n", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("Chess Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              SCREEN_SIZE, SCREEN_SIZE, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window || !renderer) {
        printf("SDL window/renderer error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    analysis_cursor = create_analysis_cursor();
    if (!analysis_cursor) {
        analysis_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    }
    set_cursor_visible(1);
    note_mouse_activity(SDL_GetTicks());

    srand((unsigned int)time(NULL));

    GameSelection *history = NULL;
    int history_count = 0;
    int history_cap = 0;
    int history_pos = -1;
    int need_new_selection = 1;
    int keep_view = 0;
    int quit = 0;
    while (!quit) {
        if (need_new_selection) {
            GameSelection sel = {0};
            if (forced_pgn_path) {
                sel.path = copy_string(forced_pgn_path);
                sel.game_index = -1;
            } else {
                if (!choose_random_selection(games_dir, &sel)) {
                    printf("No PGN files found in %s\n", games_dir);
                    break;
                }
            }
            if (!sel.path) {
                printf("No PGN files found in %s\n", games_dir);
                break;
            }
            if (!push_selection(&history, &history_count, &history_cap, sel)) {
                free(sel.path);
                break;
            }
            history_pos = history_count - 1;
            need_new_selection = 0;
            keep_view = 0;
        }

        if (history_pos < 0 || history_pos >= history_count) {
            need_new_selection = 1;
            continue;
        }

        GameSelection *sel = &history[history_pos];
        FILE *fp = fopen(sel->path, "r");
        if (!fp) {
            printf("Failed to open %s\n", sel->path);
            SDL_Delay(500);
            need_new_selection = 1;
            continue;
        }

        Game *games = NULL;
        int game_count = load_games(fp, &games);
        fclose(fp);
        if (game_count <= 0) {
            if (game_count < 0) {
                printf("Failed to load games from PGN.\n");
            }
            free_games(games, game_count);
            SDL_Delay(500);
            need_new_selection = 1;
            continue;
        }

        if (sel->game_index < 0 || sel->game_index >= game_count) {
            sel->game_index = rand() % game_count;
        }
        int game_index = sel->game_index;
        set_last_name(current_white_name, sizeof(current_white_name), games[game_index].white);
        if (current_white_name[0] == '\0') {
            strncpy(current_white_name, games[game_index].white, NAME_LEN - 1);
            current_white_name[NAME_LEN - 1] = '\0';
        }
        set_last_name(current_black_name, sizeof(current_black_name), games[game_index].black);
        if (current_black_name[0] == '\0') {
            strncpy(current_black_name, games[game_index].black, NAME_LEN - 1);
            current_black_name[NAME_LEN - 1] = '\0';
        }
        strncpy(current_white_elo, games[game_index].white_elo, NAME_LEN - 1);
        current_white_elo[NAME_LEN - 1] = '\0';
        strncpy(current_black_elo, games[game_index].black_elo, NAME_LEN - 1);
        current_black_elo[NAME_LEN - 1] = '\0';
        strncpy(current_game_year, games[game_index].year, YEAR_LEN - 1);
        current_game_year[YEAR_LEN - 1] = '\0';
        if (!keep_view) {
            view_from_white = (rand() % 2) ? 1 : 0;
        }
        keep_view = 0;
        int stop = play_game(games[game_index].moves, games[game_index].result);
        free_games(games, game_count);

        int nav = game_nav_request;
        game_nav_request = GAME_NAV_NONE;
        if (stop && nav == GAME_NAV_NONE) {
            quit = 1;
            break;
        }
        if (nav == GAME_NAV_SELECT) {
            for (int i = 0; i < history_count; i++) {
                free(history[i].path);
            }
            history_cap = 0;
            free(history);
            history = NULL;
            history_count = 0;
            history_pos = -1;
            need_new_selection = 1;
            continue;
        }
        if (nav == GAME_NAV_NEXT) {
            if (history_pos < history_count - 1) {
                history_pos++;
            } else {
                need_new_selection = 1;
            }
        } else if (nav == GAME_NAV_PREV) {
            if (history_pos > 0) {
                history_pos--;
            }
        } else if (nav == GAME_NAV_RESTART) {
            keep_view = 1;
        } else if (!stop) {
            if (history_pos < history_count - 1) {
                history_pos++;
            } else {
                need_new_selection = 1;
            }
        }
    }

    for (int i = 0; i < history_count; i++) {
        free(history[i].path);
    }
    free(history);
    catalog_free();
    free(forced_pgn_path);

    // Cleanup
    for (int i = 0; i < 256; i++) {
        if (piece_textures[i]) SDL_DestroyTexture(piece_textures[i]);
    }
    if (analysis_cursor) {
        SDL_FreeCursor(analysis_cursor);
        analysis_cursor = NULL;
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();

    return 0;
}
