#ifndef BI_H
#define BI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <ctype.h>
#include <regex.h>
#include <termios.h>
#include <unistd.h>

/* ========================================================================
 * 定数
 * ======================================================================== */

#define MAX_UNDO_LEVELS 100
#define UNKNOWN         UINT64_MAX
#define LENONSCR        (19 * 16)
#define BOTTOMLN        22
#define RELEN           128

/* ========================================================================
 * readline
 * ======================================================================== */

#ifdef HAVE_READLINE
#  include <readline/readline.h>
#  include <readline/history.h>
#else
char* readline(const char *prompt);
void  add_history(const char *line);
void  clear_history(void);
#endif

/* ========================================================================
 * 前方宣言
 * ======================================================================== */

typedef struct BiEditor BiEditor;

/* ========================================================================
 * ByteArray — 動的バイト配列
 * ======================================================================== */

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} ByteArray;

void      bytearray_init(ByteArray *arr);
void      bytearray_push(ByteArray *arr, uint8_t val);
void      bytearray_insert(ByteArray *arr, size_t pos, const uint8_t *data, size_t len);
void      bytearray_delete(ByteArray *arr, size_t start, size_t end);
void      bytearray_free(ByteArray *arr);
ByteArray bytearray_copy(const ByteArray *src);

/* ========================================================================
 * MatchArray — 検索ヒット配列
 * ======================================================================== */

typedef struct {
    size_t pos;
    size_t len;
} Match;

typedef struct {
    Match  *data;
    size_t  size;
    size_t  capacity;
} MatchArray;

void matcharray_init(MatchArray *arr);
void matcharray_push(MatchArray *arr, Match match);
void matcharray_clear(MatchArray *arr);
void matcharray_free(MatchArray *arr);

/* ========================================================================
 * 差分 Undo/Redo — 型定義
 * ======================================================================== */

typedef enum {
    DIFF_OVW,         /* memory_set: 1バイト上書き               */
    DIFF_OVW_REGION,  /* memory_overwrite: 複数バイト上書き       */
    DIFF_INS,         /* memory_insert: 挿入                     */
    DIFF_DEL          /* memory_delete: 削除                     */
} DiffOp;

typedef struct {
    DiffOp    op;
    size_t    pos;          /* 操作開始アドレス                   */
    size_t    orig_mem_len; /* 操作前の mem.size (OVW系の縮小復元用) */
    uint8_t   old_byte;     /* DIFF_OVW: 変更前の値               */
    uint8_t   new_byte;     /* DIFF_OVW: 変更後の値               */
    ByteArray old_data;     /* DIFF_DEL / DIFF_OVW_REGION: 変更前バイト列 */
    ByteArray new_data;     /* DIFF_INS / DIFF_OVW_REGION: 変更後バイト列 */
} DiffEntry;

typedef struct {
    DiffEntry *entries;
    size_t     size;
    size_t     capacity;
} DiffLog;

typedef struct {
    DiffLog  log;
    size_t   mark_before[26];
    size_t   mark_after[26];
    bool     modified_before;
    bool     lastchange_before;
} DiffState;

typedef struct {
    DiffState *data;
    size_t     size;
    size_t     capacity;
} DiffStack;

void difflog_init(DiffLog *log);
void difflog_push(DiffLog *log, const DiffEntry *e);
void difflog_free(DiffLog *log);

void diffstack_init(DiffStack *stack);
void diffstack_push(DiffStack *stack, DiffState *state);
DiffState* diffstack_pop(DiffStack *stack);
void diffstack_free(DiffStack *stack);

/* ========================================================================
 * Terminal
 * ======================================================================== */

typedef struct {
    char      termcol[32];
    int       coltab[8];
    BiEditor *editor;
} Terminal;

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
int  terminal_getch(void);

/* ========================================================================
 * MemoryBuffer
 * ======================================================================== */

typedef struct {
    ByteArray  mem;
    ByteArray  yank;
    size_t     mark[26];
    bool       modified;
    bool       lastchange;
    DiffLog   *current_diff; /* 記録中の差分ログ (NULL=非記録中) */
} MemoryBuffer;

void    memory_init(MemoryBuffer *mem);
uint8_t memory_read(MemoryBuffer *mem, size_t addr);
void    memory_set(MemoryBuffer *mem, size_t addr, uint8_t data);
void    memory_insert(MemoryBuffer *mem, size_t start, const uint8_t *data, size_t len);
bool    memory_delete(MemoryBuffer *mem, size_t start, size_t end, bool yf,
                      size_t (*yank_func)(MemoryBuffer*, size_t, size_t));
size_t  memory_yank(MemoryBuffer *mem, size_t start, size_t end);
void    memory_overwrite(MemoryBuffer *mem, size_t start, const uint8_t *data, size_t len);
void    memory_free(MemoryBuffer *mem);

/* ========================================================================
 * SearchEngine
 * ======================================================================== */

/* Display の前方宣言 (循環参照を避けるため) */
typedef struct Display Display;

typedef struct {
    MemoryBuffer *memory;
    void         *display;   /* Display* (循環参照回避のため void* ) */
    BiEditor     *editor;
    ByteArray     smem;
    bool          regexp;
    char          remem[128];
    size_t        span;
    bool          nff;
} SearchEngine;

void   search_init(SearchEngine *search, MemoryBuffer *mem, Display *disp, BiEditor *editor);
int    search_hit(SearchEngine *search, size_t addr);
int    search_hitre(SearchEngine *search, size_t addr);
size_t search_next(SearchEngine *search, size_t fp, size_t mem_len);
size_t search_last(SearchEngine *search, size_t fp, size_t mem_len);
void   search_all(SearchEngine *search, size_t mem_len, MatchArray *matches);
void   search_free(SearchEngine *search);

/* ========================================================================
 * Display
 * ======================================================================== */

struct Display {
    Terminal     *term;
    MemoryBuffer *memory;
    size_t        homeaddr;
    int           curx;
    int           cury;
    bool          utf8;
    bool          insmod;
    int           repsw;
    MatchArray    highlight_ranges;
};

void   display_init(Display *disp, Terminal *term, MemoryBuffer *mem);
size_t display_fpos(Display *disp);
void   display_jump(Display *disp, size_t addr);
bool   display_is_highlighted(Display *disp, size_t addr);
int    display_printchar(Display *disp, size_t a);
void   display_repaint(Display *disp, const char *filename);
void   display_printdata(Display *disp);
void   display_clrmm(Display *disp);
void   display_stdmm(Display *disp, const char *msg, bool scripting, bool verbose);
void   display_stdmm_wait(Display *disp, const char *msg, bool scripting, bool verbose);
void   display_stderr(Display *disp, const char *msg, bool scripting, bool verbose);
void   display_free(Display *disp);

/* ========================================================================
 * Parser
 * ======================================================================== */

typedef struct {
    MemoryBuffer *memory;
    Display      *display;
} Parser;

void     parser_init(Parser *parser, MemoryBuffer *mem, Display *disp);
size_t   parser_skipspc(const char *s, size_t idx);
uint64_t parser_get_value(Parser *parser, const char *s, size_t *idx);
uint64_t parser_expression(Parser *parser, const char *s, size_t *idx);
size_t   parser_get_restr(const char *s, size_t idx, char *result);
size_t   parser_get_hexs(Parser *parser, const char *s, size_t idx, ByteArray *result);
char*    parser_comment(const char *s);

/* ========================================================================
 * HistoryManager
 * ======================================================================== */

typedef struct {
    char  **command_history;
    size_t  command_count;
    size_t  command_capacity;
    char  **search_history;
    size_t  search_count;
    size_t  search_capacity;
} HistoryManager;

void  history_init(HistoryManager *hist);
char* history_getln(HistoryManager *hist, const char *prompt, const char *mode);
void  history_free(HistoryManager *hist);

/* ========================================================================
 * FileManager
 * ======================================================================== */

typedef struct {
    MemoryBuffer *memory;
    char          filename[4096];
    bool          newfile;
} FileManager;

void filemgr_init(FileManager *fmgr, MemoryBuffer *mem);
bool filemgr_readfile(FileManager *fmgr, const char *filename, char *msg, size_t msg_size);
bool filemgr_writefile(FileManager *fmgr, const char *filename, char *msg, size_t msg_size);
bool filemgr_readfile_partial(FileManager *fmgr, const char *filename,
                               size_t offset, size_t length,
                               char *msg, size_t msg_size);
bool filemgr_writefile_partial(FileManager *fmgr, const char *filename,
                                char *msg, size_t msg_size);

/* ========================================================================
 * BiEditor — メインエディタ
 * ======================================================================== */

struct BiEditor {
    bool          scriptingflag;
    bool          verbose;
    Terminal      term;
    MemoryBuffer  memory;
    Display       display;
    Parser        parser;
    HistoryManager history;
    SearchEngine  search;
    FileManager   filemgr;

    /* 差分 undo/redo スタック */
    DiffStack     undo_stack;
    DiffStack     redo_stack;

    /* 差分記録状態 (save_undo_state / commit_undo の間だけ有効) */
    bool          diff_active;
    size_t        diff_mark_snapshot[26];
    bool          diff_modified_snapshot;
    bool          diff_lastchange_snapshot;

    int           cp;
};

void  editor_init(BiEditor *editor, const char *termcol);
void  editor_save_undo_state(BiEditor *editor);   /* 差分記録開始 */
void  editor_commit_undo(BiEditor *editor);        /* 差分記録確定 */
bool  editor_dec_undo(BiEditor *editor);           /* 記録キャンセル */
bool  editor_undo(BiEditor *editor);
bool  editor_redo(BiEditor *editor);
void  editor_fedit(BiEditor *editor);
void  editor_free(BiEditor *editor);
int   editor_commandline(BiEditor *editor, const char *line);
int   editor_scripting(BiEditor *editor, const char *scriptfile);

void     editor_opeand(BiEditor *editor, uint64_t x, uint64_t x2, uint64_t x3);
void     editor_opeor(BiEditor *editor, uint64_t x, uint64_t x2, uint64_t x3);
void     editor_opexor(BiEditor *editor, uint64_t x, uint64_t x2, uint64_t x3);
void     editor_openot(BiEditor *editor, uint64_t x, uint64_t x2);
uint64_t editor_movmem(BiEditor *editor, uint64_t start, uint64_t end, uint64_t dest);
void     editor_shift_rotate(BiEditor *editor, uint64_t x, uint64_t x2,
                              int times, int bit, bool multibyte, char direction);
size_t   editor_searchnextnoloop(BiEditor *editor, size_t fp);
int      editor_scommand(BiEditor *editor, uint64_t start, uint64_t end,
                          bool xf, bool xf2, const char *line, size_t idx);

#endif /* BI_H */
