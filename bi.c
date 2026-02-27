#define _POSIX_C_SOURCE 200809L
#include "bi.h"

/* ========================================================================
 * readline代替実装（readlineライブラリがない環境用）
 * ======================================================================== */

#ifndef HAVE_READLINE
char* readline(const char *prompt) {
    if (prompt) {
        printf("%s", prompt);
        fflush(stdout);
    }
    
    char *line = malloc(4096);
    if (!line) {
        fprintf(stderr, "Fatal error: Memory allocation failed in readline\n");
        return NULL;
    }
    
    if (fgets(line, 4096, stdin) == NULL) {
        free(line);
        return NULL;
    }
    
    // 改行削除
    size_t len = strlen(line);
    if (len > 0 && line[len-1] == '\n') {
        line[len-1] = '\0';
    }
    
    return line;
}

void add_history(const char *line) {
    (void)line;  // 未使用警告を抑制
}

void clear_history(void) {
    // 何もしない
}
#endif

/* ========================================================================
 * 動的配列の実装
 * ======================================================================== */

void bytearray_init(ByteArray *arr) {
    arr->data = NULL;
    arr->size = 0;
    arr->capacity = 0;
}

void bytearray_push(ByteArray *arr, uint8_t val) {
    if (arr->size >= arr->capacity) {
        size_t new_cap = arr->capacity == 0 ? 16 : arr->capacity * 2;
        uint8_t *new_data = realloc(arr->data, new_cap);
        if (!new_data) {
            fprintf(stderr, "Fatal error: Memory allocation failed in bytearray_push\n");
            exit(1);
        }
        arr->data = new_data;
        arr->capacity = new_cap;
    }
    arr->data[arr->size++] = val;
}

void bytearray_insert(ByteArray *arr, size_t pos, const uint8_t *data, size_t len) {
    if (len == 0) return;
    
    size_t new_size = arr->size + len;
    if (new_size > arr->capacity) {
        size_t new_cap = arr->capacity == 0 ? 16 : arr->capacity;
        while (new_cap < new_size) new_cap *= 2;
        uint8_t *new_data = realloc(arr->data, new_cap);
        if (!new_data) {
            fprintf(stderr, "Fatal error: Memory allocation failed in bytearray_insert\n");
            exit(1);
        }
        arr->data = new_data;
        arr->capacity = new_cap;
    }
    
    if (pos < arr->size) {
        memmove(arr->data + pos + len, arr->data + pos, arr->size - pos);
    }
    memcpy(arr->data + pos, data, len);
    arr->size = new_size;
}

void bytearray_delete(ByteArray *arr, size_t start, size_t end) {
    if (start >= arr->size || end >= arr->size || start > end) return;
    
    size_t len = end - start + 1;
    if (end + 1 < arr->size) {
        memmove(arr->data + start, arr->data + end + 1, arr->size - end - 1);
    }
    arr->size -= len;
}

void bytearray_free(ByteArray *arr) {
    if (arr->data) {
        free(arr->data);
        arr->data = NULL;
    }
    arr->size = 0;
    arr->capacity = 0;
}

ByteArray bytearray_copy(const ByteArray *src) {
    ByteArray dst;
    bytearray_init(&dst);
    if (src->size > 0) {
        dst.data = malloc(src->size);
        if (!dst.data) {
            fprintf(stderr, "Fatal error: Memory allocation failed in bytearray_copy\n");
            exit(1);
        }
        memcpy(dst.data, src->data, src->size);
        dst.size = src->size;
        dst.capacity = src->size;
    }
    return dst;
}

void matcharray_init(MatchArray *arr) {
    arr->data = NULL;
    arr->size = 0;
    arr->capacity = 0;
}

void matcharray_push(MatchArray *arr, Match match) {
    if (arr->size >= arr->capacity) {
        size_t new_cap = arr->capacity == 0 ? 16 : arr->capacity * 2;
        Match *new_data = realloc(arr->data, new_cap * sizeof(Match));
        if (!new_data) {
            fprintf(stderr, "Fatal error: Memory allocation failed in matcharray_push\n");
            exit(1);
        }
        arr->data = new_data;
        arr->capacity = new_cap;
    }
    arr->data[arr->size++] = match;
}

void matcharray_clear(MatchArray *arr) {
    arr->size = 0;
}

void matcharray_free(MatchArray *arr) {
    if (arr->data) {
        free(arr->data);
        arr->data = NULL;
    }
    arr->size = 0;
    arr->capacity = 0;
}

void undostack_init(UndoStack *stack) {
    stack->data = NULL;
    stack->size = 0;
    stack->capacity = 0;
}

void undostack_push(UndoStack *stack, const UndoState *state) {
    if (stack->size >= stack->capacity) {
        size_t new_cap = stack->capacity == 0 ? 16 : stack->capacity * 2;
        UndoState *new_data = realloc(stack->data, new_cap * sizeof(UndoState));
        if (!new_data) {
            fprintf(stderr, "Fatal error: Memory allocation failed in undostack_push\n");
            exit(1);
        }
        stack->data = new_data;
        stack->capacity = new_cap;
    }
    stack->data[stack->size++] = *state;
}

UndoState* undostack_pop(UndoStack *stack) {
    if (stack->size == 0) return NULL;
    return &stack->data[--stack->size];
}

void undostack_free(UndoStack *stack) {
    for (size_t i = 0; i < stack->size; i++) {
        bytearray_free(&stack->data[i].mem);
    }
    if (stack->data) {
        free(stack->data);
        stack->data = NULL;
    }
    stack->size = 0;
    stack->capacity = 0;
}

/* ========================================================================
 * Terminal実装
 * ======================================================================== */

void terminal_init(Terminal *term, const char *termcol, BiEditor *editor) {
    strncpy(term->termcol, termcol, sizeof(term->termcol) - 1);
    term->termcol[sizeof(term->termcol) - 1] = '\0';
    int coltab[] = {0, 1, 4, 5, 2, 6, 3, 7};
    memcpy(term->coltab, coltab, sizeof(coltab));
    term->editor = editor;
}

bool terminal_scripting(Terminal *term) {
    return term->editor != NULL && term->editor->scriptingflag;
}

void terminal_nocursor(Terminal *term) {
    if (terminal_scripting(term)) return;
    printf("\x1b[?25l");
    fflush(stdout);
}

void terminal_dispcursor(Terminal *term) {
    if (terminal_scripting(term)) return;
    printf("\x1b[?25h");
    fflush(stdout);
}

void terminal_locate(Terminal *term, int x, int y) {
    if (terminal_scripting(term)) return;
    printf("\x1b[%d;%dH", y + 1, x + 1);
    fflush(stdout);
}

void terminal_clear(Terminal *term) {
    if (terminal_scripting(term)) return;
    printf("\x1b[2J");
    fflush(stdout);
    terminal_locate(term, 0, 0);
}

void terminal_clrline(Terminal *term) {
    if (terminal_scripting(term)) return;
    printf("\x1b[2K");
    fflush(stdout);
}

void terminal_color(Terminal *term, int col1, int col2) {
    if (terminal_scripting(term)) return;
    if (strcmp(term->termcol, "black") == 0) {
        printf("\x1b[3%dm\x1b[4%dm", term->coltab[col1], term->coltab[col2]);
    } else {
        printf("\x1b[3%dm\x1b[4%dm", term->coltab[0], term->coltab[7]);
    }
    fflush(stdout);
}

void terminal_resetcolor(Terminal *term) {
    if (terminal_scripting(term)) return;
    printf("\x1b[0m");
}

void terminal_highlight_color(Terminal *term) {
    if (terminal_scripting(term)) return;
    printf("\x1b[1;96;44m");
    fflush(stdout);
}

int terminal_getch(void) {
    struct termios old_settings, new_settings;
    int ch;
    
    tcgetattr(STDIN_FILENO, &old_settings);
    new_settings = old_settings;
    new_settings.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_settings);
    
    ch = getchar();
    
    tcsetattr(STDIN_FILENO, TCSANOW, &old_settings);
    return ch;
}

/* ========================================================================
 * MemoryBuffer実装
 * ======================================================================== */

void memory_init(MemoryBuffer *mem) {
    bytearray_init(&mem->mem);
    bytearray_init(&mem->yank);
    for (int i = 0; i < 26; i++) {
        mem->mark[i] = UNKNOWN;
    }
    mem->modified = false;
    mem->lastchange = false;
}

uint8_t memory_read(MemoryBuffer *mem, size_t addr) {
    if (addr >= mem->mem.size) return 0;
    return mem->mem.data[addr];
}

void memory_set(MemoryBuffer *mem, size_t addr, uint8_t data) {
    while (addr >= mem->mem.size) {
        bytearray_push(&mem->mem, 0);
    }
    mem->mem.data[addr] = data & 0xFF;
    mem->modified = true;
    mem->lastchange = true;
}

void memory_insert(MemoryBuffer *mem, size_t start, const uint8_t *data, size_t len) {
    bytearray_insert(&mem->mem, start, data, len);
    mem->modified = true;
    mem->lastchange = true;
}

bool memory_delete(MemoryBuffer *mem, size_t start, size_t end, bool yf,
                   size_t (*yank_func)(MemoryBuffer*, size_t, size_t)) {
    size_t length = end - start + 1;
    if (length == 0 || start >= mem->mem.size || end>(mem->mem.size-1)) return false;
    
    if (yf && yank_func) {
        yank_func(mem, start, end);
    }
    
    bytearray_delete(&mem->mem, start, end);
    mem->lastchange = true;
    mem->modified = true;
    return true;
}

size_t memory_yank(MemoryBuffer *mem, size_t start, size_t end) {
    size_t length = end - start + 1;
    if (length == 0 || start >= mem->mem.size) return 0;
    
    bytearray_free(&mem->yank);
    bytearray_init(&mem->yank);
    
    size_t cnt = 0;
    for (size_t j = start; j <= end && j < mem->mem.size; j++) {
        bytearray_push(&mem->yank, mem->mem.data[j]);
        cnt++;
    }
    return cnt;
}

void memory_overwrite(MemoryBuffer *mem, size_t start, const uint8_t *data, size_t len) {
    if (len == 0) return;
    
    while (start + len > mem->mem.size) {
        bytearray_push(&mem->mem, 0);
    }
    
    memcpy(mem->mem.data + start, data, len);
    mem->lastchange = true;
    mem->modified = true;
}

void memory_free(MemoryBuffer *mem) {
    bytearray_free(&mem->mem);
    bytearray_free(&mem->yank);
}

/* ========================================================================
 * SearchEngine実装
 * ======================================================================== */

void search_init(SearchEngine *search, MemoryBuffer *mem, Display *disp, BiEditor *editor) {
    search->memory = mem;
    search->display = disp;
    search->editor = editor;
    bytearray_init(&search->smem);
    search->regexp = false;
    search->remem[0] = '\0';
    search->span = 0;
    search->nff = true;
}

int search_hit(SearchEngine *search, size_t addr) {
    for (size_t i = 0; i < search->smem.size; i++) {
        if (addr + i < search->memory->mem.size &&
            search->memory->mem.data[addr + i] == search->smem.data[i]) {
            continue;
        } else {
            return 0;
        }
    }
    return 1;
}

int search_hitre(SearchEngine *search, size_t addr) {
    if (search->remem[0] == '\0') return -1;
    
    regex_t regex;
    regmatch_t match[1];
    int reti;
    
    // 検索範囲のデータを取得
    size_t len = (addr < search->memory->mem.size - RELEN) ? 
                 RELEN : search->memory->mem.size - addr;
    if (len == 0) return -1;
    
    // バイナリデータをNULL文字を考慮して処理
    // NULL文字を空白に置き換える
    char *str = malloc(len + 1);
    if (!str) return -1;
    
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = search->memory->mem.data[addr + i];
        // NULL文字を空白に置き換え、それ以外はそのまま
        str[i] = (byte == 0) ? ' ' : byte;
    }
    str[len] = '\0';
    
    // 正規表現コンパイル
    reti = regcomp(&regex, search->remem, REG_EXTENDED);
    if (reti) {
        free(str);
        return -1;
    }
    
    // マッチング - 位置0から始まるマッチのみ
    reti = regexec(&regex, str, 1, match, 0);
    regfree(&regex);
    
    if (reti == 0 && match[0].rm_so == 0) {
        search->span = match[0].rm_eo - match[0].rm_so;
        free(str);
        return 1;
    }
    
    free(str);
    return 0;
}

size_t search_next(SearchEngine *search, size_t fp, size_t mem_len) {
    size_t curpos = fp;
    size_t start = fp;
    bool wrapped = false;
    
    if (!search->regexp && search->smem.size == 0) {
        return (size_t)-1;
    }
    
    // Wait.メッセージを表示
    display_stdmm_wait(search->display, "Wait.", search->editor->scriptingflag, search->editor->verbose);
    
    while (true) {
        int f = search->regexp ? search_hitre(search, curpos) : search_hit(search, curpos);
        
        if (f == 1) {
            if (wrapped==false)
                display_clrmm(search->display);
            return curpos;
        } else if (f < 0) {
            if (wrapped==false)
                display_clrmm(search->display);
            return (size_t)-1;
        }
        
        curpos++;
        
        if (curpos >= mem_len) {
            if (search->nff) {
                if (!wrapped) {
                    // 最初のwrap around
                    display_stdmm_wait(search->display, 
                        "Search reached BOTTOM, wrap around to TOP", 
                        search->editor->scriptingflag,
                        search->editor->verbose);
                    wrapped = true;
                }
                curpos = 0;
            } else {
                display_clrmm(search->display);
                return (size_t)-1;
            }
        }
        
        if (curpos == start) {
            //display_clrmm(search->display);
            return (size_t)-1;
        }
    }
}

size_t search_last(SearchEngine *search, size_t fp, size_t mem_len) {
    size_t curpos = fp;
    size_t start = fp;
    bool wrapped = false;
    
    if (!search->regexp && search->smem.size == 0) {
        return (size_t)-1;
    }
    
    // Wait.メッセージを表示
    display_stdmm_wait(search->display, "Wait.", search->editor->scriptingflag, search->editor->verbose);
    
    while (true) {
        int f = search->regexp ? search_hitre(search, curpos) : search_hit(search, curpos);
        
        if (f == 1) {
            if (wrapped==false)
                display_clrmm(search->display);
            return curpos;
        } else if (f < 0) {
            if (wrapped==false)
                display_clrmm(search->display);
            return (size_t)-1;
        }
        
        if (curpos == 0) {
            if (!wrapped && mem_len > 0) {
                // 最初のwrap around
                display_stdmm_wait(search->display, 
                    "Search reached TOP, wrap around to BOTTOM", 
                    search->editor->scriptingflag,
                    search->editor->verbose);
                wrapped = true;
            }
            curpos = mem_len > 0 ? mem_len - 1 : 0;
        } else {
            curpos--;
        }
        
        if (curpos == start) {
            //display_clrmm(search->display);
            return (size_t)-1;
        }
    }
}

void search_all(SearchEngine *search, size_t mem_len, MatchArray *matches) {
    matcharray_clear(matches);
    
    if (!search->regexp && search->smem.size == 0) {
        return;
    }
    
    // Wait.メッセージを表示
    display_stdmm_wait(search->display, "Wait.", search->editor->scriptingflag, search->editor->verbose);
    
    size_t curpos = 0;
    size_t max_results = 10000;
    
    while (curpos < mem_len && matches->size < max_results) {
        int f = search->regexp ? search_hitre(search, curpos) : search_hit(search, curpos);
        
        if (f == 1) {
            Match m;
            m.pos = curpos;
            m.len = search->regexp ? search->span : search->smem.size;
            matcharray_push(matches, m);
            curpos += (m.len > 0) ? m.len : 1;
        } else if (f < 0) {
            break;
        } else {
            curpos++;
        }
    }
    
    display_clrmm(search->display);
}

void search_free(SearchEngine *search) {
    bytearray_free(&search->smem);
}

/* ========================================================================
 * Display実装
 * ======================================================================== */

void display_init(Display *disp, Terminal *term, MemoryBuffer *mem) {
    disp->term = term;
    disp->memory = mem;
    disp->homeaddr = 0;
    disp->curx = 0;
    disp->cury = 0;
    disp->utf8 = false;
    disp->repsw = 0;
    disp->insmod = false;
    matcharray_init(&disp->highlight_ranges);
}

size_t display_fpos(Display *disp) {
    return disp->homeaddr + disp->curx / 2 + disp->cury * 16;
}

void display_jump(Display *disp, size_t addr) {
    if (addr < disp->homeaddr || addr >= disp->homeaddr + LENONSCR) {
        disp->homeaddr = addr & ~0xFF;
    }
    size_t i = addr - disp->homeaddr;
    disp->curx = (i & 0xF) * 2;
    disp->cury = i / 16;
}

bool display_is_highlighted(Display *disp, size_t addr) {
    for (size_t i = 0; i < disp->highlight_ranges.size; i++) {
        Match *m = &disp->highlight_ranges.data[i];
        if (addr >= m->pos && addr < m->pos + m->len) {
            return true;
        }
    }
    return false;
}

int display_printchar(Display *disp, size_t a) {
    if (a >= disp->memory->mem.size) {
        printf("~");
        return 1;
    }
    
    uint8_t byte = disp->memory->mem.data[a];
    
    if (disp->utf8) {
        // UTF-8モード
        if (byte < 0x80 || (byte >= 0x80 && byte <= 0xBF) || byte >= 0xF8) {
            // ASCII or continuation byte or invalid
            printf("%c", (byte >= 0x20 && byte <= 0x7E) ? byte : '.');
            return 1;
        } else if (byte >= 0xC0 && byte <= 0xDF) {
            // 2-byte UTF-8
            if (a + 1 < disp->memory->mem.size) {
                uint8_t bytes[2] = {disp->memory->mem.data[a], disp->memory->mem.data[a + 1]};
                // UTF-8として妥当かチェック
                if ((bytes[1] & 0xC0) == 0x80) {
                    // 妥当なUTF-8
                    char utf8str[3] = {bytes[0], bytes[1], 0};
                    printf("%s", utf8str);
                    return 2;
                }
            }
            printf(".");
            return 1;
        } else if (byte >= 0xE0 && byte <= 0xEF) {
            // 3-byte UTF-8
            if (a + 2 < disp->memory->mem.size) {
                uint8_t bytes[3] = {
                    disp->memory->mem.data[a],
                    disp->memory->mem.data[a + 1],
                    disp->memory->mem.data[a + 2]
                };
                if ((bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80) {
                    char utf8str[4] = {bytes[0], bytes[1], bytes[2], 0};
                    printf("%s ", utf8str);
                    return 3;
                }
            }
            printf(".");
            return 1;
        } else if (byte >= 0xF0 && byte <= 0xF7) {
            // 4-byte UTF-8
            if (a + 3 < disp->memory->mem.size) {
                uint8_t bytes[4] = {
                    disp->memory->mem.data[a],
                    disp->memory->mem.data[a + 1],
                    disp->memory->mem.data[a + 2],
                    disp->memory->mem.data[a + 3]
                };
                if ((bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80 && (bytes[3] & 0xC0) == 0x80) {
                    char utf8str[5] = {bytes[0], bytes[1], bytes[2], bytes[3], 0};
                    printf("%s  ", utf8str);
                    return 4;
                }
            }
            printf(".");
            return 1;
        }
    }
    
    // 非UTF-8モードまたはUTF-8として無効
    printf("%c", (byte >= 0x20 && byte <= 0x7E) ? byte : '.');
    return 1;
}

void display_repaint(Display *disp, const char *filename) {
    // Print title
    terminal_locate(disp->term, 0, 0);
    terminal_color(disp->term, 6, 0);
    printf("bi C version 3.5.0 by Taisuke Maekawa           utf8mode:%s     %s   ",
           disp->utf8 ? (disp->repsw ? "on " : "off") : "off",
           disp->insmod ? "insert   " : "overwrite");
    
    terminal_color(disp->term, 5, 0);
    char fn[36];
    strncpy(fn, filename, 35);
    fn[35] = '\0';
    printf("\nfile:[%-35s] length:%zu bytes [%smodified]    ",
           fn, disp->memory->mem.size, disp->memory->modified ? "" : "not ");
    
    // Print header
    terminal_nocursor(disp->term);
    terminal_locate(disp->term, 0, 2);
    terminal_color(disp->term, 4, 0);
    printf("OFFSET       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F 0123456789ABCDEF ");
    
    // Print hex dump
    terminal_color(disp->term, 7, 0);
    for (int y = 0; y < LENONSCR / 16; y++) {
        terminal_color(disp->term, 5, 0);
        terminal_locate(disp->term, 0, 3 + y);
        printf("%012zX ", disp->homeaddr + y * 16);
        
        // Hex part
        for (int i = 0; i < 16; i++) {
            size_t a = y * 16 + i + disp->homeaddr;
            bool in_hl = disp->highlight_ranges.size > 0 && display_is_highlighted(disp, a);
            
            if (in_hl) {
                terminal_highlight_color(disp->term);
                if (a >= disp->memory->mem.size) {
                    printf("~~");
                } else {
                    printf("%02X", disp->memory->mem.data[a]);
                }
                terminal_resetcolor(disp->term);
                terminal_color(disp->term, 7, 0);
                printf(" ");
            } else {
                terminal_color(disp->term, 7, 0);
                if (a >= disp->memory->mem.size) {
                    printf("~~ ");
                } else {
                    printf("%02X ", disp->memory->mem.data[a]);
                }
            }
        }
        
        // ASCII/UTF-8 part - 黄色に変更
        terminal_color(disp->term, 6, 0);  // 黄色（coltab[6]=3）
        if (disp->utf8) {
            // UTF-8モード
            int col = 0;
            for (int i = 0; i < 16 && col < 16; ) {
                size_t a = y * 16 + i + disp->homeaddr;
                bool in_hl = disp->highlight_ranges.size > 0 && display_is_highlighted(disp, a);
                
                if (in_hl) {
                    terminal_highlight_color(disp->term);
                }
                
                int len = display_printchar(disp, a);
                
                if (in_hl) {
                    terminal_resetcolor(disp->term);
                    terminal_color(disp->term, 6, 0);
                }
                
                i += len;
                col++;
            }
            // 残りを空白で埋める
            while (col < 16) {
                printf(" ");
                col++;
            }
        } else {
            // 通常モード
            for (int i = 0; i < 16; i++) {
                size_t a = y * 16 + i + disp->homeaddr;
                bool in_hl = disp->highlight_ranges.size > 0 && display_is_highlighted(disp, a);
                
                if (in_hl) {
                    terminal_highlight_color(disp->term);
                }
                
                display_printchar(disp, a);
                
                if (in_hl) {
                    terminal_resetcolor(disp->term);
                    terminal_color(disp->term, 6, 0);
                }
            }
        }
        printf(" ");
    }
}

void display_printdata(Display *disp) {
    size_t addr = display_fpos(disp);
    uint8_t a = memory_read(disp->memory, addr);
    
    terminal_locate(disp->term, 0, 23);
    terminal_color(disp->term, 6, 0);
    
    char s[4] = ".";  // サイズを3から4に変更
    if (a < 0x20) {
        snprintf(s, sizeof(s), "^%c", a + '@');
    } else if (a >= 0x7E) {
        strcpy(s, ".");
    } else {
        snprintf(s, sizeof(s), "'%c'", a);
    }
    
    if (addr < disp->memory->mem.size) {
        printf("%012zX : 0x%02X 0b", addr, a);
        for (int i = 7; i >= 0; i--) {
            printf("%d", (a >> i) & 1);
        }
        printf(" 0o%03o %d %s      ", a, a, s);
    } else {
        printf("%012zX : ~~                                                   ", addr);
    }
    fflush(stdout);
}

void display_clrmm(Display *disp) {
    terminal_locate(disp->term, 0, BOTTOMLN);
    terminal_color(disp->term, 6, 0);
    terminal_clrline(disp->term);
}

void display_stdmm(Display *disp, const char *msg, bool scripting, bool verbose) {
    if (scripting) {
        if (verbose) {
            printf("%s\n", msg);
        }
    } else {
        display_clrmm(disp);
        terminal_color(disp->term, 4, 0);
        terminal_locate(disp->term, 0, BOTTOMLN);
        printf(" %s", msg);
        fflush(stdout);
    }
}

void display_stdmm_wait(Display *disp, const char *msg, bool scripting, bool verbose) {
    if (scripting && !verbose) {
        return;  // スクリプト中で非verboseの場合は表示しない
    }
    if (scripting && verbose) {
        printf("%s\n", msg);
    } else {
        display_clrmm(disp);
        terminal_color(disp->term, 4, 0);
        terminal_locate(disp->term, 0, BOTTOMLN);
        printf(" %s", msg);
        fflush(stdout);
    }
}

void display_stderr(Display *disp, const char *msg, bool scripting, bool verbose) {
    if (scripting) {
        fprintf(stderr, "%s\n", msg);
    } else {
        display_clrmm(disp);
        terminal_color(disp->term, 3, 0);  // マゼンタ色でエラー表示
        terminal_locate(disp->term, 0, BOTTOMLN);
        printf(" %s", msg);
        fflush(stdout);
    }
}

void display_free(Display *disp) {
    matcharray_free(&disp->highlight_ranges);
}

/* ========================================================================
 * Parser実装
 * ======================================================================== */

void parser_init(Parser *parser, MemoryBuffer *mem, Display *disp) {
    parser->memory = mem;
    parser->display = disp;
}

size_t parser_skipspc(const char *s, size_t idx) {
    while (s[idx] && s[idx] == ' ') {
        idx++;
    }
    return idx;
}

uint64_t parser_get_value(Parser *parser, const char *s, size_t *idx) {
    if (!s[*idx]) return UNKNOWN;
    
    *idx = parser_skipspc(s, *idx);
    char ch = s[*idx];
    uint64_t v = 0;
    
    if (ch == '$') {
        (*idx)++;
        v = parser->memory->mem.size > 0 ? parser->memory->mem.size - 1 : 0;
    } else if (ch == '{') {
        // {} 構文 - Pythonのeval()を使用
        (*idx)++;
        char expr[1024];
        size_t expr_idx = 0;
        
        while (s[*idx] && s[*idx] != '}' && expr_idx < sizeof(expr) - 1) {
            expr[expr_idx++] = s[(*idx)++];
        }
        expr[expr_idx] = '\0';
        
        if (s[*idx] != '}') {
            // 構文エラー: 閉じ括弧がない
            return UNKNOWN;
        }
        (*idx)++;
        
        if (expr_idx == 0) {
            // 構文エラー: 空の式
            return UNKNOWN;
        }
        
        // Pythonで評価
        FILE *tmp = fopen("/tmp/bi_eval_tmp.py", "w");
        if (tmp) {
            fprintf(tmp, "print(int(%s))\n", expr);
            fclose(tmp);
            
            FILE *pipe = popen("python3 /tmp/bi_eval_tmp.py 2>/dev/null", "r");
            if (pipe) {
                char result[64];
                if (fgets(result, sizeof(result), pipe)) {
                    v = strtoull(result, NULL, 10);
                    pclose(pipe);
                } else {
                    pclose(pipe);
                    unlink("/tmp/bi_eval_tmp.py");
                    return UNKNOWN;
                }
            } else {
                unlink("/tmp/bi_eval_tmp.py");
                return UNKNOWN;
            }
            unlink("/tmp/bi_eval_tmp.py");
        } else {
            return UNKNOWN;
        }
    } else if (ch == '.') {
        (*idx)++;
        v = display_fpos(parser->display);
    } else if (ch == '\'' && s[*idx + 1] >= 'a' && s[*idx + 1] <= 'z') {
        (*idx)++;
        v = parser->memory->mark[s[*idx] - 'a'];
        if (v == UNKNOWN) {
            (*idx)--;
            return UNKNOWN;
        }
        (*idx)++;
    } else if (ch == '\'' && s[*idx + 1]) {
        // 構文エラー: 無効なマーク文字
        return UNKNOWN;
    } else if (isxdigit((unsigned char)ch)) {
        while (isxdigit((unsigned char)s[*idx])) {
            v = 16 * v + (isdigit((unsigned char)s[*idx]) ? 
                         s[*idx] - '0' : 
                         tolower((unsigned char)s[*idx]) - 'a' + 10);
            (*idx)++;
        }
    } else if (ch == '%') {
        (*idx)++;
        if (!isdigit((unsigned char)s[*idx])) {
            // 構文エラー: %の後に数字がない
            return UNKNOWN;
        }
        while (isdigit((unsigned char)s[*idx])) {
            v = 10 * v + (s[*idx] - '0');
            (*idx)++;
        }
    } else {
        // 構文エラー: 不正な文字
        return UNKNOWN;
    }
    
    if ((int64_t)v < 0) v = 0;
    return v;
}

uint64_t parser_expression(Parser *parser, const char *s, size_t *idx) {
    uint64_t x = parser_get_value(parser, s, idx);
    
    if (x == UNKNOWN) {
        return UNKNOWN;
    }
    
    *idx = parser_skipspc(s, *idx);
    
    if (s[*idx] == '+') {
        *idx = parser_skipspc(s, *idx + 1);
        uint64_t y = parser_get_value(parser, s, idx);
        if (y == UNKNOWN) {
            // 構文エラー: +の後に有効な値がない
            return UNKNOWN;
        }
        x = x + y;
    } else if (s[*idx] == '-') {
        *idx = parser_skipspc(s, *idx + 1);
        uint64_t y = parser_get_value(parser, s, idx);
        if (y == UNKNOWN) {
            // 構文エラー: -の後に有効な値がない
            return UNKNOWN;
        }
        if (x < y) {
            x = 0;
        } else {
            x = x - y;
        }
    }
    
    return x;
}

size_t parser_get_restr(const char *s, size_t idx, char *result) {
    size_t j = 0;
    while (s[idx]) {
        if (s[idx] == '/') {
            break;
        }
        if (s[idx] == '\\' && s[idx + 1] == '\\') {
            result[j++] = '\\';
            result[j++] = '\\';
            idx += 2;
        } else if (s[idx] == '\\' && s[idx + 1] == '/') {
            result[j++] = '/';
            idx += 2;
        } else if (s[idx] == '\\' && !s[idx + 1]) {
            idx++;
            break;
        } else {
            result[j++] = s[idx++];
        }
    }
    result[j] = '\0';
    return idx;
}

size_t parser_get_hexs(Parser *parser, const char *s, size_t idx, ByteArray *result) {
    bytearray_init(result);
    idx = parser_skipspc(s, idx);

    if (idx+1<strlen(s) && s[idx]=='/' && s[idx+1]=='/') {
        idx+=2;
        }
    idx = parser_skipspc(s, idx);
    
    size_t start_idx = idx;
    while (s[idx]) {
        uint64_t v = parser_expression(parser, s, &idx);
        if (v == UNKNOWN) {
            // 構文エラー: 無効な値を検出
            if (idx == start_idx) {
                // 何も読み取れなかった場合
                break;
            }
            // 値の途中で失敗した場合は構文エラー
            bytearray_free(result);
            bytearray_init(result);
            break;
        }
        bytearray_push(result, v & 0xFF);
        start_idx = idx;
    }
    return idx;
}

char* parser_comment(const char *s) {
    static char result[4096];
    size_t idx = 0, j = 0;
    
    while (s[idx] && j < sizeof(result) - 1) {
        if (s[idx] == '#') {
            break;
        }
        if (s[idx] == '\\' && s[idx + 1] == '#') {
            result[j++] = '#';
            idx += 2;
        } else if (s[idx] == '\\' && s[idx + 1] == 'n') {
            result[j++] = '\n';
            idx += 2;
        } else {
            result[j++] = s[idx++];
        }
    }
    result[j] = '\0';
    return result;
}

/* ========================================================================
 * HistoryManager実装
 * ======================================================================== */

void history_init(HistoryManager *hist) {
    hist->command_history = NULL;
    hist->command_count = 0;
    hist->command_capacity = 0;
    hist->search_history = NULL;
    hist->search_count = 0;
    hist->search_capacity = 0;
}

char* history_getln(HistoryManager *hist, const char *prompt, const char *mode) {
    static char buffer[4096];
    
    // readline使用
    char *line = readline(prompt);
    if (!line) {
        buffer[0] = '\0';
        return buffer;
    }
    
    if (line[0]) {
        add_history(line);
    }
    
    strncpy(buffer, line, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    free(line);
    
    return buffer;
}


/* ========================================================================
 * Python版から移植: 履歴管理の改善
 * コマンド履歴と検索履歴を分離管理する機能を追加
 * ======================================================================== */

/*
 * 注: この実装は Python版の HistoryManager クラスの機能を C に移植したものです
 * 
 * Python版の特徴:
 * - self.histories = {'command': [], 'search': []}
 * - get_history_list(): readlineから履歴を取得
 * - set_history_list(mode): モード別に履歴を設定
 * - getln(s, mode): モードを切り替えて入力
 * 
 * C版での実装:
 * - command_history[], search_history[] で分離管理
 * - history_switch_mode() でモード切替
 * - history_getln_with_mode() でモード対応入力
 */

#define HISTORY_MODE_COMMAND 0
#define HISTORY_MODE_SEARCH  1

typedef struct {
    char **entries;      /* 履歴エントリ配列 */
    size_t count;        /* 現在の履歴数 */
    size_t capacity;     /* 配列の容量 */
    size_t max_size;     /* 最大履歴数 */
} HistoryStore;

typedef struct {
    HistoryStore command_hist;  /* コマンド履歴 */
    HistoryStore search_hist;   /* 検索履歴 */
    int current_mode;           /* 現在のモード */
} HistoryManagerEx;

void history_store_init(HistoryStore *store, size_t max_size) {
    store->entries = NULL;
    store->count = 0;
    store->capacity = 0;
    store->max_size = max_size;
}

void history_store_add(HistoryStore *store, const char *entry) {
    if (!entry || entry[0] == '\0') return;
    
    /* 容量チェックと拡張 */
    if (store->count >= store->capacity) {
        size_t new_cap = store->capacity == 0 ? 16 : store->capacity * 2;
        char **new_entries = realloc(store->entries, new_cap * sizeof(char*));
        if (!new_entries) return;
        store->entries = new_entries;
        store->capacity = new_cap;
    }
    
    /* 最大サイズチェック */
    if (store->count >= store->max_size) {
        /* 最古のエントリを削除 */
        free(store->entries[0]);
        memmove(store->entries, store->entries + 1, 
                (store->count - 1) * sizeof(char*));
        store->count--;
    }
    
    /* エントリ追加 */
    store->entries[store->count] = strdup(entry);
    if (store->entries[store->count]) {
        store->count++;
    }
}

void history_store_free(HistoryStore *store) {
    for (size_t i = 0; i < store->count; i++) {
        free(store->entries[i]);
    }
    free(store->entries);
    store->entries = NULL;
    store->count = 0;
    store->capacity = 0;
}

void history_manager_ex_init(HistoryManagerEx *mgr) {
    history_store_init(&mgr->command_hist, 1000);
    history_store_init(&mgr->search_hist, 1000);
    mgr->current_mode = HISTORY_MODE_COMMAND;
}

void history_manager_ex_free(HistoryManagerEx *mgr) {
    history_store_free(&mgr->command_hist);
    history_store_free(&mgr->search_hist);
}

void history_manager_ex_add(HistoryManagerEx *mgr, const char *entry, int mode) {
    /* Python版の histories[mode].append(entry) 相当 */
    if (mode == HISTORY_MODE_SEARCH) {
        history_store_add(&mgr->search_hist, entry);
    } else {
        history_store_add(&mgr->command_hist, entry);
    }
}

void history_manager_ex_switch_mode(HistoryManagerEx *mgr, int mode) {
    /* Python版の set_history_list(mode) 相当 */
    mgr->current_mode = mode;
    
#ifdef HAVE_READLINE
    /* readlineの履歴を切り替え */
    clear_history();
    
    HistoryStore *store = (mode == HISTORY_MODE_SEARCH) ? 
                          &mgr->search_hist : &mgr->command_hist;
    
    for (size_t i = 0; i < store->count; i++) {
        add_history(store->entries[i]);
    }
#endif
}

char* history_manager_ex_getln(HistoryManagerEx *mgr, const char *prompt, int mode) {
    /* Python版の getln(s, mode) 相当 */
    static char buffer[4096];
    
    /* モード切替 */
    if (mode != mgr->current_mode) {
        history_manager_ex_switch_mode(mgr, mode);
    }
    
    /* 入力取得 */
    char *line = readline(prompt);
    if (!line) {
        buffer[0] = '\0';
        return buffer;
    }
    
    /* 履歴に追加 */
    if (line[0]) {
#ifdef HAVE_READLINE
        add_history(line);
#endif
        history_manager_ex_add(mgr, line, mode);
    }
    
    strncpy(buffer, line, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    free(line);
    
    return buffer;
}

void history_free(HistoryManager *hist) {
    clear_history();
}

/* ========================================================================
 * FileManager実装
 * ======================================================================== */

void filemgr_init(FileManager *fmgr, MemoryBuffer *mem) {
    fmgr->memory = mem;
    fmgr->filename[0] = '\0';
    fmgr->newfile = false;
}

bool filemgr_readfile(FileManager *fmgr, const char *filename, char *msg, size_t msg_size) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fmgr->newfile = true;
        bytearray_init(&fmgr->memory->mem);
        if (msg) snprintf(msg, msg_size, "<new file>");
        return true;
    }
    
    fmgr->newfile = false;
    
    // ファイルサイズ取得
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fsize < 0) {
        fclose(f);
        if (msg) snprintf(msg, msg_size, "File read error.");
        return false;
    }
    
    // メモリ確保と読み込み
    bytearray_init(&fmgr->memory->mem);
    if (fsize > 0) {
        uint8_t *buffer = malloc(fsize);
        if (!buffer) {
            fclose(f);
            if (msg) snprintf(msg, msg_size, "Memory overflow.");
            return false;
        }
        
        size_t read_size = fread(buffer, 1, fsize, f);
        for (size_t i = 0; i < read_size; i++) {
            bytearray_push(&fmgr->memory->mem, buffer[i]);
        }
        free(buffer);
    }
    
    fclose(f);
    if (msg) msg[0] = '\0';
    return true;
}

bool filemgr_writefile(FileManager *fmgr, const char *filename, char *msg, size_t msg_size) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        if (msg) snprintf(msg, msg_size, "Permission denied.");
        return false;
    }
    
    if (fmgr->memory->mem.size > 0) {
        fwrite(fmgr->memory->mem.data, 1, fmgr->memory->mem.size, f);
    }
    
    fclose(f);
    if (msg) snprintf(msg, msg_size, "File written.");
    return true;
}

/* ========================================================================
 * BiEditor実装
 * ======================================================================== */

void editor_init(BiEditor *editor, const char *termcol) {
    editor->scriptingflag = false;
    editor->verbose = false;
    
    terminal_init(&editor->term, termcol, editor);
    memory_init(&editor->memory);
    display_init(&editor->display, &editor->term, &editor->memory);
    parser_init(&editor->parser, &editor->memory, &editor->display);
    history_init(&editor->history);
    search_init(&editor->search, &editor->memory, &editor->display, editor);
    filemgr_init(&editor->filemgr, &editor->memory);
    
    undostack_init(&editor->undo_stack);
    undostack_init(&editor->redo_stack);
    editor->cp = 0;
}

void editor_save_undo_state(BiEditor *editor) {
    if (editor->scriptingflag) return;
    
    UndoState state;
    state.mem = bytearray_copy(&editor->memory.mem);
    state.modified = editor->memory.modified;
    state.lastchange = editor->memory.lastchange;
    memcpy(state.mark, editor->memory.mark, sizeof(state.mark));
    
    undostack_push(&editor->undo_stack, &state);
    
    // スタックサイズ制限
    if (editor->undo_stack.size > MAX_UNDO_LEVELS) {
        bytearray_free(&editor->undo_stack.data[0].mem);
        memmove(editor->undo_stack.data, editor->undo_stack.data + 1,
                (editor->undo_stack.size - 1) * sizeof(UndoState));
        editor->undo_stack.size--;
    }
    
    // redoスタックをクリア
    undostack_free(&editor->redo_stack);
    undostack_init(&editor->redo_stack);
}

bool editor_dec_undo(BiEditor *editor) {
    if (editor->undo_stack.size == 0) {
        return false;
    }
    UndoState *state = undostack_pop(&editor->undo_stack);
    return true;
}
    

bool editor_undo(BiEditor *editor) {
    if (editor->undo_stack.size == 0) {
        display_stdmm(&editor->display, "No more undo.", editor->scriptingflag, editor->verbose);
        return false;
    }
    
    // 現在の状態をredoスタックに保存
    UndoState current;
    current.mem = bytearray_copy(&editor->memory.mem);
    current.modified = editor->memory.modified;
    current.lastchange = editor->memory.lastchange;
    memcpy(current.mark, editor->memory.mark, sizeof(current.mark));
    undostack_push(&editor->redo_stack, &current);
    
    // undoスタックから状態を復元
    UndoState *state = undostack_pop(&editor->undo_stack);
    bytearray_free(&editor->memory.mem);
    editor->memory.mem = state->mem;
    editor->memory.modified = state->modified;
    editor->memory.lastchange = state->lastchange;
    memcpy(editor->memory.mark, state->mark, sizeof(editor->memory.mark));
    
    // カーソル位置調整
    size_t pos = display_fpos(&editor->display);
    if (pos >= editor->memory.mem.size && editor->memory.mem.size > 0) {
        display_jump(&editor->display, editor->memory.mem.size - 1);
    } else if (editor->memory.mem.size == 0) {
        display_jump(&editor->display, 0);
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Undo. (%zu more)", editor->undo_stack.size);
    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
    return true;
}

bool editor_redo(BiEditor *editor) {
    if (editor->redo_stack.size == 0) {
        display_stdmm(&editor->display, "No more redo.", editor->scriptingflag, editor->verbose);
        return false;
    }
    
    // 現在の状態をundoスタックに保存
    UndoState current;
    current.mem = bytearray_copy(&editor->memory.mem);
    current.modified = editor->memory.modified;
    current.lastchange = editor->memory.lastchange;
    memcpy(current.mark, editor->memory.mark, sizeof(current.mark));
    undostack_push(&editor->undo_stack, &current);
    
    // redoスタックから状態を復元
    UndoState *state = undostack_pop(&editor->redo_stack);
    bytearray_free(&editor->memory.mem);
    editor->memory.mem = state->mem;
    editor->memory.modified = state->modified;
    editor->memory.lastchange = state->lastchange;
    memcpy(editor->memory.mark, state->mark, sizeof(editor->memory.mark));
    
    // カーソル位置調整
    size_t pos = display_fpos(&editor->display);
    if (pos >= editor->memory.mem.size && editor->memory.mem.size > 0) {
        display_jump(&editor->display, editor->memory.mem.size - 1);
    } else if (editor->memory.mem.size == 0) {
        display_jump(&editor->display, 0);
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Redo. (%zu more)", editor->redo_stack.size);
    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
    return true;
}

/* ========================================================================
 * Editor fedit - フルスクリーンエディタモード
 * ======================================================================== */

void editor_fedit(BiEditor *editor) {
    bool stroke = false;
    int ch = 0;
    editor->display.repsw = 0;
    
    while (true) {
        editor->cp = display_fpos(&editor->display);
        display_repaint(&editor->display, editor->filemgr.filename);
        display_printdata(&editor->display);
        
        // カーソル位置
        terminal_locate(&editor->term,
                       editor->display.curx / 2 * 3 + 13 + (editor->display.curx & 1),
                       editor->display.cury + 3);
        terminal_dispcursor(&editor->term);
        fflush(stdout);
        
        ch = terminal_getch();
        display_clrmm(&editor->display);
        editor->search.nff = true;
        
        // ESCシーケンス処理
        if (ch == 27) {
            int c2 = terminal_getch();
            if (c2 == '[') {
                int c3 = terminal_getch();
                if (c3 == 'A') ch = 'k';
                else if (c3 == 'B') ch = 'j';
                else if (c3 == 'C') ch = 'l';
                else if (c3 == 'D') ch = 'h';
                else if (c3 == '2') ch = 'i';
            } else {
                // ESC単独 - ハイライトクリア
                matcharray_clear(&editor->display.highlight_ranges);
                continue;
            }
        }
        
        // 検索コマンド
        if (ch == 'n') {
            size_t pos = search_next(&editor->search, display_fpos(&editor->display) + 1,
                                    editor->memory.mem.size);
            if (pos != (size_t)-1) {
                if (editor->display.highlight_ranges.size == 0) {
                    search_all(&editor->search, editor->memory.mem.size,
                             &editor->display.highlight_ranges);
                }
                display_jump(&editor->display, pos);
            } else {
                display_stdmm(&editor->display, "Not found.", editor->scriptingflag, editor->verbose);
            }
            continue;
        } else if (ch == 'N') {
            size_t pos = search_last(&editor->search, display_fpos(&editor->display) - 1,
                                    editor->memory.mem.size);
            if (pos != (size_t)-1) {
                if (editor->display.highlight_ranges.size == 0) {
                    search_all(&editor->search, editor->memory.mem.size,
                             &editor->display.highlight_ranges);
                }
                display_jump(&editor->display, pos);
            } else {
                display_stdmm(&editor->display, "Not found.", editor->scriptingflag, editor->verbose);
            }
            continue;
        }
        
        // Undo/Redo
        else if (ch == 'u') {
            editor_undo(editor);
            continue;
        } else if (ch == 18) {  // Ctrl+R
            editor_redo(editor);
            continue;
        }
        
        // スクロール
        else if (ch == 2) {  // Ctrl+B
            if (editor->display.homeaddr >= 256) {
                editor->display.homeaddr -= 256;
            } else {
                editor->display.homeaddr = 0;
            }
            continue;
        } else if (ch == 12) {  // Ctrl+L
            display_repaint(&editor->display, editor->filemgr.filename);
            continue;
        } else if (ch == 6) {  // Ctrl+F
            editor->display.homeaddr += 256;
            continue;
        } else if (ch == 21) {  // Ctrl+U
            if (editor->display.homeaddr >= 128) {
                editor->display.homeaddr -= 128;
            } else {
                editor->display.homeaddr = 0;
            }
            continue;
        } else if (ch == 4) {  // Ctrl+D
            editor->display.homeaddr += 128;
            continue;
        }
        
        // カーソル移動
        else if (ch == '^') {
            editor->display.curx = 0;
            continue;
        } else if (ch == '$') {
            editor->display.curx = 30;
            continue;
        } else if (ch == 'j') {
            if (editor->display.cury < LENONSCR / 16 - 1) {
                editor->display.cury++;
            } else {
                editor->display.homeaddr += 16;
            }
            continue;
        } else if (ch == 'k') {
            if (editor->display.cury > 0) {
                editor->display.cury--;
            } else if (editor->display.homeaddr >= 16) {
                editor->display.homeaddr -= 16;
            }
            continue;
        } else if (ch == 'h') {
            if (editor->display.curx > 0) {
                editor->display.curx--;
            } else if (display_fpos(&editor->display) != 0) {
                editor->display.curx = 31;
                if (editor->display.cury > 0) {
                    editor->display.cury--;
                } else if (editor->display.homeaddr >= 16) {
                    editor->display.homeaddr -= 16;
                }
            }
            continue;
        } else if (ch == 'l') {
            if (editor->display.curx < 31) {
                editor->display.curx++;
            } else {
                editor->display.curx = 0;
                if (editor->display.cury < LENONSCR / 16 - 1) {
                    editor->display.cury++;
                } else {
                    editor->display.homeaddr += 16;
                }
            }
            continue;
        }
        
        // 検索開始
        else if (ch == '/') {
            terminal_locate(&editor->term, 0, BOTTOMLN);
            terminal_color(&editor->term, 7, 0);
            char *input = history_getln(&editor->history, "/", "search");
            
            if (input && input[0]) {
                // 入力の先頭に / を追加してパースする
                char line[4097];  // バッファサイズを1バイト増やす
                snprintf(line, sizeof(line), "/%s", input);
                line[sizeof(line) - 1] = '\0';  // 念のため
                
                // 検索処理
                if (strlen(line) > 2 && line[0] == '/' && line[1] == '/') {
                    // Hex検索 (//で始まる)
                    ByteArray sm;
                    parser_get_hexs(&editor->parser, line, 2, &sm);
                    if (sm.size > 0) {
                        bytearray_free(&editor->search.smem);
                        editor->search.smem = sm;
                        editor->search.regexp = false;
                        editor->search.remem[0] = '\0';
                        
                        matcharray_clear(&editor->display.highlight_ranges);
                        search_all(&editor->search, editor->memory.mem.size,
                                  &editor->display.highlight_ranges);
                        
                        if (editor->display.highlight_ranges.size > 0) {
                            display_jump(&editor->display, editor->display.highlight_ranges.data[0].pos);
                            char msg[256];
                            snprintf(msg, sizeof(msg), "Found %zu match(es)",
                                    editor->display.highlight_ranges.size);
                            display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
                        } else {
                            display_stdmm(&editor->display, "Not found", editor->scriptingflag, editor->verbose);
                        }
                    }
                } else if (strlen(line) > 1 && line[0] == '/') {
                    // 正規表現検索
                    // 末尾の / を削除
                    char pattern[1024];
                    strncpy(pattern, line + 1, sizeof(pattern) - 1);
                    pattern[sizeof(pattern) - 1] = '\0';
                    
                    // 末尾の / を探して削除
                    size_t len = strlen(pattern);
                    if (len > 0 && pattern[len - 1] == '/') {
                        pattern[len - 1] = '\0';
                    }
                    
                    if (pattern[0]) {
                        strncpy(editor->search.remem, pattern, sizeof(editor->search.remem) - 1);
                        editor->search.remem[sizeof(editor->search.remem) - 1] = '\0';
                        editor->search.regexp = true;
                        bytearray_free(&editor->search.smem);
                        bytearray_init(&editor->search.smem);
                        
                        matcharray_clear(&editor->display.highlight_ranges);
                        search_all(&editor->search, editor->memory.mem.size,
                                  &editor->display.highlight_ranges);
                        
                        if (editor->display.highlight_ranges.size > 0) {
                            display_jump(&editor->display, editor->display.highlight_ranges.data[0].pos);
                            char msg[256];
                            snprintf(msg, sizeof(msg), "Found %zu match(es)",
                                    editor->display.highlight_ranges.size);
                            display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
                        } else {
                            display_stdmm(&editor->display, "Not found", editor->scriptingflag, editor->verbose);
                        }
                    }
                }
            }
            continue;
        }
        
        // 表示モード
        else if (ch == 25) {  // Ctrl+Y
            editor->display.utf8 = !editor->display.utf8;
            editor->display.repsw = !editor->display.repsw;  // repswも切り替え
            terminal_clear(&editor->term);
            display_repaint(&editor->display, editor->filemgr.filename);
            continue;
        }
        
        // ファイル操作 (Z command - 保存して終了)
        else if (ch == 'Z') {
            char msg[256];
            bool success = filemgr_writefile(&editor->filemgr, editor->filemgr.filename, msg, sizeof(msg));
            if (success) {
                return;
            } else {
                display_stderr(&editor->display, msg, editor->scriptingflag, editor->verbose);
            }
            continue;
        }
        
        // 終了 (q command)
        else if (ch == 'q') {
            if (editor->memory.lastchange) {
                display_stdmm(&editor->display, "No write since last change. To overriding quit, use 'q!'.",
                             editor->scriptingflag, editor->verbose);
            } else {
                return;
            }
            continue;
        }
        
        // マーク表示 (M command)
        else if (ch == 'M') {
            terminal_locate(&editor->term, 0, BOTTOMLN);
            terminal_color(&editor->term, 7, 0);
            
            for (int i = 0; i < 26; i++) {
                char mark_char = 'a' + i;
                uint64_t mark_val = editor->memory.mark[i];
                
                if (mark_val == UNKNOWN) {
                    printf("%c = unknown         ", mark_char);
                } else {
                    printf("%c = %012llX    ", mark_char, (unsigned long long)mark_val);
                }
                
                if ((i + 1) % 3 == 0) {
                    printf("\n");
                }
            }
            
            terminal_color(&editor->term, 4, 0);
            printf("[ hit any key ]");
            fflush(stdout);
            terminal_getch();
            terminal_clear(&editor->term);
            display_repaint(&editor->display, editor->filemgr.filename);
            continue;
        }
        
        // マーク設定 (m command)
        else if (ch == 'm') {
            int ch2 = terminal_getch();
            if (ch2 >= 'a' && ch2 <= 'z') {
                editor->memory.mark[ch2 - 'a'] = display_fpos(&editor->display);
                char msg[256];
                snprintf(msg, sizeof(msg), "Mark '%c' set at %zX", ch2, display_fpos(&editor->display));
                display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
            }
            continue;
        }
        
        // マークへジャンプ (' command)
        else if (ch == '\'') {
            int ch2 = terminal_getch();
            if (ch2 >= 'a' && ch2 <= 'z') {
                uint64_t mark_pos = editor->memory.mark[ch2 - 'a'];
                if (mark_pos != UNKNOWN) {
                    display_jump(&editor->display, mark_pos);
                } else {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Mark '%c' not set", ch2);
                    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
                }
            }
            continue;
        }
        
        // ヤンク・ペースト
        else if (ch == 'p') {
            if (editor->memory.yank.size > 0) {
                editor_save_undo_state(editor);
                memory_overwrite(&editor->memory, display_fpos(&editor->display),
                               editor->memory.yank.data, editor->memory.yank.size);
                display_jump(&editor->display, display_fpos(&editor->display) + editor->memory.yank.size);
            }
            continue;
        } else if (ch == 'P') {
            if (editor->memory.yank.size > 0) {
                editor_save_undo_state(editor);
                matcharray_clear(&editor->display.highlight_ranges);
                memory_insert(&editor->memory, display_fpos(&editor->display),
                            editor->memory.yank.data, editor->memory.yank.size);
                display_jump(&editor->display, display_fpos(&editor->display) + editor->memory.yank.size);
            }
            continue;
        }
        
        // 編集モード
        if (ch == 'i') {
            editor->display.insmod = !editor->display.insmod;
            stroke = false;
        } else if (isxdigit(ch)) {
            size_t addr = display_fpos(&editor->display);
            int c = isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
            int sh = (editor->display.curx & 1) ? 0 : 4;
            int mask = (editor->display.curx & 1) ? 0xF0 : 0x0F;
            
            if (editor->display.insmod) {
                if (!stroke && addr < editor->memory.mem.size) {
                    editor_save_undo_state(editor);
                    matcharray_clear(&editor->display.highlight_ranges);
                    uint8_t byte = c << sh;
                    memory_insert(&editor->memory, addr, &byte, 1);
                } else {
                    if (!stroke) editor_save_undo_state(editor);
                    memory_set(&editor->memory, addr, (memory_read(&editor->memory, addr) & mask) | (c << sh));
                }
                stroke = (editor->display.curx & 1) ? false : !stroke;
            } else {
                if ((editor->display.curx & 1) == 0) {
                    editor_save_undo_state(editor);
                }
                memory_set(&editor->memory, addr, (memory_read(&editor->memory, addr) & mask) | (c << sh));
            }
            
            // カーソル移動
            if (editor->display.curx < 31) {
                editor->display.curx++;
            } else {
                editor->display.curx = 0;
                if (editor->display.cury < LENONSCR / 16 - 1) {
                    editor->display.cury++;
                } else {
                    editor->display.homeaddr += 16;
                }
            }
        } else if (ch == 'x') {
            editor_save_undo_state(editor);
            if (memory_delete(&editor->memory, display_fpos(&editor->display),
                            display_fpos(&editor->display), false, memory_yank)) {
                matcharray_clear(&editor->display.highlight_ranges);
            } else {
                display_stderr(&editor->display, "Invalid range.", editor->scriptingflag,editor->verbose);
                editor_dec_undo(editor);
            }
        } else if (ch == ':') {
            // コマンドモード
            size_t before_len = editor->memory.mem.size;
            char *line = history_getln(&editor->history, ":", "command");
            int f = editor_commandline(editor, line);
            if (editor->memory.mem.size != before_len) {
                matcharray_clear(&editor->display.highlight_ranges);
            }
            if (f == 1) return;
            else if (f == 0) return;
        }
    }
}

void editor_free(BiEditor *editor) {
    memory_free(&editor->memory);
    search_free(&editor->search);
    display_free(&editor->display);
    history_free(&editor->history);
    undostack_free(&editor->undo_stack);
    undostack_free(&editor->redo_stack);
}

/* ========================================================================
 * コマンドライン処理
 * ======================================================================== */

// 前方宣言
int execute_command(BiEditor *editor, const char *line, size_t idx, 
                    uint64_t x, uint64_t x2, bool xf, bool xf2);

int editor_commandline(BiEditor *editor, const char *line) {
    editor->cp = display_fpos(&editor->display);
    const char *parsed_line = parser_comment(line);
    
    if (parsed_line[0] == '\0') return -1;
    
    // 終了コマンド
    if (strcmp(parsed_line, "q") == 0) {
        if (editor->memory.lastchange) {
            display_stderr(&editor->display, "No write since last change. To overriding quit, use 'q!'.",
                          editor->scriptingflag, editor->verbose);
            return -1;
        }
        return 0;
    } else if (strcmp(parsed_line, "q!") == 0) {
        return 0;
    } else if (strcmp(parsed_line, "wq") == 0 || strcmp(parsed_line, "wq!") == 0) {
        char msg[256];
        bool success = filemgr_writefile(&editor->filemgr, editor->filemgr.filename, msg, sizeof(msg));
        if (success) {
            editor->memory.lastchange = false;
            return 0;
        } else {
            return -1;
        }
    }
    
    // Undo/Redo
    else if (strcmp(parsed_line, "u") == 0 || strcmp(parsed_line, "undo") == 0) {
        editor_undo(editor);
        return -1;
    } else if (strcmp(parsed_line, "red") == 0 || strcmp(parsed_line, "redo") == 0) {
        editor_redo(editor);
        return -1;
    }
    
    // ファイル書き込み
    else if (parsed_line[0] == 'w') {
        char msg[256];
        const char *fname = editor->filemgr.filename;
        if (strlen(parsed_line) >= 2) {
            fname = parsed_line + 1;
            while (*fname == ' ') fname++;
        }
        bool success = filemgr_writefile(&editor->filemgr, fname, msg, sizeof(msg));
        if (strlen(parsed_line) < 2 && success) {
            editor->memory.lastchange = false;
        }
        if (msg[0]) {
            if (success) {
                display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
            } else {
                display_stderr(&editor->display, msg, editor->scriptingflag, editor->verbose);
            }
        }
        return -1;
    }
    
    // Python実行 (@コマンド)
    else if (parsed_line[0] == '@') {
        if (strlen(parsed_line) >= 2) {
            const char *python_code = parsed_line + 1;
            
            // 空白をスキップ
            while (*python_code == ' ') python_code++;
            
            if (*python_code == '\0') {
                // 構文エラー: Pythonコードが空
                display_stderr(&editor->display, "Syntax error: No Python code specified.",
                              editor->scriptingflag, editor->verbose);
                return -1;
            }
            
            // 一時ファイルにPythonコードを書き出し
            FILE *tmp = fopen("/tmp/bi_python_tmp.py", "w");
            if (tmp) {
                fprintf(tmp, "%s\n", python_code);
                fclose(tmp);
                
                if (!editor->scriptingflag) {
                    display_clrmm(&editor->display);
                    terminal_color(&editor->term, 7, 0);
                    terminal_locate(&editor->term, 0, BOTTOMLN);
                }
                
                int ret = system("python3 /tmp/bi_python_tmp.py 2>&1");
                (void)ret;
                
                if (!editor->scriptingflag) {
                    terminal_color(&editor->term, 4, 0);
                    printf("[ Hit a key ]");
                    fflush(stdout);
                    terminal_getch();
                    terminal_clear(&editor->term);
                }
                
                unlink("/tmp/bi_python_tmp.py");
            } else {
                display_stderr(&editor->display, "Cannot create temporary file.", 
                              editor->scriptingflag, editor->verbose);
            }
        } else {
            // 構文エラー: @の後に何もない
            display_stderr(&editor->display, "Syntax error: No Python code specified.",
                          editor->scriptingflag, editor->verbose);
        }
        return -1;
    }
    
    // 特殊コマンド
    // シェルコマンド実行
    else if (parsed_line[0] == '!') {
        if (strlen(parsed_line) >= 2) {
            const char *shell_cmd = parsed_line + 1;
            while (*shell_cmd == ' ') shell_cmd++;
            
            if (*shell_cmd == '\0') {
                // 構文エラー: シェルコマンドが空
                display_stderr(&editor->display, "Syntax error: No shell command specified.",
                              editor->scriptingflag, editor->verbose);
                return -1;
            }
            
            if (!editor->scriptingflag) {
                terminal_color(&editor->term, 7, 0);
                printf("\n");
                fflush(stdout);
            }
            int ret = system(parsed_line + 1);
            (void)ret;  // 返り値を使用（警告抑制）
            
            if (!editor->scriptingflag) {
                terminal_color(&editor->term, 4, 0);
                printf("[ Hit any key to return ]");
                fflush(stdout);
                terminal_getch();
                terminal_clear(&editor->term);
            }
        } else {
            // 構文エラー: !の後に何もない
            display_stderr(&editor->display, "Syntax error: No shell command specified.",
                          editor->scriptingflag, editor->verbose);
        }
        return -1;
    }
    // 値の計算と表示
    else if (parsed_line[0] == '?') {
        if (strlen(parsed_line) >= 2) {
            size_t idx = 1;
            uint64_t v = parser_expression(&editor->parser, parsed_line, &idx);
            if (v == UNKNOWN) {
                display_stderr(&editor->display, "Syntax error: Invalid expression.",
                              editor->scriptingflag, editor->verbose);
                return -1;
            }
            if (v != UNKNOWN) {
                char s[4] = ".";
                if (v < 0x20) {
                    snprintf(s, sizeof(s), "^%c", (char)(v + '@'));
                } else if (v >= 0x7E) {
                    strcpy(s, ".");
                } else {
                    snprintf(s, sizeof(s), "'%c'", (char)v);
                }
                
                char msg[256];
                snprintf(msg, sizeof(msg),
                        "d%llu  x%016llX  o%024llo %s",
                        (unsigned long long)v, (unsigned long long)v,
                        (unsigned long long)v, s);
                display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
                
                if (!editor->scriptingflag) {
                    // インタラクティブモードのみバイナリ表示してキー入力待ち
                    terminal_locate(&editor->term, 0, BOTTOMLN + 1);
                    terminal_color(&editor->term, 6, 0);
                    printf("b");
                    bool first=true;
                    for (int i = 63; i >= 0; i--) {
                        if (i % 4 == 3 && !first) printf(" ");
                        printf("%d", (int)((v >> i) & 1));
                        first=false;
                    }
                    fflush(stdout);
                    terminal_getch();
                    terminal_locate(&editor->term, 0, BOTTOMLN + 1);
                    printf("%*s", 80, "");
                    fflush(stdout);
                } else {
                    // スクリプトモードではバイナリ表示のみ
                    printf("b");
                    for (int i = 63; i >= 0; i--) {
                        if (i % 4 == 3) printf(" ");
                        printf("%d", (int)((v >> i) & 1));
                    }
                    printf("\n");
                }
            }
        }
        return -1;
    }
    // 検索
    else if (parsed_line[0] == '/') {
        // 検索処理
        if (strlen(parsed_line) > 2 && parsed_line[0] == '/' && parsed_line[1] == '/') {
            // Hex検索 (//で始まる)
            ByteArray sm;
            parser_get_hexs(&editor->parser, parsed_line, 2, &sm);
            if (sm.size > 0) {
                bytearray_free(&editor->search.smem);
                editor->search.smem = sm;
                editor->search.regexp = false;
                editor->search.remem[0] = '\0';
                
                matcharray_clear(&editor->display.highlight_ranges);
                search_all(&editor->search, editor->memory.mem.size,
                          &editor->display.highlight_ranges);
                
                if (editor->display.highlight_ranges.size > 0) {
                    display_jump(&editor->display, editor->display.highlight_ranges.data[0].pos);
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Found %zu match(es)",
                            editor->display.highlight_ranges.size);
                    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
                } else {
                    display_stdmm(&editor->display, "Not found", editor->scriptingflag, editor->verbose);
                }
            } else {
                // 構文エラー: 検索パターンが無効
                display_stderr(&editor->display, "Syntax error: Invalid hex search pattern.",
                              editor->scriptingflag, editor->verbose);
                return -1;
            }
        } else if (strlen(parsed_line) > 1 && parsed_line[0] == '/') {
            // 正規表現検索
            // 末尾の / を削除
            char pattern[1024];
            strncpy(pattern, parsed_line + 1, sizeof(pattern) - 1);
            pattern[sizeof(pattern) - 1] = '\0';
            
            // 末尾の / を探して削除
            size_t len = strlen(pattern);
            if (len > 0 && pattern[len - 1] == '/') {
                pattern[len - 1] = '\0';
            } else {
                // 構文エラー: 正規表現の終了/がない
                display_stderr(&editor->display, "Syntax error: Missing closing '/' in regex.",
                              editor->scriptingflag, editor->verbose);
                return -1;
            }
            
            if (pattern[0]) {
                strncpy(editor->search.remem, pattern, sizeof(editor->search.remem) - 1);
                editor->search.remem[sizeof(editor->search.remem) - 1] = '\0';
                editor->search.regexp = true;
                bytearray_free(&editor->search.smem);
                bytearray_init(&editor->search.smem);
                
                matcharray_clear(&editor->display.highlight_ranges);
                search_all(&editor->search, editor->memory.mem.size,
                          &editor->display.highlight_ranges);
                
                if (editor->display.highlight_ranges.size > 0) {
                    display_jump(&editor->display, editor->display.highlight_ranges.data[0].pos);
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Found %zu match(es)",
                            editor->display.highlight_ranges.size);
                    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
                } else {
                    display_stdmm(&editor->display, "Not found", editor->scriptingflag, editor->verbose);
                }
            } else {
                // 構文エラー: 空の正規表現
                display_stderr(&editor->display, "Syntax error: Empty regex pattern.",
                              editor->scriptingflag, editor->verbose);
                return -1;
            }
        }
        return -1;
    }
    
    // ファイル読み込み（範囲なし）
    else if (parsed_line[0] == 'r') {
        if (strlen(parsed_line) < 2) {
            char msg[256];
            bool success = filemgr_readfile(&editor->filemgr, editor->filemgr.filename, msg, sizeof(msg));
            if (msg[0]) {
                display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
            } else {
                display_stdmm(&editor->display, "Original file read.", 
                             editor->scriptingflag, editor->verbose);
            }
            return -1;
        }
    }
    
    // スクリプト実行（T/tコマンド）
    else if (parsed_line[0] == 'T' || parsed_line[0] == 't') {
        if (strlen(parsed_line) >= 2) {
            bool old_verbose = editor->verbose;
            bool old_scripting = editor->scriptingflag;
            
            editor->verbose = (parsed_line[0] == 'T');
            
            const char *scriptfile = parsed_line + 1;
            while (*scriptfile == ' ') scriptfile++;
            
            if (*scriptfile) {
                printf("\n");
                int result = editor_scripting(editor, scriptfile);
                
                editor->verbose = old_verbose;
                editor->scriptingflag = old_scripting;
                
                if (result == 0 || result == 1) {
                    return result;
                }
            } else {
                // 構文エラー: スクリプトファイルが指定されていない
                display_stderr(&editor->display, "Syntax error: No script file specified.",
                              editor->scriptingflag, editor->verbose);
                editor->verbose = old_verbose;
                editor->scriptingflag = old_scripting;
            }
        } else {
            // 構文エラー: T/tの後に何もない
            display_stderr(&editor->display, "Syntax error: No script file specified.",
                          editor->scriptingflag, editor->verbose);
        }
        return -1;
    }
    
    // 検索（n/N）
    else if (parsed_line[0] == 'n') {
        size_t pos = search_next(&editor->search, display_fpos(&editor->display) + 1,
                                editor->memory.mem.size);
        if (pos != (size_t)-1) {
            if (editor->display.highlight_ranges.size == 0) {
                search_all(&editor->search, editor->memory.mem.size,
                          &editor->display.highlight_ranges);
            }
            display_jump(&editor->display, pos);
        }
        return -1;
    } else if (parsed_line[0] == 'N') {
        size_t pos = search_last(&editor->search, display_fpos(&editor->display) - 1,
                                editor->memory.mem.size);
        if (pos != (size_t)-1) {
            if (editor->display.highlight_ranges.size == 0) {
                search_all(&editor->search, editor->memory.mem.size,
                          &editor->display.highlight_ranges);
            }
            display_jump(&editor->display, pos);
        }
        return -1;
    }
    
    // 範囲コマンドのパース
    size_t idx = parser_skipspc(parsed_line, 0);
    uint64_t x = parser_expression(&editor->parser, parsed_line, &idx);
    bool xf = false, xf2 = false;
    uint64_t x2 = x;
    
    if (x == UNKNOWN) {
        x = display_fpos(&editor->display);
    } else {
        xf = true;
    }
    
    idx = parser_skipspc(parsed_line, idx);
    if (parsed_line[idx] == ',') {
        idx = parser_skipspc(parsed_line, idx + 1);
        if (parsed_line[idx] == '*') {
            idx = parser_skipspc(parsed_line, idx + 1);
            uint64_t t = parser_expression(&editor->parser, parsed_line, &idx);
            if (t == UNKNOWN) t = 1;
            x2 = x + t - 1;
        } else {
            uint64_t t = parser_expression(&editor->parser, parsed_line, &idx);
            if (t != UNKNOWN) {
                x2 = t;
                xf2 = true;
            }
        }
    }
    
    if (x2 < x) x2 = x;
    idx = parser_skipspc(parsed_line, idx);
    
    if (parsed_line[idx] == '\0') {
        display_jump(&editor->display, x);
        return -1;
    }
    
    return execute_command(editor, parsed_line, idx, x, x2, xf, xf2);
}

int execute_command(BiEditor *editor, const char *line, size_t idx, 
                    uint64_t x, uint64_t x2, bool xf, bool xf2) {
    // yank
    if (line[idx] == 'y') {
        idx++;
        if (!xf && !xf2) {
            ByteArray m;
            idx = parser_get_hexs(&editor->parser, line, idx, &m);
            bytearray_free(&editor->memory.yank);
            editor->memory.yank = m;
        } else {
            memory_yank(&editor->memory, x, x2);
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "%zu bytes yanked.", editor->memory.yank.size);
        display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
        return -1;
    }
    
    // paste
    if (line[idx] == 'p') {
        if (editor->memory.yank.size > 0) {
            editor_save_undo_state(editor);
            memory_overwrite(&editor->memory, x, editor->memory.yank.data, editor->memory.yank.size);
            display_jump(&editor->display, x + editor->memory.yank.size);
        }
        return -1;
    }
    
    if (line[idx] == 'P') {
        if (editor->memory.yank.size > 0) {
            editor_save_undo_state(editor);
            memory_insert(&editor->memory, x, editor->memory.yank.data, editor->memory.yank.size);
            display_jump(&editor->display, x + editor->memory.yank.size);
        }
        return -1;
    }
    
    // mark
    if (line[idx] == 'm') {
        if (line[idx + 1] >= 'a' && line[idx + 1] <= 'z') {
            editor->memory.mark[line[idx + 1] - 'a'] = x;
            return -1;
        } else if (line[idx + 1] != '\0') {
            // 構文エラー: 無効なマーク文字
            display_stderr(&editor->display, "Syntax error: Invalid mark character (use 'ma' to 'mz').",
                          editor->scriptingflag, editor->verbose);
            return -1;
        }
        // 'm'だけの場合は次の処理へ（未認識コマンドとして処理される）
    }
    
    // read file (r/R commands)
    if (line[idx] == 'r' || line[idx] == 'R') {
        char cmd = line[idx];
        idx++;
        idx = parser_skipspc(line, idx);
        
        if (idx >= strlen(line)) {
            display_stderr(&editor->display, "File name not specified.", 
                          editor->scriptingflag, editor->verbose);
            return -1;
        }
        
        const char *filename = line + idx;
        FILE *f = fopen(filename, "rb");
        if (!f) {
            display_stderr(&editor->display, "File read error.", 
                          editor->scriptingflag, editor->verbose);
            return -1;
        }
        
        // ファイルサイズを取得
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        if (fsize > 0) {
            uint8_t *buffer = malloc(fsize);
            if (buffer) {
                size_t read_size = fread(buffer, 1, fsize, f);
                fclose(f);
                
                editor_save_undo_state(editor);
                if (cmd == 'r') {
                    // overwrite
                    memory_overwrite(&editor->memory, x, buffer, read_size);
                } else {
                    // insert
                    memory_insert(&editor->memory, x, buffer, read_size);
                }
                
                char msg[256];
                snprintf(msg, sizeof(msg), "%zu bytes read from %s", read_size, filename);
                display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
                display_jump(&editor->display, x + read_size);
                
                free(buffer);
            } else {
                fclose(f);
                display_stderr(&editor->display, "Memory allocation error.", 
                              editor->scriptingflag, editor->verbose);
            }
        } else {
            fclose(f);
        }
        return -1;
    }
    
    // delete
    if (line[idx] == 'd') {
        editor_save_undo_state(editor);
        if (memory_delete(&editor->memory, x, x2, true, memory_yank)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%llu bytes deleted.", (unsigned long long)(x2 - x + 1));
            display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
            display_jump(&editor->display, x);
        } else {
            display_stderr(&editor->display, "Invalid range.", editor->scriptingflag,editor->verbose);
            editor_dec_undo(editor);
        }
        return -1;
    }
    
    // insert/overwrite
    // execute_command() 内の i / I 処理部分を以下のように置き換え

    if (line[idx] == 'i' || line[idx] == 'I') {
        char ch = line[idx];
        idx++;
        idx = parser_skipspc(line, idx);

        // データ部分を読み込む
        ByteArray pattern;
        bytearray_init(&pattern);

        bool is_repeat = false;
        uint64_t repeat_count = 1;

        // まずパターンを読む（/.../ か 16進数列）
        if (line[idx] == '/') {
            char str[1024];
            idx = parser_get_restr(line, idx + 1, str);
            for (size_t i = 0; str[i]; i++) {
                bytearray_push(&pattern, (uint8_t)str[i]);
            }
        } else {
            idx = parser_get_hexs(&editor->parser, line, idx, &pattern);
        }

        if (pattern.size == 0) {
            display_stderr(&editor->display, "No data specified.", 
                           editor->scriptingflag, editor->verbose);
            bytearray_free(&pattern);
            return -1;
        }

        // * n のパターンをチェック
        idx = parser_skipspc(line, idx);
        if (line[idx] == '*') {
            idx++;
            idx = parser_skipspc(line, idx);
            uint64_t n = parser_expression(&editor->parser, line, &idx);
            if (n != UNKNOWN && n > 0) {
                is_repeat = true;
                repeat_count = n;
            } else {
                display_stderr(&editor->display, "Invalid repeat count.", 
                               editor->scriptingflag, editor->verbose);
                bytearray_free(&pattern);
                return -1;
            }
        }

        // 範囲チェック（削除・上書き系で重要）
        if (xf && xf2) {  // 範囲指定あり
            if (x > x2) {
                display_stderr(&editor->display, "Invalid range (start > end).", 
                               editor->scriptingflag, editor->verbose);
                bytearray_free(&pattern);
                return -1;
            }
            if (x >= editor->memory.mem.size) {
                display_stderr(&editor->display, "Invalid range.", 
                               editor->scriptingflag, editor->verbose);
                bytearray_free(&pattern);
                return -1;
            }
            // endがファイルサイズを超えていたら縮める（要件3対応）
            if (x2 >= editor->memory.mem.size) {
                x2 = editor->memory.mem.size - 1;
            }
        } else {
            // 範囲指定なし → カーソル位置から
            x = display_fpos(&editor->display);
            x2 = x;  // とりあえず1バイト扱い（後で調整）
        }

        editor_save_undo_state(editor);

        if (ch == 'I') {  // insert (挿入)
            ByteArray data_to_insert;
            bytearray_init(&data_to_insert);

            if (is_repeat) {
                // * n の場合 → パターンをn回繰り返す
                for (uint64_t r = 0; r < repeat_count; r++) {
                    for (size_t k = 0; k < pattern.size; k++) {
                        bytearray_push(&data_to_insert, pattern.data[k]);
                    }
                }
            } else if (xf && xf2) {
                // 範囲指定あり → 範囲長に合わせて繰り返し
                uint64_t range_len = x2 - x + 1;
                uint64_t full = range_len / pattern.size;
                uint64_t rem  = range_len % pattern.size;

                for (uint64_t r = 0; r < full; r++) {
                    for (size_t k = 0; k < pattern.size; k++) {
                        bytearray_push(&data_to_insert, pattern.data[k]);
                    }
                }
                for (size_t k = 0; k < rem; k++) {
                    bytearray_push(&data_to_insert, pattern.data[k]);
                }
            } else {
                // 範囲なし → パターンをそのまま1回
                for (size_t k = 0; k < pattern.size; k++) {
                    bytearray_push(&data_to_insert, pattern.data[k]);
                }
            }

            memory_insert(&editor->memory, x, data_to_insert.data, data_to_insert.size);
            display_jump(&editor->display, x + data_to_insert.size);

            char msg[256];
            snprintf(msg, sizeof(msg), "%zu bytes inserted.", data_to_insert.size);
            display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);

            bytearray_free(&data_to_insert);
        }
        else {  // 'i' → overwrite
            uint64_t range_len = 1;
            if (!xf&&!xf2) {
                range_len = pattern.size * repeat_count;
            } else if (xf&&xf2) {
                range_len=x2-x+1;
            } else if (xf&&!xf2) {
                range_len=pattern.size * repeat_count;
            } else {
                range_len=0;
            }
            ByteArray data_to_write;
            bytearray_init(&data_to_write);

            if (is_repeat) {
                // * n の場合 → パターンをn回（範囲を超えてもOK）
                for (uint64_t r = 0; r < repeat_count; r++) {
                    for (size_t k = 0; k < pattern.size; k++) {
                        bytearray_push(&data_to_write, pattern.data[k]);
                    }
                }
                // 範囲より長い場合は切り捨て
                if (data_to_write.size > range_len) {
                    data_to_write.size = range_len;
                }
            } else {
                // 範囲に合わせて繰り返し埋める
                uint64_t full = range_len / pattern.size;
                uint64_t rem  = range_len % pattern.size;

                for (uint64_t r = 0; r < full; r++) {
                    for (size_t k = 0; k < pattern.size; k++) {
                        bytearray_push(&data_to_write, pattern.data[k]);
                    }
                }
                for (size_t k = 0; k < rem; k++) {
                    bytearray_push(&data_to_write, pattern.data[k]);
                }
            }

            memory_overwrite(&editor->memory, x, data_to_write.data, data_to_write.size);
            display_jump(&editor->display, x + data_to_write.size);

            char msg[256];
            snprintf(msg, sizeof(msg), "%zu bytes overwritten.", data_to_write.size);
            display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);

            bytearray_free(&data_to_write);
        }

        bytearray_free(&pattern);
        return -1;
    }
    
    // substitute (s command)
    if (line[idx] == 's') {
        editor_save_undo_state(editor);
        return editor_scommand(editor, x, x2, xf, xf2, line, idx + 1);
    }
    
    // NOT (~command)
    if (line[idx] == '~') {
        editor_save_undo_state(editor);
        editor_openot(editor, x, x2);
        display_jump(&editor->display, x2 + 1);
        return -1;
    }
    
    // Shift/Rotate (<, > commands)
    if (line[idx] == '<' || line[idx] == '>') {
        char direction = line[idx];
        idx++;
        bool multibyte = false;
        
        if (idx < strlen(line) && line[idx] == direction) {
            multibyte = true;
            idx++;
        }
        
        int times = 1;
        idx = parser_skipspc(line, idx);
        uint64_t t = parser_expression(&editor->parser, line, &idx);
        if (t != UNKNOWN) {
            times = (int)t;
        }
        
        int bit = -1;
        idx = parser_skipspc(line, idx);
        if (idx < strlen(line) && line[idx] == ',') {
            idx++;
            uint64_t b = parser_expression(&editor->parser, line, &idx);
            if (b != UNKNOWN) {
                bit = (int)b;
            }
        }
        
        editor_save_undo_state(editor);
        editor_shift_rotate(editor, x, x2, times, bit, multibyte, direction);
        return -1;
    }
    
    // コマンド文字を探す
    char cmd = 0;
    size_t cmd_idx = idx;
    while (cmd_idx < strlen(line)) {
        char ch = line[cmd_idx];
        if (ch == 'c' || ch == 'C' || ch == 'v' || ch == '&' || ch == '|' || ch == '^') {
            cmd = ch;
            idx = cmd_idx + 1;
            break;
        }
        cmd_idx++;
    }
    
    if (cmd == 0) {
        // コマンド文字が見つからない場合
        if (idx < strlen(line) && line[idx] != '\0' && line[idx] != ' ') {
            // 未認識のコマンド文字がある
            display_stderr(&editor->display, "Unrecognized command.", 
                          editor->scriptingflag, editor->verbose);
        }
        return -1;
    }
    
    // 第3引数を取得
    idx = parser_skipspc(line, idx);
    uint64_t x3 = parser_expression(&editor->parser, line, &idx);
    if (x3 == UNKNOWN) {
        display_stderr(&editor->display, "Invalid parameter.", 
                      editor->scriptingflag, editor->verbose);
        return -1;
    }
    
    // copy/Copy (c/C commands)
    if (cmd == 'c' || cmd == 'C') {
        editor_save_undo_state(editor);
        
        ByteArray m;
        bytearray_init(&m);
        
        // データを読み出し
        for (uint64_t i = x; i <= x2 && i < editor->memory.mem.size; i++) {
            bytearray_push(&m, editor->memory.mem.data[i]);
        }
        
        // yankバッファにも保存
        bytearray_free(&editor->memory.yank);
        editor->memory.yank = bytearray_copy(&m);
        
        if (cmd == 'c') {
            // overwrite
            memory_overwrite(&editor->memory, x3, m.data, m.size);
            char msg[256];
            snprintf(msg, sizeof(msg), "%llu bytes copied.", (unsigned long long)(x2 - x + 1));
            display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
            display_jump(&editor->display, x3 + m.size);
        } else {
            // insert
            memory_insert(&editor->memory, x3, m.data, m.size);
            char msg[256];
            snprintf(msg, sizeof(msg), "%llu bytes inserted.", (unsigned long long)m.size);
            display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
            display_jump(&editor->display, x3 + m.size);
        }
        
        bytearray_free(&m);
        return -1;
    }
    
    // move (v command)
    if (cmd == 'v') {
        editor_save_undo_state(editor);
        uint64_t xp = editor_movmem(editor, x, x2, x3);
        display_jump(&editor->display, xp);
        return -1;
    }
    
    // ビット演算
    if (cmd == '&') {
        editor_save_undo_state(editor);
        editor_opeand(editor, x, x2, x3);
        display_jump(&editor->display, x2 + 1);
        return -1;
    }
    if (cmd == '|') {
        editor_save_undo_state(editor);
        editor_opeor(editor, x, x2, x3);
        display_jump(&editor->display, x2 + 1);
        return -1;
    }
    if (cmd == '^') {
        editor_save_undo_state(editor);
        editor_opexor(editor, x, x2, x3);
        display_jump(&editor->display, x2 + 1);
        return -1;
    }
    
    display_stderr(&editor->display, "Unrecognized command.", editor->scriptingflag, editor->verbose);
    return -1;
}

/* ========================================================================
 * 編集操作関数の実装
 * ======================================================================== */

void editor_opeand(BiEditor *editor, uint64_t x, uint64_t x2, uint64_t x3) {
    for (uint64_t i = x; i <= x2 && i < editor->memory.mem.size; i++) {
        memory_set(&editor->memory, i, memory_read(&editor->memory, i) & (x3 & 0xFF));
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "%llu bytes anded.", (unsigned long long)(x2 - x + 1));
    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
}

void editor_opeor(BiEditor *editor, uint64_t x, uint64_t x2, uint64_t x3) {
    for (uint64_t i = x; i <= x2 && i < editor->memory.mem.size; i++) {
        memory_set(&editor->memory, i, memory_read(&editor->memory, i) | (x3 & 0xFF));
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "%llu bytes ored.", (unsigned long long)(x2 - x + 1));
    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
}

void editor_opexor(BiEditor *editor, uint64_t x, uint64_t x2, uint64_t x3) {
    for (uint64_t i = x; i <= x2 && i < editor->memory.mem.size; i++) {
        memory_set(&editor->memory, i, memory_read(&editor->memory, i) ^ (x3 & 0xFF));
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "%llu bytes xored.", (unsigned long long)(x2 - x + 1));
    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
}

void editor_openot(BiEditor *editor, uint64_t x, uint64_t x2) {
    for (uint64_t i = x; i <= x2 && i < editor->memory.mem.size; i++) {
        memory_set(&editor->memory, i, (~memory_read(&editor->memory, i)) & 0xFF);
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "%llu bytes noted.", (unsigned long long)(x2 - x + 1));
    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
}

uint64_t editor_movmem(BiEditor *editor, uint64_t start, uint64_t end, uint64_t dest) {
    if (start <= dest && dest <= end) {
        return end + 1;
    }
    
    size_t len = editor->memory.mem.size;
    if (start >= len) {
        return dest;
    }
    
    // データを読み出し
    ByteArray m;
    bytearray_init(&m);
    for (uint64_t i = start; i <= end && i < len; i++) {
        bytearray_push(&m, editor->memory.mem.data[i]);
    }
    
    // 元の位置から削除（yankにも保存）
    memory_delete(&editor->memory, start, end, true, memory_yank);
    
    uint64_t xp;
    if (dest > len) {
        memory_overwrite(&editor->memory, dest, m.data, m.size);
        xp = dest + m.size;
    } else {
        if (dest > start) {
            memory_insert(&editor->memory, dest - (end - start + 1), m.data, m.size);
            xp = dest - (end - start) + m.size - 1;
        } else {
            memory_insert(&editor->memory, dest, m.data, m.size);
            xp = dest + m.size;
        }
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "%llu bytes moved.", (unsigned long long)(end - start + 1));
    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
    
    bytearray_free(&m);
    return xp;
}

void editor_shift_rotate(BiEditor *editor, uint64_t x, uint64_t x2, int times, 
                        int bit, bool multibyte, char direction) {
    for (int t = 0; t < times; t++) {
        if (!multibyte) {
            // バイト単位
            if (bit != 0 && bit != 1) {
                // ローテート
                if (direction == '<') {
                    for (uint64_t i = x; i <= x2 && i < editor->memory.mem.size; i++) {
                        uint8_t m = memory_read(&editor->memory, i);
                        uint8_t c = (m & 0x80) >> 7;
                        memory_set(&editor->memory, i, (m << 1) | c);
                    }
                } else {
                    for (uint64_t i = x; i <= x2 && i < editor->memory.mem.size; i++) {
                        uint8_t m = memory_read(&editor->memory, i);
                        uint8_t c = (m & 0x01) << 7;
                        memory_set(&editor->memory, i, (m >> 1) | c);
                    }
                }
            } else {
                // シフト
                uint8_t carry = bit & 1;
                if (direction == '<') {
                    for (uint64_t i = x; i <= x2 && i < editor->memory.mem.size; i++) {
                        memory_set(&editor->memory, i, (memory_read(&editor->memory, i) << 1) | carry);
                    }
                } else {
                    for (uint64_t i = x; i <= x2 && i < editor->memory.mem.size; i++) {
                        memory_set(&editor->memory, i, (memory_read(&editor->memory, i) >> 1) | (carry << 7));
                    }
                }
            }
        } else {
            // マルチバイト
            uint64_t len = x2 - x + 1;
            if (len == 0 || x >= editor->memory.mem.size) continue;
            
            // 値を読み出し
            uint64_t v = 0;
            for (uint64_t i = x2; i >= x && i < editor->memory.mem.size; i--) {
                v = (v << 8) | memory_read(&editor->memory, i);
                if (i == 0) break;
            }
            
            if (bit != 0 && bit != 1) {
                // ローテート
                if (direction == '<') {
                    uint64_t c = (v & (1ULL << (len * 8 - 1))) ? 1 : 0;
                    v = (v << 1) | c;
                } else {
                    uint64_t c = (v & 1) ? 1 : 0;
                    v = (v >> 1) | (c << (len * 8 - 1));
                }
            } else {
                // シフト
                uint64_t carry = bit & 1;
                if (direction == '<') {
                    v = (v << 1) | carry;
                } else {
                    v = (v >> 1) | (carry << (len * 8 - 1));
                }
            }
            
            // 値を書き戻し
            for (uint64_t i = x; i <= x2 && i < editor->memory.mem.size; i++) {
                memory_set(&editor->memory, i, v & 0xFF);
                v >>= 8;
            }
        }
    }
}

size_t editor_searchnextnoloop(BiEditor *editor, size_t fp) {
    if (!editor->search.regexp && editor->search.smem.size == 0) {
        return (size_t)-1;
    }
    
    size_t curpos = fp;
    while (curpos < editor->memory.mem.size) {
        int f = editor->search.regexp ? 
                search_hitre(&editor->search, curpos) : 
                search_hit(&editor->search, curpos);
        
        if (f == 1) {
            return curpos;
        } else if (f < 0) {
            return (size_t)-1;
        }
        curpos++;
    }
    return (size_t)-1;
}

int editor_scommand(BiEditor *editor, uint64_t start, uint64_t end, 
                    bool xf, bool xf2, const char *line, size_t idx) {
    editor->search.nff = false;
    size_t pos = display_fpos(&editor->display);
    
    idx = parser_skipspc(line, idx);
    if (!xf && !xf2) {
        start = 0;
        end = editor->memory.mem.size > 0 ? editor->memory.mem.size - 1 : 0;
    }
    
    // 検索パターンを取得
    if (idx < strlen(line) && line[idx] == '/') {
        idx++;
        if (idx < strlen(line) && line[idx] != '/') {
            // 正規表現
            char pattern[1024];
            idx = parser_get_restr(line, idx, pattern);
            editor->search.regexp = true;
            strncpy(editor->search.remem, pattern, sizeof(editor->search.remem) - 1);
            editor->search.remem[sizeof(editor->search.remem) - 1] = '\0';
            editor->search.span = strlen(pattern);
        } else if (idx < strlen(line) && line[idx] == '/') {
            // 16進数
            ByteArray sm;
            idx = parser_get_hexs(&editor->parser, line, idx + 1, &sm);
            bytearray_free(&editor->search.smem);
            editor->search.smem = sm;
            editor->search.regexp = false;
            editor->search.remem[0] = '\0';
            editor->search.span = sm.size;
        } else {
            display_stderr(&editor->display, "Invalid syntax.", 
                          editor->scriptingflag, editor->verbose);
            return -1;
        }
    }
    
    if (editor->search.span == 0) {
        display_stderr(&editor->display, "Specify search object.", 
                      editor->scriptingflag, editor->verbose);
        return -1;
    }
    
    // 置換テキストを取得
    ByteArray replacement;
    bytearray_init(&replacement);
    
    idx = parser_skipspc(line, idx);
    if (idx < strlen(line) && line[idx] == '/') {
        idx++;
        if (idx >= strlen(line)) {
            // 構文エラー: /の後に何もない
            display_stderr(&editor->display, "Syntax error: Missing replacement pattern.", 
                          editor->scriptingflag, editor->verbose);
            bytearray_free(&replacement);
            return -1;
        }
        if (line[idx] == '/') {
            // 16進数
            idx = parser_get_hexs(&editor->parser, line, idx + 1, &replacement);
        } else {
            // 文字列
            char str[1024];
            idx = parser_get_restr(line, idx, str);
            for (size_t i = 0; str[i]; i++) {
                bytearray_push(&replacement, str[i]);
            }
        }
    }
    
    // 置換実行
    int cnt = 0;
    display_jump(&editor->display, start);
    
    while (true) {
        size_t found_pos = editor_searchnextnoloop(editor, display_fpos(&editor->display));
        
        if (found_pos == (size_t)-1) {
            break;
        }
        
        display_jump(&editor->display, found_pos);
        size_t i = display_fpos(&editor->display);
        
        if (i <= end) {
            memory_delete(&editor->memory, i, i + editor->search.span - 1, false, memory_yank);
            if (replacement.size > 0) {
                memory_insert(&editor->memory, i, replacement.data, replacement.size);
            }
            pos = i + replacement.size;
            cnt++;
            display_jump(&editor->display, pos);
        } else {
            break;
        }
    }
    
    display_jump(&editor->display, pos);
    char msg[256];
    snprintf(msg, sizeof(msg), "  %d times replaced.", cnt);
    display_stdmm(&editor->display, msg, editor->scriptingflag, editor->verbose);
    
    bytearray_free(&replacement);
    return -1;
}

/* ========================================================================
 * スクリプト実行
 * ======================================================================== */

int editor_scripting(BiEditor *editor, const char *scriptfile) {
    FILE *f = fopen(scriptfile, "r");
    if (!f) {
        display_stderr(&editor->display, "Script file open error.", 
                      editor->scriptingflag, editor->verbose);
        return -1;
    }
    
    char line[4096];
    int flag = -1;
    editor->scriptingflag = true;
    
    while (fgets(line, sizeof(line), f)) {
        // 改行を削除
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        
        if (line[0] == '\0') continue;  // 空行をスキップ
        
        if (editor->verbose) {
            printf("%s\n", line);
        }
        
        flag = editor_commandline(editor, line);
        
        if (flag == 0) {
            fclose(f);
            return 0;
        } else if (flag == 1) {
            fclose(f);
            return 1;
        }
    }
    
    fclose(f);
    return 0;
}

/* ========================================================================
 * main関数
 * ======================================================================== */

int main(int argc, char *argv[]) {
    // コマンドライン引数処理
    const char *filename = NULL;
    const char *scriptfile = NULL;
    const char *termcol = "black";
    bool verbose = false;
    bool write_on_exit = false;
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file> [-s script.bi] [-t termcolor] [-v] [-w]\n", argv[0]);
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  -s <script>  Execute script file\n");
        fprintf(stderr, "  -t <color>   Terminal color (black/white)\n");
        fprintf(stderr, "  -v           Verbose mode (show commands when scripting)\n");
        fprintf(stderr, "  -w           Write file when exiting script\n");
        return 1;
    }
    
    filename = argv[1];
    
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            scriptfile = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            termcol = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-w") == 0) {
            write_on_exit = true;
        }
    }
    
    // エディタ初期化
    BiEditor editor;
    editor_init(&editor, termcol);
    editor.verbose = verbose;
    strncpy(editor.filemgr.filename, filename, sizeof(editor.filemgr.filename) - 1);
    
    // 画面クリア（スクリプトモード以外）
    if (!scriptfile) {
        terminal_clear(&editor.term);
    } else {
        editor.scriptingflag = true;
    }
    
    // ファイル読み込み
    char msg[256];
    bool success = filemgr_readfile(&editor.filemgr, filename, msg, sizeof(msg));
    if (!success) {
        fprintf(stderr, "%s\n", msg);
        editor_free(&editor);
        return 1;
    } else if (msg[0]) {
        display_stdmm(&editor.display, msg, editor.scriptingflag, editor.verbose);
    }
    
    // スクリプト実行またはインタラクティブモード
    if (scriptfile) {
        int result = editor_scripting(&editor, scriptfile);
        
        if (write_on_exit && editor.memory.lastchange) {
            char write_msg[256];
            success = filemgr_writefile(&editor.filemgr, filename, write_msg, sizeof(write_msg));
            if (success && editor.verbose) {
                printf("%s\n", write_msg);
            }
        }
        
        editor_free(&editor);
        return result;
    } else {
        // インタラクティブモード
        editor_fedit(&editor);
        
        // 終了処理
        terminal_color(&editor.term, 7, 0);
        terminal_dispcursor(&editor.term);
        terminal_locate(&editor.term, 0, 23);
        
        editor_free(&editor);
        return 0;
    }
}
