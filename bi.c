/* 
 * Complete C port of bi.go (a terminal hex editor) - FULL VERSION
 * This is a complete, feature-compatible translation from Go to C.
 *
 * Build:
 *   gcc -o bi bi.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <regex.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>

#define ESC "\033["
#define LENONSCR (19 * 16)
#define BOTTOMLN 22
#define RELEN 128
#define UNKNOWN ((int64_t)1LL << 62)
#define MAX_MEM_SIZE (1024LL * 1024 * 1024 *10)  // 10GB limit
#define MAX_LINE 4096
#define MAX_HISTORY 100
#define MAX_FILENAME 256

// Global variables
static unsigned char *mem = NULL;
static int64_t mem_len = 0;
static int64_t mem_cap = 0;
static unsigned char *yank = NULL;
static int64_t yank_len = 0;
static int64_t yank_cap = 0;
static int coltab[] = {0, 1, 4, 5, 2, 6, 3, 7};
static char filename[MAX_FILENAME] = "";
static char termcol[16] = "black";
static bool lastchange = false;
static bool modified = false;
static bool newfile = false;
static int64_t homeaddr = 0;
static bool utf8mode = false;
static bool insmod = false;
static int curx = 0;
static int cury = 0;
static int64_t mark[26];
static unsigned char *smem = NULL;
static int64_t smem_len = 0;
static int64_t smem_cap = 0;
static bool regexpMode = false;
static int repsw = 0;
static char remem[MAX_LINE] = "";
static int span = 0;
static bool nff = true;
static bool verbose = false;
static bool scriptingflag = false;
static int64_t cp = 0;
static regex_t reObj;
static bool reObj_compiled = false;

// Stack for script nesting
typedef struct {
    void **items;
    int count;
    int capacity;
} Stack;

static Stack stack = {NULL, 0, 0};

// History structure
typedef struct {
    char **items;
    int count;
    int capacity;
} History;

static History cmd_history = {NULL, 0, 0};
static History search_history = {NULL, 0, 0};

// Terminal state
static struct termios orig_termios;
static bool raw_mode_active = false;

// Forward declarations
static void clrmm(void);
static void stdmm(const char *msg);
static void stderr_msg(const char *msg);
static int64_t expression(const char *line, int idx, int *new_idx);
static int64_t get_value(const char *s, int idx, int *new_idx);
static void jump(int64_t addr);
static int64_t fpos(void);
static void repaint(void);
static bool searchnext(int64_t fp);
static bool searchlast(int64_t fp);

// Terminal control helpers
static void escnocursor(void) { printf("%s?25l", ESC); fflush(stdout); }
static void escdispcursor(void) { printf("%s?25h", ESC); fflush(stdout); }
static void escup(int n) { printf("%s%dA", ESC, n); }
static void escdown(int n) { printf("%s%dB", ESC, n); }
static void escright(int n) { printf("%s%dC", ESC, n); }
static void escleft(int n) { printf("%s%dD", ESC, n); }
static void esclocate(int x, int y) { printf("%s%d;%dH", ESC, y + 1, x + 1); }
static void escscrollup(int n) { printf("%s%dS", ESC, n); }
static void escscrolldown(int n) { printf("%s%dT", ESC, n); }
static void escclear(void) { printf("%s2J", ESC); esclocate(0, 0); }
static void escclraftcur(void) { printf("%s0J", ESC); }
static void escclrline(void) { printf("%s2K", ESC); }

static void esccolor(int col1, int col2) {
    if (strcmp(termcol, "black") == 0) {
        printf("%s3%dm%s4%dm", ESC, coltab[col1], ESC, coltab[col2]);
    } else {
        printf("%s3%dm%s4%dm", ESC, coltab[0], ESC, coltab[7]);
    }
}

static void escresetcolor(void) { printf("%s0m", ESC); }

// Raw-mode handling
static void disable_raw_mode(void) {
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_active = false;
    }
}

static void enable_raw_mode(void) {
    if (!raw_mode_active) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        raw_mode_active = true;
    }
}

static unsigned char getch_byte(void) {
    enable_raw_mode();
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        c = 0;
    }
    disable_raw_mode();
    return c;
}

// Memory management helpers
static void ensure_mem_capacity(int64_t needed) {
    if (needed > MAX_MEM_SIZE) {
        fprintf(stderr, "Memory limit exceeded\n");
        exit(1);
    }
    if (needed > mem_cap) {
        int64_t new_cap = mem_cap == 0 ? 4096 : mem_cap * 2;
        while (new_cap < needed) new_cap *= 2;
        if (new_cap > MAX_MEM_SIZE) new_cap = MAX_MEM_SIZE;
        mem = realloc(mem, new_cap);
        if (!mem) {
            perror("Memory allocation failed");
            exit(1);
        }
        mem_cap = new_cap;
    }
}

static void ensure_yank_capacity(int64_t needed) {
    if (needed > yank_cap) {
        int64_t new_cap = yank_cap == 0 ? 1024 : yank_cap * 2;
        while (new_cap < needed) new_cap *= 2;
        yank = realloc(yank, new_cap);
        if (!yank) {
            perror("Yank buffer allocation failed");
            exit(1);
        }
        yank_cap = new_cap;
    }
}

static void ensure_smem_capacity(int64_t needed) {
    if (needed > smem_cap) {
        int64_t new_cap = smem_cap == 0 ? 256 : smem_cap * 2;
        while (new_cap < needed) new_cap *= 2;
        smem = realloc(smem, new_cap);
        if (!smem) {
            perror("Search buffer allocation failed");
            exit(1);
        }
        smem_cap = new_cap;
    }
}

// String helpers
static int skipspc(const char *s, int idx) {
    while (s[idx] == ' ' && s[idx] != '\0') idx++;
    return idx;
}

// Message display
static void stderr_msg(const char *msg) {
    if (scriptingflag) {
        fprintf(stderr, "%s\n", msg);
    } else {
        clrmm();
        esccolor(3, 0);
        esclocate(0, BOTTOMLN);
        printf(" %s", msg);
        for (size_t i = strlen(msg) + 1; i < 80; i++) printf(" ");
        fflush(stdout);
    }
}

static void stdmm(const char *msg) {
    if (scriptingflag) {
        if (verbose) {
            printf("%s\n", msg);
        }
    } else {
        clrmm();
        esccolor(4, 0);
        esclocate(0, BOTTOMLN);
        printf(" %s", msg);
        for (size_t i = strlen(msg) + 1; i < 80; i++) printf(" ");
        fflush(stdout);
    }
}

static void clrmm(void) {
    esclocate(0, BOTTOMLN);
    esccolor(6, 0);
    escclrline();
    fflush(stdout);
}

// Position helpers
static int64_t fpos(void) {
    return homeaddr + (int64_t)(cury * 16 + curx / 2);
}

static void jump(int64_t addr) {
    if (addr < homeaddr || addr >= homeaddr + LENONSCR) {
        homeaddr = addr & ~0xffLL;  // align to 256 bytes
    }
    int64_t i = addr - homeaddr;
    curx = (int)((i & 0xf) * 2);
    cury = (int)(i / 16);
}

static void scrup(void) {
    if (homeaddr >= 16) {
        homeaddr -= 16;
    }
}

static void scrdown(void) {
    homeaddr += 16;
}

static void inccurx(void) {
    if (curx < 31) {
        curx++;
    } else {
        curx = 0;
        if (cury < LENONSCR / 16 - 1) {
            cury++;
        } else {
            scrdown();
        }
    }
}

// Memory operations
static int readmem(int64_t addr) {
    if (addr < 0 || addr >= mem_len) return 0;
    return mem[addr] & 0xff;
}

static void setmem(int64_t addr, int data) {
    if (addr < 0) return;
    ensure_mem_capacity(addr + 1);
    
    if (addr >= mem_len) {
        int64_t padding = addr - mem_len + 1;
        if (padding > 0) {
            memset(mem + mem_len, 0, padding);
            mem_len = addr + 1;
        }
    }
    
    if (data >= 0 && data <= 255) {
        mem[addr] = (unsigned char)data;
    } else {
        mem[addr] = 0;
    }
    modified = true;
    lastchange = true;
}

static void insmem(int64_t start, const unsigned char *mem2, int64_t len2) {
    if (len2 <= 0) return;
    ensure_mem_capacity(mem_len + len2);
    
    if (start >= mem_len) {
        if (start > mem_len) {
            memset(mem + mem_len, 0, start - mem_len);
        }
        memcpy(mem + start, mem2, len2);
        mem_len = start + len2;
    } else {
        memmove(mem + start + len2, mem + start, mem_len - start);
        memcpy(mem + start, mem2, len2);
        mem_len += len2;
    }
    modified = true;
    lastchange = true;
}

static void delmem(int64_t start, int64_t end, bool yf);

static void yankmem(int64_t start, int64_t end) {
    int64_t length = end - start + 1;
    if (length <= 0 || start >= mem_len) {
        stderr_msg("Invalid range.");
        return;
    }
    
    ensure_yank_capacity(length);
    yank_len = 0;
    
    for (int64_t j = start; j <= end && j < mem_len; j++) {
        yank[yank_len++] = mem[j] & 0xff;
    }
    
    char msg[128];
    snprintf(msg, sizeof(msg), "%lld bytes yanked.", (long long)yank_len);
    stdmm(msg);
}

static void delmem(int64_t start, int64_t end, bool yf) {
    int64_t length = end - start + 1;
    if (length <= 0 || start >= mem_len) {
        stderr_msg("Invalid range.");
        return;
    }
    if (yf) {
        yankmem(start, end);
    }
    if (start < 0) start = 0;
    if (end >= mem_len) end = mem_len - 1;
    
    memmove(mem + start, mem + end + 1, mem_len - end - 1);
    mem_len -= (end - start + 1);
    lastchange = true;
    modified = true;
}

static void ovwmem(int64_t start, const unsigned char *mem0, int64_t len0) {
    if (len0 == 0) return;
    ensure_mem_capacity(start + len0);
    
    if (start + len0 > mem_len) {
        if (start > mem_len) {
            memset(mem + mem_len, 0, start - mem_len);
        }
        mem_len = start + len0;
    }
    
    for (int64_t j = 0; j < len0; j++) {
        mem[start + j] = mem0[j] & 0xff;
    }
    lastchange = true;
    modified = true;
}

static unsigned char *redmem(int64_t start, int64_t end, int64_t *out_len) {
    int64_t len = end - start + 1;
    unsigned char *m = malloc(len);
    if (!m) {
        *out_len = 0;
        return NULL;
    }
    
    *out_len = 0;
    for (int64_t i = start; i <= end; i++) {
        if (i >= 0 && i < mem_len) {
            m[(*out_len)++] = mem[i] & 0xff;
        } else {
            m[(*out_len)++] = 0;
        }
    }
    return m;
}

static void cpymem(int64_t start, int64_t end, int64_t dest) {
    int64_t len;
    unsigned char *m = redmem(start, end, &len);
    if (m) {
        ovwmem(dest, m, len);
        free(m);
    }
}

static int64_t movmem(int64_t start, int64_t end, int64_t dest) {
    if (start <= dest && dest <= end) {
        return end + 1;
    }
    int64_t l = mem_len;
    if (start >= l) {
        return dest;
    }
    
    int64_t len;
    unsigned char *m = redmem(start, end, &len);
    yankmem(start, end);
    delmem(start, end, false);
    
    if (dest > l) {
        ovwmem(dest, m, len);
        free(m);
        return dest + len;
    } else {
        if (dest > start) {
            insmem(dest - (end - start + 1), m, len);
            int64_t ret = dest - (end - start) + len - 1;
            free(m);
            return ret;
        } else {
            insmem(dest, m, len);
            int64_t ret = dest + len;
            free(m);
            return ret;
        }
    }
}

// UTF-8 validation helper
static bool is_valid_utf8_seq(const unsigned char *s, int len, int64_t max_check) {
    if (len == 2 && max_check >= 2) {
        return (s[0] >= 0xc0 && s[0] <= 0xdf) && (s[1] >= 0x80 && s[1] <= 0xbf);
    } else if (len == 3 && max_check >= 3) {
        return (s[0] >= 0xe0 && s[0] <= 0xef) && 
               (s[1] >= 0x80 && s[1] <= 0xbf) &&
               (s[2] >= 0x80 && s[2] <= 0xbf);
    } else if (len == 4 && max_check >= 4) {
        return (s[0] >= 0xf0 && s[0] <= 0xf7) &&
               (s[1] >= 0x80 && s[1] <= 0xbf) &&
               (s[2] >= 0x80 && s[2] <= 0xbf) &&
               (s[3] >= 0x80 && s[3] <= 0xbf);
    }
    return false;
}

static int printchar(int64_t a) {
    if (a >= mem_len) {
        printf("~");
        return 1;
    }
    
    if (utf8mode) {
        unsigned char b = mem[a];
        if (b < 0x80 || (b >= 0x80 && b <= 0xbf) || (b >= 0xf8 && b <= 0xff)) {
            if (b >= 0x20 && b <= 0x7e) {
                printf("%c", b);
            } else {
                printf(".");
            }
            return 1;
        } else if (b >= 0xc0 && b <= 0xdf) {
            if (a + 1 < mem_len && is_valid_utf8_seq(mem + a, 2, mem_len - a)) {
                printf("%c%c", mem[a], mem[a + 1]);
                return 2;
            }
            printf(".");
            return 1;
        } else if (b >= 0xe0 && b <= 0xef) {
            if (a + 2 < mem_len && is_valid_utf8_seq(mem + a, 3, mem_len - a)) {
                printf("%c%c%c ", mem[a], mem[a + 1], mem[a + 2]);
                return 3;
            }
            printf(".");
            return 1;
        } else if (b >= 0xf0 && b <= 0xf7) {
            if (a + 3 < mem_len && is_valid_utf8_seq(mem + a, 4, mem_len - a)) {
                printf("%c%c%c%c  ", mem[a], mem[a + 1], mem[a + 2], mem[a + 3]);
                return 4;
            }
            printf(".");
            return 1;
        }
    }
    
    // ASCII fallback
    unsigned char ch = mem[a];
    if (ch >= 0x20 && ch <= 0x7e) {
        printf("%c", ch);
    } else {
        printf(".");
    }
    return 1;
}

static void print_title(void) {
    esclocate(0, 0);
    esccolor(6, 0);
    const char *mode = insmod ? "insert   " : "overwrite";
    char utf8str[16];
    if (utf8mode) {
        snprintf(utf8str, sizeof(utf8str), "%d", repsw);
    } else {
        strcpy(utf8str, "off");
    }
    printf("bi C version 3.4.4 by Taisuke Maekawa           utf8mode:%s     %s   \n", 
           utf8str, mode);
    
    esccolor(5, 0);
    char fn[40];
    strncpy(fn, filename, 35);
    fn[35] = '\0';
    const char *mod = modified ? "modified" : "not modified";
    printf("file:[%-35s] length:%lld bytes [%s]    \n", fn, (long long)mem_len, mod);
}

static void repaint(void) {
    print_title();
    escnocursor();
    esclocate(0, 2);
    esccolor(4, 0);
    printf("OFFSET       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F 0123456789ABCDEF ");
    esccolor(7, 0);
    
    int64_t addr = homeaddr;
    for (int y = 0; y < LENONSCR / 16; y++) {
        esccolor(5, 0);
        esclocate(0, 3 + y);
        printf("%012llX ", (unsigned long long)((addr + y * 16) & 0xffffffffffffLL));
        esccolor(7, 0);
        
        for (int i = 0; i < 16; i++) {
            int64_t pos = addr + y * 16 + i;
            if (pos >= mem_len) {
                printf("~~ ");
            } else {
                printf("%02X ", mem[pos] & 0xff);
            }
        }
        
        esccolor(6, 0);
        int64_t a = addr + y * 16;
        int by = 0;
        while (by < 16) {
            int c = printchar(a);
            a += c;
            by += c;
        }
        printf("  ");
    }
    esccolor(0, 0);
    escdispcursor();
    fflush(stdout);
}

static void printdata(void) {
    int64_t addr = fpos();
    int a = readmem(addr);
    esclocate(0, 23);
    esccolor(6, 0);
    
    char s[16];
    if (a < 0x20) {
        snprintf(s, sizeof(s), "^%c", a + '@');
    } else if (a >= 0x7e) {
        strcpy(s, ".");
    } else {
        snprintf(s, sizeof(s), "'%c'", a);
    }
    
    if (addr < mem_len) {
        printf("%012llX : 0x%02X 0b", (unsigned long long)addr, a);
        for (int i = 7; i >= 0; i--) {
            printf("%d", (a >> i) & 1);
        }
        printf(" 0o%03o %d %s      ", a, a, s);
    } else {
        printf("%012llX : ~~                                                   ",
               (unsigned long long)addr);
    }
    fflush(stdout);
}

static void disp_curpos(void) {
    esccolor(4, 0);
    esclocate(curx / 2 * 3 + 12, cury + 3);
    printf("[");
    esclocate(curx / 2 * 3 + 15, cury + 3);
    printf("]");
    fflush(stdout);
}

static void erase_curpos(void) {
    esccolor(7, 0);
    esclocate(curx / 2 * 3 + 12, cury + 3);
    printf(" ");
    esclocate(curx / 2 * 3 + 15, cury + 3);
    printf(" ");
    fflush(stdout);
}

static void disp_marks(void) {
    int j = 0;
    esclocate(0, BOTTOMLN);
    esccolor(7, 0);
    for (int i = 0; i < 26; i++) {
        if (mark[i] == UNKNOWN) {
            printf("%c = unknown         ", 'a' + i);
        } else {
            printf("%c = %012llX    ", 'a' + i, (unsigned long long)mark[i]);
        }
        j++;
        if (j % 3 == 0) {
            printf("\n");
        }
    }
    esccolor(4, 0);
    printf("[ hit any key ]");
    fflush(stdout);
    getch_byte();
    escclear();
}

static void invoke_shell(const char *line) {
    esccolor(7, 0);
    printf("\n");
    fflush(stdout);
    
    // Execute shell command
    int ret = system(line);
    (void)ret;  // Suppress warning
    
    esccolor(4, 0);
    printf("[ Hit any key to return ]");
    fflush(stdout);
    getch_byte();
    escclear();
}

// Expression and value parsing
static int64_t get_value(const char *s, int idx, int *new_idx) {
    if (idx >= (int)strlen(s)) {
        *new_idx = idx;
        return UNKNOWN;
    }
    idx = skipspc(s, idx);
    if (idx >= (int)strlen(s)) {
        *new_idx = idx;
        return UNKNOWN;
    }
    
    char ch = s[idx];
    
    // '$' = end of memory
    if (ch == '$') {
        idx++;
        *new_idx = idx;
        if (mem_len != 0) {
            return mem_len - 1;
        }
        return 0;
    }
    
    // '{...}' = expression evaluation
    if (ch == '{') {
        idx++;
        char u[MAX_LINE] = "";
        int ui = 0;
        while (idx < (int)strlen(s) && s[idx] != '}') {
            u[ui++] = s[idx++];
        }
        u[ui] = '\0';
        if (s[idx] == '}') idx++;
        
        if (strlen(u) == 0) {
            stderr_msg("Invalid eval expression.");
            *new_idx = idx;
            return UNKNOWN;
        }
        
        // Parse as integer (supports 0x prefix)
        char *endptr;
        int64_t v = strtoll(u, &endptr, 0);
        if (*endptr != '\0') {
            stderr_msg("Invalid eval expression.");
            *new_idx = idx;
            return UNKNOWN;
        }
        if (v < 0) v = 0;
        *new_idx = idx;
        return v;
    }
    
    // '.' = current position
    if (ch == '.') {
        idx++;
        *new_idx = idx;
        return fpos();
    }
    
    // 'mark
    if (ch == '\'' && idx + 1 < (int)strlen(s) && s[idx + 1] >= 'a' && s[idx + 1] <= 'z') {
        idx++;
        int64_t v = mark[s[idx] - 'a'];
        if (v == UNKNOWN) {
            stderr_msg("Unknown mark.");
            *new_idx = idx;
            return UNKNOWN;
        }
        idx++;
        *new_idx = idx;
        return v;
    }
    
    // Hex number (default base 16)
    if (strchr("0123456789abcdefABCDEF", ch)) {
        int64_t x = 0;
        while (idx < (int)strlen(s) && strchr("0123456789abcdefABCDEF", s[idx])) {
            char d = s[idx];
            int val;
            if (d >= '0' && d <= '9') val = d - '0';
            else if (d >= 'a' && d <= 'f') val = d - 'a' + 10;
            else val = d - 'A' + 10;
            x = 16 * x + val;
            idx++;
        }
        if (x < 0) x = 0;
        *new_idx = idx;
        return x;
    }
    
    // Decimal number with % prefix
    if (ch == '%') {
        idx++;
        int64_t x = 0;
        while (idx < (int)strlen(s) && s[idx] >= '0' && s[idx] <= '9') {
            x = x * 10 + (s[idx] - '0');
            idx++;
        }
        if (x < 0) x = 0;
        *new_idx = idx;
        return x;
    }
    
    *new_idx = idx;
    return UNKNOWN;
}

static int64_t expression(const char *s, int idx, int *new_idx) {
    int64_t x = get_value(s, idx, &idx);
    
    if (idx < (int)strlen(s) && x != UNKNOWN && s[idx] == '+') {
        int64_t y = get_value(s, idx + 1, &idx);
        x = x + y;
    } else if (idx < (int)strlen(s) && x != UNKNOWN && s[idx] == '-') {
        int64_t y = get_value(s, idx + 1, &idx);
        x = x - y;
        if (x < 0) x = 0;
    }
    
    *new_idx = idx;
    return x;
}

// Search helpers
static int hit(int64_t addr) {
    for (int64_t i = 0; i < smem_len; i++) {
        if (addr + i < mem_len && mem[addr + i] == smem[i]) {
            continue;
        } else {
            return 0;
        }
    }
    return 1;
}

static int hitre(int64_t addr) {
    if (remem[0] == '\0') {
        return -1;
    }
    span = 0;
    
    unsigned char m[RELEN + 1];
    int64_t m_len = 0;
    
    if (addr < mem_len - RELEN) {
        memcpy(m, mem + addr, RELEN);
        m_len = RELEN;
    } else if (addr < mem_len) {
        m_len = mem_len - addr;
        memcpy(m, mem + addr, m_len);
    } else {
        return 0;
    }
    m[m_len] = '\0';
    
    // Compile regex if not already compiled
    if (!reObj_compiled || strcmp(remem, "") != 0) {
        if (reObj_compiled) {
            regfree(&reObj);
        }
        int ret = regcomp(&reObj, remem, REG_EXTENDED);
        if (ret != 0) {
            stderr_msg("Bad regular expression.");
            return -1;
        }
        reObj_compiled = true;
    }
    
    // Match at the beginning only
    regmatch_t pmatch[1];
    int ret = regexec(&reObj, (char *)m, 1, pmatch, 0);
    if (ret == REG_NOMATCH) {
        return 0;
    }
    if (ret != 0) {
        return -1;
    }
    
    // Check if match is at beginning
    if (pmatch[0].rm_so != 0) {
        return 0;
    }
    
    span = pmatch[0].rm_eo - pmatch[0].rm_so;
    return 1;
}

static int searchnextnoloop(int64_t fp) {
    int64_t curPos = fp;
    if (!regexpMode && smem_len == 0) {
        return 0;
    }
    
    while (1) {
        int f;
        if (regexpMode) {
            f = hitre(curPos);
        } else {
            f = hit(curPos);
        }
        
        if (f == 1) {
            jump(curPos);
            return 1;
        } else if (f < 0) {
            return -1;
        }
        
        curPos++;
        if (curPos >= mem_len) {
            jump(mem_len);
            return 0;
        }
    }
}

static bool searchnext(int64_t fp) {
    int64_t curpos = fp;
    int64_t start = fp;
    
    if (!regexpMode && smem_len == 0) {
        return false;
    }
    
    while (1) {
        int f;
        if (regexpMode) {
            f = hitre(curpos);
        } else {
            f = hit(curpos);
        }
        
        if (f == 1) {
            jump(curpos);
            return true;
        } else if (f < 0) {
            return false;
        }
        
        curpos++;
        if (curpos >= mem_len) {
            if (nff) {
                stdmm("Search reached to bottom, continuing from top.");
            }
            curpos = 0;
            esccolor(0, 0);
        }
        
        if (curpos == start) {
            if (nff) {
                stdmm("Not found.");
            }
            return false;
        }
    }
}

static bool searchlast(int64_t fp) {
    int64_t curpos = fp;
    int64_t start = fp;
    
    if (!regexpMode && smem_len == 0) {
        return false;
    }
    
    while (1) {
        int f;
        if (regexpMode) {
            f = hitre(curpos);
        } else {
            f = hit(curpos);
        }
        
        if (f == 1) {
            jump(curpos);
            return true;
        } else if (f < 0) {
            return false;
        }
        
        curpos--;
        if (curpos < 0) {
            stdmm("Search reached to top, continuing from bottom.");
            esccolor(0, 0);
            curpos = mem_len - 1;
        }
        
        if (curpos == start) {
            stdmm("Not found.");
            return false;
        }
    }
}

// String parsing helpers
static int get_restr(const char *s, int idx, char *out, int *new_idx) {
    int out_idx = 0;
    while (idx < (int)strlen(s)) {
        if (s[idx] == '/') {
            break;
        }
        if (idx + 1 < (int)strlen(s) && s[idx] == '\\' && s[idx + 1] == '\\') {
            out[out_idx++] = '\\';
            out[out_idx++] = '\\';
            idx += 2;
        } else if (idx + 1 < (int)strlen(s) && s[idx] == '\\' && s[idx + 1] == '/') {
            out[out_idx++] = '/';
            idx += 2;
        } else if (s[idx] == '\\' && idx + 1 == (int)strlen(s)) {
            idx++;
            break;
        } else {
            out[out_idx++] = s[idx];
            idx++;
        }
    }
    out[out_idx] = '\0';
    *new_idx = idx;
    return out_idx;
}

static int get_hexs(const char *s, int idx, unsigned char *out, int *new_idx) {
    int out_len = 0;
    while (idx < (int)strlen(s)) {
        int64_t v = expression(s, idx, &idx);
        if (v == UNKNOWN) {
            break;
        }
        out[out_len++] = (unsigned char)(v & 0xff);
    }
    *new_idx = idx;
    return out_len;
}

static void comment(const char *s, char *out) {
    int idx = 0;
    int out_idx = 0;
    
    while (idx < (int)strlen(s)) {
        if (s[idx] == '#') {
            break;
        }
        if (idx + 1 < (int)strlen(s) && s[idx] == '\\' && s[idx + 1] == '#') {
            out[out_idx++] = '#';
            idx += 2;
            continue;
        }
        if (idx + 1 < (int)strlen(s) && s[idx] == '\\' && s[idx + 1] == 'n') {
            out[out_idx++] = '\n';
            idx += 2;
            continue;
        }
        out[out_idx++] = s[idx];
        idx++;
    }
    out[out_idx] = '\0';
}

static bool searchstr(const char *s) {
    if (s[0] != '\0') {
        regexpMode = true;
        strncpy(remem, s, sizeof(remem) - 1);
        remem[sizeof(remem) - 1] = '\0';
        
        if (reObj_compiled) {
            regfree(&reObj);
            reObj_compiled = false;
        }
        
        return searchnext(fpos());
    }
    return false;
}

static bool searchhex(const unsigned char *sm, int64_t sm_len) {
    remem[0] = '\0';
    regexpMode = false;
    if (sm_len > 0) {
        ensure_smem_capacity(sm_len);
        memcpy(smem, sm, sm_len);
        smem_len = sm_len;
        return searchnext(fpos());
    }
    return false;
}

static bool searchsub(const char *line) {
    if (strlen(line) > 2 && line[0] == '/' && line[1] == '/') {
        unsigned char sm[MAX_LINE];
        int idx;
        int sm_len = get_hexs(line, 2, sm, &idx);
        return searchhex(sm, sm_len);
    } else if (strlen(line) > 1 && line[0] == '/') {
        char m[MAX_LINE];
        int idx;
        get_restr(line, 1, m, &idx);
        return searchstr(m);
    }
    return false;
}

static char *getln(const char *prompt, const char *mode) {
    static char line[MAX_LINE];
    
    if (scriptingflag) {
        // In scripting mode, just read from stdin
        if (!fgets(line, sizeof(line), stdin)) {
            line[0] = '\0';
            return line;
        }
    } else {
        printf("%s", prompt);
        fflush(stdout);
        
        if (!fgets(line, sizeof(line), stdin)) {
            line[0] = '\0';
            return line;
        }
    }
    
    // Remove newline
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
        line[len - 1] = '\0';
    }
    if (len > 1 && line[len - 2] == '\r') {
        line[len - 2] = '\0';
    }
    
    return line;
}

static void search(void) {
    disp_curpos();
    esclocate(0, BOTTOMLN);
    esccolor(7, 0);
    printf("/");
    fflush(stdout);
    
    char *s = getln("", "search");
    
    char commented[MAX_LINE];
    comment(s, commented);
    
    // Prepend '/' to make searchsub happy
    char full_search[MAX_LINE];
    snprintf(full_search, sizeof(full_search), "/%s", commented);
    searchsub(full_search);
    
    erase_curpos();
}

// Bitwise operations
static void opeand(int64_t x, int64_t x2, int x3) {
    for (int64_t i = x; i <= x2; i++) {
        setmem(i, readmem(i) & (x3 & 0xff));
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "%lld bytes anded.", (long long)(x2 - x + 1));
    stdmm(msg);
}

static void opeor(int64_t x, int64_t x2, int x3) {
    for (int64_t i = x; i <= x2; i++) {
        setmem(i, readmem(i) | (x3 & 0xff));
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "%lld bytes ored.", (long long)(x2 - x + 1));
    stdmm(msg);
}

static void opexor(int64_t x, int64_t x2, int x3) {
    for (int64_t i = x; i <= x2; i++) {
        setmem(i, readmem(i) ^ (x3 & 0xff));
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "%lld bytes xored.", (long long)(x2 - x + 1));
    stdmm(msg);
}

static void openot(int64_t x, int64_t x2) {
    for (int64_t i = x; i <= x2; i++) {
        setmem(i, (~readmem(i)) & 0xff);
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "%lld bytes noted.", (long long)(x2 - x + 1));
    stdmm(msg);
}

// Shift and rotate operations
static void left_shift_byte(int64_t x, int64_t x2, int c) {
    for (int64_t i = x; i <= x2; i++) {
        setmem(i, (readmem(i) << 1) | (c & 1));
    }
}

static void right_shift_byte(int64_t x, int64_t x2, int c) {
    for (int64_t i = x; i <= x2; i++) {
        setmem(i, (readmem(i) >> 1) | ((c & 1) << 7));
    }
}

static void left_rotate_byte(int64_t x, int64_t x2) {
    for (int64_t i = x; i <= x2; i++) {
        int m = readmem(i);
        int c = (m & 0x80) >> 7;
        setmem(i, (m << 1) | c);
    }
}

static void right_rotate_byte(int64_t x, int64_t x2) {
    for (int64_t i = x; i <= x2; i++) {
        int m = readmem(i);
        int c = (m & 0x01) << 7;
        setmem(i, (m >> 1) | c);
    }
}

static int64_t get_multibyte_value(int64_t x, int64_t x2) {
    int64_t v = 0;
    for (int64_t i = x2; i >= x; i--) {
        v = (v << 8) | (int64_t)readmem(i);
    }
    return v;
}

static void put_multibyte_value(int64_t x, int64_t x2, int64_t v) {
    for (int64_t i = x; i <= x2; i++) {
        setmem(i, (int)(v & 0xff));
        v >>= 8;
    }
}

static void left_shift_multibyte(int64_t x, int64_t x2, int c) {
    int64_t v = get_multibyte_value(x, x2);
    put_multibyte_value(x, x2, (v << 1) | (int64_t)c);
}

static void right_shift_multibyte(int64_t x, int64_t x2, int c) {
    int64_t v = get_multibyte_value(x, x2);
    put_multibyte_value(x, x2, (v >> 1) | ((int64_t)c << ((x2 - x) * 8 + 7)));
}

static void left_rotate_multibyte(int64_t x, int64_t x2) {
    int64_t v = get_multibyte_value(x, x2);
    int c = 0;
    if (v & (1LL << ((x2 - x) * 8 + 7))) {
        c = 1;
    }
    put_multibyte_value(x, x2, (v << 1) | (int64_t)c);
}

static void right_rotate_multibyte(int64_t x, int64_t x2) {
    int64_t v = get_multibyte_value(x, x2);
    int c = 0;
    if (v & 0x1) {
        c = 1;
    }
    put_multibyte_value(x, x2, (v >> 1) | ((int64_t)c << ((x2 - x) * 8 + 7)));
}

static void shift_rotate(int64_t x, int64_t x2, int64_t times, int64_t bit, bool multibyte, char direction) {
    for (int64_t i = 0; i < times; i++) {
        if (!multibyte) {
            if (bit != 0 && bit != 1) {
                if (direction == '<') {
                    left_rotate_byte(x, x2);
                } else {
                    right_rotate_byte(x, x2);
                }
            } else {
                if (direction == '<') {
                    left_shift_byte(x, x2, (int)(bit & 1));
                } else {
                    right_shift_byte(x, x2, (int)(bit & 1));
                }
            }
        } else {
            if (bit != 0 && bit != 1) {
                if (direction == '<') {
                    left_rotate_multibyte(x, x2);
                } else {
                    right_rotate_multibyte(x, x2);
                }
            } else {
                if (direction == '<') {
                    left_shift_multibyte(x, x2, (int)(bit & 1));
                } else {
                    right_shift_multibyte(x, x2, (int)(bit & 1));
                }
            }
        }
    }
}

// Search and replace command
static void scommand(int64_t start, int64_t end, bool xf, bool xf2, const char *line, int idx) {
    nff = false;
    int64_t pos = fpos();
    idx = skipspc(line, idx);
    
    if (!xf && !xf2) {
        start = 0;
        end = mem_len - 1;
    }
    
    if (idx < (int)strlen(line) && line[idx] == '/') {
        idx++;
        if (idx < (int)strlen(line) && line[idx] != '/') {
            char m[MAX_LINE];
            int idx2;
            get_restr(line, idx, m, &idx2);
            idx = idx2;
            regexpMode = true;
            strncpy(remem, m, sizeof(remem) - 1);
            remem[sizeof(remem) - 1] = '\0';
            span = strlen(m);
            
            if (reObj_compiled) {
                regfree(&reObj);
                reObj_compiled = false;
            }
            int ret = regcomp(&reObj, remem, REG_EXTENDED);
            if (ret != 0) {
                stderr_msg("Bad regular expression.");
                return;
            }
            reObj_compiled = true;
        } else if (idx < (int)strlen(line) && line[idx] == '/') {
            unsigned char sm[MAX_LINE];
            int idx2;
            int sm_len = get_hexs(line, idx + 1, sm, &idx2);
            idx = idx2;
            regexpMode = false;
            remem[0] = '\0';
            ensure_smem_capacity(sm_len);
            memcpy(smem, sm, sm_len);
            smem_len = sm_len;
            span = sm_len;
        } else {
            stderr_msg("Invalid syntax.");
            return;
        }
    }
    
    if (span == 0) {
        stderr_msg("Specify search object.");
        return;
    }
    
    // Get replacement
    unsigned char n[MAX_LINE];
    int n_len = 0;
    idx = skipspc(line, idx);
    if (idx < (int)strlen(line) && line[idx] == '/') {
        idx++;
        if (idx < (int)strlen(line) && line[idx] == '/') {
            int idx2;
            n_len = get_hexs(line, idx + 1, n, &idx2);
            idx = idx2;
        } else {
            char s[MAX_LINE];
            int idx2;
            get_restr(line, idx, s, &idx2);
            n_len = strlen(s);
            memcpy(n, s, n_len);
            idx = idx2;
        }
    }
    
    int64_t i = start;
    int cnt = 0;
    jump(i);
    
    while (1) {
        int f = searchnextnoloop(fpos());
        i = fpos();
        if (f < 0) {
            return;
        } else if (i <= end && f == 1) {
            delmem(i, i + span - 1, false);
            insmem(i, n, n_len);
            pos = i + n_len;
            cnt++;
            i = pos;
            jump(i);
        } else {
            jump(pos);
            char msg[128];
            snprintf(msg, sizeof(msg), "  %d times replaced.", cnt);
            stdmm(msg);
            return;
        }
    }
}

// String/hex input parsing
static int get_str_or_hexs(const char *line, int idx, unsigned char *out, int *new_idx) {
    idx = skipspc(line, idx);
    if (idx < (int)strlen(line) && line[idx] == '/') {
        idx++;
        if (idx < (int)strlen(line) && line[idx] == '/') {
            return get_hexs(line, idx + 1, out, new_idx);
        }
        char s[MAX_LINE];
        int idx2;
        get_restr(line, idx, s, &idx2);
        *new_idx = idx2;
        int len = strlen(s);
        memcpy(out, s, len);
        return len;
    }
    *new_idx = idx;
    return 0;
}

static int get_str(const char *line, int idx, unsigned char *out, int *new_idx) {
    char s[MAX_LINE];
    int idx2;
    get_restr(line, idx, s, &idx2);
    *new_idx = idx2;
    int len = strlen(s);
    memcpy(out, s, len);
    return len;
}

// Print value in multiple formats
static void split_every(const char *s, int n, char *out) {
    int s_len = strlen(s);
    int out_idx = 0;
    for (int i = 0; i < s_len; i += n) {
        if (i > 0) out[out_idx++] = ' ';
        for (int j = 0; j < n && i + j < s_len; j++) {
            out[out_idx++] = s[i + j];
        }
    }
    out[out_idx] = '\0';
}

static void printvalue(const char *s) {
    int idx;
    int64_t v = expression(s, 0, &idx);
    if (v == UNKNOWN) {
        return;
    }
    
    char vis[16];
    if (v < 0x20) {
        snprintf(vis, sizeof(vis), "^%c ", (char)(v + '@'));
    } else if (v >= 0x7e) {
        strcpy(vis, " . ");
    } else {
        snprintf(vis, sizeof(vis), "'%c'", (char)v);
    }
    
    char x[128], o[128], b[256];
    snprintf(x, sizeof(x), "%016llX", (unsigned long long)v);
    snprintf(o, sizeof(o), "%024llo", (unsigned long long)v);
    snprintf(b, sizeof(b), "%064s", "");
    
    // Generate binary string
    for (int i = 0; i < 64; i++) {
        b[63 - i] = ((v >> i) & 1) ? '1' : '0';
    }
    b[64] = '\0';
    
    char spacedHex[256], spacedOct[256], spacedBin[512];
    split_every(x, 4, spacedHex);
    split_every(o, 4, spacedOct);
    split_every(b, 4, spacedBin);
    
    char msg[1024];
    snprintf(msg, sizeof(msg), "d%10lld  x%s  o%s %s\nb%s", 
             (long long)v, spacedHex, spacedOct, vis, spacedBin);
    
    if (scriptingflag) {
        if (verbose) {
            printf("%s\n", msg);
        }
    } else {
        clrmm();
        esccolor(6, 0);
        esclocate(0, BOTTOMLN);
        printf("%s", msg);
        fflush(stdout);
        getch_byte();
        esclocate(0, BOTTOMLN + 1);
        for (int i = 0; i < 80; i++) printf(" ");
        fflush(stdout);
    }
}

// File I/O
static bool readfile(const char *fn) {
    FILE *f = fopen(fn, "rb");
    if (!f) {
        newfile = true;
        stdmm("<new file>");
        mem_len = 0;
        return true;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    ensure_mem_capacity(size);
    mem_len = fread(mem, 1, size, f);
    fclose(f);
    newfile = false;
    return true;
}

static void regulate_mem(void) {
    for (int64_t i = 0; i < mem_len; i++) {
        mem[i] = mem[i] & 0xff;
    }
}

static bool writefile(const char *fn) {
    regulate_mem();
    FILE *f = fopen(fn, "wb");
    if (!f) {
        stderr_msg("Permission denied.");
        return false;
    }
    fwrite(mem, 1, mem_len, f);
    fclose(f);
    stdmm("File written.");
    lastchange = false;
    return true;
}

static bool wrtfile(int64_t start, int64_t end, const char *fn) {
    regulate_mem();
    FILE *f = fopen(fn, "wb");
    if (!f) {
        stderr_msg("Permission denied.");
        return false;
    }
    for (int64_t i = start; i <= end; i++) {
        unsigned char byte;
        if (i >= 0 && i < mem_len) {
            byte = mem[i];
        } else {
            byte = 0;
        }
        fwrite(&byte, 1, 1, f);
    }
    fclose(f);
    return true;
}

// Scripting
static int scripting(const char *scriptfile);
static int commandline(const char *line);

static int scripting(const char *scriptfile) {
    FILE *fh = fopen(scriptfile, "r");
    if (!fh) {
        stderr_msg("Script file open error.");
        return -1;
    }
    
    scriptingflag = true;
    int flagv = -1;
    char line[MAX_LINE];
    
    while (fgets(line, sizeof(line), fh)) {
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (len > 1 && line[len - 2] == '\r') line[len - 2] = '\0';
        
        // Trim
        int start = 0;
        while (line[start] == ' ' || line[start] == '\t') start++;
        int end = strlen(line + start) - 1;
        while (end >= 0 && (line[start + end] == ' ' || line[start + end] == '\t')) end--;
        line[start + end + 1] = '\0';
        
        if (verbose) {
            printf("%s\n", line + start);
        }
        
        flagv = commandline(line + start);
        if (flagv == 0) {
            fclose(fh);
            return 0;
        } else if (flagv == 1) {
            fclose(fh);
            return 1;
        }
    }
    
    fclose(fh);
    return 0;
}

// Main command line parser
static int commandline_(const char *line);

static int commandline(const char *line) {
    // Simple error handling
    return commandline_(line);
}

static int commandline_(const char *line) {
    cp = fpos();
    
    char commented[MAX_LINE];
    comment(line, commented);
    line = commented;
    
    if (strlen(line) == 0) {
        return -1;
    }
    
    // Quit commands
    if (strcmp(line, "q") == 0) {
        if (lastchange) {
            stderr_msg("No write since last change. To overriding quit, use 'q!'.");
            return -1;
        }
        return 0;
    } else if (strcmp(line, "q!") == 0) {
        return 0;
    } else if (strcmp(line, "wq") == 0 || strcmp(line, "wq!") == 0) {
        if (writefile(filename)) {
            lastchange = false;
            return 0;
        }
        return -1;
    }
    
    // Write command
    if (line[0] == 'w') {
        if (strlen(line) >= 2 && line[1] != ' ') {
            // Not a write command, continue parsing
        } else {
            if (strlen(line) >= 2) {
                const char *fn = line + 1;
                while (*fn == ' ') fn++;
                writefile(fn);
            } else {
                writefile(filename);
                lastchange = false;
            }
            return -1;
        }
    }
    
    // Read command
    if (line[0] == 'r' && strlen(line) == 1) {
        readfile(filename);
        stdmm("Original file read.");
        return -1;
    }
    
    // Script execution
    if (line[0] == 'T' || line[0] == 't') {
        if (strlen(line) >= 2) {
            const char *script_fn = line + 1;
            while (*script_fn == ' ') script_fn++;
            
            // Save state
            bool old_scripting = scriptingflag;
            bool old_verbose = verbose;
            
            if (line[0] == 'T') {
                verbose = true;
            } else {
                verbose = false;
            }
            
            printf("\n");
            scripting(script_fn);
            
            if (verbose) {
                stdmm("[ Hit any key ]");
                getch_byte();
            }
            
            // Restore state
            verbose = old_verbose;
            scriptingflag = old_scripting;
            escclear();
            return -1;
        } else {
            stderr_msg("Specify script file name.");
            return -1;
        }
    }
    
    // Search next/previous
    if (line[0] == 'n' && strlen(line) == 1) {
        searchnext(fpos() + 1);
        return -1;
    } else if (line[0] == 'N' && strlen(line) == 1) {
        searchlast(fpos() - 1);
        return -1;
    }
    
    // Shell command
    if (line[0] == '!') {
        if (strlen(line) >= 2) {
            invoke_shell(line + 1);
            return -1;
        }
        return -1;
    }
    
    // Print value
    if (line[0] == '?') {
        printvalue(line + 1);
        return -1;
    }
    
    // Search
    if (line[0] == '/') {
        searchsub(line);
        return -1;
    }
    
    // Parse address range
    int idx = skipspc(line, 0);
    int64_t x = expression(line, idx, &idx);
    bool xf = false;
    bool xf2 = false;
    
    if (x == UNKNOWN) {
        x = fpos();
    } else {
        xf = true;
    }
    
    int64_t x2 = x;
    idx = skipspc(line, idx);
    
    if (idx < (int)strlen(line) && line[idx] == ',') {
        idx = skipspc(line, idx + 1);
        if (idx < (int)strlen(line) && line[idx] == '*') {
            idx = skipspc(line, idx + 1);
            int64_t t = expression(line, idx, &idx);
            if (t == UNKNOWN) {
                t = 1;
            }
            x2 = x + t - 1;
        } else {
            int64_t t = expression(line, idx, &idx);
            if (t == UNKNOWN) {
                x2 = x;
            } else {
                x2 = t;
                xf2 = true;
            }
        }
    } else {
        x2 = x;
    }
    
    if (x2 < x) {
        x2 = x;
    }
    
    idx = skipspc(line, idx);
    
    // Just address - jump
    if (idx == (int)strlen(line)) {
        jump(x);
        return -1;
    }
    
    // Yank
    if (idx < (int)strlen(line) && line[idx] == 'y') {
        idx++;
        if (!xf && !xf2) {
            unsigned char m[MAX_LINE];
            int idx2;
            int m_len = get_str_or_hexs(line, idx, m, &idx2);
            ensure_yank_capacity(m_len);
            memcpy(yank, m, m_len);
            yank_len = m_len;
        } else {
            yankmem(x, x2);
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "%lld bytes yanked.", (long long)yank_len);
        stdmm(msg);
        return -1;
    }
    
    // Paste overwrite
    if (idx < (int)strlen(line) && line[idx] == 'p') {
        if (yank && yank_len > 0) {
            ovwmem(x, yank, yank_len);
            jump(x + yank_len);
        }
        return -1;
    }
    
    // Paste insert
    if (idx < (int)strlen(line) && line[idx] == 'P') {
        if (yank && yank_len > 0) {
            insmem(x, yank, yank_len);
            jump(x + yank_len);
        }
        return -1;
    }
    
    // Mark
    if (idx + 1 < (int)strlen(line) && line[idx] == 'm') {
        if (line[idx + 1] >= 'a' && line[idx + 1] <= 'z') {
            mark[line[idx + 1] - 'a'] = x;
        }
        return -1;
    }
    
    // Read file
    if (idx < (int)strlen(line) && (line[idx] == 'r' || line[idx] == 'R')) {
        char ch = line[idx];
        idx++;
        if (idx >= (int)strlen(line)) {
            stderr_msg("File name not specified.");
            return -1;
        }
        
        const char *fn = line + idx;
        while (*fn == ' ') fn++;
        
        if (strlen(fn) == 0) {
            stderr_msg("File name not specified.");
        } else {
            FILE *f = fopen(fn, "rb");
            if (!f) {
                stderr_msg("File read error.");
            } else {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                
                unsigned char *data = malloc(size);
                fread(data, 1, size, f);
                fclose(f);
                
                if (ch == 'r') {
                    ovwmem(x, data, size);
                } else {
                    insmem(x, data, size);
                }
                
                free(data);
                jump(x + size);
                return -1;
            }
        }
    }
    
    // Get command character
    char ch = 0;
    if (idx < (int)strlen(line)) {
        ch = line[idx];
    }
    
    // Delete
    if (ch == 'd') {
        delmem(x, x2, true);
        char msg[128];
        snprintf(msg, sizeof(msg), "%lld bytes deleted.", (long long)(x2 - x + 1));
        stdmm(msg);
        jump(x);
        return -1;
    }
    
    // Write to file
    if (ch == 'w') {
        idx++;
        const char *fn = line + idx;
        while (*fn == ' ') fn++;
        wrtfile(x, x2, fn);
        return -1;
    }
    
    // Search and replace
    if (ch == 's') {
        scommand(x, x2, xf, xf2, line, idx + 1);
        return -1;
    }
    
    // NOT operation
    if (idx < (int)strlen(line) && line[idx] == '~') {
        idx++;
        openot(x, x2);
        jump(x2 + 1);
        return -1;
    }
    
    // Complex operations
    if (idx < (int)strlen(line) && strchr("fIivCc&|^<>", line[idx])) {
        ch = line[idx];
        idx++;
        
        // Shift/rotate
        if (ch == '<' || ch == '>') {
            bool multibyte = false;
            if (idx < (int)strlen(line) && line[idx] == ch) {
                idx++;
                multibyte = true;
            }
            int64_t times = expression(line, idx, &idx);
            if (times == UNKNOWN) {
                times = 1;
            }
            int64_t bit = UNKNOWN;
            if (idx < (int)strlen(line) && line[idx] == ',') {
                bit = expression(line, idx + 1, &idx);
            }
            shift_rotate(x, x2, times, bit, multibyte, ch);
            return -1;
        }
        
        // Insert/overwrite data
        if (ch == 'i') {
            idx = skipspc(line, idx);
            unsigned char m[MAX_LINE];
            int m_len;
            int idx2;
            if (idx < (int)strlen(line) && line[idx] == '/') {
                m_len = get_str(line, idx + 1, m, &idx2);
                idx = idx2;
            } else {
                m_len = get_hexs(line, idx, m, &idx2);
                idx = idx2;
            }
            
            if (xf2) {
                if (m_len > 0) {
                    int total = (int)(x2 - x + 1);
                    int rep = total / m_len;
                    int rem = total % m_len;
                    
                    unsigned char *data = malloc(total);
                    for (int i = 0; i < rep; i++) {
                        memcpy(data + i * m_len, m, m_len);
                    }
                    memcpy(data + rep * m_len, m, rem);
                    
                    ovwmem(x, data, total);
                    free(data);
                    
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%d bytes filled.", total);
                    stdmm(msg);
                    jump(x + total);
                } else {
                    stderr_msg("Invalid syntax.");
                }
                return -1;
            }
            
            int64_t length = 1;
            if (idx < (int)strlen(line) && line[idx] == '*') {
                idx++;
                length = expression(line, idx, &idx);
            }
            
            int data_len = m_len * (int)length;
            unsigned char *data = malloc(data_len);
            for (int i = 0; i < (int)length; i++) {
                memcpy(data + i * m_len, m, m_len);
            }
            
            ovwmem(x, data, data_len);
            free(data);
            
            char msg[128];
            snprintf(msg, sizeof(msg), "%d bytes overwritten.", data_len);
            stdmm(msg);
            jump(x + data_len);
            return -1;
        }
        
        // Insert data
        if (ch == 'I') {
            idx = skipspc(line, idx);
            unsigned char m[MAX_LINE];
            int m_len;
            int idx2;
            if (idx < (int)strlen(line) && line[idx] == '/') {
                m_len = get_str(line, idx + 1, m, &idx2);
                idx = idx2;
            } else {
                m_len = get_hexs(line, idx, m, &idx2);
                idx = idx2;
            }
            
            if (idx < (int)strlen(line) && line[idx] == '*') {
                idx++;
                expression(line, idx, &idx);
            }
            
            if (xf2) {
                stderr_msg("Invalid syntax.");
                return -1;
            }
            
            insmem(x, m, m_len);
            char msg[128];
            snprintf(msg, sizeof(msg), "%d bytes inserted.", m_len);
            stdmm(msg);
            jump(x + m_len);
            return -1;
        }
        
        // Operations requiring third parameter
        int64_t x3 = expression(line, idx, &idx);
        if (x3 == UNKNOWN) {
            stderr_msg("Invalid parameter.");
            return -1;
        }
        
        switch (ch) {
            case 'c':  // Copy
                yankmem(x, x2);
                cpymem(x, x2, x3);
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%lld bytes copied.", (long long)(x2 - x + 1));
                    stdmm(msg);
                }
                jump(x3 + (x2 - x + 1));
                return -1;
                
            case 'C':  // Copy with insert
                {
                    int64_t len;
                    unsigned char *mm = redmem(x, x2, &len);
                    yankmem(x, x2);
                    insmem(x3, mm, len);
                    free(mm);
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%lld bytes inserted.", (long long)(x2 - x + 1));
                    stdmm(msg);
                    jump(x3 + len);
                }
                return -1;
                
            case 'v':  // Move
                {
                    int64_t xp = movmem(x, x2, x3);
                    jump(xp);
                }
                return -1;
                
            case '&':  // AND
                opeand(x, x2, (int)x3);
                jump(x2 + 1);
                return -1;
                
            case '|':  // OR
                opeor(x, x2, (int)x3);
                jump(x2 + 1);
                return -1;
                
            case '^':  // XOR
                opexor(x, x2, (int)x3);
                jump(x2 + 1);
                return -1;
        }
    }
    
    stderr_msg("Unrecognized command.");
    return -1;
}

static int commandln(void) {
    esclocate(0, BOTTOMLN);
    esccolor(7, 0);
    char *line = getln(":", "command");
    
    // Trim
    int start = 0;
    while (line[start] == ' ' || line[start] == '\t') start++;
    int end = strlen(line + start) - 1;
    while (end >= 0 && (line[start + end] == ' ' || line[start + end] == '\t')) end--;
    line[start + end + 1] = '\0';
    
    return commandline(line + start);
}

// Main editor loop
static bool fedit(void) {
    bool stroke = false;
    unsigned char ch;
    repsw = 0;
    
    while (1) {
        cp = fpos();
        repaint();
        printdata();
        esclocate(curx / 2 * 3 + 13 + (curx & 1), cury + 3);
        fflush(stdout);
        
        ch = getch_byte();
        clrmm();
        nff = true;
        
        // Arrow key handling
        if (ch == 0x1b) {
            unsigned char b2 = getch_byte();
            unsigned char b3 = getch_byte();
            if (b3 == 'A') ch = 'k';
            else if (b3 == 'B') ch = 'j';
            else if (b3 == 'C') ch = 'l';
            else if (b3 == 'D') ch = 'h';
            else if (b2 == '[' && b3 == '2') ch = 'i';
        }
        
        // Search next/previous
        if (ch == 'n') {
            searchnext(fpos() + 1);
            continue;
        } else if (ch == 'N') {
            searchlast(fpos() - 1);
            continue;
        }
        
        // Page navigation
        if (ch == 0x02) {  // ctrl-b
            if (homeaddr >= 256) homeaddr -= 256;
            else homeaddr = 0;
            continue;
        } else if (ch == 0x06) {  // ctrl-f
            homeaddr += 256;
            continue;
        } else if (ch == 0x15) {  // ctrl-u
            if (homeaddr >= 128) homeaddr -= 128;
            else homeaddr = 0;
            continue;
        } else if (ch == 0x04) {  // ctrl-d
            homeaddr += 128;
            continue;
        }
        
        // Line navigation
        if (ch == '^') {
            curx = 0;
            continue;
        } else if (ch == '$') {
            curx = 30;
            continue;
        }
        
        // Cursor movement
        if (ch == 'j') {
            if (cury < LENONSCR / 16 - 1) cury++;
            else scrdown();
            continue;
        } else if (ch == 'k') {
            if (cury > 0) cury--;
            else scrup();
            continue;
        } else if (ch == 'h') {
            if (curx > 0) {
                curx--;
            } else {
                if (fpos() != 0) {
                    curx = 31;
                    if (cury > 0) cury--;
                    else scrup();
                }
            }
            continue;
        } else if (ch == 'l') {
            inccurx();
            continue;
        }
        
        // UTF-8 mode toggle
        if (ch == 0x19) {  // ctrl-y
            utf8mode = !utf8mode;
            escclear();
            repaint();
            continue;
        }
        
        // Refresh screen
        if (ch == 0x0c) {  // ctrl-l
            escclear();
            if (utf8mode) {
                repsw = (repsw + 1) % 4;
            }
            repaint();
            continue;
        }
        
        // Write and quit
        if (ch == 'Z') {
            if (writefile(filename)) return true;
            continue;
        }
        
        // Quit
        if (ch == 'q') {
            if (lastchange) {
                stdmm("No write since last change. To overriding quit, use 'q!'.");
                continue;
            }
            return false;
        }
        
        // Display marks
        if (ch == 'M') {
            disp_marks();
            continue;
        }
        
        // Set mark
        if (ch == 'm') {
            unsigned char c = getch_byte();
            c = tolower(c);
            if (c >= 'a' && c <= 'z') {
                mark[c - 'a'] = fpos();
            }
            continue;
        }
        
        // Search
        if (ch == '/') {
            search();
            continue;
        }
        
        // Jump to mark
        if (ch == '\'') {
            unsigned char c = getch_byte();
            c = tolower(c);
            if (c >= 'a' && c <= 'z') {
                jump(mark[c - 'a']);
            }
            continue;
        }
        
        // Paste overwrite
        if (ch == 'p') {
            if (yank && yank_len > 0) {
                ovwmem(fpos(), yank, yank_len);
                jump(fpos() + yank_len);
            }
            continue;
        }
        
        // Paste insert
        if (ch == 'P') {
            if (yank && yank_len > 0) {
                insmem(fpos(), yank, yank_len);
                jump(fpos() + yank_len);
            }
            continue;
        }
        
        // Toggle insert mode
        if (ch == 'i') {
            insmod = !insmod;
            stroke = false;
        }
        // Hex input
        else if (strchr("0123456789abcdefABCDEF", ch)) {
            int64_t addr = fpos();
            int cval;
            if (ch >= '0' && ch <= '9') cval = ch - '0';
            else if (ch >= 'a' && ch <= 'f') cval = ch - 'a' + 10;
            else cval = ch - 'A' + 10;
            
            int sh = (curx & 1) ? 0 : 4;
            int mask = (curx & 1) ? 0xf0 : 0x0f;
            
            if (insmod) {
                if (!stroke && addr < mem_len) {
                    unsigned char byte = cval << sh;
                    insmem(addr, &byte, 1);
                } else {
                    setmem(addr, (readmem(addr) & mask) | (cval << sh));
                }
                if ((curx & 1) == 0) {
                    stroke = !stroke;
                } else {
                    stroke = false;
                }
            } else {
                setmem(addr, (readmem(addr) & mask) | (cval << sh));
            }
            inccurx();
        }
        // Delete byte
        else if (ch == 'x') {
            delmem(fpos(), fpos(), false);
        }
        // Command mode
        else if (ch == ':') {
            disp_curpos();
            int f = commandln();
            erase_curpos();
            if (f == 1) return true;
            else if (f == 0) return false;
        }
    }
}

// Main function
int main(int argc, char *argv[]) {
    // Initialize marks
    for (int i = 0; i < 26; i++) {
        mark[i] = UNKNOWN;
    }
    
    // Parse command-line arguments
    char *script_file = NULL;
    bool write_flag = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            script_file = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            strncpy(termcol, argv[++i], sizeof(termcol) - 1);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-w") == 0) {
            write_flag = true;
        } else if (argv[i][0] != '-') {
            strncpy(filename, argv[i], sizeof(filename) - 1);
        }
    }
    
    if (strlen(filename) == 0) {
        fprintf(stderr, "Usage: bi [options] file\n");
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  -s <script>  Execute script file\n");
        fprintf(stderr, "  -t <color>   Terminal background color (black/white)\n");
        fprintf(stderr, "  -v           Verbose mode\n");
        fprintf(stderr, "  -w           Write file when exiting script\n");
        return 1;
    }
    
    if (!script_file) {
        escclear();
    } else {
        scriptingflag = true;
    }
    
    if (!readfile(filename)) {
        return 1;
    }
    
    // Error handling for crashes
    if (script_file) {
        scripting(script_file);
        if (write_flag && lastchange) {
            writefile(filename);
        }
    } else {
        fedit();
    }
    
    esccolor(7, 0);
    escdispcursor();
    esclocate(0, 23);
    printf("\n");
    
    // Cleanup
    if (mem) free(mem);
    if (yank) free(yank);
    if (smem) free(smem);
    if (reObj_compiled) regfree(&reObj);
    
    return 0;
}
