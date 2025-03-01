#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <windows.h>
#include <conio.h>
#include <io.h>
#include <fcntl.h>
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#define CTRL_KEY(k) ((k)&0x1f)
#define ITE_VERSION "0.0.1"
#define ITE_TAB_STOP 8
#define ITE_QUIT_TIMES 3
enum editorKey { 
    BACKSPACE = 127, 
    ARROW_LEFT = 1000, 
    ARROW_RIGHT, 
    ARROW_UP, 
    ARROW_DOWN, 
    DEL_KEY, 
    HOME_KEY, 
    END_KEY, 
    PAGE_UP, 
    PAGE_DOWN, 
    F2_KEY = 1010
};
typedef struct erow {
    int index, size, rendered_size;
    char *characters;
    char *rendered_characters;
} erow;
struct editorConfig {
    int file_position_x, file_position_y, screen_position_x;
    int row_offset, column_offset, screen_rows, screen_columns;
    int number_of_rows, dirty;
    erow *row;
    char *filename, status_message[80];
    time_t status_message_time;
} E;
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
void editorQuit();
char *editorPrompt(char *prompt, void (*callback)(char *, int));
static DWORD orig_mode_in = 0, orig_mode_out = 0;
void die(const char *s) {
    const char *clear = "\x1b[2J\x1b[H";
    _write(STDOUT_FILENO, clear, (unsigned int)strlen(clear));
    perror(s);
    exit(1);
}
void disableRawMode() {
    _write(STDOUT_FILENO, "\x1b[?1049l", 8);
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), orig_mode_in);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), orig_mode_out);
}
void enableRawMode() {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE) die("GetStdHandle");
    if (!GetConsoleMode(hStdin, &orig_mode_in)) die("GetConsoleMode");
    if (!SetConsoleMode(hStdin, orig_mode_in & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT))) die("SetConsoleMode");
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdout == INVALID_HANDLE_VALUE) die("GetStdHandle");
    if (!GetConsoleMode(hStdout, &orig_mode_out)) die("GetConsoleMode");
    if (!SetConsoleMode(hStdout, orig_mode_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) die("SetConsoleMode");
    atexit(disableRawMode);
    _write(STDOUT_FILENO, "\x1b[?1049h", 8);
}
int editorReadKey() {
    int c = _getch();
    if (c == 0 || c == 224) {
        c = _getch();
        switch (c) {
            case 72: return ARROW_UP;
            case 80: return ARROW_DOWN;
            case 75: return ARROW_LEFT;
            case 77: return ARROW_RIGHT;
            case 83: return DEL_KEY;
            case 60: return F2_KEY;
            default: return c;
        }
    }
    return c;
}
int getWindowSize(int *rows, int *cols) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleScreenBufferInfo(hStdout, &csbi)) return -1;
    *cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return 0;
}
int editorRowFilePositionXToScreenPositionX(erow *row, int file_x) {
    int screen_x = 0;
    for (int j = 0; j < file_x; j++) {
        if (row->characters[j] == '\t')
            screen_x += (ITE_TAB_STOP - 1) - (screen_x % ITE_TAB_STOP);
        screen_x++;
    }
    return screen_x;
}
int editorRowScreenPositionXToFilePositionX(erow *row, int screen_x) {
    int cur = 0, file_x;
    for (file_x = 0; file_x < row->size; file_x++) {
        if (row->characters[file_x] == '\t')
            cur += (ITE_TAB_STOP - 1) - (cur % ITE_TAB_STOP);
        cur++;
        if (cur > screen_x) return file_x;
    }
    return file_x;
}
void editorUpdateRow(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->characters[j] == '\t')
            tabs++;
    free(row->rendered_characters);
    size_t new_size = row->size + tabs * (ITE_TAB_STOP - 1) + 1;
    row->rendered_characters = malloc(new_size);
    if (!row->rendered_characters) {
        die("Memory allocation failure in editorUpdateRow");
    }
    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->characters[j] == '\t') {
            row->rendered_characters[idx++] = ' ';
            while (idx % ITE_TAB_STOP)
                row->rendered_characters[idx++] = ' ';
        } else {
            row->rendered_characters[idx++] = row->characters[j];
        }
    }
    row->rendered_characters[idx] = '\0';
    row->rendered_size = idx;
}
void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.number_of_rows) return;
    erow *new_rows = realloc(E.row, sizeof(erow) * (E.number_of_rows + 1));
    if (!new_rows) {
        die("Memory allocation failure in editorInsertRow");
    }
    E.row = new_rows;
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.number_of_rows - at));
    for (int j = at + 1; j <= E.number_of_rows; j++)
        E.row[j].index++;
    E.row[at].index = at;
    E.row[at].size = (int)len;
    E.row[at].characters = malloc(len + 1);
    if (!E.row[at].characters) {
        die("Memory allocation failure in editorInsertRow for characters");
    }
    memcpy(E.row[at].characters, s, len);
    E.row[at].characters[len] = '\0';
    E.row[at].rendered_size = 0;
    E.row[at].rendered_characters = NULL;
    editorUpdateRow(&E.row[at]);
    E.number_of_rows++;
    E.dirty++;
}
void editorFreeRow(erow *row) {
    free(row->rendered_characters);
    free(row->characters);
}
void editorDelRow(int at) {
    if (at < 0 || at >= E.number_of_rows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.number_of_rows - at - 1));
    for (int j = at; j < E.number_of_rows - 1; j++)
        E.row[j].index--;
    E.number_of_rows--;
    E.dirty++;
}
void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    char *new_chars = realloc(row->characters, row->size + 2);
    if (!new_chars) {
        die("Memory allocation failure in editorRowInsertChar");
    }
    row->characters = new_chars;
    memmove(&row->characters[at + 1], &row->characters[at], row->size - at + 1);
    row->size++;
    row->characters[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}
void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->characters[at], &row->characters[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}
void editorInsertChar(int c) {
    if (E.file_position_y == E.number_of_rows) {
        for (int i = 0; i <= E.file_position_y; i++) {
            if (i >= E.number_of_rows) {
                editorInsertRow(i, "", 0);
            }
        }
    }
    editorRowInsertChar(&E.row[E.file_position_y], E.file_position_x, c);
    E.file_position_x++;
}
void editorRowAppendString(erow *row, char *s, size_t len) {
    char *new_chars = realloc(row->characters, row->size + len + 1);
    if (!new_chars) {
        die("Memory allocation failure in editorRowAppendString");
    }
    row->characters = new_chars;
    memcpy(&row->characters[row->size], s, len);
    row->size += (int)len;
    row->characters[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}
void editorInsertNewline() {
    if (E.file_position_x == 0) {
        editorInsertRow(E.file_position_y, "", 0);
    } else {
        erow *row = &E.row[E.file_position_y];
        editorInsertRow(E.file_position_y + 1, &row->characters[E.file_position_x], row->size - E.file_position_x);
        row = &E.row[E.file_position_y];
        row->size = E.file_position_x;
        row->characters[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.file_position_y++;
    E.file_position_x = 0;
}
void editorDelChar() {
    if (E.file_position_y == E.number_of_rows) return;
    if (E.file_position_x == 0 && E.file_position_y == 0) return;
    erow *row = &E.row[E.file_position_y];
    if (E.file_position_x > 0) {
        editorRowDelChar(row, E.file_position_x - 1);
        E.file_position_x--;
    } else {
        E.file_position_x = E.row[E.file_position_y - 1].size;
        editorRowAppendString(&E.row[E.file_position_y - 1], row->characters, row->size);
        editorDelRow(E.file_position_y);
        E.file_position_y--;
    }
}
#if !defined(_SSIZE_T_DEFINED)
typedef long ssize_t;
#define _SSIZE_T_DEFINED
#endif
ssize_t win_getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) return -1;
    size_t pos = 0;
    int c;
    if (*lineptr == NULL) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t new_size = *n * 2;
            char *temp = realloc(*lineptr, new_size);
            if (!temp) {
                free(*lineptr);
                *lineptr = NULL;
                return -1;
            }
            *lineptr = temp;
            *n = new_size;
        }
        (*lineptr)[pos++] = c;
        if (c == '\n') break;
    }
    if (pos == 0) return -1;
    (*lineptr)[pos] = '\0';
    return pos;
}
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int j = 0; j < E.number_of_rows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen;
    char *buf = malloc(totlen);
    if (!buf) {
        die("Memory allocation failure in editorRowsToString");
    }
    char *p = buf;
    for (int j = 0; j < E.number_of_rows; j++) {
        memcpy(p, E.row[j].characters, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}
void editorOpen(char *filename) {
    HANDLE hFile = CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) { die("CreateFile"); }
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) { CloseHandle(hFile); die("GetFileSizeEx"); }
    if (fileSize.QuadPart == 0) {
        free(E.filename);
        E.filename = strdup(filename);
        E.dirty = 0;
        CloseHandle(hFile);
        return;
    }
    HANDLE hMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMapping) { CloseHandle(hFile); die("CreateFileMapping"); }
    char *fileData = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!fileData) { CloseHandle(hMapping); CloseHandle(hFile); die("MapViewOfFile"); }
    typedef struct { const char *line_start; size_t line_length; } line_info;
    size_t line_count = 0, line_capacity = 1024;
    line_info *lines = malloc(line_capacity * sizeof(line_info));
    if (!lines) { UnmapViewOfFile(fileData); CloseHandle(hMapping); CloseHandle(hFile); die("Memory allocation failure for line info"); }
    const char *p = fileData, *end = fileData + fileSize.QuadPart;
    while (p < end) {
        const char *newline = memchr(p, '\n', end - p);
        size_t len;
        if (newline) { len = newline - p; if (len && p[len - 1] == '\r') len--; }
        else { len = end - p; if (len && p[len - 1] == '\r') len--; }
        if (line_count >= line_capacity) {
            line_capacity *= 2;
            line_info *tmp = realloc(lines, line_capacity * sizeof(line_info));
            if (!tmp) { free(lines); UnmapViewOfFile(fileData); CloseHandle(hMapping); CloseHandle(hFile); die("Memory allocation failure while expanding line info"); }
            lines = tmp;
        }
        lines[line_count].line_start = p;
        lines[line_count].line_length = len;
        line_count++;
        p = newline ? newline + 1 : end;
    }
    E.number_of_rows = (int)line_count;
    E.row = malloc(sizeof(erow) * E.number_of_rows);
    if (!E.row) { free(lines); UnmapViewOfFile(fileData); CloseHandle(hMapping); CloseHandle(hFile); die("Memory allocation failure for editor rows"); }
    free(E.filename);
    E.filename = strdup(filename);
    if (!E.filename) { free(lines); UnmapViewOfFile(fileData); CloseHandle(hMapping); CloseHandle(hFile); die("Memory allocation failure for filename"); }
    for (size_t i = 0; i < line_count; i++) {
        E.row[i].index = (int)i;
        E.row[i].size = (int)lines[i].line_length;
        E.row[i].characters = malloc(lines[i].line_length + 1);
        if (!E.row[i].characters) { die("Memory allocation failure for row characters"); }
        memcpy(E.row[i].characters, lines[i].line_start, lines[i].line_length);
        E.row[i].characters[lines[i].line_length] = '\0';
        E.row[i].rendered_characters = NULL;
        E.row[i].rendered_size = 0;
        editorUpdateRow(&E.row[i]);
    }
    free(lines);
    UnmapViewOfFile(fileData);
    CloseHandle(hMapping);
    CloseHandle(hFile);
    E.dirty = 0;
}
int editorConfirm(const char *prompt, char default_yes) {
    editorSetStatusMessage("%s", prompt);
    editorRefreshScreen();
    int c = editorReadKey();
    editorSetStatusMessage("");
    return default_yes ? (c == '\r' || tolower(c) == 'y') : (tolower(c) == 'y');
}
int editorSave() {
    if (!E.filename && !(E.filename = editorPrompt("File: %s", NULL))) {
        editorSetStatusMessage("Save aborted");
        return 0;
    }
    if (GetFileAttributes(E.filename) != INVALID_FILE_ATTRIBUTES && !editorConfirm("File already exists. Overwrite? (y/N)", 0)) {
        editorSetStatusMessage("Save cancelled");
        return 0;
    }
    HANDLE hFile = CreateFile(E.filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        editorSetStatusMessage("Save error: %lu", GetLastError());
        return 0;
    }
    size_t chunkSize = 64 * 1024, bufferOffset = 0;
    char *buffer = malloc(chunkSize);
    if (!buffer) {
        CloseHandle(hFile);
        editorSetStatusMessage("Save error: Memory allocation failure");
        return 0;
    }
    DWORD bytesWritten;
    for (int j = 0; j < E.number_of_rows; j++) {
        if (bufferOffset + E.row[j].size + 1 >= chunkSize) {
            if (!WriteFile(hFile, buffer, bufferOffset, &bytesWritten, NULL) || bytesWritten != bufferOffset) {
                CloseHandle(hFile);
                free(buffer);
                editorSetStatusMessage("Save error: %lu", GetLastError());
                return 0;
            }
            bufferOffset = 0;
        }
        memcpy(buffer + bufferOffset, E.row[j].characters, E.row[j].size);
        bufferOffset += E.row[j].size;
        buffer[bufferOffset++] = '\n';
    }
    if (bufferOffset && (!WriteFile(hFile, buffer, bufferOffset, &bytesWritten, NULL) || bytesWritten != bufferOffset)) {
        CloseHandle(hFile);
        free(buffer);
        editorSetStatusMessage("Save error: %lu", GetLastError());
        return 0;
    }
    free(buffer);
    CloseHandle(hFile);
    E.dirty = 0;
    editorSetStatusMessage("%d lines written", E.number_of_rows);
    return 1;
}
void editorFindCallback(char *query, int key) {
    static int last_match = -1, direction = 1;
    if (query[0] == '\0')
        return;    
    if (key == '\r' || key == '\x1b') { 
        last_match = -1; 
        direction = 1; 
        return; 
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }
    if (last_match == -1)
        direction = 1;
    int current = last_match;
    for (int i = 0; i < E.number_of_rows; i++) {
        current += direction;
        if (current < 0)
            current = E.number_of_rows - 1;
        else if (current >= E.number_of_rows)
            current = 0;
        erow *row = &E.row[current];
        char *match = strstr(row->rendered_characters, query);
        if (match) {
            last_match = current;
            E.file_position_y = current;
            E.file_position_x = editorRowScreenPositionXToFilePositionX(row, (int)(match - row->rendered_characters));
            E.row_offset = current;
            break;
        }
    }
}
void editorFind() {
    int saved_x = E.file_position_x, saved_y = E.file_position_y, saved_col = E.column_offset, saved_row = E.row_offset;
    char *query = editorPrompt("Search: %s (ESC/Arrows/Enter)", editorFindCallback);
    if (query) free(query);
    else {
        E.file_position_x = saved_x;
        E.file_position_y = saved_y;
        E.column_offset = saved_col;
        E.row_offset = saved_row;
    }
}
struct abuf {
    char *b;
    int len;
    int capacity;
};
#define ABUF_INIT { NULL, 0, 0 }
void abAppend(struct abuf *ab, const char *s, int len) {
    if (ab->len + len > ab->capacity) {
        int new_capacity = (ab->capacity > 0) ? ab->capacity : 64;
        while (new_capacity < ab->len + len)
            new_capacity *= 2;
        char *new_buf = realloc(ab->b, new_capacity);
        if (!new_buf)
            die("Memory allocation failure in abAppend");
        ab->b = new_buf;
        ab->capacity = new_capacity;
    }
    memcpy(&ab->b[ab->len], s, len);
    ab->len += len;
}
void abFree(struct abuf *ab) { free(ab->b); }
void editorScroll() {
    E.screen_position_x = 0;
    if (E.file_position_y < E.number_of_rows)
        E.screen_position_x = editorRowFilePositionXToScreenPositionX(&E.row[E.file_position_y], E.file_position_x);
    if (E.file_position_y < E.row_offset) E.row_offset = E.file_position_y;
    if (E.file_position_y >= E.row_offset + E.screen_rows) E.row_offset = E.file_position_y - E.screen_rows + 1;
    if (E.screen_position_x < E.column_offset) E.column_offset = E.screen_position_x;
    if (E.screen_position_x >= E.column_offset + E.screen_columns) E.column_offset = E.screen_position_x - E.screen_columns + 1;
}
void editorDrawRows(struct abuf *ab) {
    int digits = 1;
    int max_lines = (E.number_of_rows > 0 ? E.number_of_rows : 1);
    while (max_lines >= 10) { max_lines /= 10; digits++; }
    int ln_width = digits + 3;
    int content_width = E.screen_columns - ln_width;
    for (int y = 0; y < E.screen_rows; y++) {
        int filerow = y + E.row_offset;
        char buf[32];
        if (filerow < E.number_of_rows) {
            snprintf(buf, sizeof(buf), "\x1b[38;5;244m%*d\x1b[39m | ", digits, filerow + 1);
            abAppend(ab, buf, (int)strlen(buf));
            int len = E.row[filerow].rendered_size - E.column_offset;
            if (len < 0) len = 0;
            if (len > content_width) len = content_width;
            char *c = &E.row[filerow].rendered_characters[E.column_offset];
            abAppend(ab, c, len);
        } else {
            snprintf(buf, sizeof(buf), "\x1b[38;5;244m%*s\x1b[39m   ", digits, "~");
            abAppend(ab, buf, (int)strlen(buf));
            if (E.number_of_rows == 0 && y == E.screen_rows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Improved Terminal Editor v%s", ITE_VERSION);
                if (welcomelen > content_width) welcomelen = content_width;
                int padding = (content_width - welcomelen) / 2;
                for (int i = 0; i < padding; i++) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
        }
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}
void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char left[100], status[200];
    char *fname = E.filename ? E.filename : "No name";
    int cur_line = (E.file_position_y < E.number_of_rows ? E.file_position_y + 1 : E.number_of_rows);
    int cur_col = E.file_position_x + 1;
    int len_left = snprintf(left, sizeof(left), "%.30s%s (%d,%d)", fname, E.dirty ? " +" : "", cur_line, cur_col);
    int filler = E.screen_columns - len_left;
    if (filler < 0)
        filler = 0;
    int n = snprintf(status, sizeof(status), "%s%*s", left, filler, "");
    if (n > E.screen_columns)
        n = E.screen_columns;
    abAppend(ab, status, n);
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}
void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = (int)strlen(E.status_message);
    if (msglen > E.screen_columns) msglen = E.screen_columns;
    if (msglen && time(NULL) - E.status_message_time < 5)
        abAppend(ab, E.status_message, msglen);
}
void editorRefreshScreen() {
    editorScroll();
    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);
    if (E.number_of_rows > 0) {
        int digits = 1;
        int max_lines = E.number_of_rows;
        while (max_lines >= 10) { max_lines /= 10; digits++; }
        int ln_width = digits + 3;
        char buf[32];
        int cursor_y = E.file_position_y - E.row_offset;
        int cursor_x = ln_width + (E.screen_position_x - E.column_offset);
        if (E.file_position_y >= E.number_of_rows) {
            cursor_y = E.number_of_rows - E.row_offset;
            cursor_x = ln_width;
        }
        if (cursor_y >= E.screen_rows) cursor_y = E.screen_rows - 1;
        if (cursor_y < 0) cursor_y = 0;
        if (cursor_x < ln_width) cursor_x = ln_width;
        snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cursor_y + 1, cursor_x + 1);
        abAppend(&ab, buf, (int)strlen(buf));
        abAppend(&ab, "\x1b[?25h", 6);
    }
    _write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status_message, sizeof(E.status_message), fmt, ap);
    va_end(ap);
    E.status_message_time = time(NULL);
}
#define PROMPT_MAX_LENGTH 4096
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128, buflen = 0;
    char *buf = malloc(bufsize);
    if (!buf) return NULL;
    buf[0] = '\0';
    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();
        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl((unsigned char)c) && c < 128) {
            if (buflen < PROMPT_MAX_LENGTH - 1) {
                if (buflen == bufsize - 1) {
                    bufsize *= 2;
                    char *temp = realloc(buf, bufsize);
                    if (!temp) {
                        free(buf);
                        die("Memory allocation failure in editorPrompt");
                    }
                    buf = temp;
                }
                buf[buflen++] = c;
                buf[buflen] = '\0';
            } else {
                editorSetStatusMessage("Input limit reached");
            }
        }
        if (callback) callback(buf, c);
    }
}
void editorMoveCursor(int key) {
    erow *row = (E.file_position_y >= E.number_of_rows) ? NULL : &E.row[E.file_position_y];
    switch (key) {
        case ARROW_LEFT:
            if (E.file_position_x)
                E.file_position_x--;
            else if (E.file_position_y > 0) {
                E.file_position_y--;
                E.file_position_x = E.row[E.file_position_y].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.file_position_x < row->size)
                E.file_position_x++;
            else if (row && E.file_position_x == row->size) {
                if (E.file_position_y + 1 > E.number_of_rows) {
                    editorInsertRow(E.number_of_rows, "", 0);
                }
                E.file_position_y++;
                E.file_position_x = 0;
            }
            break;
        case ARROW_UP:
            if (E.file_position_y) E.file_position_y--;
            break;
        case ARROW_DOWN:
            if (E.file_position_y < E.number_of_rows - 1) {
                E.file_position_y++;
            } else if (E.file_position_y == E.number_of_rows - 1) {
                editorInsertRow(E.number_of_rows, "", 0);
                E.file_position_y++;
            }
            break;
    }
    row = (E.file_position_y >= E.number_of_rows) ? NULL : &E.row[E.file_position_y];
    int rowlen = row ? row->size : 0;
    if (E.file_position_x > rowlen) E.file_position_x = rowlen;
}
void editorDelCharAtCursor() {
    if (E.file_position_y >= E.number_of_rows) return;
    erow *row = &E.row[E.file_position_y];
    if (E.file_position_x < row->size) {
        editorRowDelChar(row, E.file_position_x);
    } else if (E.file_position_x == row->size && E.file_position_y < E.number_of_rows - 1) {
        editorRowAppendString(row, E.row[E.file_position_y + 1].characters, E.row[E.file_position_y + 1].size);
        editorDelRow(E.file_position_y + 1);
    }
    E.dirty++;
}
void editorQuit() {
    if (!E.dirty || (E.filename == NULL && !E.number_of_rows)) {
        exit(0);
    }
    if (E.filename == NULL) {
        for (int i = 0; i < E.number_of_rows; i++) {
            if (E.row[i].size) {
                goto ask;
            }
        }
        exit(0);
    }
ask:
    if (editorConfirm("Save changes? (Y/n)", 1)) {
        if (editorSave()) exit(0);
    } else {
        exit(0);
    }
}
void editorProcessKeypress() {
    int c = editorReadKey();
    switch (c) {
        case '\r':
            if (E.file_position_y >= E.number_of_rows) {
                if (E.number_of_rows == 0) {
                    editorInsertRow(0, "", 0);
                    E.file_position_y = 0;
                    E.file_position_x = 0;
                } else {
                    for (int i = E.number_of_rows; i <= E.file_position_y; i++) {
                        editorInsertRow(i, "", 0);
                    }
                    editorInsertNewline();
                }
            } else {
                editorInsertNewline();
            }
            break;
        case CTRL_KEY('q'):
        case '\x1b':
            editorQuit();
            break;
        case CTRL_KEY('s'):
        case F2_KEY:
            editorSave();
            break;
        case HOME_KEY:
            E.file_position_x = 0;
            break;
        case END_KEY:
            if (E.file_position_y < E.number_of_rows)
                E.file_position_x = E.row[E.file_position_y].size;
            break;
        case CTRL_KEY('f'):
            editorFind();
            break;
        case BACKSPACE: case CTRL_KEY('h'):
            editorDelChar();
            break;
        case DEL_KEY:
            editorDelCharAtCursor();
            break;
        case PAGE_UP: case PAGE_DOWN:
            E.file_position_y = (c == PAGE_UP) ? E.row_offset : E.row_offset + E.screen_rows - 1;
            if (E.file_position_y > E.number_of_rows)
                E.file_position_y = E.number_of_rows;
            for (int i = E.screen_rows; i--; )
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            break;
        case ARROW_UP: case ARROW_DOWN: case ARROW_LEFT: case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        case CTRL_KEY('l'):
            break;
        default:
            if (E.file_position_y >= E.number_of_rows) {
                for (int i = E.number_of_rows; i <= E.file_position_y; i++) {
                    editorInsertRow(i, "", 0);
                }
            }
            editorInsertChar(c);
            break;
    }
}
void initEditor() {
    E.file_position_x = E.file_position_y = E.screen_position_x = E.row_offset = E.column_offset = 0;
    E.number_of_rows = E.dirty = 0;
    E.row = NULL;
    E.filename = NULL;
    E.status_message[0] = '\0';
    E.status_message_time = 0;
    if (getWindowSize(&E.screen_rows, &E.screen_columns) == -1) die("getWindowSize");
    E.screen_rows = (E.screen_rows > 2) ? (E.screen_rows - 2) : 0;
}
int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) editorOpen(argv[1]);
    editorSetStatusMessage("Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}