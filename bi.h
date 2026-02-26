#ifndef BI_H
#define BI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <regex.h>

// readlineが利用可能な場合のみ使用
#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#else
// readline代替実装用
char* readline(const char *prompt);
void add_history(const char *line);
void clear_history(void);
#endif

#define VERSION "3.4.5"
#define MAX_UNDO_LEVELS 100
#define LENONSCR (19 * 16)
#define BOTTOMLN 22
#define RELEN 128
#define UNKNOWN 0xFFFFFFFFFFFFFFFFULL

// 動的配列の実装
typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} ByteArray;

typedef struct {
    uint64_t *data;
    size_t size;
    size_t capacity;
} Uint64Array;

typedef struct {
    size_t pos;
    size_t len;
} Match;

typedef struct {
    Match *data;
    size_t size;
    size_t capacity;
} MatchArray;

// 前方宣言
typedef struct BiEditor BiEditor;

// Terminal構造体
typedef struct {
    char termcol[16];
    int coltab[8];
    BiEditor *editor;  // scriptingflagにアクセスするため
} Terminal;

// MemoryBuffer構造体
typedef struct {
    ByteArray mem;
    ByteArray yank;
    uint64_t mark[26];
    bool modified;
    bool lastchange;
} MemoryBuffer;

// Undo状態
typedef struct {
    ByteArray mem;
    bool modified;
    bool lastchange;
    uint64_t mark[26];
} UndoState;

typedef struct {
    UndoState *data;
    size_t size;
    size_t capacity;
} UndoStack;

// SearchEngine構造体
typedef struct {
    MemoryBuffer *memory;
    struct Display *display;
    BiEditor *editor;
    ByteArray smem;
    bool regexp;
    char remem[1024];
    size_t span;
    bool nff;
} SearchEngine;

// Display構造体
typedef struct Display {
    Terminal *term;
    MemoryBuffer *memory;
    size_t homeaddr;
    int curx;
    int cury;
    bool utf8;
    int repsw;
    bool insmod;
    MatchArray highlight_ranges;
} Display;

// Parser構造体
typedef struct {
    MemoryBuffer *memory;
    Display *display;
} Parser;

// HistoryManager構造体
typedef struct {
    char **command_history;
    size_t command_count;
    size_t command_capacity;
    char **search_history;
    size_t search_count;
    size_t search_capacity;
} HistoryManager;

// FileManager構造体
typedef struct {
    MemoryBuffer *memory;
    char filename[256];
    bool newfile;
} FileManager;

// BiEditor構造体
struct BiEditor {
    bool scriptingflag;
    bool verbose;
    Terminal term;
    MemoryBuffer memory;
    Display display;
    Parser parser;
    HistoryManager history;
    SearchEngine search;
    FileManager filemgr;
    UndoStack undo_stack;
    UndoStack redo_stack;
    size_t cp;
};

// 動的配列関数
void bytearray_init(ByteArray *arr);
void bytearray_push(ByteArray *arr, uint8_t val);
void bytearray_insert(ByteArray *arr, size_t pos, const uint8_t *data, size_t len);
void bytearray_delete(ByteArray *arr, size_t start, size_t end);
void bytearray_free(ByteArray *arr);
ByteArray bytearray_copy(const ByteArray *src);

void uint64array_init(Uint64Array *arr);
void uint64array_push(Uint64Array *arr, uint64_t val);
void uint64array_free(Uint64Array *arr);

void matcharray_init(MatchArray *arr);
void matcharray_push(MatchArray *arr, Match match);
void matcharray_clear(MatchArray *arr);
void matcharray_free(MatchArray *arr);

void undostack_init(UndoStack *stack);
void undostack_push(UndoStack *stack, const UndoState *state);
UndoState* undostack_pop(UndoStack *stack);
void undostack_free(UndoStack *stack);

// Terminal関数
void terminal_init(Terminal *term, const char *termcol, BiEditor *editor);
bool terminal_scripting(Terminal *term);
void terminal_nocursor(Terminal *term);
void terminal_dispcursor(Terminal *term);
void terminal_locate(Terminal *term, int x, int y);
void terminal_clear(Terminal *term);
void terminal_clrline(Terminal *term);
void terminal_color(Terminal *term, int col1, int col2);
void terminal_resetcolor(Terminal *term);
void terminal_highlight_color(Terminal *term);
int terminal_getch(void);

// MemoryBuffer関数
void memory_init(MemoryBuffer *mem);
uint8_t memory_read(MemoryBuffer *mem, size_t addr);
void memory_set(MemoryBuffer *mem, size_t addr, uint8_t data);
void memory_insert(MemoryBuffer *mem, size_t start, const uint8_t *data, size_t len);
bool memory_delete(MemoryBuffer *mem, size_t start, size_t end, bool yf, 
                   size_t (*yank_func)(MemoryBuffer*, size_t, size_t));
size_t memory_yank(MemoryBuffer *mem, size_t start, size_t end);
void memory_overwrite(MemoryBuffer *mem, size_t start, const uint8_t *data, size_t len);
void memory_free(MemoryBuffer *mem);

// SearchEngine関数
void search_init(SearchEngine *search, MemoryBuffer *mem, Display *disp, BiEditor *editor);
int search_hit(SearchEngine *search, size_t addr);
int search_hitre(SearchEngine *search, size_t addr);
size_t search_next(SearchEngine *search, size_t fp, size_t mem_len);
size_t search_last(SearchEngine *search, size_t fp, size_t mem_len);
void search_all(SearchEngine *search, size_t mem_len, MatchArray *matches);
void search_free(SearchEngine *search);

// Display関数
void display_init(Display *disp, Terminal *term, MemoryBuffer *mem);
size_t display_fpos(Display *disp);
void display_jump(Display *disp, size_t addr);
void display_repaint(Display *disp, const char *filename);
void display_printdata(Display *disp);
void display_clrmm(Display *disp);
void display_stdmm(Display *disp, const char *msg, bool scripting, bool verbose);
void display_stderr(Display *disp, const char *msg, bool scripting, bool verbose);
void display_free(Display *disp);

// Parser関数
void parser_init(Parser *parser, MemoryBuffer *mem, Display *disp);
size_t parser_skipspc(const char *s, size_t idx);
uint64_t parser_get_value(Parser *parser, const char *s, size_t *idx);
uint64_t parser_expression(Parser *parser, const char *s, size_t *idx);
size_t parser_get_restr(const char *s, size_t idx, char *result);
size_t parser_get_hexs(Parser *parser, const char *s, size_t idx, ByteArray *result);
char* parser_comment(const char *s);

// HistoryManager関数
void history_init(HistoryManager *hist);
char* history_getln(HistoryManager *hist, const char *prompt, const char *mode);
void history_free(HistoryManager *hist);

// FileManager関数
void filemgr_init(FileManager *fmgr, MemoryBuffer *mem);
bool filemgr_readfile(FileManager *fmgr, const char *filename, char *msg, size_t msg_size);
bool filemgr_writefile(FileManager *fmgr, const char *filename, char *msg, size_t msg_size);

// BiEditor関数
void editor_init(BiEditor *editor, const char *termcol);
void editor_save_undo_state(BiEditor *editor);
bool editor_undo(BiEditor *editor);
bool editor_redo(BiEditor *editor);
void editor_fedit(BiEditor *editor);
int editor_commandline(BiEditor *editor, const char *line);
void editor_free(BiEditor *editor);

#endif // BI_H
