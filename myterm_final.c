#define _XOPEN_SOURCE 700
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <glob.h>
#include <dirent.h>

#ifndef _U_TYPES_DEFINED
#define _U_TYPES_DEFINED
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
#endif

#include <pthread.h>
#include <sys/types.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <util.h>
#include <sys/select.h>
#include <sys/syslimits.h>
#include <time.h>
#include <ctype.h>
#define HISTORY_FILE ".myterm_history"
#define MAX_HISTORY 10000

#define WIN_W 1000
#define WIN_H 700
#define TAB_HEIGHT 28
#define TAB_WIDTH 140
#define MAX_TABS 12
#define MAX_LINES 20000
#define INPUT_MAX 8192
#define MAX_JOBS 64
volatile sig_atomic_t multiwatch_active = 1;
pid_t fg_pid = -1;
volatile sig_atomic_t ui_needs_redraw = 0;
char pending_signal_msg[256] = "";
volatile sig_atomic_t signal_msg_ready = 0;
extern int active; // ensure global scope

typedef struct
{
    pid_t pid;
    int master_fd; // fd to read job output (pipe or pty)
    int active;
    char cmd[256];
} Job;

typedef struct
{
    char *lines[MAX_LINES];
    int line_count;
} TextBuffer;

typedef struct
{
    TextBuffer tb;
    char input[INPUT_MAX];
    int input_len;
    char title[64];
    char cwd[PATH_MAX];
    Job jobs[MAX_JOBS];
    int job_count;
    int scroll_offset;
    int multiline_mode;
    char *history[MAX_HISTORY];
    int hist_count;
    int hist_index;
    int cursor_pos; // For Ctrl+A / Ctrl+E navigation
    int search_mode; /* 0 = off, 1 = on */
    char search_buf[256];
    int search_len;

} Tab;

typedef struct
{
    Tab *tab;
    char cmds[8][256];
    int ncmds;
} MultiWatchArgs;

Tab tabs[MAX_TABS];
int tab_count = 0, active = -1;
extern Tab tabs[MAX_TABS]; // your global tab array

// ===== Utility =====
static void tb_init(TextBuffer *tb) { tb->line_count = 0; }

static void tb_append(TextBuffer *tb, const char *s)
{
    if (!s)
        return;
    const char *p = s;
    while (*p)
    {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        char *line = malloc(len + 1);
        if (!line)
            return;
        memcpy(line, p, len);
        line[len] = '\0';
        if (tb->line_count >= MAX_LINES)
        {
            free(tb->lines[0]);
            memmove(&tb->lines[0], &tb->lines[1], sizeof(char *) * (MAX_LINES - 1));
            tb->line_count--;
        }
        tb->lines[tb->line_count++] = line;
        // Automatically keep view scrolled to bottom unless user manually scrolled
        if (active >= 0)
        {
            Tab *t = &tabs[active];
            // Auto-scroll only if user was at bottom before
            if (t->scroll_offset == 0)
                ui_needs_redraw = 1;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
}

static void tb_free(TextBuffer *tb)
{
    for (int i = 0; i < tb->line_count; i++)
        free(tb->lines[i]);
    tb->line_count = 0;
}
// ===== Persistent Command History =====
static void load_history(Tab *t)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", getenv("HOME"), HISTORY_FILE);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    char line[INPUT_MAX];
    while (fgets(line, sizeof(line), fp))
    {
        line[strcspn(line, "\n")] = 0; // remove newline
        if (strlen(line) == 0)
            continue;
        if (t->hist_count < MAX_HISTORY)
            t->history[t->hist_count++] = strdup(line);
    }
    fclose(fp);
    tb_append(&t->tb, "Command history loaded from ~/.myterm_history");
}

static void save_history(Tab *t)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", getenv("HOME"), HISTORY_FILE);
    FILE *fp = fopen(path, "w");
    if (!fp)
        return;

    for (int i = 0; i < t->hist_count; i++)
        fprintf(fp, "%s\n", t->history[i]);
    fclose(fp);
}
static void search_history(Tab *t)
{
    tb_append(&t->tb, "Enter search term: ");
    ui_needs_redraw = 1;

    char term[INPUT_MAX];
    int len = 0;
    int c;

    // read character by character from stdin
    while ((c = getchar()) != '\n' && c != EOF && len < INPUT_MAX - 1)
        term[len++] = c;
    term[len] = '\0';

    if (len == 0)
    {
        tb_append(&t->tb, "No term entered.");
        return;
    }

    // --- 1️⃣ Exact match search ---
    int exact_index = -1;
    for (int i = t->hist_count - 1; i >= 0; i--)
    {
        if (strcmp(t->history[i], term) == 0)
        {
            exact_index = i;
            break;
        }
    }

    if (exact_index >= 0)
    {
        char msg[INPUT_MAX + 64];
        snprintf(msg, sizeof(msg),
                 "Exact match found: %s", t->history[exact_index]);
        tb_append(&t->tb, msg);
        return;
    }

    // --- Substring match (longest) ---
    int best_len = 0;
    char *matches[20];
    int mcount = 0;

    for (int i = 0; i < t->hist_count; i++)
    {
        const char *h = t->history[i];
        for (int j = 0; j < len; j++)
        {
            for (int k = len; k > j; k--)
            {
                int sublen = k - j;
                if (sublen <= best_len || sublen <= 2)
                    continue;
                char sub[INPUT_MAX];
                strncpy(sub, term + j, sublen);
                sub[sublen] = '\0';
                if (strstr(h, sub))
                {
                    if (sublen > best_len)
                    {
                        best_len = sublen;
                        mcount = 0;
                        matches[mcount++] = (char *)h;
                    }
                    else if (sublen == best_len && mcount < 20)
                        matches[mcount++] = (char *)h;
                    break;
                }
            }
        }
    }

    if (best_len > 2 && mcount > 0)
    {
        tb_append(&t->tb, "Closest matches:");
        for (int i = 0; i < mcount; i++)
            tb_append(&t->tb, matches[i]);
    }
    else
    {
        tb_append(&t->tb, "No match for search term in history.");
    }
}
/* return index in history for exact match (most recent), or -1 */
static int history_exact_match(Tab *t, const char *term)
{
    if (!term || term[0] == '\0')
        return -1;
    for (int i = t->hist_count - 1; i >= 0; --i)
    {
        if (strcmp(t->history[i], term) == 0)
            return i;
    }
    return -1;
}

/* return index of one history entry that contains the longest substring
   of 'term' (substring length must be >2), or -1 if none */
static int history_longest_substring(Tab *t, const char *term)
{
    if (!term)
        return -1;
    int tlen = strlen(term);
    if (tlen < 3)
        return -1;
    int best_len = 0;
    int best_idx = -1;
    char sub[256];

    for (int i = 0; i < t->hist_count; ++i)
    {
        const char *h = t->history[i];
        for (int len = tlen; len >= 3; --len)
        { // attempt longer substrings first
            for (int start = 0; start + len <= tlen; ++start)
            {
                if (len >= (int)sizeof(sub))
                    continue;
                memcpy(sub, term + start, len);
                sub[len] = '\0';
                if (strstr(h, sub))
                {
                    if (len > best_len)
                    {
                        best_len = len;
                        best_idx = i;
                    }
                    goto next_history_entry; // found a substring for this history line
                }
            }
        }
    next_history_entry:;
    }
    return best_idx;
}

static void set_nonblock(int fd)
{
    int f = fcntl(fd, F_GETFL, 0);
    if (f >= 0)
        fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

// ===== Job Handling =====
static void add_job(Tab *t, pid_t pid, int master_fd, const char *cmd)
{
    if (t->job_count >= MAX_JOBS)
        return;
    t->jobs[t->job_count].pid = pid;
    t->jobs[t->job_count].master_fd = master_fd;
    t->jobs[t->job_count].active = 1;
    strncpy(t->jobs[t->job_count].cmd, cmd, sizeof(t->jobs[t->job_count].cmd) - 1);
    t->jobs[t->job_count].cmd[sizeof(t->jobs[t->job_count].cmd) - 1] = '\0';
    if (master_fd >= 0)
        set_nonblock(master_fd);
    t->job_count++;
}
// === Signal handlers for Ctrl+C (SIGINT) and Ctrl+Z (SIGTSTP) ===
void handle_sigint(int sig)
{
    if (fg_pid > 0)
    {
        kill(fg_pid, SIGINT);
        snprintf(pending_signal_msg, sizeof(pending_signal_msg),
                 "[MyTerm] Foreground process (%d) interrupted", fg_pid);
    }
    else
    {
        snprintf(pending_signal_msg, sizeof(pending_signal_msg),
                 "[MyTerm] No foreground job to interrupt");
    }
    signal_msg_ready = 1;
}

void handle_sigtstp(int sig)
{
    if (fg_pid > 0)
    {
        kill(fg_pid, SIGTSTP);
        snprintf(pending_signal_msg, sizeof(pending_signal_msg),
                 "[MyTerm] Foreground process (%d) stopped (backgrounded)", fg_pid);
        if (active >= 0)
            add_job(&tabs[active], fg_pid, -1, "Suspended job");
        fg_pid = -1;
    }
    else
    {
        snprintf(pending_signal_msg, sizeof(pending_signal_msg),
                 "[MyTerm] No foreground job to stop");
    }
    signal_msg_ready = 1;
}

// check_jobs: non-blocking reads from job fds and read pids with WNOHANG
static void check_jobs(Tab *t)
{
    for (int i = 0; i < t->job_count; i++)
    {
        if (!t->jobs[i].active)
            continue;

        // Read any available output from job master fd
        if (t->jobs[i].master_fd >= 0)
        {
            char buf[4096];
            ssize_t r;
            while ((r = read(t->jobs[i].master_fd, buf, sizeof(buf) - 1)) > 0)
            {
                buf[r] = '\0';
                tb_append(&t->tb, buf);
            }
            if (r == 0)
            {
                // EOF on job output - close fd (but still wait for process reap)
                close(t->jobs[i].master_fd);
                t->jobs[i].master_fd = -1;
            }
            else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                // Unexpected read error
                close(t->jobs[i].master_fd);
                t->jobs[i].master_fd = -1;
            }
        }

        // Reap job if it finished
        int st = 0;
        pid_t done = waitpid(t->jobs[i].pid, &st, WNOHANG);
        if (done > 0)
        {
            // job finished
            t->jobs[i].active = 0;
            if (t->jobs[i].master_fd >= 0)
            {
                close(t->jobs[i].master_fd);
                t->jobs[i].master_fd = -1;
            }
            char msg[256];
            if (WIFEXITED(st))
            {
                snprintf(msg, sizeof(msg), "[%d] Done (exit %d)  %s", t->jobs[i].pid, WEXITSTATUS(st), t->jobs[i].cmd);
            }
            else if (WIFSIGNALED(st))
            {
                snprintf(msg, sizeof(msg), "[%d] Terminated by signal %d  %s", t->jobs[i].pid, WTERMSIG(st), t->jobs[i].cmd);
            }
            else
            {
                snprintf(msg, sizeof(msg), "[%d] Done  %s", t->jobs[i].pid, t->jobs[i].cmd);
            }
            tb_append(&t->tb, msg);
        }
    }
}
// === Auto-complete helper ===
static void autocomplete(Tab *t)
{
    if (t->input_len == 0)
        return;

    char *start = strrchr(t->input, ' ');
    const char *prefix = start ? start + 1 : t->input;

    if (strlen(prefix) == 0)
        return;

    DIR *d = opendir(t->cwd);
    if (!d)
        return;

    char *matches[256];
    int match_count = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && match_count < 256)
    {
        if (strncmp(de->d_name, prefix, strlen(prefix)) == 0)
        {
            matches[match_count++] = strdup(de->d_name);
        }
    }
    closedir(d);

    if (match_count == 0)
        return;

    // === Case 1: Single match ===
    if (match_count == 1)
    {
        char completion[PATH_MAX];
        strcpy(completion, t->input);
        if (start)
            strcpy(start + 1, matches[0]);
        else
            strcpy(t->input, matches[0]);
        t->input_len = strlen(t->input);
        t->cursor_pos = t->input_len;

        char msg[256];
        snprintf(msg, sizeof(msg), "Auto-completed: %s", matches[0]);
        tb_append(&t->tb, msg);
        ui_needs_redraw = 1;
    }
    else
    {
        // === Case 2: Multiple matches — find longest common prefix ===
        int prefix_len = strlen(matches[0]);
        for (int i = 1; i < match_count; i++)
        {
            int j = 0;
            while (j < prefix_len && matches[0][j] == matches[i][j])
                j++;
            prefix_len = j;
        }

        if (prefix_len > strlen(prefix))
        {
            // Extend the typed text to longest common prefix
            char common[PATH_MAX];
            strncpy(common, matches[0], prefix_len);
            common[prefix_len] = '\0';
            if (start)
            {
                strcpy(start + 1, common);
            }
            else
            {
                strcpy(t->input, common);
            }
            t->input_len = strlen(t->input);
            t->cursor_pos = t->input_len;
            tb_append(&t->tb, "Partial auto-complete (multiple matches)");
        }
        else
        {
            // === Case 3: Still multiple choices — list them ===
            tb_append(&t->tb, "Multiple matches:");
            for (int i = 0; i < match_count; i++)
            {
                char msg[256];
                snprintf(msg, sizeof(msg), "%d. %s", i + 1, matches[i]);
                tb_append(&t->tb, msg);
            }
            tb_append(&t->tb, "Enter number to select file:");
            ui_needs_redraw = 1;
            // You can later extend this to let the user type a number
        }
    }

    for (int i = 0; i < match_count; i++)
        free(matches[i]);
}

// ===== MultiWatch Thread =====
void *multiwatch_thread(void *arg)
{
    MultiWatchArgs *mw = (MultiWatchArgs *)arg;
    Tab *t = mw->tab;
    char buf[4096];
    tb_append(&t->tb, "multiWatch started (refresh every 2s)...");
    ui_needs_redraw = 1;

    while (multiwatch_active)
    {
        for (int i = 0; i < mw->ncmds; i++)
        {
            int pipefd[2];
            if (pipe(pipefd) < 0)
                continue;

            pid_t pid = fork();
            if (pid == 0)
            {
                // --- CHILD ---
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[0]);
                close(pipefd[1]);
                execlp("sh", "sh", "-c", mw->cmds[i], NULL);
                _exit(127);
            }
            else if (pid > 0)
            {
                // --- PARENT ---
                close(pipefd[1]);
                ssize_t r;
                // 🔁 Keep reading until EOF (so no output is missed)
                while ((r = read(pipefd[0], buf, sizeof(buf) - 1)) > 0)
                {
                    buf[r] = '\0';
                    char timebuf[64];
                    time_t now = time(NULL);
                    strftime(timebuf, sizeof(timebuf), "[%H:%M:%S]", localtime(&now));

                    char label[INPUT_MAX];
                    snprintf(label, sizeof(label),
                             "%s --- %s ---\n%s",
                             timebuf, mw->cmds[i], buf);
                    tb_append(&t->tb, label);
                    ui_needs_redraw = 1;
                }
                close(pipefd[0]);
                waitpid(pid, NULL, 0);
            }
        }

        tb_append(&t->tb, "------ refresh complete ------");
        ui_needs_redraw = 1;
        sleep(2);
    }

    tb_append(&t->tb, "multiWatch stopped.");
    ui_needs_redraw = 1;
    free(mw);
    return NULL;
}

// ===== Tabs =====
static int create_tab(Tab *tabs, int *tab_count, int *active)
{
    if (*tab_count >= MAX_TABS)
        return -1;
    Tab *t = &tabs[*tab_count];
    tb_init(&t->tb);
    t->input_len = 0;
    t->input[0] = '\0';
    t->job_count = 0;
    t->scroll_offset = 0;
    t->multiline_mode = 0;
    t->hist_count = 0;
    t->hist_index = -1;
    t->cursor_pos = 0;
    t->search_mode = 0;
    t->search_buf[0] = '\0';
    t->search_len = 0;
    getcwd(t->cwd, sizeof(t->cwd));
    snprintf(t->title, sizeof(t->title), "tab %d", *tab_count + 1);
    tb_append(&t->tb, "New tab created.");
    load_history(t);
    (*tab_count)++;
    if (*active == -1)
        *active = 0;
    return *tab_count - 1;
}

static void close_tab(Tab *tabs, int *tab_count, int *active, int idx)
{
    if (idx < 0 || idx >= *tab_count)
        return;
    for (int j = 0; j < tabs[idx].job_count; ++j)
        if (tabs[idx].jobs[j].active)
        {
            kill(tabs[idx].jobs[j].pid, SIGKILL);
            if (tabs[idx].jobs[j].master_fd >= 0)
                close(tabs[idx].jobs[j].master_fd);
        }
    tb_free(&tabs[idx].tb);
    for (int k = idx; k < *tab_count - 1; ++k)
        tabs[k] = tabs[k + 1];
    (*tab_count)--;
    if (*tab_count == 0)
        *active = -1;
    else if (*active >= *tab_count)
        *active = *tab_count - 1;
}

// ===== Drawing (multiline typing fixed) =====
static void draw_ui(Display *dpy, Window win, GC gc, Tab *tabs, int tab_count, int active)
{
    XWindowAttributes wa;
    XGetWindowAttributes(dpy, win, &wa);
    XClearWindow(dpy, win);

    // TAB BAR
    for (int i = 0; i < tab_count; i++)
    {
        int x = i * TAB_WIDTH;
        if (i == active)
        {
            XFillRectangle(dpy, win, gc, x + 2, 2, TAB_WIDTH - 6, TAB_HEIGHT - 6);
            XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
            XDrawString(dpy, win, gc, x + 8, 18, tabs[i].title, strlen(tabs[i].title));
            XDrawString(dpy, win, gc, x + TAB_WIDTH - 18, 16, "x", 1);
            XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
        }
        else
        {
            XDrawRectangle(dpy, win, gc, x + 2, 2, TAB_WIDTH - 6, TAB_HEIGHT - 6);
            XDrawString(dpy, win, gc, x + 8, 18, tabs[i].title, strlen(tabs[i].title));
            XDrawString(dpy, win, gc, x + TAB_WIDTH - 18, 16, "x", 1);
        }
    }

    // "+" Button
    int plus_x = tab_count * TAB_WIDTH + 8;
    XDrawRectangle(dpy, win, gc, plus_x, 4, 32, TAB_HEIGHT - 8);
    XDrawString(dpy, win, gc, plus_x + 10, 18, "+", 1);

    if (active >= 0 && active < tab_count)
    {
        Tab *t = &tabs[active];
        int font_h = 16, margin = 8;

        // Output area
        int y = TAB_HEIGHT + margin + font_h;
        int visible = (wa.height - TAB_HEIGHT - margin * 3) / font_h;

        // Ensure scroll_offset never exceeds content height
        if (t->scroll_offset > t->tb.line_count - visible)
            t->scroll_offset = t->tb.line_count - visible;
        if (t->scroll_offset < 0)
            t->scroll_offset = 0;

        // Auto-scroll: if at bottom (scroll_offset == 0), always follow new output
        int start = t->tb.line_count - visible - t->scroll_offset;
        if (start < 0)
            start = 0;
        int end = t->tb.line_count - t->scroll_offset;
        if (end < start)
            end = start;

        if (end < start)
            end = start;
        for (int i = start; i < end && y < wa.height - 3 * font_h; i++, y += font_h)
            XDrawString(dpy, win, gc, margin, y, t->tb.lines[i], strlen(t->tb.lines[i]));

        int base_y = wa.height - margin - font_h;
        int cur_y = base_y;

        // === Search mode UI (Ctrl+R active) ===
        if (t->search_mode)
        {
            char search_prompt[INPUT_MAX + 64];
            snprintf(search_prompt, sizeof(search_prompt), "Search: %s", t->search_buf);
            XDrawString(dpy, win, gc, margin, cur_y, search_prompt, strlen(search_prompt));

            // show preview of current best match dynamically
            int best_idx = -1;
            int best_len = 0;
            char term[256];
            strncpy(term, t->search_buf, sizeof(term) - 1);
            term[sizeof(term) - 1] = '\0';
            int len = strlen(term);

            if (len > 0)
            {
                for (int i = t->hist_count - 1; i >= 0; i--)
                {
                    if (strstr(t->history[i], term))
                    {
                        best_idx = i;
                        best_len = len;
                        break;
                    }
                }
            }

            if (best_idx >= 0)
            {
                char preview[INPUT_MAX + 64];
                snprintf(preview, sizeof(preview), "Match: %s", t->history[best_idx]);
                XDrawString(dpy, win, gc, margin, cur_y + font_h, preview, strlen(preview));
            }
            else if (len > 0)
            {
                XDrawString(dpy, win, gc, margin, cur_y + font_h, "No match found.", 15);
            }

            // Draw hint line
            XDrawString(dpy, win, gc, margin, cur_y + 3 * font_h,
                        "Press Enter to select, ESC to cancel", 36);
            return; // only draw search UI, skip normal input UI
        }

        // === Normal input UI (non-search) ===
        char prompt[PATH_MAX + 64];
        snprintf(prompt, sizeof(prompt), "%s%s> ",
                 t->multiline_mode ? "(multi) " : "", t->cwd);
        int prompt_width = strlen(prompt) * 8;
        XDrawString(dpy, win, gc, margin, cur_y, prompt, strlen(prompt));
        int line_x = margin + prompt_width;

        int line_start = 0;
        for (int i = 0; i < t->input_len; i++)
        {
            if (t->input[i] == '\n')
            {
                char temp = t->input[i];
                t->input[i] = '\0';
                XDrawString(dpy, win, gc, line_x, cur_y, &t->input[line_start],
                            strlen(&t->input[line_start]));
                t->input[i] = temp;
                line_start = i + 1;
                cur_y += font_h;
                line_x = margin + 20;
            }
        }

        if (line_start < t->input_len)
            XDrawString(dpy, win, gc, line_x, cur_y,
                        &t->input[line_start], strlen(&t->input[line_start]));

        if (t->multiline_mode)
            XDrawString(dpy, win, gc, margin + 20, cur_y + font_h,
                        "↳ multiline input active", 25);

        // --- Draw cursor position ---
        int cursor_x = margin + prompt_width + (t->cursor_pos * 8);
        int cursor_y = cur_y;
        XDrawLine(dpy, win, gc, cursor_x, cursor_y - 12, cursor_x, cursor_y + 3);
    }
}

// ===== Command execution (with async background fix) =====

#include <glob.h>

static void run_command(Tab *t)
{
    t->input[t->input_len] = '\0';
    if (t->input_len == 0)
        return;

    tb_append(&t->tb, t->input);

    // ---- Command History ----
    if (t->hist_count < MAX_HISTORY)
        t->history[t->hist_count++] = strdup(t->input);
    else
    {
        free(t->history[0]);
        memmove(&t->history[0], &t->history[1], sizeof(char *) * (MAX_HISTORY - 1));
        t->history[MAX_HISTORY - 1] = strdup(t->input);
    }
    save_history(t);
    t->hist_index = -1;
    t->scroll_offset = 0;

    char cmdline[INPUT_MAX];
    strncpy(cmdline, t->input, sizeof(cmdline) - 1);
    cmdline[sizeof(cmdline) - 1] = '\0';

    // ---- Background Detection ----
    int background = 0;
    char *amp = strrchr(cmdline, '&');
    if (amp && (amp == cmdline || *(amp - 1) == ' '))
    {
        background = 1;
        *amp = '\0';
    }

    // ---- Built-in: cd ----
    if (strncmp(cmdline, "cd", 2) == 0 && (cmdline[2] == ' ' || cmdline[2] == '\0'))
    {
        char *path = cmdline + 2;
        while (*path == ' ')
            path++;
        if (*path == '\0')
            path = getenv("HOME");
        if (!path)
            path = "/";
        if (*path == '~')
        {
            const char *home = getenv("HOME");
            if (home)
            {
                static char expanded[PATH_MAX];
                snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
                path = expanded;
            }
        }
        if (chdir(path) == 0)
        {
            getcwd(t->cwd, sizeof(t->cwd));
            char msg[PATH_MAX + 32];
            snprintf(msg, sizeof(msg), "Changed directory to: %s", t->cwd);
            tb_append(&t->tb, msg);
        }
        else
        {
            char msg[PATH_MAX + 64];
            snprintf(msg, sizeof(msg), "cd: No such file or directory: %s", path);
            tb_append(&t->tb, msg);
        }
        return;
    }

    // ---- Built-ins: jobs / kill / fg ----
    if (strncmp(cmdline, "history", 7) == 0)
    {
        int start = (t->hist_count > 1000) ? t->hist_count - 1000 : 0;
        for (int i = start; i < t->hist_count; i++)
        {
            char line[INPUT_MAX + 32];
            snprintf(line, sizeof(line), "%4d  %s", i + 1, t->history[i]);
            tb_append(&t->tb, line);
        }
        return;
    }

    if (strncmp(cmdline, "jobs", 4) == 0)
    {
        for (int i = 0; i < t->job_count; i++)
            if (t->jobs[i].active)
            {
                char line[256];
                snprintf(line, sizeof(line), "[%d] Running  %s",
                         t->jobs[i].pid, t->jobs[i].cmd);
                tb_append(&t->tb, line);
            }
        return;
    }
    if (strncmp(cmdline, "kill", 4) == 0)
    {
        pid_t pid = atoi(cmdline + 5);
        if (pid > 0 && kill(pid, SIGKILL) == 0)
            tb_append(&t->tb, "Process killed.");
        else
            tb_append(&t->tb, "Usage: kill <pid>");
        return;
    }
    if (strncmp(cmdline, "fg", 2) == 0)
    {
        pid_t pid = atoi(cmdline + 3);
        if (pid > 0)
        {
            tb_append(&t->tb, "Bringing job to foreground...");
            int st;
            waitpid(pid, &st, 0);
            tb_append(&t->tb, "Foreground job finished.");
            for (int i = 0; i < t->job_count; i++)
                if (t->jobs[i].pid == pid)
                    t->jobs[i].active = 0;
        }
        else
            tb_append(&t->tb, "Usage: fg <pid>");
        return;
    }

    // ---- Built-in: multiWatch ----
    if (strncmp(cmdline, "multiWatch", 10) == 0)
    {
        const char *start = strchr(cmdline, '[');
        const char *end = strrchr(cmdline, ']');
        if (!start || !end || end <= start + 1)
        {
            tb_append(&t->tb, "Usage: multiWatch [\"cmd1\", \"cmd2\", ...]");
            ui_needs_redraw = 1;
            return;
        }

        char listbuf[INPUT_MAX];
        strncpy(listbuf, start + 1, end - start - 1);
        listbuf[end - start - 1] = '\0';

        MultiWatchArgs *mw = malloc(sizeof(MultiWatchArgs));
        mw->tab = t;
        mw->ncmds = 0;

        char *saveptr;
        char *tok = strtok_r(listbuf, ",", &saveptr);
        while (tok && mw->ncmds < 8)
        {
            while (*tok == ' ' || *tok == '"' || *tok == '\'')
                tok++;
            char *endq = tok + strlen(tok) - 1;
            while (endq > tok && (*endq == '"' || *endq == '\'' || *endq == ' '))
                *endq-- = '\0';
            strncpy(mw->cmds[mw->ncmds], tok,
                    sizeof(mw->cmds[mw->ncmds]) - 1);
            mw->ncmds++;
            tok = strtok_r(NULL, ",", &saveptr);
        }
        if (mw->ncmds == 0)
        {
            tb_append(&t->tb, "multiWatch: no valid commands.");
            ui_needs_redraw = 1;
            free(mw);
            return;
        }

        multiwatch_active = 1;
        pthread_t tid;
        pthread_create(&tid, NULL, multiwatch_thread, mw);
        pthread_detach(tid);
        tb_append(&t->tb, "multiWatch running (use 'multiWatch-stop' to end).");
        ui_needs_redraw = 1;
        return;
    }

    if (strncmp(cmdline, "multiWatch-stop", 15) == 0)
    {
        if (!multiwatch_active)
        {
            tb_append(&t->tb, "No active multiWatch session.");
        }
        else
        {
            multiwatch_active = 0;
            tb_append(&t->tb, "Stopping multiWatch threads...");
        }
        ui_needs_redraw = 1;
        return;
    }

    // ---- Normal Commands (Pipes, Redirection, Background) ----
    char *commands[16];
    int ncmds = 0;
    char *saveptr;
    char *seg = strtok_r(cmdline, "|", &saveptr);
    while (seg && ncmds < 16)
    {
        while (*seg == ' ')
            seg++;
        commands[ncmds++] = seg;
        seg = strtok_r(NULL, "|", &saveptr);
    }
    if (ncmds == 0)
        return;

    int pipes[15][2];
    for (int i = 0; i < ncmds - 1; i++)
        if (pipe(pipes[i]) == -1)
        {
            tb_append(&t->tb, "pipe() failed");
            return;
        }

    pid_t pids[16];
    int capture_pipe[2];
    pipe(capture_pipe);

    for (int i = 0; i < ncmds; i++)
    {
        char *argv[128];
        int argc = 0;
        char *infile = NULL, *outfile = NULL;
        int append_mode = 0;

        char *dup_allocs[128];
        int dup_count = 0;

        char *tok = strtok(commands[i], " ");
        while (tok && argc < 127)
        {
            if (strcmp(tok, "<") == 0)
            {
                tok = strtok(NULL, " ");
                infile = tok;
            }
            else if (strcmp(tok, ">") == 0)
            {
                tok = strtok(NULL, " ");
                outfile = tok;
                append_mode = 0;
            }
            else if (strcmp(tok, ">>") == 0)
            {
                tok = strtok(NULL, " ");
                outfile = tok;
                append_mode = 1;
            }
            else
            {
                if (strpbrk(tok, "*?[]~"))
                {
                    glob_t g;
                    int flags = GLOB_TILDE | GLOB_NOCHECK;
                    if (glob(tok, flags, NULL, &g) == 0)
                    {
                        for (size_t gi = 0; gi < g.gl_pathc && argc < 127; gi++)
                        {
                            argv[argc] = strdup(g.gl_pathv[gi]);
                            dup_allocs[dup_count++] = argv[argc];
                            argc++;
                        }
                        globfree(&g);
                    }
                    else
                    {
                        argv[argc] = strdup(tok);
                        dup_allocs[dup_count++] = argv[argc];
                        argc++;
                    }
                }
                else
                {
                    argv[argc] = strdup(tok);
                    dup_allocs[dup_count++] = argv[argc];
                    argc++;
                }
            }
            tok = strtok(NULL, " ");
        }
        argv[argc] = NULL;

        pid_t pid = fork();
        if (pid == 0)
        {
            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < ncmds - 1)
                dup2(pipes[i][1], STDOUT_FILENO);
            else
                dup2(capture_pipe[1], STDOUT_FILENO);

            dup2(STDOUT_FILENO, STDERR_FILENO);

            if (infile)
            {
                int fd = open(infile, O_RDONLY);
                if (fd < 0)
                    _exit(1);
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (outfile)
            {
                int fd = open(outfile,
                              O_WRONLY | O_CREAT | (append_mode ? O_APPEND : O_TRUNC),
                              0644);
                if (fd < 0)
                    _exit(1);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }

            for (int j = 0; j < ncmds - 1; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(capture_pipe[0]);
            close(capture_pipe[1]);
            execvp(argv[0], argv);
            _exit(127);
        }
        else if (pid > 0)
        {
            pids[i] = pid;
            for (int u = 0; u < dup_count; u++)
                free(dup_allocs[u]);
            if (i > 0)
                close(pipes[i - 1][0]);
            if (i < ncmds - 1)
                close(pipes[i][1]);
        }
        else
        {
            tb_append(&t->tb, "fork failed");
            for (int u = 0; u < dup_count; u++)
                free(dup_allocs[u]);
            ui_needs_redraw = 1;
            return;
        }
    }

    close(capture_pipe[1]);
    fg_pid = pids[ncmds - 1];

    if (background)
    {
        pid_t last_pid = pids[ncmds - 1];
        add_job(t, last_pid, capture_pipe[0], t->input);
        char msg[256];
        snprintf(msg, sizeof(msg), "[%d] running in background", last_pid);
        tb_append(&t->tb, msg);
    }
    else
    {
        char buf[4096];
        set_nonblock(capture_pipe[0]);
        int status;
        for (int i = 0; i < ncmds; i++)
            waitpid(pids[i], &status, 0);

        while (1)
        {
            ssize_t r = read(capture_pipe[0], buf, sizeof(buf) - 1);
            if (r > 0)
            {
                buf[r] = '\0';
                tb_append(&t->tb, buf);
            }
            else
                break;
        }
        close(capture_pipe[0]);
        tb_append(&t->tb, "Command finished.");
        t->scroll_offset = 0; // ✅ auto-scroll to bottom
        ui_needs_redraw = 1;  // ✅ force UI update
        fg_pid = -1;
    }
}

// ===== Main =====
int main()
{
    setlocale(LC_CTYPE, "");
    // --- Register signal handlers (Part 9) ---
    signal(SIGINT, handle_sigint);
    signal(SIGTSTP, handle_sigtstp);
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
    {
        fprintf(stderr, "Start XQuartz first.\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Window win = XCreateSimpleWindow(dpy, root, 40, 40, WIN_W, WIN_H, 1,
                                     BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    XMapWindow(dpy, win);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));
    XStoreName(dpy, win, "MyTerm - Async Background Jobs");

    Tab tabs[MAX_TABS];
    int tab_count = 0, active = -1;
    create_tab(tabs, &tab_count, &active);

    while (1)
    {
        // Poll background jobs (reads their output into buffers)
        if (active >= 0)
            check_jobs(&tabs[active]);
        // You may want to check jobs in all tabs; do it below:
        for (int ti = 0; ti < tab_count; ++ti)
            check_jobs(&tabs[ti]);

        draw_ui(dpy, win, gc, tabs, tab_count, active);

        while (XPending(dpy))
        {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose)
            {
                draw_ui(dpy, win, gc, tabs, tab_count, active);
            }
            else if (ev.type == ButtonPress)
            {
                int bx = ev.xbutton.x, by = ev.xbutton.y;

                // === Scroll wheel handling ===
                if (active >= 0 && by > TAB_HEIGHT)
                {
                    Tab *t = &tabs[active];
                    if (ev.xbutton.button == Button4)
                    { // scroll up
                        t->scroll_offset += 3;
                        if (t->scroll_offset > t->tb.line_count - 1)
                            t->scroll_offset = t->tb.line_count - 1;
                    }
                    else if (ev.xbutton.button == Button5)
                    { // scroll down
                        t->scroll_offset -= 3;
                        if (t->scroll_offset < 0)
                            t->scroll_offset = 0;
                    }
                    draw_ui(dpy, win, gc, tabs, tab_count, active);
                    continue;
                }

                // === Tab click handling ===
                if (by <= TAB_HEIGHT)
                {
                    int clicked = bx / TAB_WIDTH;
                    if (clicked < tab_count)
                    {
                        int close_x = (clicked + 1) * TAB_WIDTH - 18;
                        if (bx >= close_x - 5 && bx <= close_x + 10)
                            close_tab(tabs, &tab_count, &active, clicked);
                        else
                            active = clicked;
                    }
                    else
                    {
                        int plus_x = tab_count * TAB_WIDTH + 8;
                        if (bx >= plus_x && bx <= plus_x + 32)
                            create_tab(tabs, &tab_count, &active);
                    }
                }
            }
            else if (ev.type == KeyPress && active >= 0)
            {
                Tab *t = &tabs[active];
                KeySym ks;
                char buf[256];
                int len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, NULL);

                // --- Ctrl+C and Ctrl+Z handling ---
                if ((ev.xkey.state & ControlMask) && (ks == XK_c || ks == XK_C))
                {
                    handle_sigint(SIGINT);
                    continue;
                }
                if ((ev.xkey.state & ControlMask) && (ks == XK_z || ks == XK_Z))
                {
                    handle_sigtstp(SIGTSTP);
                    continue;
                }

                // === Scroll with keyboard ===
                if (ks == XK_Up)
                {
                    if (t->hist_count > 0)
                    {
                        if (t->hist_index == -1)
                            t->hist_index = t->hist_count - 1;
                        else if (t->hist_index > 0)
                            t->hist_index--;
                        strncpy(t->input, t->history[t->hist_index], INPUT_MAX - 1);
                        t->input_len = strlen(t->input);
                    }
                    continue;
                }
                else if (ks == XK_Down)
                {
                    if (t->hist_index >= 0)
                    {
                        t->hist_index++;
                        if (t->hist_index >= t->hist_count)
                        {
                            t->hist_index = -1;
                            t->input[0] = '\0';
                            t->input_len = 0;
                        }
                        else
                        {
                            strncpy(t->input, t->history[t->hist_index], INPUT_MAX - 1);
                            t->input_len = strlen(t->input);
                        }
                    }
                    continue;
                }
                else if (ks == XK_Page_Up)
                {
                    t->scroll_offset += 10;
                    if (t->scroll_offset > t->tb.line_count - 1)
                        t->scroll_offset = t->tb.line_count - 1;
                    draw_ui(dpy, win, gc, tabs, tab_count, active);
                    continue;
                }
                else if (ks == XK_Page_Down)
                {
                    t->scroll_offset -= 10;
                    if (t->scroll_offset < 0)
                        t->scroll_offset = 0;
                    draw_ui(dpy, win, gc, tabs, tab_count, active);
                    continue;
                }

                // === Normal typing and editing ===
                if (len > 0)
                {
                    buf[len] = '\0';
                    unsigned char c = buf[0];

                    // --- Ctrl+A: move to start of line ---
                    if (c == 1)
                    {
                        t->cursor_pos = 0;
                        continue;
                    }

                    // --- Ctrl+E: move to end of line ---
                    if (c == 5)
                    {
                        t->cursor_pos = t->input_len;
                        continue;
                    }

                    // --- Ctrl+R: Activate Non-blocking Search Mode ---
                    if (c == 18) // Ctrl + R
                    {
                        if (!t->search_mode)
                        {
                            t->search_mode = 1;
                            t->search_len = 0;
                            t->search_buf[0] = '\0';
                            tb_append(&t->tb, "[Search mode enabled]");
                            ui_needs_redraw = 1;
                        }
                        continue;
                    }

                    // --- Handle search mode (non-blocking) ---
                    if (t->search_mode)
                    {
                        // ESC → cancel search
                        if (c == 27)
                        {
                            t->search_mode = 0;
                            t->search_len = 0;
                            t->search_buf[0] = '\0';
                            tb_append(&t->tb, "[Search cancelled]");
                            ui_needs_redraw = 1;
                            continue;
                        }

                        // Backspace → edit search term
                        if (c == 127 || ks == XK_BackSpace)
                        {
                            if (t->search_len > 0)
                            {
                                t->search_buf[--t->search_len] = '\0';
                                ui_needs_redraw = 1;
                            }
                            continue;
                        }

                        // Enter → search in history
                        if (c == '\r' || c == '\n')
                        {
                            if (t->search_len == 0)
                            {
                                tb_append(&t->tb, "No term entered.");
                                t->search_mode = 0;
                                ui_needs_redraw = 1;
                                continue;
                            }

                            int exact_idx = -1, partial_idx = -1, best_len = 0;
                            for (int i = t->hist_count - 1; i >= 0; i--)
                            {
                                if (strcmp(t->history[i], t->search_buf) == 0)
                                {
                                    exact_idx = i;
                                    break;
                                }
                                if (strstr(t->history[i], t->search_buf))
                                {
                                    int len = strlen(t->search_buf);
                                    if (len > best_len)
                                    {
                                        best_len = len;
                                        partial_idx = i;
                                    }
                                }
                            }

                            if (exact_idx >= 0)
                            {
                                char msg[INPUT_MAX + 64];
                                snprintf(msg, sizeof(msg), "Exact match: %s", t->history[exact_idx]);
                                tb_append(&t->tb, msg);
                            }
                            else if (partial_idx >= 0)
                            {
                                char msg[INPUT_MAX + 64];
                                snprintf(msg, sizeof(msg), "Closest match: %s", t->history[partial_idx]);
                                tb_append(&t->tb, msg);
                            }
                            else
                            {
                                tb_append(&t->tb, "No match found in history.");
                            }

                            t->search_mode = 0;
                            t->search_len = 0;
                            t->search_buf[0] = '\0';
                            ui_needs_redraw = 1;
                            continue;
                        }

                        // Append printable char to search term
                        if (isprint(c) && t->search_len + 1 < (int)sizeof(t->search_buf) - 1)
                        {
                            t->search_buf[t->search_len++] = c;
                            t->search_buf[t->search_len] = '\0';
                            ui_needs_redraw = 1;
                        }
                        continue;
                    }

                    // --- Enter pressed ---
                    if (c == '\r' || c == '\n')
                    {
                        if (t->input_len > 0 && t->input[t->input_len - 1] == '\\')
                        {
                            t->input_len--;
                            t->input[t->input_len++] = '\n';
                            t->input[t->input_len] = '\0';
                            t->multiline_mode = 1;
                        }
                        else
                        {
                            run_command(t);
                            t->input_len = 0;
                            t->input[0] = '\0';
                            t->cursor_pos = 0;
                            t->multiline_mode = 0;
                        }
                        continue;
                    }

                    // --- Backspace ---
                    if (c == 127 || ks == XK_BackSpace)
                    {
                        if (t->cursor_pos > 0)
                        {
                            memmove(t->input + t->cursor_pos - 1,
                                    t->input + t->cursor_pos,
                                    t->input_len - t->cursor_pos);
                            t->input_len--;
                            t->cursor_pos--;
                            t->input[t->input_len] = '\0';
                        }
                        continue;
                    }
                    // --- Tab Key (Auto-complete) ---
                    if (ks == XK_Tab)
                    {
                        autocomplete(t);
                        continue;
                    }

                    // --- Printable character insertion ---
                    if (isprint(c) && t->input_len + 1 < INPUT_MAX - 1)
                    {
                        memmove(t->input + t->cursor_pos + 1,
                                t->input + t->cursor_pos,
                                t->input_len - t->cursor_pos);
                        t->input[t->cursor_pos] = c;
                        t->cursor_pos++;
                        t->input_len++;
                        t->input[t->input_len] = '\0';
                        continue;
                    }
                }
            }
        }
        // === Handle pending signal messages safely ===
        if (signal_msg_ready && active >= 0)
        {
            tb_append(&tabs[active].tb, pending_signal_msg);
            ui_needs_redraw = 1;
            signal_msg_ready = 0;
        }

        if (ui_needs_redraw)
        {
            draw_ui(dpy, win, gc, tabs, tab_count, active);
            ui_needs_redraw = 0;
        }

        // small sleep to avoid busy loop; jobs are polled each iteration
        usleep(10000);
    }
    // cleanup on exit
    for (int i = 0; i < tab_count; i++)
    {
        save_history(&tabs[i]);
        for (int j = 0; j < tabs[i].job_count; j++)
            if (tabs[i].jobs[j].active)
                kill(tabs[i].jobs[j].pid, SIGKILL);
        for (int h = 0; h < tabs[i].hist_count; h++)
            free(tabs[i].history[h]);
        tb_free(&tabs[i].tb);
    }

    return 0;
}
