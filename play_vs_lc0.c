#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#define BOARD_SIZE 8
#define SCREEN_SIZE 800
#define MAX_HISTORY 1024
#define STATUS_LEN 256
#define ENGINE_LINE_LEN 4096
#define ENGINE_TIMEOUT_MS 30000
#define ENGINE_MOVE_TIME_MS 3000
#define ENGINE_MOVE_TIME_JITTER_MS 700
#define ENGINE_MOVE_ANIM_MS 260
#define ENGINE_VARIETY_MULTI_PV 4
#define ENGINE_VARIETY_CP_WINDOW_OPENING 45
#define ENGINE_VARIETY_CP_WINDOW_LATE 18
#define ENGINE_VARIETY_OPENING_PLIES 24
#define HINT_MOVETIME_MS 1400
#define HINT_MULTI_PV 3
#define HINT_DISPLAY_MS 12000

typedef struct {
    int square;
    int board_px;
    int offset_x;
    int offset_y;
    int screen_w;
    int screen_h;
} BoardView;

typedef struct {
    int from_r;
    int from_f;
    int to_r;
    int to_f;
    char promo;
    int is_castle;
    int is_en_passant;
    int is_double_push;
    char captured;
} Move;

typedef struct {
    char board[BOARD_SIZE][BOARD_SIZE];
    int turn_is_white;
    int white_king_moved;
    int white_rook_a_moved;
    int white_rook_h_moved;
    int black_king_moved;
    int black_rook_a_moved;
    int black_rook_h_moved;
    int en_passant_target_r;
    int en_passant_target_f;
    int halfmove_clock;
    int fullmove_number;
} GameState;

typedef struct {
#ifdef _WIN32
    HANDLE process;
    HANDLE thread;
    HANDLE stdin_write;
    HANDLE stdout_read;
#endif
    int ready;
} UciEngine;

typedef struct {
    char piece;
    SDL_Rect rect;
} PaletteSlot;

typedef struct {
    char *name;
    int type;  // 0=save here, 1=dir, 2=up
} SaveDirEntry;

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *piece_textures[256] = {0};

static GameState g_state;
static int g_human_is_white = 1;
static int g_game_over = 0;
static int g_running = 1;
static int g_engine_move_pending = 0;
static char g_status[STATUS_LEN] = "";
static char g_history[MAX_HISTORY][8];
static int g_history_count = 0;
static int g_last_from_r = -1;
static int g_last_from_f = -1;
static int g_last_to_r = -1;
static int g_last_to_f = -1;

static int g_dragging = 0;
static int g_drag_from_r = -1;
static int g_drag_from_f = -1;
static char g_drag_piece = '.';
static int g_mouse_x = 0;
static int g_mouse_y = 0;
static int g_drag_from_board = 0;
static int g_anim_active = 0;
static int g_anim_from_r = -1;
static int g_anim_from_f = -1;
static char g_anim_piece = '.';
static float g_anim_x = 0.0f;
static float g_anim_y = 0.0f;
static int g_anim_has_rook = 0;
static int g_anim_rook_from_r = -1;
static int g_anim_rook_from_f = -1;
static char g_anim_rook_piece = '.';
static float g_anim_rook_x = 0.0f;
static float g_anim_rook_y = 0.0f;

static UciEngine g_engine;
static int g_view_from_white = 1;
static int g_setup_mode = 0;
static int g_use_startpos = 1;
static char g_base_fen[128] = "";
static PaletteSlot g_palette_slots[12];
static int g_palette_count = 0;
static SDL_Rect g_white_palette_col = {0, 0, 0, 0};
static SDL_Rect g_black_palette_col = {0, 0, 0, 0};
static int g_save_menu_active = 0;
static SaveDirEntry *g_save_entries = NULL;
static int g_save_entry_count = 0;
static int g_save_index = 0;
static int g_save_scroll = 0;
static char g_save_rel_dir[512] = "";
static int g_browser_mode = 0;  // 1=save, 2=flashcards
static char g_active_deck[512] = "";
static char **g_active_deck_files = NULL;
static int g_active_deck_file_count = 0;
static int g_active_deck_index = -1;
static int g_flashcards_mode = 0;
static int *g_flash_order = NULL;
static int g_flash_order_count = 0;
static int g_flash_order_pos = 0;
static Uint32 g_flash_next_ms = 0;
static int g_card_browser_active = 0;
static int g_card_index = 0;
static int g_card_scroll = 0;
static int g_card_preview_index = -1;
static int g_card_preview_valid = 0;
static GameState g_card_preview_state;
static char g_card_prev_status[STATUS_LEN] = "";
static int g_review_mode = 0;
static int g_review_ply = 0;
static int g_show_help = 0;
static char g_hint_lines[HINT_MULTI_PV][64];
static int g_hint_count = 0;
static Uint32 g_hint_until = 0;

static int engine_new_game(UciEngine *e);
static void set_setup_status(void);
static void close_save_menu(void);
static void close_card_browser(void);
static int parse_fen_to_state(const char *fen, GameState *out);
static int start_from_loaded_fen(const char *fen);
static int load_active_deck_index(int idx);
static void start_new_game(void);
static void restart_current_position(void);
static void render_help_overlay(const BoardView *view);
static void render_hint_overlay(const BoardView *view);
static void render_end_overlay(const BoardView *view);
static void clear_hints(void);

static int is_white_piece(char p) {
    return p >= 'A' && p <= 'Z';
}

static int same_color_piece(char a, char b) {
    if (a == '.' || b == '.') return 0;
    return is_white_piece(a) == is_white_piece(b);
}

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    char title[STATUS_LEN + 64];
    snprintf(title, sizeof(title), "Leela Play - %s", g_status);
    if (window) SDL_SetWindowTitle(window, title);
}

static void clear_hints(void) {
    for (int i = 0; i < HINT_MULTI_PV; i++) {
        g_hint_lines[i][0] = '\0';
    }
    g_hint_count = 0;
    g_hint_until = 0;
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
    {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},
    {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
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
    return font_glyphs[12].rows;
}

static int text_width_px(const char *text, int scale) {
    int len = (int)strlen(text);
    if (len <= 0) return 0;
    return (len * 6 - 1) * scale;
}

static void draw_text(int x, int y, int scale, const char *text, SDL_Color color) {
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

static void reset_position(GameState *s) {
    static const char *rows[BOARD_SIZE] = {
        "rnbqkbnr",
        "pppppppp",
        "........",
        "........",
        "........",
        "........",
        "PPPPPPPP",
        "RNBQKBNR"
    };
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            s->board[r][f] = rows[r][f];
        }
    }
    s->turn_is_white = 1;
    s->white_king_moved = 0;
    s->white_rook_a_moved = 0;
    s->white_rook_h_moved = 0;
    s->black_king_moved = 0;
    s->black_rook_a_moved = 0;
    s->black_rook_h_moved = 0;
    s->en_passant_target_r = -1;
    s->en_passant_target_f = -1;
    s->halfmove_clock = 0;
    s->fullmove_number = 1;
}

static void get_board_view(BoardView *view) {
    int w = SCREEN_SIZE;
    int h = SCREEN_SIZE;
    if (renderer && SDL_GetRendererOutputSize(renderer, &w, &h) != 0) {
        w = SCREEN_SIZE;
        h = SCREEN_SIZE;
    }
    // Keep a half-square border around the 8x8 board (total 9x9 grid space).
    int square_w = w / (BOARD_SIZE + 1);
    int square_h = h / (BOARD_SIZE + 1);
    view->square = (square_w < square_h) ? square_w : square_h;
    if (view->square < 1) view->square = 1;
    view->board_px = view->square * BOARD_SIZE;
    view->offset_x = (w - view->board_px) / 2;
    view->offset_y = (h - view->board_px) / 2;
    view->screen_w = w;
    view->screen_h = h;
}

static void board_to_screen(const BoardView *view, int board_r, int board_f, int *x, int *y) {
    int draw_r = g_view_from_white ? board_r : (BOARD_SIZE - 1 - board_r);
    int draw_f = g_view_from_white ? board_f : (BOARD_SIZE - 1 - board_f);
    if (x) *x = view->offset_x + draw_f * view->square;
    if (y) *y = view->offset_y + draw_r * view->square;
}

static int screen_to_board(const BoardView *view, int x, int y, int *r, int *f) {
    if (x < view->offset_x || y < view->offset_y) return 0;
    if (x >= view->offset_x + view->board_px || y >= view->offset_y + view->board_px) return 0;

    int draw_f = (x - view->offset_x) / view->square;
    int draw_r = (y - view->offset_y) / view->square;
    if (draw_r < 0 || draw_r >= BOARD_SIZE || draw_f < 0 || draw_f >= BOARD_SIZE) return 0;

    int inset = view->square / 8;
    int local_x = (x - view->offset_x) - draw_f * view->square;
    int local_y = (y - view->offset_y) - draw_r * view->square;
    if (local_x < inset || local_x >= view->square - inset) return 0;
    if (local_y < inset || local_y >= view->square - inset) return 0;

    if (r) *r = g_view_from_white ? draw_r : (BOARD_SIZE - 1 - draw_r);
    if (f) *f = g_view_from_white ? draw_f : (BOARD_SIZE - 1 - draw_f);
    return 1;
}

static SDL_Texture *get_piece_texture(char piece) {
    if (piece == '.') return NULL;
    unsigned char idx = (unsigned char)piece;
    if (piece_textures[idx]) return piece_textures[idx];

    char letter = (char)tolower((unsigned char)piece);
    const char *color = isupper((unsigned char)piece) ? "lt" : "dt";
    char path[64];
    snprintf(path, sizeof(path), "pieces/Chess_%c%s.png", letter, color);

    SDL_Texture *tex = IMG_LoadTexture(renderer, path);
    if (!tex) {
        printf("Failed to load texture %s: %s\n", path, IMG_GetError());
    }
    piece_textures[idx] = tex;
    return tex;
}

static int signi(int v) {
    return (v > 0) ? 1 : ((v < 0) ? -1 : 0);
}

static int is_path_clear(const GameState *s, int from_r, int from_f, int to_r, int to_f) {
    int dr = signi(to_r - from_r);
    int df = signi(to_f - from_f);
    int steps = abs(to_r - from_r);
    int fsteps = abs(to_f - from_f);
    if (fsteps > steps) steps = fsteps;
    for (int i = 1; i < steps; i++) {
        int r = from_r + dr * i;
        int f = from_f + df * i;
        if (s->board[r][f] != '.') return 0;
    }
    return 1;
}

static int piece_attacks_square(const GameState *s, char piece, int from_r, int from_f, int to_r, int to_f) {
    if (piece == '.') return 0;
    int is_white = is_white_piece(piece);
    int dr = to_r - from_r;
    int df = to_f - from_f;
    int adr = abs(dr);
    int adf = abs(df);
    char p = (char)toupper((unsigned char)piece);
    int dir = is_white ? -1 : 1;

    switch (p) {
        case 'P':
            return (adr == 1 && adf == 1 && dr == dir);
        case 'N':
            return ((adr == 2 && adf == 1) || (adr == 1 && adf == 2));
        case 'B':
            return (adr == adf && adr > 0 && is_path_clear(s, from_r, from_f, to_r, to_f));
        case 'R':
            return ((adr == 0 || adf == 0) && (adr + adf > 0) && is_path_clear(s, from_r, from_f, to_r, to_f));
        case 'Q':
            return ((((adr == adf) || (adr == 0 || adf == 0)) && (adr + adf > 0) &&
                     is_path_clear(s, from_r, from_f, to_r, to_f)));
        case 'K':
            return (adr <= 1 && adf <= 1 && (adr + adf > 0));
        default:
            return 0;
    }
}

static int find_king(const GameState *s, int is_white, int *out_r, int *out_f) {
    char king = is_white ? 'K' : 'k';
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            if (s->board[r][f] == king) {
                *out_r = r;
                *out_f = f;
                return 1;
            }
        }
    }
    return 0;
}

static int is_square_attacked(const GameState *s, int r, int f, int by_white) {
    for (int rr = 0; rr < BOARD_SIZE; rr++) {
        for (int ff = 0; ff < BOARD_SIZE; ff++) {
            char p = s->board[rr][ff];
            if (p == '.') continue;
            if (is_white_piece(p) != by_white) continue;
            if (piece_attacks_square(s, p, rr, ff, r, f)) return 1;
        }
    }
    return 0;
}

static int is_in_check(const GameState *s, int is_white) {
    int kr = -1;
    int kf = -1;
    if (!find_king(s, is_white, &kr, &kf)) return 0;
    return is_square_attacked(s, kr, kf, !is_white);
}

static int can_castle_kingside(const GameState *s, int is_white) {
    int row = is_white ? 7 : 0;
    char king = is_white ? 'K' : 'k';
    char rook = is_white ? 'R' : 'r';

    if (s->board[row][4] != king || s->board[row][7] != rook) return 0;
    if (is_white) {
        if (s->white_king_moved || s->white_rook_h_moved) return 0;
    } else {
        if (s->black_king_moved || s->black_rook_h_moved) return 0;
    }
    if (s->board[row][5] != '.' || s->board[row][6] != '.') return 0;
    if (is_in_check(s, is_white)) return 0;
    if (is_square_attacked(s, row, 5, !is_white)) return 0;
    if (is_square_attacked(s, row, 6, !is_white)) return 0;
    return 1;
}

static int can_castle_queenside(const GameState *s, int is_white) {
    int row = is_white ? 7 : 0;
    char king = is_white ? 'K' : 'k';
    char rook = is_white ? 'R' : 'r';

    if (s->board[row][4] != king || s->board[row][0] != rook) return 0;
    if (is_white) {
        if (s->white_king_moved || s->white_rook_a_moved) return 0;
    } else {
        if (s->black_king_moved || s->black_rook_a_moved) return 0;
    }
    if (s->board[row][1] != '.' || s->board[row][2] != '.' || s->board[row][3] != '.') return 0;
    if (is_in_check(s, is_white)) return 0;
    if (is_square_attacked(s, row, 3, !is_white)) return 0;
    if (is_square_attacked(s, row, 2, !is_white)) return 0;
    return 1;
}

static int build_move(const GameState *s, int from_r, int from_f, int to_r, int to_f, char promo, Move *out) {
    if (from_r < 0 || from_r >= BOARD_SIZE || from_f < 0 || from_f >= BOARD_SIZE) return 0;
    if (to_r < 0 || to_r >= BOARD_SIZE || to_f < 0 || to_f >= BOARD_SIZE) return 0;
    if (from_r == to_r && from_f == to_f) return 0;

    char piece = s->board[from_r][from_f];
    if (piece == '.') return 0;

    int moving_white = is_white_piece(piece);
    if (moving_white != s->turn_is_white) return 0;

    char dst = s->board[to_r][to_f];
    if (same_color_piece(piece, dst)) return 0;

    Move m;
    memset(&m, 0, sizeof(m));
    m.from_r = from_r;
    m.from_f = from_f;
    m.to_r = to_r;
    m.to_f = to_f;
    m.promo = promo;
    m.captured = dst;

    char p = (char)toupper((unsigned char)piece);
    int dr = to_r - from_r;
    int df = to_f - from_f;
    int adr = abs(dr);
    int adf = abs(df);
    int dir = moving_white ? -1 : 1;

    if (p == 'P') {
        int start_row = moving_white ? 6 : 1;
        int promo_row = moving_white ? 0 : 7;

        if (df == 0) {
            if (dst != '.') return 0;
            if (dr == dir) {
                m.is_double_push = 0;
            } else if (dr == 2 * dir && from_r == start_row) {
                int mid_r = from_r + dir;
                if (s->board[mid_r][from_f] != '.') return 0;
                m.is_double_push = 1;
            } else {
                return 0;
            }
        } else if (adf == 1 && dr == dir) {
            if (dst != '.') {
                if (is_white_piece(dst) == moving_white) return 0;
                m.captured = dst;
            } else {
                if (s->en_passant_target_r != to_r || s->en_passant_target_f != to_f) return 0;
                m.is_en_passant = 1;
                int cap_r = to_r - dir;
                char cap_piece = s->board[cap_r][to_f];
                if (cap_piece == '.' || (char)toupper((unsigned char)cap_piece) != 'P' ||
                    is_white_piece(cap_piece) == moving_white) {
                    return 0;
                }
                m.captured = cap_piece;
            }
        } else {
            return 0;
        }

        if (to_r == promo_row) {
            if (m.promo == '\0') m.promo = 'q';
            m.promo = (char)tolower((unsigned char)m.promo);
            if (!(m.promo == 'q' || m.promo == 'r' || m.promo == 'b' || m.promo == 'n')) return 0;
        } else {
            m.promo = '\0';
        }
    } else if (p == 'N') {
        if (!((adr == 2 && adf == 1) || (adr == 1 && adf == 2))) return 0;
    } else if (p == 'B') {
        if (!(adr == adf && adr > 0 && is_path_clear(s, from_r, from_f, to_r, to_f))) return 0;
    } else if (p == 'R') {
        if (!((adr == 0 || adf == 0) && (adr + adf > 0) && is_path_clear(s, from_r, from_f, to_r, to_f))) return 0;
    } else if (p == 'Q') {
        if (!((((adr == adf) || (adr == 0 || adf == 0)) && (adr + adf > 0) &&
               is_path_clear(s, from_r, from_f, to_r, to_f)))) return 0;
    } else if (p == 'K') {
        if (dr == 0 && adf == 2) {
            if (to_f == 6) {
                if (!can_castle_kingside(s, moving_white)) return 0;
            } else if (to_f == 2) {
                if (!can_castle_queenside(s, moving_white)) return 0;
            } else {
                return 0;
            }
            m.is_castle = 1;
        } else {
            if (!(adr <= 1 && adf <= 1 && (adr + adf > 0))) return 0;
        }
    } else {
        return 0;
    }

    *out = m;
    return 1;
}

static void apply_move(GameState *s, const Move *m) {
    char piece = s->board[m->from_r][m->from_f];
    int moving_white = is_white_piece(piece);

    int is_capture = (m->captured != '.');
    if ((char)toupper((unsigned char)piece) == 'P' || is_capture) {
        s->halfmove_clock = 0;
    } else {
        s->halfmove_clock++;
    }

    s->en_passant_target_r = -1;
    s->en_passant_target_f = -1;

    if ((char)toupper((unsigned char)piece) == 'K') {
        if (moving_white) s->white_king_moved = 1;
        else s->black_king_moved = 1;
    }
    if ((char)toupper((unsigned char)piece) == 'R') {
        if (moving_white) {
            if (m->from_r == 7 && m->from_f == 0) s->white_rook_a_moved = 1;
            if (m->from_r == 7 && m->from_f == 7) s->white_rook_h_moved = 1;
        } else {
            if (m->from_r == 0 && m->from_f == 0) s->black_rook_a_moved = 1;
            if (m->from_r == 0 && m->from_f == 7) s->black_rook_h_moved = 1;
        }
    }

    if (m->captured == 'R') {
        if (m->to_r == 7 && m->to_f == 0) s->white_rook_a_moved = 1;
        if (m->to_r == 7 && m->to_f == 7) s->white_rook_h_moved = 1;
    } else if (m->captured == 'r') {
        if (m->to_r == 0 && m->to_f == 0) s->black_rook_a_moved = 1;
        if (m->to_r == 0 && m->to_f == 7) s->black_rook_h_moved = 1;
    }

    if (m->is_en_passant) {
        int dir = moving_white ? -1 : 1;
        int cap_r = m->to_r - dir;
        s->board[cap_r][m->to_f] = '.';
    }

    s->board[m->to_r][m->to_f] = piece;
    s->board[m->from_r][m->from_f] = '.';

    if ((char)toupper((unsigned char)piece) == 'P' && m->promo != '\0') {
        s->board[m->to_r][m->to_f] = moving_white
            ? (char)toupper((unsigned char)m->promo)
            : (char)tolower((unsigned char)m->promo);
    }

    if (m->is_castle) {
        int row = moving_white ? 7 : 0;
        if (m->to_f == 6) {
            char rook = s->board[row][7];
            s->board[row][5] = rook;
            s->board[row][7] = '.';
            if (moving_white) s->white_rook_h_moved = 1;
            else s->black_rook_h_moved = 1;
        } else {
            char rook = s->board[row][0];
            s->board[row][3] = rook;
            s->board[row][0] = '.';
            if (moving_white) s->white_rook_a_moved = 1;
            else s->black_rook_a_moved = 1;
        }
    }

    if ((char)toupper((unsigned char)piece) == 'P' && m->is_double_push) {
        int dir = moving_white ? -1 : 1;
        s->en_passant_target_r = m->from_r + dir;
        s->en_passant_target_f = m->from_f;
    }

    if (!moving_white) s->fullmove_number++;
    s->turn_is_white = !s->turn_is_white;
}

static int is_legal_move(const GameState *s, const Move *m) {
    GameState tmp = *s;
    int moving_white = s->turn_is_white;
    apply_move(&tmp, m);
    return !is_in_check(&tmp, moving_white);
}

static int move_to_uci(const Move *m, char *out, size_t out_size) {
    if (out_size < 6) return 0;
    out[0] = (char)('a' + m->from_f);
    out[1] = (char)('8' - m->from_r);
    out[2] = (char)('a' + m->to_f);
    out[3] = (char)('8' - m->to_r);
    int len = 4;
    if (m->promo != '\0') {
        out[4] = (char)tolower((unsigned char)m->promo);
        len = 5;
    }
    out[len] = '\0';
    return 1;
}

static int parse_uci_to_move(const GameState *s, const char *uci, Move *out) {
    size_t len = strlen(uci);
    if (len < 4) return 0;
    if (uci[0] < 'a' || uci[0] > 'h' || uci[2] < 'a' || uci[2] > 'h') return 0;
    if (uci[1] < '1' || uci[1] > '8' || uci[3] < '1' || uci[3] > '8') return 0;

    int from_f = uci[0] - 'a';
    int from_r = '8' - uci[1];
    int to_f = uci[2] - 'a';
    int to_r = '8' - uci[3];
    char promo = '\0';
    if (len >= 5 && isalpha((unsigned char)uci[4])) {
        promo = (char)tolower((unsigned char)uci[4]);
    }

    Move m;
    if (!build_move(s, from_r, from_f, to_r, to_f, promo, &m)) return 0;
    if (!is_legal_move(s, &m)) return 0;
    *out = m;
    return 1;
}

static int has_any_legal_move(const GameState *s) {
    for (int from_r = 0; from_r < BOARD_SIZE; from_r++) {
        for (int from_f = 0; from_f < BOARD_SIZE; from_f++) {
            char piece = s->board[from_r][from_f];
            if (piece == '.') continue;
            if (is_white_piece(piece) != s->turn_is_white) continue;

            for (int to_r = 0; to_r < BOARD_SIZE; to_r++) {
                for (int to_f = 0; to_f < BOARD_SIZE; to_f++) {
                    char p = (char)toupper((unsigned char)piece);
                    if (p == 'P' && (to_r == 0 || to_r == 7)) {
                        const char promos[4] = {'q', 'r', 'b', 'n'};
                        for (int i = 0; i < 4; i++) {
                            Move m;
                            if (!build_move(s, from_r, from_f, to_r, to_f, promos[i], &m)) continue;
                            if (is_legal_move(s, &m)) return 1;
                        }
                    } else {
                        Move m;
                        if (!build_move(s, from_r, from_f, to_r, to_f, '\0', &m)) continue;
                        if (is_legal_move(s, &m)) return 1;
                    }
                }
            }
        }
    }
    return 0;
}

static int is_insufficient_material(const GameState *s) {
    int white_bishops = 0, white_knights = 0;
    int black_bishops = 0, black_knights = 0;
    int white_bishop_colors = 0;  // bit0=dark, bit1=light
    int black_bishop_colors = 0;  // bit0=dark, bit1=light

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            char p = s->board[r][f];
            if (p == '.') continue;
            char up = (char)toupper((unsigned char)p);

            if (up == 'P' || up == 'R' || up == 'Q') {
                return 0;
            }
            if (up == 'B') {
                int color_bit = ((r + f) & 1) ? 1 : 2;
                if (is_white_piece(p)) {
                    white_bishops++;
                    white_bishop_colors |= color_bit;
                } else {
                    black_bishops++;
                    black_bishop_colors |= color_bit;
                }
            } else if (up == 'N') {
                if (is_white_piece(p)) white_knights++;
                else black_knights++;
            }
        }
    }

    int white_minors = white_bishops + white_knights;
    int black_minors = black_bishops + black_knights;

    // K vs K
    if (white_minors == 0 && black_minors == 0) return 1;
    // K+B/N vs K
    if (white_minors == 1 && black_minors == 0) return 1;
    if (black_minors == 1 && white_minors == 0) return 1;
    // K+B vs K+B with bishops on same color squares
    if (white_knights == 0 && black_knights == 0 &&
        white_bishops == 1 && black_bishops == 1 &&
        (white_bishop_colors & black_bishop_colors) != 0) {
        return 1;
    }

    return 0;
}

static void evaluate_game_end(GameState *s) {
    g_flash_next_ms = 0;
    if (s->halfmove_clock >= 100) {
        g_game_over = 1;
        set_status("Draw by 50-move rule");
        if (g_flashcards_mode) g_flash_next_ms = SDL_GetTicks() + 1800;
        return;
    }

    if (is_insufficient_material(s)) {
        g_game_over = 1;
        set_status("Draw by insufficient material");
        if (g_flashcards_mode) g_flash_next_ms = SDL_GetTicks() + 1800;
        return;
    }

    if (has_any_legal_move(s)) {
        if (s->turn_is_white == g_human_is_white) {
            set_status("Your move");
        } else {
            set_status("Engine to move");
        }
        return;
    }

    g_game_over = 1;
    if (is_in_check(s, s->turn_is_white)) {
        int engine_mated = (s->turn_is_white != g_human_is_white);
        if (engine_mated) {
            set_status("Checkmate: you win");
        } else {
            set_status("Checkmate: engine wins");
        }
    } else {
        set_status("Draw by stalemate");
    }
    if (g_flashcards_mode) g_flash_next_ms = SDL_GetTicks() + 1800;
}

static void push_history(const char *uci) {
    if (g_history_count >= MAX_HISTORY) return;
    strncpy(g_history[g_history_count], uci, sizeof(g_history[g_history_count]) - 1);
    g_history[g_history_count][sizeof(g_history[g_history_count]) - 1] = '\0';
    g_history_count++;
}

static void clear_special_state(GameState *s) {
    s->white_king_moved = 1;
    s->white_rook_a_moved = 1;
    s->white_rook_h_moved = 1;
    s->black_king_moved = 1;
    s->black_rook_a_moved = 1;
    s->black_rook_h_moved = 1;
    s->en_passant_target_r = -1;
    s->en_passant_target_f = -1;
    s->halfmove_clock = 0;
    if (s->fullmove_number < 1) s->fullmove_number = 1;
}

static int count_piece_on_board(char piece) {
    int count = 0;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            if (g_state.board[r][f] == piece) count++;
        }
    }
    return count;
}

static void build_fen(const GameState *s, char *out, size_t out_size) {
    size_t pos = 0;
    if (!out || out_size == 0) return;

    for (int r = 0; r < BOARD_SIZE; r++) {
        int empty = 0;
        for (int f = 0; f < BOARD_SIZE; f++) {
            char p = s->board[r][f];
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
    if (pos + 1 < out_size) out[pos++] = s->turn_is_white ? 'w' : 'b';
    if (pos + 1 < out_size) out[pos++] = ' ';

    char castle[5];
    int cpos = 0;
    if (!s->white_king_moved && !s->white_rook_h_moved && s->board[7][4] == 'K' && s->board[7][7] == 'R') {
        castle[cpos++] = 'K';
    }
    if (!s->white_king_moved && !s->white_rook_a_moved && s->board[7][4] == 'K' && s->board[7][0] == 'R') {
        castle[cpos++] = 'Q';
    }
    if (!s->black_king_moved && !s->black_rook_h_moved && s->board[0][4] == 'k' && s->board[0][7] == 'r') {
        castle[cpos++] = 'k';
    }
    if (!s->black_king_moved && !s->black_rook_a_moved && s->board[0][4] == 'k' && s->board[0][0] == 'r') {
        castle[cpos++] = 'q';
    }
    if (cpos == 0) {
        if (pos + 1 < out_size) out[pos++] = '-';
    } else {
        for (int i = 0; i < cpos; i++) {
            if (pos + 1 < out_size) out[pos++] = castle[i];
        }
    }

    if (pos + 1 < out_size) out[pos++] = ' ';
    if (s->en_passant_target_r >= 0 && s->en_passant_target_r < BOARD_SIZE &&
        s->en_passant_target_f >= 0 && s->en_passant_target_f < BOARD_SIZE) {
        if (pos + 1 < out_size) out[pos++] = (char)('a' + s->en_passant_target_f);
        if (pos + 1 < out_size) out[pos++] = (char)('8' - s->en_passant_target_r);
    } else {
        if (pos + 1 < out_size) out[pos++] = '-';
    }

    char suffix[32];
    int fullmove = s->fullmove_number < 1 ? 1 : s->fullmove_number;
    snprintf(suffix, sizeof(suffix), " %d %d", s->halfmove_clock, fullmove);
    for (const char *p = suffix; *p; p++) {
        if (pos + 1 < out_size) out[pos++] = *p;
    }

    if (pos < out_size) out[pos] = '\0';
    else out[out_size - 1] = '\0';
}

static char *copy_string(const char *s) {
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len + 1);
    return out;
}

static void free_string_list(char **items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static int push_string_item(char ***items, int *count, int *cap, const char *value) {
    if (*count >= *cap) {
        int next_cap = (*cap == 0) ? 16 : (*cap * 2);
        char **next = (char **)realloc(*items, (size_t)next_cap * sizeof(char *));
        if (!next) return 0;
        *items = next;
        *cap = next_cap;
    }
    (*items)[*count] = copy_string(value);
    if (!(*items)[*count]) return 0;
    (*count)++;
    return 1;
}

static int str_case_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static int str_case_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;

    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

static int has_fen_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot[1] == '\0') return 0;
    return str_case_eq(dot + 1, "fen");
}

static int path_join(char *out, size_t out_size, const char *a, const char *b) {
    if (!out || out_size == 0 || !a || !b) return 0;
    size_t alen = strlen(a);
    int need_sep = (alen > 0 && a[alen - 1] != '/' && a[alen - 1] != '\\');
    int n = snprintf(out, out_size, "%s%s%s", a, need_sep ? "\\" : "", b);
    return n > 0 && (size_t)n < out_size;
}

static int cmp_string_ptrs(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcmp(sa, sb);
}

static void clear_active_deck_files(void) {
    free_string_list(g_active_deck_files, g_active_deck_file_count);
    g_active_deck_files = NULL;
    g_active_deck_file_count = 0;
    g_active_deck_index = -1;
}

static void clear_flash_order(void) {
    free(g_flash_order);
    g_flash_order = NULL;
    g_flash_order_count = 0;
    g_flash_order_pos = 0;
}

static void rebuild_flash_order(void) {
    clear_flash_order();
    if (g_active_deck_file_count <= 0) return;

    g_flash_order = (int *)malloc((size_t)g_active_deck_file_count * sizeof(int));
    if (!g_flash_order) return;
    g_flash_order_count = g_active_deck_file_count;
    for (int i = 0; i < g_flash_order_count; i++) {
        g_flash_order[i] = i;
    }
    for (int i = g_flash_order_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = g_flash_order[i];
        g_flash_order[i] = g_flash_order[j];
        g_flash_order[j] = t;
    }
    g_flash_order_pos = 0;
}

static int refresh_active_deck_files(void) {
    char old_name[256];
    old_name[0] = '\0';
    int old_index = g_active_deck_index;
    if (g_active_deck_files && g_active_deck_index >= 0 && g_active_deck_index < g_active_deck_file_count) {
        strncpy(old_name, g_active_deck_files[g_active_deck_index], sizeof(old_name) - 1);
        old_name[sizeof(old_name) - 1] = '\0';
    }

    clear_active_deck_files();
    if (g_active_deck[0] == '\0') return 0;

    char **files = NULL;
    int count = 0;
    int cap = 0;

#ifdef _WIN32
    char search[1024];
    if (!path_join(search, sizeof(search), g_active_deck, "*")) return 0;
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(search, &data);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!has_fen_extension(data.cFileName)) continue;
        if (!push_string_item(&files, &count, &cap, data.cFileName)) {
            FindClose(h);
            free_string_list(files, count);
            return 0;
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *d = opendir(g_active_deck);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (!has_fen_extension(ent->d_name)) continue;
        if (!push_string_item(&files, &count, &cap, ent->d_name)) {
            closedir(d);
            free_string_list(files, count);
            return 0;
        }
    }
    closedir(d);
#endif

    if (count > 1) {
        qsort(files, (size_t)count, sizeof(files[0]), cmp_string_ptrs);
    }

    g_active_deck_files = files;
    g_active_deck_file_count = count;
    if (count > 0) {
        g_active_deck_index = 0;
        if (old_name[0] != '\0') {
            for (int i = 0; i < count; i++) {
                if (strcmp(files[i], old_name) == 0) {
                    g_active_deck_index = i;
                    break;
                }
            }
        } else if (old_index >= 0 && old_index < count) {
            g_active_deck_index = old_index;
        }
    }
    return count > 0;
}

static int parse_fen_to_state(const char *fen, GameState *out) {
    char board_part[128];
    char side_part[8];
    char castle_part[8];
    char ep_part[8];
    int halfmove = 0;
    int fullmove = 1;

    int read = sscanf(fen, "%127s %7s %7s %7s %d %d",
                      board_part, side_part, castle_part, ep_part, &halfmove, &fullmove);
    if (read < 4) return 0;

    GameState s;
    memset(&s, 0, sizeof(s));
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            s.board[r][f] = '.';
        }
    }

    int r = 0;
    int f = 0;
    for (const char *p = board_part; *p; p++) {
        char c = *p;
        if (c == '/') {
            if (f != 8) return 0;
            r++;
            f = 0;
            continue;
        }
        if (isdigit((unsigned char)c)) {
            int n = c - '0';
            if (n < 1 || n > 8) return 0;
            f += n;
            if (f > 8) return 0;
            continue;
        }
        if (strchr("PNBRQKpnbrqk", c) == NULL) return 0;
        if (r < 0 || r >= 8 || f < 0 || f >= 8) return 0;
        s.board[r][f] = c;
        f++;
    }
    if (r != 7 || f != 8) return 0;

    s.turn_is_white = (side_part[0] == 'w');
    clear_special_state(&s);

    if (!str_case_eq(castle_part, "-")) {
        if (strchr(castle_part, 'K')) {
            s.white_king_moved = 0;
            s.white_rook_h_moved = 0;
        }
        if (strchr(castle_part, 'Q')) {
            s.white_king_moved = 0;
            s.white_rook_a_moved = 0;
        }
        if (strchr(castle_part, 'k')) {
            s.black_king_moved = 0;
            s.black_rook_h_moved = 0;
        }
        if (strchr(castle_part, 'q')) {
            s.black_king_moved = 0;
            s.black_rook_a_moved = 0;
        }
    }

    s.en_passant_target_r = -1;
    s.en_passant_target_f = -1;
    if (!str_case_eq(ep_part, "-")) {
        if (strlen(ep_part) == 2 &&
            ep_part[0] >= 'a' && ep_part[0] <= 'h' &&
            ep_part[1] >= '1' && ep_part[1] <= '8') {
            s.en_passant_target_f = ep_part[0] - 'a';
            s.en_passant_target_r = '8' - ep_part[1];
        }
    }

    s.halfmove_clock = (read >= 5) ? halfmove : 0;
    s.fullmove_number = (read >= 6 && fullmove > 0) ? fullmove : 1;

    *out = s;
    return 1;
}

static int build_state_at_ply(int ply, GameState *out,
                              int *last_from_r, int *last_from_f,
                              int *last_to_r, int *last_to_f) {
    if (!out) return 0;

    if (ply < 0) ply = 0;
    if (ply > g_history_count) ply = g_history_count;

    GameState s;
    if (g_use_startpos) {
        reset_position(&s);
    } else {
        if (!parse_fen_to_state(g_base_fen, &s)) return 0;
    }

    int lfr = -1;
    int lff = -1;
    int ltr = -1;
    int ltf = -1;

    for (int i = 0; i < ply; i++) {
        Move m;
        if (!parse_uci_to_move(&s, g_history[i], &m)) return 0;
        apply_move(&s, &m);
        lfr = m.from_r;
        lff = m.from_f;
        ltr = m.to_r;
        ltf = m.to_f;
    }

    *out = s;
    if (last_from_r) *last_from_r = lfr;
    if (last_from_f) *last_from_f = lff;
    if (last_to_r) *last_to_r = ltr;
    if (last_to_f) *last_to_f = ltf;
    return 1;
}

static void restore_live_status(void) {
    if (g_game_over) {
        evaluate_game_end(&g_state);
        return;
    }
    if (g_engine_move_pending) {
        set_status("Engine thinking...");
    } else if (g_state.turn_is_white == g_human_is_white) {
        set_status("Your move");
    } else {
        set_status("Engine to move");
    }
}

static int set_review_ply(int ply) {
    GameState s;
    int lfr = -1, lff = -1, ltr = -1, ltf = -1;
    if (!build_state_at_ply(ply, &s, &lfr, &lff, &ltr, &ltf)) return 0;

    g_state = s;
    g_last_from_r = lfr;
    g_last_from_f = lff;
    g_last_to_r = ltr;
    g_last_to_f = ltf;

    g_review_ply = ply;
    g_review_mode = (ply != g_history_count);
    if (g_review_mode) {
        set_status("REVIEW: %d/%d | LEFT/RIGHT STEP | ENTER LIVE", g_review_ply, g_history_count);
    } else {
        restore_live_status();
    }
    return 1;
}

static void step_review(int delta) {
    if (g_setup_mode || g_save_menu_active || g_card_browser_active) return;
    if (g_engine_move_pending) {
        set_status("Wait for engine move, then review");
        return;
    }
    if (g_history_count <= 0) {
        set_status("No moves to review");
        return;
    }

    int from = g_review_mode ? g_review_ply : g_history_count;
    int target = from + delta;
    if (target < 0) target = 0;
    if (target > g_history_count) target = g_history_count;
    set_review_ply(target);
}

static void restart_current_position(void) {
    if (g_setup_mode) {
        set_status("Restart unavailable in setup mode");
        return;
    }
    if (g_save_menu_active || g_card_browser_active) {
        set_status("Close browser before restarting");
        return;
    }

    if (g_flashcards_mode && g_active_deck_file_count > 0 &&
        g_active_deck_index >= 0 && g_active_deck_index < g_active_deck_file_count) {
        load_active_deck_index(g_active_deck_index);
        return;
    }

    if (!g_use_startpos && g_base_fen[0] != '\0') {
        start_from_loaded_fen(g_base_fen);
        return;
    }

    start_new_game();
}

static int start_from_loaded_fen(const char *fen) {
    GameState next;
    if (!parse_fen_to_state(fen, &next)) {
        set_status("Invalid FEN in deck");
        return 0;
    }
    g_state = next;
    strncpy(g_base_fen, fen, sizeof(g_base_fen) - 1);
    g_base_fen[sizeof(g_base_fen) - 1] = '\0';
    g_use_startpos = 0;
    g_setup_mode = 0;
    g_game_over = 0;
    g_engine_move_pending = 0;
    g_review_mode = 0;
    g_review_ply = 0;
    clear_hints();
    g_history_count = 0;
    g_last_from_r = g_last_from_f = g_last_to_r = g_last_to_f = -1;

    if (!engine_new_game(&g_engine)) {
        g_game_over = 1;
        set_status("Engine reset failed");
        return 0;
    }

    evaluate_game_end(&g_state);
    if (!g_game_over && g_state.turn_is_white != g_human_is_white) {
        g_engine_move_pending = 1;
        set_status("Engine thinking...");
    }
    return 1;
}

static int load_active_deck_index(int idx) {
    if (idx < 0 || idx >= g_active_deck_file_count) return 0;
    char path[1024];
    if (!path_join(path, sizeof(path), g_active_deck, g_active_deck_files[idx])) return 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        set_status("Failed to open deck card");
        return 0;
    }
    char fen[256];
    if (!fgets(fen, sizeof(fen), f)) {
        fclose(f);
        set_status("Deck card is empty");
        return 0;
    }
    fclose(f);
    fen[strcspn(fen, "\r\n")] = '\0';

    if (!start_from_loaded_fen(fen)) return 0;
    g_active_deck_index = idx;
    return 1;
}

static void cycle_active_deck(int delta) {
    if (g_active_deck[0] == '\0') {
        set_status("No active deck. Save with S first.");
        return;
    }
    if (!refresh_active_deck_files()) {
        set_status("Deck has no .fen cards: %s", g_active_deck);
        return;
    }
    if (g_active_deck_file_count == 0) {
        set_status("Deck has no .fen cards: %s", g_active_deck);
        return;
    }
    if (g_active_deck_index < 0 || g_active_deck_index >= g_active_deck_file_count) {
        g_active_deck_index = 0;
    } else {
        int n = g_active_deck_file_count;
        g_active_deck_index = (g_active_deck_index + delta + n) % n;
    }
    load_active_deck_index(g_active_deck_index);
}

static int flashcards_next_random(void) {
    if (g_active_deck[0] == '\0') {
        set_status("No active flashcards deck");
        return 0;
    }
    if (!refresh_active_deck_files() || g_active_deck_file_count <= 0) {
        set_status("Deck has no .fen cards: %s", g_active_deck);
        return 0;
    }

    if (!g_flash_order || g_flash_order_count != g_active_deck_file_count || g_flash_order_pos >= g_flash_order_count) {
        rebuild_flash_order();
    }
    if (!g_flash_order || g_flash_order_count <= 0) {
        set_status("Failed to prepare flashcards order");
        return 0;
    }

    int idx = g_flash_order[g_flash_order_pos++];
    if (!load_active_deck_index(idx)) return 0;
    g_flash_next_ms = 0;
    set_status("FLASHCARDS: %s (%d/%d)", g_active_deck_files[idx], g_flash_order_pos, g_flash_order_count);
    return 1;
}

static int activate_flashcards_directory(const char *folder) {
    if (!folder || folder[0] == '\0') return 0;
    strncpy(g_active_deck, folder, sizeof(g_active_deck) - 1);
    g_active_deck[sizeof(g_active_deck) - 1] = '\0';
    if (!refresh_active_deck_files() || g_active_deck_file_count <= 0) {
        set_status("No .fen cards in %s", g_active_deck);
        return 0;
    }

    g_setup_mode = 0;
    g_flashcards_mode = 1;
    g_game_over = 0;
    g_engine_move_pending = 0;
    g_review_mode = 0;
    g_review_ply = 0;
    clear_hints();
    g_flash_next_ms = 0;
    clear_flash_order();
    return flashcards_next_random();
}

static int load_card_preview_index(int idx) {
    g_card_preview_index = idx;
    g_card_preview_valid = 0;
    memset(&g_card_preview_state, 0, sizeof(g_card_preview_state));

    if (idx < 0 || idx >= g_active_deck_file_count) return 0;
    if (g_active_deck[0] == '\0') return 0;

    char path[1024];
    if (!path_join(path, sizeof(path), g_active_deck, g_active_deck_files[idx])) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char fen[256];
    if (!fgets(fen, sizeof(fen), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    fen[strcspn(fen, "\r\n")] = '\0';

    GameState s;
    if (!parse_fen_to_state(fen, &s)) return 0;
    g_card_preview_state = s;
    g_card_preview_valid = 1;
    return 1;
}

static void update_card_browser_status(void) {
    if (!g_card_browser_active) return;
    if (g_active_deck_file_count <= 0) {
        set_status("No cards in active deck");
        return;
    }
    const char *name = g_active_deck_files[g_card_index];
    set_status(
        "CARDS: %s | %s (%d/%d) | Enter load | Esc close",
        g_active_deck, name, g_card_index + 1, g_active_deck_file_count
    );
}

static void close_card_browser(void) {
    g_card_browser_active = 0;
    g_card_index = 0;
    g_card_scroll = 0;
    g_card_preview_index = -1;
    g_card_preview_valid = 0;
}

static void open_card_browser(void) {
    if (g_save_menu_active) {
        set_status("Close save/practice browser first");
        return;
    }
    if (g_setup_mode) {
        set_status("Card browser unavailable in setup mode");
        return;
    }
    if (g_review_mode) {
        set_status("Return to live position before browsing cards");
        return;
    }
    if (g_active_deck[0] == '\0') {
        set_status("No active deck. Choose one with P.");
        return;
    }
    if (!refresh_active_deck_files() || g_active_deck_file_count <= 0) {
        set_status("Deck has no .fen cards: %s", g_active_deck);
        return;
    }

    strncpy(g_card_prev_status, g_status, sizeof(g_card_prev_status) - 1);
    g_card_prev_status[sizeof(g_card_prev_status) - 1] = '\0';

    g_card_browser_active = 1;
    g_card_index = g_active_deck_index;
    if (g_card_index < 0 || g_card_index >= g_active_deck_file_count) g_card_index = 0;
    g_card_scroll = 0;
    load_card_preview_index(g_card_index);
    update_card_browser_status();
}

static int handle_card_browser_event(const SDL_Event *e) {
    if (!g_card_browser_active) return 0;
    if (e->type != SDL_KEYDOWN) return 1;

    SDL_Keycode key = e->key.keysym.sym;
    if (key == SDLK_ESCAPE) {
        close_card_browser();
        if (g_card_prev_status[0] != '\0') {
            set_status("%s", g_card_prev_status);
        }
        return 1;
    }

    int old = g_card_index;
    if (key == SDLK_UP) {
        if (g_card_index > 0) g_card_index--;
    } else if (key == SDLK_DOWN) {
        if (g_card_index < g_active_deck_file_count - 1) g_card_index++;
    } else if (key == SDLK_PAGEUP) {
        g_card_index -= 8;
        if (g_card_index < 0) g_card_index = 0;
    } else if (key == SDLK_PAGEDOWN) {
        g_card_index += 8;
        if (g_card_index >= g_active_deck_file_count) g_card_index = g_active_deck_file_count - 1;
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        int pick = g_card_index;
        close_card_browser();
        load_active_deck_index(pick);
        return 1;
    } else {
        return 1;
    }

    if (g_card_index != old) {
        load_card_preview_index(g_card_index);
        update_card_browser_status();
    }
    return 1;
}

static int is_directory_path(const char *path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
#else
    DIR *d = opendir(path);
    if (!d) return 0;
    closedir(d);
    return 1;
#endif
}

static int save_entry_cmp(const void *a, const void *b) {
    const SaveDirEntry *ea = (const SaveDirEntry *)a;
    const SaveDirEntry *eb = (const SaveDirEntry *)b;
    if (ea->type != eb->type) return (ea->type < eb->type) ? -1 : 1;
    return strcmp(ea->name, eb->name);
}

static int push_save_entry(SaveDirEntry **items, int *count, int *cap, const char *name, int type) {
    if (*count >= *cap) {
        int next_cap = (*cap == 0) ? 16 : (*cap * 2);
        SaveDirEntry *next = (SaveDirEntry *)realloc(*items, (size_t)next_cap * sizeof(*next));
        if (!next) return 0;
        *items = next;
        *cap = next_cap;
    }
    (*items)[*count].name = copy_string(name);
    if (!(*items)[*count].name) return 0;
    (*items)[*count].type = type;
    (*count)++;
    return 1;
}

static void free_save_entries(void) {
    if (!g_save_entries) return;
    for (int i = 0; i < g_save_entry_count; i++) {
        free(g_save_entries[i].name);
    }
    free(g_save_entries);
    g_save_entries = NULL;
    g_save_entry_count = 0;
}

static void free_save_entries_local(SaveDirEntry *items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) {
        free(items[i].name);
    }
    free(items);
}

static void save_menu_set_dir(const char *rel_dir) {
    if (!rel_dir) {
        g_save_rel_dir[0] = '\0';
        return;
    }
    strncpy(g_save_rel_dir, rel_dir, sizeof(g_save_rel_dir) - 1);
    g_save_rel_dir[sizeof(g_save_rel_dir) - 1] = '\0';
}

static void save_menu_dir_up(void) {
    size_t len = strlen(g_save_rel_dir);
    if (len == 0) return;
    for (size_t i = len; i > 0; i--) {
        if (g_save_rel_dir[i - 1] == '\\' || g_save_rel_dir[i - 1] == '/') {
            g_save_rel_dir[i - 1] = '\0';
            return;
        }
    }
    g_save_rel_dir[0] = '\0';
}

static int save_menu_current_path(char *out, size_t out_size) {
    if (g_save_rel_dir[0] == '\0') {
        return snprintf(out, out_size, ".") > 0;
    }
    return snprintf(out, out_size, "%s", g_save_rel_dir) > 0;
}

static int save_menu_load_entries(void) {
    SaveDirEntry *entries = NULL;
    int count = 0;
    int cap = 0;
    char base[1024];
    if (!save_menu_current_path(base, sizeof(base))) return 0;

#ifdef _WIN32
    char search[1024];
    if (!path_join(search, sizeof(search), base, "*")) return 0;
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(search, &data);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!push_save_entry(&entries, &count, &cap, data.cFileName, 1)) {
            FindClose(h);
            free_save_entries_local(entries, count);
            return 0;
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *d = opendir(base);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[1024];
        if (!path_join(full, sizeof(full), base, ent->d_name)) continue;
        if (!is_directory_path(full)) continue;
        if (!push_save_entry(&entries, &count, &cap, ent->d_name, 1)) {
            closedir(d);
            free_save_entries_local(entries, count);
            return 0;
        }
    }
    closedir(d);
#endif

    if (g_save_rel_dir[0] != '\0') {
        if (!push_save_entry(&entries, &count, &cap, "..", 2)) {
            free_save_entries_local(entries, count);
            return 0;
        }
    }
    const char *pick_label = (g_browser_mode == 2) ? "[PRACTICE HERE]" : "[SAVE HERE]";
    if (!push_save_entry(&entries, &count, &cap, pick_label, 0)) {
        free_save_entries_local(entries, count);
        return 0;
    }

    if (count > 1) {
        qsort(entries, (size_t)count, sizeof(entries[0]), save_entry_cmp);
    }

    free_save_entries();
    g_save_entries = entries;
    g_save_entry_count = count;
    g_save_index = 0;
    g_save_scroll = 0;
    return 1;
}

static void close_save_menu(void) {
    free_save_entries();
    g_save_index = 0;
    g_save_scroll = 0;
    g_save_menu_active = 0;
    g_browser_mode = 0;
}

static void update_save_menu_status(void) {
    if (!g_save_menu_active || g_save_entry_count <= 0) return;
    char base[1024];
    save_menu_current_path(base, sizeof(base));
    if (g_browser_mode == 2) {
        set_status(
            "FLASHCARDS DIR: %s | %s (%d/%d) | Enter select | Esc cancel",
            base, g_save_entries[g_save_index].name, g_save_index + 1, g_save_entry_count
        );
    } else {
        set_status(
            "SAVE: %s | %s (%d/%d) | Enter select | Esc cancel",
            base, g_save_entries[g_save_index].name, g_save_index + 1, g_save_entry_count
        );
    }
}

static int save_current_position_to_folder(const char *folder) {
    char fen[128];
    build_fen(&g_state, fen, sizeof(fen));

    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif

    char file_name[64];
    strftime(file_name, sizeof(file_name), "card_%Y%m%d_%H%M%S.fen", &tmv);

    char path[1024];
    if (!path_join(path, sizeof(path), folder, file_name)) {
        set_status("Save path too long");
        return 0;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        set_status("Failed to save to %s", folder);
        return 0;
    }
    fprintf(f, "%s\n", fen);
    fclose(f);

    strncpy(g_active_deck, folder, sizeof(g_active_deck) - 1);
    g_active_deck[sizeof(g_active_deck) - 1] = '\0';
    refresh_active_deck_files();
    if (g_active_deck_file_count > 0) {
        g_active_deck_index = g_active_deck_file_count - 1;
    }

    set_status("Saved card to %s", path);
    return 1;
}

static void open_save_menu(void) {
    close_card_browser();
    close_save_menu();
    g_browser_mode = 1;
    if (is_directory_path("flashcards")) {
        save_menu_set_dir("flashcards");
    } else {
        save_menu_set_dir("");
    }
    if (!save_menu_load_entries() || g_save_entry_count <= 0) {
        set_status("No folders found for save menu");
        return;
    }
    g_save_menu_active = 1;
    update_save_menu_status();
}

static void open_flashcards_menu(void) {
    close_card_browser();
    close_save_menu();
    g_browser_mode = 2;
    if (is_directory_path("flashcards")) {
        save_menu_set_dir("flashcards");
    } else {
        save_menu_set_dir("");
    }
    if (!save_menu_load_entries() || g_save_entry_count <= 0) {
        set_status("No folders found for flashcards");
        return;
    }
    g_save_menu_active = 1;
    update_save_menu_status();
}

static int handle_save_menu_event(const SDL_Event *e) {
    if (!g_save_menu_active) return 0;
    if (e->type != SDL_KEYDOWN) return 1;

    SDL_Keycode key = e->key.keysym.sym;
    if (key == SDLK_ESCAPE) {
        int mode = g_browser_mode;
        close_save_menu();
        if (g_setup_mode) set_setup_status();
        else if (mode == 2) set_status("Flashcards selection canceled");
        else set_status("Save canceled");
        return 1;
    }
    if (key == SDLK_UP) {
        if (g_save_index > 0) g_save_index--;
        update_save_menu_status();
        return 1;
    }
    if (key == SDLK_DOWN) {
        if (g_save_index < g_save_entry_count - 1) g_save_index++;
        update_save_menu_status();
        return 1;
    }
    if (key == SDLK_PAGEUP) {
        g_save_index -= 6;
        if (g_save_index < 0) g_save_index = 0;
        update_save_menu_status();
        return 1;
    }
    if (key == SDLK_PAGEDOWN) {
        g_save_index += 6;
        if (g_save_index >= g_save_entry_count) g_save_index = g_save_entry_count - 1;
        update_save_menu_status();
        return 1;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        SaveDirEntry *entry = &g_save_entries[g_save_index];
        if (entry->type == 0) {
            char folder[1024];
            save_menu_current_path(folder, sizeof(folder));
            if (g_browser_mode == 2) {
                if (activate_flashcards_directory(folder)) {
                    close_save_menu();
                }
            } else {
                save_current_position_to_folder(folder);
                close_save_menu();
            }
        } else if (entry->type == 1) {
            char next[512];
            if (g_save_rel_dir[0] == '\0') {
                snprintf(next, sizeof(next), "%s", entry->name);
            } else {
                snprintf(next, sizeof(next), "%s\\%s", g_save_rel_dir, entry->name);
            }
            save_menu_set_dir(next);
            save_menu_load_entries();
            update_save_menu_status();
        } else if (entry->type == 2) {
            save_menu_dir_up();
            save_menu_load_entries();
            update_save_menu_status();
        }
        return 1;
    }
    return 1;
}

static void set_setup_status(void) {
    set_status(
        "SETUP: drag pieces | Enter play | S save | T turn=%s | F you=%s | C clear | N normal",
        g_state.turn_is_white ? "White" : "Black",
        g_human_is_white ? "White" : "Black"
    );
}

static void render_help_overlay(const BoardView *view) {
    if (!g_show_help) return;

    const char *lines[] = {
        "HELP",
        "GENERAL:",
        "  ESC: TOGGLE HELP",
        "  Q: QUIT",
        "  N: NEW GAME (OR NEXT CARD IN PRACTICE)",
        "  R: RESTART CURRENT PUZZLE/POSITION",
        "  F: FLIP SIDE/VIEW",
        "  H: SHOW ENGINE HINTS",
        "  E: ENTER SETUP MODE",
        "  S: SAVE POSITION TO FLASHCARDS",
        "  P: PRACTICE MODE (CHOOSE DECK)",
        "  B: BROWSE CARDS IN ACTIVE DECK",
        "  [: PREV CARD IN ACTIVE DECK",
        "  ]: NEXT CARD IN ACTIVE DECK",
        "PLAY MODE:",
        "  LEFT DRAG: MOVE PIECE",
        "  LEFT/RIGHT: REVIEW GAME MOVES",
        "SETUP MODE:",
        "  LEFT DRAG: PLACE/MOVE PIECES",
        "  ENTER: START FROM THIS POSITION",
        "  T: TOGGLE SIDE TO MOVE",
        "  C: CLEAR BOARD",
        "BROWSER (SAVE/PRACTICE):",
        "  UP/DOWN: SELECT",
        "  PGUP/PGDN: JUMP",
        "  ENTER: OPEN DIR OR SELECT",
        "  ESC: CLOSE BROWSER",
        "CARD BROWSER (B):",
        "  UP/DOWN: SELECT CARD",
        "  ENTER: LOAD CARD",
        "  ESC: CLOSE"
    };
    int line_count = (int)(sizeof(lines) / sizeof(lines[0]));

    int scale = (view->square >= 60) ? 3 : 2;
    int line_gap = (scale >= 3) ? 4 : 3;
    int text_h = 7 * scale;
    int pad = (scale >= 3) ? 10 : 8;

    int max_w = 0;
    for (int i = 0; i < line_count; i++) {
        int w = text_width_px(lines[i], scale);
        if (w > max_w) max_w = w;
    }

    int total_h = line_count * text_h + (line_count - 1) * line_gap;
    int box_w = max_w + pad * 2;
    int box_h = total_h + pad * 2;
    int x = (view->screen_w - box_w) / 2;
    int y = (view->screen_h - box_h) / 2;
    if (x < 8) x = 8;
    if (y < 8) y = 8;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect bg = {x, y, box_w, box_h};
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 190);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color text_color = {255, 255, 255, 255};
    int tx = x + pad;
    int ty = y + pad;
    for (int i = 0; i < line_count; i++) {
        draw_text(tx, ty, scale, lines[i], text_color);
        ty += text_h + line_gap;
    }
}

static void render_hint_overlay(const BoardView *view) {
    if (g_hint_count <= 0) return;
    if (g_hint_until != 0 && SDL_GetTicks() > g_hint_until) {
        clear_hints();
        return;
    }

    int side_margin = 8;
    int board_gap = 12;
    int board_left = view->offset_x;
    int board_right = view->offset_x + view->board_px;
    int avail_left = board_left - side_margin;
    int avail_right = view->screen_w - board_right - side_margin;
    int max_side = (avail_left > avail_right) ? avail_left : avail_right;

    int scale = (view->square >= 60) ? 3 : 2;
    int line_gap = 0;
    int text_h = 0;
    int pad = 0;
    int line_h = 0;
    int max_w = 0;
    int box_w = 0;
    int box_h = 0;

    while (1) {
        line_gap = (scale >= 3) ? 4 : ((scale == 2) ? 3 : 2);
        text_h = 7 * scale;
        pad = (scale >= 3) ? 10 : ((scale == 2) ? 8 : 6);
        line_h = text_h + line_gap;
        max_w = text_width_px("HINTS", scale);
        for (int i = 0; i < g_hint_count; i++) {
            int w = text_width_px(g_hint_lines[i], scale);
            if (w > max_w) max_w = w;
        }
        box_w = max_w + pad * 2;
        box_h = pad * 2 + text_h + g_hint_count * line_h;
        if (max_side <= 0 || box_w <= max_side - board_gap || scale <= 1) break;
        scale--;
    }

    int use_right = (avail_right >= avail_left);
    if (avail_left < box_w + board_gap && avail_right >= box_w + board_gap) use_right = 1;
    if (avail_right < box_w + board_gap && avail_left >= box_w + board_gap) use_right = 0;

    int x = use_right ? (board_right + board_gap) : (board_left - box_w - board_gap);
    int y = view->offset_y + 12;
    int board_bottom = view->offset_y + view->board_px;
    if (y + box_h > board_bottom - 8) y = board_bottom - box_h - 8;
    if (x < 8) x = 8;
    if (x + box_w > view->screen_w - 8) x = view->screen_w - box_w - 8;
    if (y < 8) y = 8;
    if (y + box_h > view->screen_h - 8) y = view->screen_h - box_h - 8;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect bg = {x, y, box_w, box_h};
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 190);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color title_color = {240, 210, 80, 255};
    SDL_Color text_color = {255, 255, 255, 255};
    int tx = x + pad;
    int ty = y + pad;
    draw_text(tx, ty, scale, "HINTS", title_color);
    ty += line_h;
    for (int i = 0; i < g_hint_count; i++) {
        draw_text(tx, ty, scale, g_hint_lines[i], text_color);
        ty += line_h;
    }
}

static void render_end_overlay(const BoardView *view) {
    if (!g_game_over) return;

    const char *line1 = NULL;
    const char *line2 = NULL;
    if (str_case_contains(g_status, "checkmate")) {
        line1 = "CHECKMATE";
    } else if (str_case_contains(g_status, "stalemate")) {
        line1 = "STALEMATE";
        line2 = "DRAW";
    }
    if (!line1) return;

    int side_margin = 8;
    int board_gap = 12;
    int board_left = view->offset_x;
    int board_right = view->offset_x + view->board_px;
    int avail_left = board_left - side_margin;
    int avail_right = view->screen_w - board_right - side_margin;
    int max_side = (avail_left > avail_right) ? avail_left : avail_right;

    int scale = (view->square >= 60) ? 3 : 2;
    int line_gap = 0;
    int text_h = 0;
    int pad = 0;
    int line_h = 0;
    int max_w = 0;
    int box_w = 0;
    int box_h = 0;

    while (1) {
        line_gap = (scale >= 3) ? 4 : ((scale == 2) ? 3 : 2);
        text_h = 7 * scale;
        pad = (scale >= 3) ? 10 : ((scale == 2) ? 8 : 6);
        line_h = text_h + line_gap;

        max_w = text_width_px(line1, scale);
        if (line2) {
            int w2 = text_width_px(line2, scale);
            if (w2 > max_w) max_w = w2;
        }
        box_w = max_w + pad * 2;
        box_h = pad * 2 + text_h + (line2 ? line_h : 0);
        if (max_side <= 0 || box_w <= max_side - board_gap || scale <= 1) break;
        scale--;
    }

    int use_right = (avail_right >= avail_left);
    if (avail_left < box_w + board_gap && avail_right >= box_w + board_gap) use_right = 1;
    if (avail_right < box_w + board_gap && avail_left >= box_w + board_gap) use_right = 0;

    int x = use_right ? (board_right + board_gap) : (board_left - box_w - board_gap);
    int y = view->offset_y + view->board_px - box_h - 12;
    if (x < 8) x = 8;
    if (x + box_w > view->screen_w - 8) x = view->screen_w - box_w - 8;
    if (y < 8) y = 8;
    if (y + box_h > view->screen_h - 8) y = view->screen_h - box_h - 8;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect bg = {x, y, box_w, box_h};
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 190);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color title_color = {240, 210, 80, 255};
    SDL_Color text_color = {255, 255, 255, 255};
    int tx = x + pad;
    int ty = y + pad;
    draw_text(tx, ty, scale, line1, title_color);
    if (line2) {
        ty += line_h;
        draw_text(tx, ty, scale, line2, text_color);
    }
}

static void update_palette_slots(const BoardView *view) {
    static const char white_pieces[6] = {'K', 'Q', 'R', 'B', 'N', 'P'};
    static const char black_pieces[6] = {'k', 'q', 'r', 'b', 'n', 'p'};

    int pad = 12;
    int slot = view->square;
    if (slot > 96) slot = 96;
    if (slot < 32) slot = 32;

    int side_left = view->offset_x;
    int side_right = view->screen_w - (view->offset_x + view->board_px);
    int min_side = (side_left < side_right) ? side_left : side_right;
    if (min_side > 0 && slot + pad * 2 > min_side) {
        slot = min_side - pad * 2;
        if (slot < 24) slot = 24;
    }

    int total_h = 6 * slot + 5 * pad;
    int y0 = view->offset_y + (view->board_px - total_h) / 2;
    if (y0 < pad) y0 = pad;

    int left_x = view->offset_x - slot - pad;
    int right_x = view->offset_x + view->board_px + pad;
    if (left_x < pad) left_x = pad;
    if (right_x + slot > view->screen_w - pad) right_x = view->screen_w - pad - slot;

    g_white_palette_col.x = left_x - pad / 2;
    g_white_palette_col.y = y0 - pad / 2;
    g_white_palette_col.w = slot + pad;
    g_white_palette_col.h = total_h + pad;

    g_black_palette_col.x = right_x - pad / 2;
    g_black_palette_col.y = y0 - pad / 2;
    g_black_palette_col.w = slot + pad;
    g_black_palette_col.h = total_h + pad;

    g_palette_count = 0;
    for (int i = 0; i < 6; i++) {
        PaletteSlot s;
        s.piece = white_pieces[i];
        s.rect.x = left_x;
        s.rect.y = y0 + i * (slot + pad);
        s.rect.w = slot;
        s.rect.h = slot;
        g_palette_slots[g_palette_count++] = s;
    }
    for (int i = 0; i < 6; i++) {
        PaletteSlot s;
        s.piece = black_pieces[i];
        s.rect.x = right_x;
        s.rect.y = y0 + i * (slot + pad);
        s.rect.w = slot;
        s.rect.h = slot;
        g_palette_slots[g_palette_count++] = s;
    }
}

static int palette_piece_at(int x, int y, char *out_piece) {
    for (int i = 0; i < g_palette_count; i++) {
        SDL_Rect r = g_palette_slots[i].rect;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
            if (out_piece) *out_piece = g_palette_slots[i].piece;
            return 1;
        }
    }
    return 0;
}

static void clear_board_for_setup(void) {
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            g_state.board[r][f] = '.';
        }
    }
    g_state.turn_is_white = 1;
    g_state.fullmove_number = 1;
    clear_special_state(&g_state);
}

static void enter_setup_mode(void) {
    close_save_menu();
    close_card_browser();
    g_flashcards_mode = 0;
    g_flash_next_ms = 0;
    clear_flash_order();
    g_setup_mode = 1;
    g_game_over = 0;
    g_engine_move_pending = 0;
    g_review_mode = 0;
    g_review_ply = 0;
    clear_hints();
    g_history_count = 0;
    g_last_from_r = g_last_from_f = g_last_to_r = g_last_to_f = -1;
    g_dragging = 0;
    g_drag_from_board = 0;
    clear_special_state(&g_state);
    set_setup_status();
}

static int start_from_setup_position(void) {
    if (count_piece_on_board('K') != 1 || count_piece_on_board('k') != 1) {
        set_status("SETUP invalid: need exactly one white king and one black king");
        return 0;
    }

    build_fen(&g_state, g_base_fen, sizeof(g_base_fen));
    g_use_startpos = 0;
    g_setup_mode = 0;
    g_flashcards_mode = 0;
    g_flash_next_ms = 0;
    clear_flash_order();
    g_game_over = 0;
    g_engine_move_pending = 0;
    g_review_mode = 0;
    g_review_ply = 0;
    clear_hints();
    g_history_count = 0;
    g_last_from_r = g_last_from_f = g_last_to_r = g_last_to_f = -1;

    if (!engine_new_game(&g_engine)) {
        g_game_over = 1;
        set_status("Engine reset failed");
        return 0;
    }

    evaluate_game_end(&g_state);
    if (!g_game_over && g_state.turn_is_white != g_human_is_white) {
        g_engine_move_pending = 1;
        set_status("Engine thinking...");
    }
    return 1;
}

static void render_board(void) {
    BoardView view;
    get_board_view(&view);
    if (g_setup_mode) {
        update_palette_slots(&view);
    }

    SDL_SetRenderDrawColor(renderer, 8, 8, 8, 255);
    SDL_RenderClear(renderer);

    SDL_Color light = {125, 125, 125, 255};
    SDL_Color dark = {105, 105, 105, 255};

    // Board frame area: half-square gray border around the board.
    int border = view.square / 2;
    if (border < 1) border = 1;
    SDL_Rect frame = {
        view.offset_x - border,
        view.offset_y - border,
        view.board_px + border * 2,
        view.board_px + border * 2
    };
    if (frame.x < 0) {
        frame.w += frame.x;
        frame.x = 0;
    }
    if (frame.y < 0) {
        frame.h += frame.y;
        frame.y = 0;
    }
    if (frame.x + frame.w > view.screen_w) frame.w = view.screen_w - frame.x;
    if (frame.y + frame.h > view.screen_h) frame.h = view.screen_h - frame.y;
    SDL_SetRenderDrawColor(renderer, 34, 34, 34, 255);
    SDL_RenderFillRect(renderer, &frame);

    // Orientation strips: white strip on white starting side, black on black starting side.
    int strip = border / 8;
    if (strip < 1) strip = 1;
    int x_white = 0, y_white = 0, x_black = 0, y_black = 0;
    board_to_screen(&view, 7, 4, &x_white, &y_white);  // white back rank
    board_to_screen(&view, 0, 4, &x_black, &y_black);  // black back rank
    int white_on_top = (y_white < y_black);

    SDL_Rect white_strip = {
        view.offset_x,
        white_on_top ? (view.offset_y - strip) : (view.offset_y + view.board_px),
        view.board_px,
        strip
    };
    SDL_Rect black_strip = {
        view.offset_x,
        white_on_top ? (view.offset_y + view.board_px) : (view.offset_y - strip),
        view.board_px,
        strip
    };
    SDL_SetRenderDrawColor(renderer, 232, 232, 232, 255);
    SDL_RenderFillRect(renderer, &white_strip);
    SDL_SetRenderDrawColor(renderer, 12, 12, 12, 255);
    SDL_RenderFillRect(renderer, &black_strip);

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            int x = 0;
            int y = 0;
            board_to_screen(&view, r, f, &x, &y);
            SDL_Rect sq = {x, y, view.square, view.square};

            SDL_Color c = ((r + f) % 2 == 0) ? light : dark;
            SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
            SDL_RenderFillRect(renderer, &sq);
        }
    }

    if (g_setup_mode) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 180);
        SDL_RenderFillRect(renderer, &g_white_palette_col);
        SDL_RenderFillRect(renderer, &g_black_palette_col);

        SDL_Color side_hi = {230, 180, 40, 220};
        SDL_Rect hi = g_state.turn_is_white ? g_white_palette_col : g_black_palette_col;
        SDL_SetRenderDrawColor(renderer, side_hi.r, side_hi.g, side_hi.b, side_hi.a);
        SDL_RenderDrawRect(renderer, &hi);

        for (int i = 0; i < g_palette_count; i++) {
            SDL_Rect r = g_palette_slots[i].rect;
            SDL_SetRenderDrawColor(renderer, 70, 70, 70, 200);
            SDL_RenderFillRect(renderer, &r);
            SDL_SetRenderDrawColor(renderer, 120, 120, 120, 220);
            SDL_RenderDrawRect(renderer, &r);

            SDL_Texture *tex = get_piece_texture(g_palette_slots[i].piece);
            if (tex) {
                SDL_RenderCopy(renderer, tex, NULL, &r);
            }
        }
    }

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            int skip = 0;
            if (g_dragging && g_drag_from_board && r == g_drag_from_r && f == g_drag_from_f) {
                skip = 1;
            }
            if (g_anim_active && r == g_anim_from_r && f == g_anim_from_f) {
                skip = 1;
            }
            if (g_anim_active && g_anim_has_rook && r == g_anim_rook_from_r && f == g_anim_rook_from_f) {
                skip = 1;
            }
            if (skip) continue;
            char piece = g_state.board[r][f];
            SDL_Texture *tex = get_piece_texture(piece);
            if (!tex) continue;
            int x = 0;
            int y = 0;
            board_to_screen(&view, r, f, &x, &y);
            SDL_Rect dst = {x, y, view.square, view.square};
            SDL_RenderCopy(renderer, tex, NULL, &dst);
        }
    }

    if (g_anim_active && g_anim_piece != '.') {
        SDL_Texture *tex = get_piece_texture(g_anim_piece);
        if (tex) {
            SDL_Rect dst = {
                (int)(g_anim_x + 0.5f),
                (int)(g_anim_y + 0.5f),
                view.square,
                view.square
            };
            SDL_RenderCopy(renderer, tex, NULL, &dst);
        }
        if (g_anim_has_rook && g_anim_rook_piece != '.') {
            SDL_Texture *rook_tex = get_piece_texture(g_anim_rook_piece);
            if (rook_tex) {
                SDL_Rect rook_dst = {
                    (int)(g_anim_rook_x + 0.5f),
                    (int)(g_anim_rook_y + 0.5f),
                    view.square,
                    view.square
                };
                SDL_RenderCopy(renderer, rook_tex, NULL, &rook_dst);
            }
        }
    }

    if (g_dragging && g_drag_piece != '.') {
        SDL_Texture *tex = get_piece_texture(g_drag_piece);
        if (tex) {
            SDL_Rect dst = {
                g_mouse_x - view.square / 2,
                g_mouse_y - view.square / 2,
                view.square,
                view.square
            };
            SDL_RenderCopy(renderer, tex, NULL, &dst);
        }
    }

    if (g_save_menu_active) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 180);
        SDL_Rect dim = {0, 0, view.screen_w, view.screen_h};
        SDL_RenderFillRect(renderer, &dim);

        int box_w = view.screen_w / 3;
        if (box_w < 320) box_w = 320;
        int box_h = (view.screen_h * 3) / 4;
        if (box_h < 260) box_h = 260;
        SDL_Rect box = {(view.screen_w - box_w) / 2, (view.screen_h - box_h) / 2, box_w, box_h};
        SDL_SetRenderDrawColor(renderer, 80, 80, 80, 220);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 130, 130, 130, 255);
        SDL_RenderDrawRect(renderer, &box);

        int scale = (view.square >= 60) ? 3 : 2;
        int text_h = 7 * scale;
        int line_gap = (scale >= 3) ? 4 : 3;
        int pad = (scale >= 3) ? 10 : 8;
        int line_h = text_h + line_gap;

        char path_label[640];
        if (g_save_rel_dir[0] == '\0') {
            snprintf(path_label, sizeof(path_label), "PATH: .");
        } else {
            snprintf(path_label, sizeof(path_label), "PATH: %s", g_save_rel_dir);
        }

        SDL_Color text_color = {255, 255, 255, 255};
        if (g_browser_mode == 2) {
            draw_text(box.x + pad, box.y + pad, scale, "FLASHCARDS MODE", text_color);
        } else {
            draw_text(box.x + pad, box.y + pad, scale, "SAVE FLASHCARD", text_color);
        }
        draw_text(box.x + pad, box.y + pad + line_h, scale, path_label, text_color);

        int list_top = box.y + pad + line_h * 3;
        int list_h = box.h - (list_top - box.y) - pad;
        int max_lines = list_h / line_h;
        if (max_lines < 1) max_lines = 1;

        if (g_save_index < g_save_scroll) g_save_scroll = g_save_index;
        if (g_save_index >= g_save_scroll + max_lines) {
            g_save_scroll = g_save_index - max_lines + 1;
        }

        for (int i = 0; i < max_lines; i++) {
            int idx = g_save_scroll + i;
            if (idx >= g_save_entry_count) break;

            char label[700];
            SaveDirEntry *entry = &g_save_entries[idx];
            if (entry->type == 1) {
                snprintf(label, sizeof(label), "[DIR] %s", entry->name);
            } else if (entry->type == 2) {
                snprintf(label, sizeof(label), "[..]");
            } else {
                snprintf(label, sizeof(label), "%s", entry->name);
            }

            SDL_Rect row = {box.x + pad - 3, list_top + i * line_h - 2, box.w - pad * 2 + 6, text_h + 6};
            if (idx == g_save_index) {
                SDL_SetRenderDrawColor(renderer, 40, 120, 255, 220);
            } else {
                SDL_SetRenderDrawColor(renderer, 120, 120, 120, 90);
            }
            SDL_RenderFillRect(renderer, &row);
            draw_text(box.x + pad, list_top + i * line_h, scale, label, text_color);
        }
    }

    if (g_card_browser_active) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 190);
        SDL_Rect dim = {0, 0, view.screen_w, view.screen_h};
        SDL_RenderFillRect(renderer, &dim);

        int box_w = (view.screen_w * 5) / 6;
        int box_h = (view.screen_h * 5) / 6;
        if (box_w < 560) box_w = 560;
        if (box_h < 360) box_h = 360;
        if (box_w > view.screen_w - 20) box_w = view.screen_w - 20;
        if (box_h > view.screen_h - 20) box_h = view.screen_h - 20;

        SDL_Rect box = {(view.screen_w - box_w) / 2, (view.screen_h - box_h) / 2, box_w, box_h};
        SDL_SetRenderDrawColor(renderer, 80, 80, 80, 225);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 130, 130, 130, 255);
        SDL_RenderDrawRect(renderer, &box);

        int scale = (view.square >= 60) ? 3 : 2;
        int text_h = 7 * scale;
        int line_gap = (scale >= 3) ? 4 : 3;
        int line_h = text_h + line_gap;
        int pad = (scale >= 3) ? 10 : 8;

        SDL_Color text_color = {255, 255, 255, 255};
        SDL_Color dim_color = {220, 220, 220, 255};
        draw_text(box.x + pad, box.y + pad, scale, "FLASHCARDS BROWSER", text_color);

        char deck_line[700];
        snprintf(deck_line, sizeof(deck_line), "DECK: %s", g_active_deck[0] ? g_active_deck : "(none)");
        draw_text(box.x + pad, box.y + pad + line_h, scale, deck_line, dim_color);
        draw_text(box.x + pad, box.y + pad + line_h * 2, scale, "UP/DOWN SELECT  ENTER LOAD  ESC CLOSE", dim_color);

        int content_top = box.y + pad + line_h * 3;
        int content_h = box.h - (content_top - box.y) - pad;
        int split_x = box.x + (box.w * 46) / 100;

        SDL_Rect list_rect = {box.x + pad, content_top, split_x - (box.x + pad) - pad, content_h};
        SDL_Rect prev_rect = {split_x + pad, content_top, box.x + box.w - (split_x + pad) - pad, content_h};

        SDL_SetRenderDrawColor(renderer, 65, 65, 65, 210);
        SDL_RenderFillRect(renderer, &list_rect);
        SDL_RenderFillRect(renderer, &prev_rect);
        SDL_SetRenderDrawColor(renderer, 110, 110, 110, 220);
        SDL_RenderDrawRect(renderer, &list_rect);
        SDL_RenderDrawRect(renderer, &prev_rect);

        int list_pad = 8;
        int list_top = list_rect.y + list_pad;
        int list_h = list_rect.h - list_pad * 2;
        int max_lines = list_h / line_h;
        if (max_lines < 1) max_lines = 1;

        if (g_card_index < g_card_scroll) g_card_scroll = g_card_index;
        if (g_card_index >= g_card_scroll + max_lines) g_card_scroll = g_card_index - max_lines + 1;
        if (g_card_scroll < 0) g_card_scroll = 0;

        for (int i = 0; i < max_lines; i++) {
            int idx = g_card_scroll + i;
            if (idx >= g_active_deck_file_count) break;

            SDL_Rect row = {list_rect.x + 3, list_top + i * line_h - 1, list_rect.w - 6, text_h + 5};
            if (idx == g_card_index) {
                SDL_SetRenderDrawColor(renderer, 40, 120, 255, 220);
            } else {
                SDL_SetRenderDrawColor(renderer, 120, 120, 120, 85);
            }
            SDL_RenderFillRect(renderer, &row);

            char label[700];
            snprintf(label, sizeof(label), "%s", g_active_deck_files[idx]);
            draw_text(list_rect.x + 8, list_top + i * line_h, scale, label, text_color);
        }

        int prev_pad = 10;
        int prev_title_y = prev_rect.y + prev_pad;
        draw_text(prev_rect.x + prev_pad, prev_title_y, scale, "THUMBNAIL", text_color);

        if (g_card_preview_valid) {
            char side_line[64];
            snprintf(side_line, sizeof(side_line), "SIDE TO MOVE: %s", g_card_preview_state.turn_is_white ? "WHITE" : "BLACK");
            draw_text(prev_rect.x + prev_pad, prev_title_y + line_h, scale, side_line, dim_color);

            int board_area_top = prev_title_y + line_h * 3;
            int avail_w = prev_rect.w - prev_pad * 2;
            int avail_h = prev_rect.y + prev_rect.h - board_area_top - prev_pad;
            int sq = avail_w / 8;
            int sq_h = avail_h / 8;
            if (sq_h < sq) sq = sq_h;
            if (sq < 8) sq = 8;
            int board_px = sq * 8;
            int bx = prev_rect.x + (prev_rect.w - board_px) / 2;
            int by = board_area_top + (avail_h - board_px) / 2;

            SDL_Color light = {125, 125, 125, 255};
            SDL_Color dark = {105, 105, 105, 255};
            for (int r = 0; r < BOARD_SIZE; r++) {
                for (int f = 0; f < BOARD_SIZE; f++) {
                    SDL_Rect sqr = {bx + f * sq, by + r * sq, sq, sq};
                    SDL_Color c = ((r + f) % 2 == 0) ? light : dark;
                    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
                    SDL_RenderFillRect(renderer, &sqr);

                    char piece = g_card_preview_state.board[r][f];
                    SDL_Texture *tex = get_piece_texture(piece);
                    if (tex) SDL_RenderCopy(renderer, tex, NULL, &sqr);
                }
            }
            SDL_SetRenderDrawColor(renderer, 130, 130, 130, 255);
            SDL_Rect border = {bx, by, board_px, board_px};
            SDL_RenderDrawRect(renderer, &border);
        } else {
            draw_text(prev_rect.x + prev_pad, prev_title_y + line_h * 2, scale, "PREVIEW UNAVAILABLE", dim_color);
        }
    }

    render_hint_overlay(&view);
    render_end_overlay(&view);
    render_help_overlay(&view);

    SDL_RenderPresent(renderer);
}

#ifdef _WIN32
static int engine_send_line(UciEngine *e, const char *fmt, ...) {
    if (!e || !e->stdin_write) return 0;

    char line[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    size_t len = strlen(line);
    if (len + 2 >= sizeof(line)) return 0;
    line[len++] = '\n';
    line[len] = '\0';

    DWORD written = 0;
    if (!WriteFile(e->stdin_write, line, (DWORD)len, &written, NULL)) {
        return 0;
    }
    return written == len;
}

static int engine_read_line(UciEngine *e, char *out, size_t out_size, DWORD timeout_ms) {
    if (!e || !e->stdout_read || !out || out_size == 0) return 0;

    DWORD start = GetTickCount();
    size_t used = 0;
    out[0] = '\0';

    while (1) {
        DWORD avail = 0;
        if (!PeekNamedPipe(e->stdout_read, NULL, 0, NULL, &avail, NULL)) {
            return 0;
        }

        if (avail > 0) {
            char ch = 0;
            DWORD got = 0;
            if (!ReadFile(e->stdout_read, &ch, 1, &got, NULL) || got == 0) {
                return 0;
            }
            if (ch == '\r') continue;
            if (ch == '\n') {
                out[used] = '\0';
                return 1;
            }
            if (used + 1 < out_size) {
                out[used++] = ch;
            }
        } else {
            DWORD wait = GetTickCount() - start;
            if (wait >= timeout_ms) {
                if (used > 0) {
                    out[used] = '\0';
                    return 1;
                }
                return 0;
            }

            DWORD proc_state = WaitForSingleObject(e->process, 0);
            if (proc_state == WAIT_OBJECT_0) {
                if (used > 0) {
                    out[used] = '\0';
                    return 1;
                }
                return 0;
            }
            Sleep(5);
        }
    }
}

static int engine_wait_for_token(UciEngine *e, const char *token, DWORD timeout_ms) {
    DWORD start = GetTickCount();
    char line[ENGINE_LINE_LEN];

    while ((GetTickCount() - start) < timeout_ms) {
        DWORD left = timeout_ms - (GetTickCount() - start);
        if (!engine_read_line(e, line, sizeof(line), left)) {
            continue;
        }
        if (strstr(line, token) != NULL) return 1;
    }
    return 0;
}

static int launch_engine(UciEngine *e, const char *engine_path, const char *weights_path) {
    memset(e, 0, sizeof(*e));

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE child_stdout_read = NULL;
    HANDLE child_stdout_write = NULL;
    HANDLE child_stdin_read = NULL;
    HANDLE child_stdin_write = NULL;

    if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0)) return 0;
    if (!SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0)) return 0;

    if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0)) return 0;
    if (!SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) return 0;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" --weights=\"%s\"", engine_path, weights_path);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_read;
    si.hStdOutput = child_stdout_write;
    si.hStdError = child_stdout_write;

    BOOL ok = CreateProcessA(
        NULL,
        cmd,
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    );

    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);

    if (!ok) {
        CloseHandle(child_stdout_read);
        CloseHandle(child_stdin_write);
        return 0;
    }

    e->process = pi.hProcess;
    e->thread = pi.hThread;
    e->stdin_write = child_stdin_write;
    e->stdout_read = child_stdout_read;
    e->ready = 0;
    return 1;
}

static int engine_handshake(UciEngine *e) {
    if (!engine_send_line(e, "uci")) return 0;
    if (!engine_wait_for_token(e, "uciok", ENGINE_TIMEOUT_MS)) return 0;

    // Human-like preset: objective/no contempt and mildly draw-biased.
    if (!engine_send_line(e, "setoption name ContemptMode value disable")) return 0;
    if (!engine_send_line(e, "setoption name DrawScore value 0.10")) return 0;
    // Optional tablebases if a local "syzygy" folder exists.
    if (is_directory_path("syzygy")) {
        if (!engine_send_line(e, "setoption name SyzygyPath value syzygy")) return 0;
    }

    if (!engine_send_line(e, "isready")) return 0;
    if (!engine_wait_for_token(e, "readyok", ENGINE_TIMEOUT_MS)) return 0;

    if (!engine_send_line(e, "ucinewgame")) return 0;
    if (!engine_send_line(e, "isready")) return 0;
    if (!engine_wait_for_token(e, "readyok", ENGINE_TIMEOUT_MS)) return 0;

    e->ready = 1;
    return 1;
}

static int engine_new_game(UciEngine *e) {
    if (!engine_send_line(e, "ucinewgame")) return 0;
    if (!engine_send_line(e, "isready")) return 0;
    return engine_wait_for_token(e, "readyok", ENGINE_TIMEOUT_MS);
}

static int engine_get_bestmove(UciEngine *e, char history[][8], int history_count,
                               int movetime_ms, char *bestmove, size_t bestmove_size) {
    if (!e || !e->ready) return 0;

    char pos[16384];
    if (g_use_startpos) {
        strcpy(pos, "position startpos");
    } else {
        snprintf(pos, sizeof(pos), "position fen %s", g_base_fen);
    }
    if (history_count > 0) {
        strcat(pos, " moves");
        for (int i = 0; i < history_count; i++) {
            strcat(pos, " ");
            strcat(pos, history[i]);
        }
    }

    if (!engine_send_line(e, "%s", pos)) return 0;
    if (!engine_send_line(e, "go movetime %d", movetime_ms)) return 0;

    char line[ENGINE_LINE_LEN];
    DWORD start = GetTickCount();
    while ((GetTickCount() - start) < ENGINE_TIMEOUT_MS) {
        DWORD left = ENGINE_TIMEOUT_MS - (GetTickCount() - start);
        if (!engine_read_line(e, line, sizeof(line), left)) continue;
        if (strncmp(line, "bestmove ", 9) == 0) {
            char token[16] = {0};
            if (sscanf(line + 9, "%15s", token) != 1) return 0;
            if (strcmp(token, "(none)") == 0) return 0;
            strncpy(bestmove, token, bestmove_size - 1);
            bestmove[bestmove_size - 1] = '\0';
            return 1;
        }
    }
    return 0;
}

static int parse_hint_info_line(const char *line, int *out_mpv,
                                char *out_move, size_t out_move_size,
                                char *out_score, size_t out_score_size,
                                int *out_score_kind, int *out_score_value) {
    if (!line || strncmp(line, "info ", 5) != 0) return 0;

    const char *mp = strstr(line, " multipv ");
    const char *pv = strstr(line, " pv ");
    if (!mp || !pv) return 0;

    int mpv = 0;
    if (sscanf(mp + 9, "%d", &mpv) != 1 || mpv < 1) return 0;

    char move[16] = {0};
    if (sscanf(pv + 4, "%15s", move) != 1) return 0;

    if (out_move && out_move_size > 0) {
        strncpy(out_move, move, out_move_size - 1);
        out_move[out_move_size - 1] = '\0';
    }
    if (out_score_kind) *out_score_kind = 0;
    if (out_score_value) *out_score_value = 0;
    if (out_score && out_score_size > 0) {
        out_score[0] = '\0';
        const char *mate = strstr(line, " score mate ");
        const char *cp = strstr(line, " score cp ");
        if (mate) {
            int mate_n = 0;
            if (sscanf(mate + 12, "%d", &mate_n) == 1) {
                snprintf(out_score, out_score_size, "MATE %d", mate_n);
                if (out_score_kind) *out_score_kind = 2;
                if (out_score_value) *out_score_value = mate_n;
            }
        } else if (cp) {
            int cp_n = 0;
            if (sscanf(cp + 10, "%d", &cp_n) == 1) {
                snprintf(out_score, out_score_size, "CP %d", cp_n);
                if (out_score_kind) *out_score_kind = 1;
                if (out_score_value) *out_score_value = cp_n;
            }
        }
    } else {
        const char *mate = strstr(line, " score mate ");
        const char *cp = strstr(line, " score cp ");
        if (mate) {
            int mate_n = 0;
            if (sscanf(mate + 12, "%d", &mate_n) == 1) {
                if (out_score_kind) *out_score_kind = 2;
                if (out_score_value) *out_score_value = mate_n;
            }
        } else if (cp) {
            int cp_n = 0;
            if (sscanf(cp + 10, "%d", &cp_n) == 1) {
                if (out_score_kind) *out_score_kind = 1;
                if (out_score_value) *out_score_value = cp_n;
            }
        }
    }
    if (out_mpv) *out_mpv = mpv;
    return 1;
}

static int engine_get_hints(UciEngine *e, char history[][8], int history_count,
                            int movetime_ms, char out_lines[][64], int max_lines, int *out_count) {
    if (out_count) *out_count = 0;
    if (!e || !e->ready || !out_lines || max_lines <= 0) return 0;

    for (int i = 0; i < max_lines; i++) out_lines[i][0] = '\0';

    char pos[16384];
    if (g_use_startpos) {
        strcpy(pos, "position startpos");
    } else {
        snprintf(pos, sizeof(pos), "position fen %s", g_base_fen);
    }
    if (history_count > 0) {
        strcat(pos, " moves");
        for (int i = 0; i < history_count; i++) {
            strcat(pos, " ");
            strcat(pos, history[i]);
        }
    }

    int ok = 0;
    if (!engine_send_line(e, "setoption name MultiPV value %d", max_lines)) goto done;
    if (!engine_send_line(e, "isready")) goto done;
    if (!engine_wait_for_token(e, "readyok", ENGINE_TIMEOUT_MS)) goto done;
    if (!engine_send_line(e, "%s", pos)) goto done;
    if (!engine_send_line(e, "go movetime %d", movetime_ms)) goto done;

    char line[ENGINE_LINE_LEN];
    DWORD start = GetTickCount();
    while ((GetTickCount() - start) < ENGINE_TIMEOUT_MS) {
        DWORD left = ENGINE_TIMEOUT_MS - (GetTickCount() - start);
        if (!engine_read_line(e, line, sizeof(line), left)) continue;
        if (strncmp(line, "bestmove ", 9) == 0) {
            break;
        }
        int mpv = 0;
        char move[16] = {0};
        char score[32] = {0};
        if (!parse_hint_info_line(line, &mpv, move, sizeof(move), score, sizeof(score), NULL, NULL)) continue;
        if (mpv < 1 || mpv > max_lines) continue;
        if (score[0] != '\0') {
            snprintf(out_lines[mpv - 1], 64, "%d. %s %s", mpv, move, score);
        } else {
            snprintf(out_lines[mpv - 1], 64, "%d. %s", mpv, move);
        }
        ok = 1;
    }

done:
    engine_send_line(e, "setoption name MultiPV value 1");
    engine_send_line(e, "isready");
    engine_wait_for_token(e, "readyok", ENGINE_TIMEOUT_MS);

    int count = 0;
    for (int i = 0; i < max_lines; i++) {
        if (out_lines[i][0] == '\0') continue;
        if (count != i) {
            strncpy(out_lines[count], out_lines[i], 63);
            out_lines[count][63] = '\0';
            out_lines[i][0] = '\0';
        }
        count++;
    }
    if (out_count) *out_count = count;
    return ok && count > 0;
}

static int engine_get_varied_move(UciEngine *e, char history[][8], int history_count,
                                  int movetime_ms, char *bestmove, size_t bestmove_size) {
    if (!e || !e->ready || !bestmove || bestmove_size == 0) return 0;

    typedef struct {
        int have;
        char move[16];
        int score_kind;   // 0=unknown, 1=cp, 2=mate
        int score_value;
    } PvMove;

    PvMove pv[ENGINE_VARIETY_MULTI_PV];
    memset(pv, 0, sizeof(pv));

    char pos[16384];
    if (g_use_startpos) {
        strcpy(pos, "position startpos");
    } else {
        snprintf(pos, sizeof(pos), "position fen %s", g_base_fen);
    }
    if (history_count > 0) {
        strcat(pos, " moves");
        for (int i = 0; i < history_count; i++) {
            strcat(pos, " ");
            strcat(pos, history[i]);
        }
    }

    char strict_best[16] = {0};
    if (!engine_send_line(e, "setoption name MultiPV value %d", ENGINE_VARIETY_MULTI_PV)) return 0;
    if (!engine_send_line(e, "isready")) return 0;
    if (!engine_wait_for_token(e, "readyok", ENGINE_TIMEOUT_MS)) return 0;
    if (!engine_send_line(e, "%s", pos)) return 0;
    if (!engine_send_line(e, "go movetime %d", movetime_ms)) return 0;

    char line[ENGINE_LINE_LEN];
    DWORD start = GetTickCount();
    while ((GetTickCount() - start) < ENGINE_TIMEOUT_MS) {
        DWORD left = ENGINE_TIMEOUT_MS - (GetTickCount() - start);
        if (!engine_read_line(e, line, sizeof(line), left)) continue;

        if (strncmp(line, "bestmove ", 9) == 0) {
            char token[16] = {0};
            if (sscanf(line + 9, "%15s", token) == 1 && strcmp(token, "(none)") != 0) {
                strncpy(strict_best, token, sizeof(strict_best) - 1);
                strict_best[sizeof(strict_best) - 1] = '\0';
            }
            break;
        }

        int mpv = 0;
        int score_kind = 0;
        int score_value = 0;
        char move[16] = {0};
        if (!parse_hint_info_line(line, &mpv, move, sizeof(move), NULL, 0, &score_kind, &score_value)) continue;
        if (mpv < 1 || mpv > ENGINE_VARIETY_MULTI_PV) continue;
        int idx = mpv - 1;
        pv[idx].have = 1;
        pv[idx].score_kind = score_kind;
        pv[idx].score_value = score_value;
        strncpy(pv[idx].move, move, sizeof(pv[idx].move) - 1);
        pv[idx].move[sizeof(pv[idx].move) - 1] = '\0';
    }

    engine_send_line(e, "setoption name MultiPV value 1");
    engine_send_line(e, "isready");
    engine_wait_for_token(e, "readyok", ENGINE_TIMEOUT_MS);

    int best_idx = -1;
    for (int i = 0; i < ENGINE_VARIETY_MULTI_PV; i++) {
        if (!pv[i].have) continue;
        best_idx = i;
        break;
    }

    if (best_idx < 0) {
        if (strict_best[0] == '\0') return 0;
        strncpy(bestmove, strict_best, bestmove_size - 1);
        bestmove[bestmove_size - 1] = '\0';
        return 1;
    }

    int cp_window = (history_count < ENGINE_VARIETY_OPENING_PLIES)
        ? ENGINE_VARIETY_CP_WINDOW_OPENING
        : ENGINE_VARIETY_CP_WINDOW_LATE;

    int picks[ENGINE_VARIETY_MULTI_PV];
    int weights[ENGINE_VARIETY_MULTI_PV];
    int pick_count = 0;
    int total_weight = 0;

    for (int i = 0; i < ENGINE_VARIETY_MULTI_PV; i++) {
        if (!pv[i].have) continue;

        int allow = 0;
        if (i == best_idx) {
            allow = 1;
        } else if (pv[best_idx].score_kind == 1 && pv[i].score_kind == 1) {
            int loss = pv[best_idx].score_value - pv[i].score_value;
            if (loss <= cp_window) allow = 1;
        } else if (pv[best_idx].score_kind == 2 && pv[i].score_kind == 2) {
            int b = pv[best_idx].score_value;
            int c = pv[i].score_value;
            int same_sign = ((b > 0 && c > 0) || (b < 0 && c < 0));
            int diff = b - c;
            if (diff < 0) diff = -diff;
            if (same_sign && diff <= 1) allow = 1;
        } else if (pv[best_idx].score_kind == 0 || pv[i].score_kind == 0) {
            if (i <= best_idx + 1) allow = 1;
        }

        if (!allow) continue;

        int w = 20;
        if (i == 0) w = 100;
        else if (i == 1) w = 58;
        else if (i == 2) w = 34;

        if (history_count >= ENGINE_VARIETY_OPENING_PLIES) {
            w = (w * 65) / 100;
            if (w < 1) w = 1;
        }

        picks[pick_count] = i;
        weights[pick_count] = w;
        total_weight += w;
        pick_count++;
    }

    int chosen = best_idx;
    if (pick_count > 1 && total_weight > 0) {
        if (history_count >= ENGINE_VARIETY_OPENING_PLIES && (rand() % 100) < 65) {
            chosen = best_idx;
        } else {
            int roll = rand() % total_weight;
            int acc = 0;
            for (int i = 0; i < pick_count; i++) {
                acc += weights[i];
                if (roll < acc) {
                    chosen = picks[i];
                    break;
                }
            }
        }
    } else if (pick_count == 1) {
        chosen = picks[0];
    }

    if (!pv[chosen].have || pv[chosen].move[0] == '\0') {
        if (strict_best[0] == '\0') return 0;
        strncpy(bestmove, strict_best, bestmove_size - 1);
        bestmove[bestmove_size - 1] = '\0';
        return 1;
    }

    strncpy(bestmove, pv[chosen].move, bestmove_size - 1);
    bestmove[bestmove_size - 1] = '\0';
    return 1;
}

static void close_engine(UciEngine *e) {
    if (!e) return;

    if (e->stdin_write) {
        engine_send_line(e, "quit");
    }
    if (e->process) {
        WaitForSingleObject(e->process, 2000);
    }

    if (e->stdin_write) CloseHandle(e->stdin_write);
    if (e->stdout_read) CloseHandle(e->stdout_read);
    if (e->thread) CloseHandle(e->thread);
    if (e->process) CloseHandle(e->process);

    memset(e, 0, sizeof(*e));
}
#else
static int launch_engine(UciEngine *e, const char *engine_path, const char *weights_path) {
    (void)e;
    (void)engine_path;
    (void)weights_path;
    return 0;
}

static int engine_handshake(UciEngine *e) {
    (void)e;
    return 0;
}

static int engine_new_game(UciEngine *e) {
    (void)e;
    return 0;
}

static int engine_get_bestmove(UciEngine *e, char history[][8], int history_count,
                               int movetime_ms, char *bestmove, size_t bestmove_size) {
    (void)e;
    (void)history;
    (void)history_count;
    (void)movetime_ms;
    (void)bestmove;
    (void)bestmove_size;
    return 0;
}

static int engine_get_hints(UciEngine *e, char history[][8], int history_count,
                            int movetime_ms, char out_lines[][64], int max_lines, int *out_count) {
    (void)e;
    (void)history;
    (void)history_count;
    (void)movetime_ms;
    (void)out_lines;
    if (out_count) *out_count = 0;
    (void)max_lines;
    return 0;
}

static int engine_get_varied_move(UciEngine *e, char history[][8], int history_count,
                                  int movetime_ms, char *bestmove, size_t bestmove_size) {
    (void)e;
    (void)history;
    (void)history_count;
    (void)movetime_ms;
    (void)bestmove;
    (void)bestmove_size;
    return 0;
}

static void close_engine(UciEngine *e) {
    (void)e;
}
#endif

static int try_make_human_move(int from_r, int from_f, int to_r, int to_f) {
    if (g_review_mode) return 0;
    char piece = g_state.board[from_r][from_f];
    if (piece == '.') return 0;
    if (is_white_piece(piece) != g_human_is_white) return 0;
    if (g_state.turn_is_white != g_human_is_white) return 0;

    char promo = '\0';
    if ((char)toupper((unsigned char)piece) == 'P' && (to_r == 0 || to_r == 7)) {
        promo = 'q';
    }

    Move m;
    if (!build_move(&g_state, from_r, from_f, to_r, to_f, promo, &m)) return 0;
    if (!is_legal_move(&g_state, &m)) return 0;

    char uci[8];
    move_to_uci(&m, uci, sizeof(uci));
    apply_move(&g_state, &m);
    push_history(uci);
    clear_hints();

    g_last_from_r = m.from_r;
    g_last_from_f = m.from_f;
    g_last_to_r = m.to_r;
    g_last_to_f = m.to_f;

    evaluate_game_end(&g_state);
    if (!g_game_over && g_state.turn_is_white != g_human_is_white) {
        g_engine_move_pending = 1;
        set_status("Engine thinking...");
    }

    return 1;
}

static void animate_engine_move(const Move *m) {
    if (!m) return;
    char piece = g_state.board[m->from_r][m->from_f];
    if (piece == '.') return;

    g_anim_active = 1;
    g_anim_from_r = m->from_r;
    g_anim_from_f = m->from_f;
    g_anim_piece = piece;
    g_anim_has_rook = 0;

    if (m->is_castle) {
        int row = is_white_piece(piece) ? 7 : 0;
        int rook_from_f = (m->to_f == 6) ? 7 : 0;
        char rook_piece = g_state.board[row][rook_from_f];
        if (rook_piece != '.') {
            g_anim_has_rook = 1;
            g_anim_rook_from_r = row;
            g_anim_rook_from_f = rook_from_f;
            g_anim_rook_piece = rook_piece;
        }
    }

    Uint32 start = SDL_GetTicks();
    while (g_running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                g_running = 0;
                break;
            }
        }
        if (!g_running) break;

        Uint32 now = SDL_GetTicks();
        float t = (ENGINE_MOVE_ANIM_MS > 0)
            ? (float)(now - start) / (float)ENGINE_MOVE_ANIM_MS
            : 1.0f;
        if (t > 1.0f) t = 1.0f;
        BoardView view;
        get_board_view(&view);
        int sx = 0, sy = 0, ex = 0, ey = 0;
        board_to_screen(&view, m->from_r, m->from_f, &sx, &sy);
        board_to_screen(&view, m->to_r, m->to_f, &ex, &ey);
        g_anim_x = (float)sx + ((float)(ex - sx) * t);
        g_anim_y = (float)sy + ((float)(ey - sy) * t);

        if (g_anim_has_rook) {
            int rook_to_f = (m->to_f == 6) ? 5 : 3;
            int rsx = 0, rsy = 0, rex = 0, rey = 0;
            board_to_screen(&view, g_anim_rook_from_r, g_anim_rook_from_f, &rsx, &rsy);
            board_to_screen(&view, g_anim_rook_from_r, rook_to_f, &rex, &rey);
            g_anim_rook_x = (float)rsx + ((float)(rex - rsx) * t);
            g_anim_rook_y = (float)rsy + ((float)(rey - rsy) * t);
        }

        render_board();

        if (t >= 1.0f) break;
        SDL_Delay(10);
    }

    g_anim_active = 0;
    g_anim_from_r = -1;
    g_anim_from_f = -1;
    g_anim_piece = '.';
    g_anim_has_rook = 0;
    g_anim_rook_from_r = -1;
    g_anim_rook_from_f = -1;
    g_anim_rook_piece = '.';
}

static int randomized_engine_movetime_ms(void) {
    int jitter = ENGINE_MOVE_TIME_JITTER_MS;
    if (jitter <= 0) return ENGINE_MOVE_TIME_MS;
    int span = jitter * 2 + 1;
    int delta = (rand() % span) - jitter;
    int movetime = ENGINE_MOVE_TIME_MS + delta;
    if (movetime < 300) movetime = 300;
    return movetime;
}

static void request_hints(void) {
    if (g_setup_mode) {
        set_status("Hints unavailable in setup mode");
        return;
    }
    if (g_save_menu_active) {
        set_status("Close browser before requesting hints");
        return;
    }
    if (g_card_browser_active) {
        set_status("Close card browser before requesting hints");
        return;
    }
    if (g_review_mode) {
        set_status("Return to live position before requesting hints");
        return;
    }
    if (g_game_over) {
        set_status("Game over");
        return;
    }
    if (g_engine_move_pending || g_state.turn_is_white != g_human_is_white) {
        set_status("Wait for your turn to request hints");
        return;
    }

    clear_hints();
    if (!engine_get_hints(&g_engine, g_history, g_history_count,
                          HINT_MOVETIME_MS, g_hint_lines, HINT_MULTI_PV, &g_hint_count)) {
        clear_hints();
        set_status("Hint request failed");
        return;
    }

    g_hint_until = SDL_GetTicks() + HINT_DISPLAY_MS;
    set_status("Hints ready");
}

static int do_engine_move(void) {
    char bestmove[16];
    int movetime = randomized_engine_movetime_ms();
    if (!engine_get_varied_move(&g_engine, g_history, g_history_count,
                                movetime, bestmove, sizeof(bestmove))) {
        if (!engine_get_bestmove(&g_engine, g_history, g_history_count,
                                 movetime, bestmove, sizeof(bestmove))) {
            g_game_over = 1;
            set_status("Engine error: no bestmove");
            return 0;
        }
    }

    if (bestmove[0] == '\0') {
        g_game_over = 1;
        set_status("Engine error: no bestmove");
        return 0;
    }

    Move m;
    if (!parse_uci_to_move(&g_state, bestmove, &m)) {
        g_game_over = 1;
        set_status("Engine sent illegal move: %s", bestmove);
        return 0;
    }

    animate_engine_move(&m);
    if (!g_running) return 0;

    apply_move(&g_state, &m);
    push_history(bestmove);
    clear_hints();

    g_last_from_r = m.from_r;
    g_last_from_f = m.from_f;
    g_last_to_r = m.to_r;
    g_last_to_f = m.to_f;

    evaluate_game_end(&g_state);
    return 1;
}

static void start_new_game(void) {
    close_card_browser();
    close_save_menu();
    g_flashcards_mode = 0;
    g_flash_next_ms = 0;
    clear_flash_order();
    reset_position(&g_state);
    g_setup_mode = 0;
    g_game_over = 0;
    g_engine_move_pending = 0;
    g_review_mode = 0;
    g_review_ply = 0;
    clear_hints();
    g_history_count = 0;
    g_last_from_r = g_last_from_f = g_last_to_r = g_last_to_f = -1;
    g_use_startpos = 1;
    g_base_fen[0] = '\0';

    if (!engine_new_game(&g_engine)) {
        g_game_over = 1;
        set_status("Engine reset failed");
        return;
    }

    set_status("Your move");

    if (g_state.turn_is_white != g_human_is_white) {
        g_engine_move_pending = 1;
        set_status("Engine thinking...");
    }
}

static int init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 0;
    }
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
        printf("SDL_image init failed: %s\n", IMG_GetError());
        SDL_Quit();
        return 0;
    }

    window = SDL_CreateWindow(
        "Leela Play",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_SIZE,
        SCREEN_SIZE,
        SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP
    );
    if (!window) {
        printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        IMG_Quit();
        SDL_Quit();
        return 0;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        printf("SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        window = NULL;
        IMG_Quit();
        SDL_Quit();
        return 0;
    }

    return 1;
}

static void cleanup(void) {
    close_save_menu();
    close_card_browser();
    clear_active_deck_files();
    clear_flash_order();
    for (int i = 0; i < 256; i++) {
        if (piece_textures[i]) {
            SDL_DestroyTexture(piece_textures[i]);
            piece_textures[i] = NULL;
        }
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = NULL;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
    IMG_Quit();
    SDL_Quit();
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    srand((unsigned int)time(NULL));
    if (!init_sdl()) return 1;

    if (!launch_engine(&g_engine, "lc0.exe", "791556.pb.gz")) {
        printf("Failed to launch lc0.exe\n");
        cleanup();
        return 1;
    }

    if (!engine_handshake(&g_engine)) {
        printf("Failed UCI handshake with lc0\n");
        close_engine(&g_engine);
        cleanup();
        return 1;
    }

    start_new_game();

    while (g_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                g_running = 0;
                continue;
            }

            if (g_save_menu_active && handle_save_menu_event(&ev)) {
                continue;
            }
            if (g_card_browser_active && handle_card_browser_event(&ev)) {
                continue;
            }

            if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode key = ev.key.keysym.sym;
                if (key == SDLK_q) {
                    g_running = 0;
                } else if (key == SDLK_ESCAPE) {
                    g_show_help = !g_show_help;
                } else if (g_show_help) {
                    continue;
                } else if (key == SDLK_n) {
                    if (g_flashcards_mode) {
                        flashcards_next_random();
                    } else {
                        start_new_game();
                    }
                } else if (key == SDLK_r) {
                    restart_current_position();
                } else if (key == SDLK_s) {
                    open_save_menu();
                } else if (key == SDLK_e) {
                    if (!g_setup_mode) {
                        enter_setup_mode();
                    }
                } else if (key == SDLK_p) {
                    open_flashcards_menu();
                } else if (key == SDLK_b) {
                    open_card_browser();
                } else if (key == SDLK_h) {
                    request_hints();
                } else if (key == SDLK_LEFT) {
                    step_review(-1);
                } else if (key == SDLK_RIGHT) {
                    step_review(1);
                } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                    if (g_review_mode) {
                        set_review_ply(g_history_count);
                    } else if (g_setup_mode) {
                        start_from_setup_position();
                    }
                } else if (key == SDLK_LEFTBRACKET) {
                    if (!g_setup_mode) cycle_active_deck(-1);
                } else if (key == SDLK_RIGHTBRACKET) {
                    if (!g_setup_mode) cycle_active_deck(1);
                } else if (key == SDLK_t) {
                    if (g_setup_mode) {
                        g_state.turn_is_white = !g_state.turn_is_white;
                        set_setup_status();
                    }
                } else if (key == SDLK_c) {
                    if (g_setup_mode) {
                        clear_board_for_setup();
                        set_setup_status();
                    }
                } else if (key == SDLK_f) {
                    g_human_is_white = !g_human_is_white;
                    g_view_from_white = g_human_is_white;
                    if (g_setup_mode) {
                        set_setup_status();
                    } else if (!g_game_over && g_state.turn_is_white != g_human_is_white) {
                        g_engine_move_pending = 1;
                        set_status("Engine thinking...");
                    } else if (!g_game_over) {
                        set_status("Your move");
                    }
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                BoardView view;
                get_board_view(&view);
                int r = -1;
                int f = -1;
                int on_board = screen_to_board(&view, ev.button.x, ev.button.y, &r, &f);

                if (g_setup_mode) {
                    update_palette_slots(&view);
                    char pp = '.';
                    if (on_board) {
                        char p = g_state.board[r][f];
                        if (p == '.') continue;
                        g_dragging = 1;
                        g_drag_from_board = 1;
                        g_drag_from_r = r;
                        g_drag_from_f = f;
                        g_drag_piece = p;
                        g_mouse_x = ev.button.x;
                        g_mouse_y = ev.button.y;
                    } else if (palette_piece_at(ev.button.x, ev.button.y, &pp)) {
                        g_dragging = 1;
                        g_drag_from_board = 0;
                        g_drag_from_r = -1;
                        g_drag_from_f = -1;
                        g_drag_piece = pp;
                        g_mouse_x = ev.button.x;
                        g_mouse_y = ev.button.y;
                    }
                } else {
                    if (g_review_mode) continue;
                    if (g_game_over || g_state.turn_is_white != g_human_is_white) continue;
                    if (!on_board) continue;

                    char p = g_state.board[r][f];
                    if (p == '.') continue;
                    if (is_white_piece(p) != g_human_is_white) continue;

                    g_dragging = 1;
                    g_drag_from_board = 1;
                    g_drag_from_r = r;
                    g_drag_from_f = f;
                    g_drag_piece = p;
                    g_mouse_x = ev.button.x;
                    g_mouse_y = ev.button.y;
                }
            } else if (ev.type == SDL_MOUSEMOTION) {
                if (g_dragging) {
                    g_mouse_x = ev.motion.x;
                    g_mouse_y = ev.motion.y;
                }
            } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                if (!g_dragging) continue;

                BoardView view;
                get_board_view(&view);
                int to_r = -1;
                int to_f = -1;
                int on_board = screen_to_board(&view, ev.button.x, ev.button.y, &to_r, &to_f);

                int from_r = g_drag_from_r;
                int from_f = g_drag_from_f;
                int from_board = g_drag_from_board;
                char drag_piece = g_drag_piece;
                g_dragging = 0;
                g_drag_from_board = 0;
                g_drag_from_r = g_drag_from_f = -1;
                g_drag_piece = '.';

                if (g_setup_mode) {
                    if (from_board && from_r >= 0 && from_f >= 0) {
                        g_state.board[from_r][from_f] = '.';
                    }
                    if (on_board) {
                        g_state.board[to_r][to_f] = drag_piece;
                    }
                } else if (on_board && from_board) {
                    try_make_human_move(from_r, from_f, to_r, to_f);
                }
            }
        }

        render_board();

        if (g_engine_move_pending && !g_game_over && !g_setup_mode &&
            !g_card_browser_active && !g_review_mode) {
            g_engine_move_pending = 0;
            render_board();
            do_engine_move();
        }

        if (g_flashcards_mode && g_game_over && g_flash_next_ms != 0 &&
            !g_card_browser_active && !g_review_mode) {
            if (SDL_GetTicks() >= g_flash_next_ms) {
                flashcards_next_random();
            }
        }

        SDL_Delay(8);
    }

    close_engine(&g_engine);
    cleanup();
    return 0;
}
