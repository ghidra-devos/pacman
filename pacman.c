#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// pacman in a terminal. no curses, just escape codes. every tile is two columns wide so the
// maze comes out about square. only tiles that changed get sent, one write per frame

#define MW 28
#define MH 31
#define ROWS (MH + 3)
#define COLS (MW * 2)

static const char *const maze_src[MH] = {
	"############################",
	"#............##............#",
	"#.####.#####.##.#####.####.#",
	"#o####.#####.##.#####.####o#",
	"#.####.#####.##.#####.####.#",
	"#..........................#",
	"#.####.##.########.##.####.#",
	"#.####.##.########.##.####.#",
	"#......##....##....##......#",
	"######.##### ## #####.######",
	"     #.##### ## #####.#     ",
	"     #.##          ##.#     ",
	"     #.## ###--### ##.#     ",
	"######.## #      # ##.######",
	"      .   #      #   .      ",
	"######.## #      # ##.######",
	"     #.## ######## ##.#     ",
	"     #.##          ##.#     ",
	"     #.## ######## ##.#     ",
	"######.## ######## ##.######",
	"#............##............#",
	"#.####.#####.##.#####.####.#",
	"#.####.#####.##.#####.####.#",
	"#o..##.......  .......##..o#",
	"###.##.##.########.##.##.###",
	"###.##.##.########.##.##.###",
	"#......##....##....##......#",
	"#.##########.##.##########.#",
	"#.##########.##.##########.#",
	"#..........................#",
	"############################",
};

enum { RIGHT, LEFT, UP, DOWN };
static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, -1, 1};

struct ghost {
	int x, y, dir;
	const char *color;
	int out, wait, scared, home_x;
};

static char maze[MH][MW];
static struct ghost ghosts[4];
static int px, py, pdir, pwant, mouth, dots, fright, flash_walls;
static int score, best, eat_value;

static char out[1 << 16];
static int out_len;
static char shown[ROWS][COLS / 2][32];
static int ox, oy;
static struct termios saved;
static int raw_on;

static void put(const char *s)
{
	int n = strlen(s);
	if (out_len + n > (int)sizeof out) {
		write(1, out, out_len);
		out_len = 0;
	}
	memcpy(out + out_len, s, n);
	out_len += n;
}

static void flush(void)
{
	int at = 0;
	while (at < out_len) {
		ssize_t n = write(1, out + at, out_len - at);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		at += n;
	}
	out_len = 0;
}

static void restore(void)
{
	if (!raw_on)
		return;
	raw_on = 0;
	put("\x1b[0m\x1b[?25h\x1b[?1049l");
	flush();
	tcsetattr(0, TCSAFLUSH, &saved);
}

static void on_signal(int sig)
{
	restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void raw_mode(void)
{
	struct termios t;
	tcgetattr(0, &saved);
	t = saved;
	t.c_lflag &= ~(ICANON | ECHO | ISIG);
	t.c_iflag &= ~(IXON | ICRNL);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSAFLUSH, &t);
	raw_on = 1;
	atexit(restore);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	put("\x1b[?1049h\x1b[?25l\x1b[0m\x1b[2J");
	flush();
}

static void nap(int ms)
{
	struct timespec ts = {0, ms * 1000000L};
	nanosleep(&ts, NULL);
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

enum { K_NONE = -1, K_QUIT = 4, K_PAUSE, K_ENTER, K_OTHER };

static int read_byte(int wait_ms)
{
	struct pollfd p = {0, POLLIN, 0};
	unsigned char c;
	if (poll(&p, 1, wait_ms) <= 0 || read(0, &c, 1) != 1)
		return -1;
	return c;
}

// arrows come as ESC [ A, a lone ESC is quit
static int key(void)
{
	int c = read_byte(0);
	if (c < 0)
		return K_NONE;
	if (c == 27) {
		int b = read_byte(15);
		if (b != '[' && b != 'O')
			return K_QUIT;
		switch (read_byte(15)) {
		case 'A': return UP;
		case 'B': return DOWN;
		case 'C': return RIGHT;
		case 'D': return LEFT;
		}
		return K_OTHER;
	}
	switch (c) {
	case 'w': case 'k': return UP;
	case 's': case 'j': return DOWN;
	case 'd': case 'l': return RIGHT;
	case 'a': case 'h': return LEFT;
	case 'q': case 3: return K_QUIT;
	case 'p': return K_PAUSE;
	case '\r': case '\n': case ' ': return K_ENTER;
	}
	return K_OTHER;
}

static void cell(int row, int x, const char *what)
{
	if (!strcmp(shown[row][x], what))
		return;
	snprintf(shown[row][x], sizeof shown[row][x], "%s", what);
	char at[32];
	snprintf(at, sizeof at, "\x1b[%d;%dH", oy + row + 1, ox + x * 2 + 1);
	put(at);
	put(what);
	put("\x1b[0m");
}

static void text(int row, int col, const char *s, const char *color)
{
	char at[32];
	snprintf(at, sizeof at, "\x1b[%d;%dH", oy + row + 1, ox + col + 1);
	put(at);
	put(color);
	put(s);
	put("\x1b[0m");
	for (int x = col / 2; x <= (col + (int)strlen(s)) / 2 && x < COLS / 2; x++)
		shown[row][x][0] = 0;
}

static int wrap(int x)
{
	return x < 0 ? MW - 1 : x >= MW ? 0 : x;
}

static int open_tile(int x, int y)
{
	char c = maze[y][wrap(x)];
	return c != '#' && c != '-';
}

static unsigned rnd(unsigned n)
{
	static unsigned long s;
	if (!s)
		s = (unsigned long)now_ms() * 2654435761u | 1;
	s ^= s << 13;
	s ^= s >> 7;
	s ^= s << 17;
	return s % n;
}

static const char *ghost_look(struct ghost *g)
{
	static char buf[32];
	if (g->scared) {
		int white = fright < 12 && fright / 2 % 2;
		return white ? "\x1b[47;31m~~" : "\x1b[44;97m~~";
	}
	snprintf(buf, sizeof buf, "%s\"\"", g->color);
	return buf;
}

static void paint(int x, int y)
{
	if (x == px && y == py) {
		static const char *const open_mouth[4] = {"\x1b[43;30m <", "\x1b[43;30m> ", "\x1b[43;30m\\/", "\x1b[43;30m/\\"};
		cell(y + 1, x, mouth ? open_mouth[pdir] : "\x1b[43m  ");
		return;
	}
	for (int i = 0; i < 4; i++)
		if (ghosts[i].x == x && ghosts[i].y == y) {
			cell(y + 1, x, ghost_look(&ghosts[i]));
			return;
		}
	switch (maze[y][x]) {
	case '#':
		cell(y + 1, x, flash_walls ? "\x1b[47m  " : "\x1b[44m  ");
		break;
	case '.':
		cell(y + 1, x, "\x1b[37m \xc2\xb7");
		break;
	case 'o':
		cell(y + 1, x, "\x1b[97m()");
		break;
	case '-':
		cell(y + 1, x, "\x1b[95m--");
		break;
	default:
		cell(y + 1, x, "  ");
	}
}

static void paint_all(void)
{
	for (int y = 0; y < MH; y++)
		for (int x = 0; x < MW; x++)
			paint(x, y);
}

static char last_status[128];

static void status(int level, int paused)
{

	char line[128];
	snprintf(line, sizeof line, "PACMAN  score %-6d best %-6d level %-3d%s", score, best, level,
		 paused ? "paused" : "      ");
	if (!strcmp(line, last_status))
		return;
	strcpy(last_status, line);
	text(0, 0, line, "\x1b[97m");
}

static void lives_row(int lives)
{
	for (int i = 0; i < 5; i++)
		cell(MH + 1, 1 + i, i < lives - 1 ? "\x1b[43;30m> " : "  ");
	text(MH + 1, COLS - 18, "p pause  q quit", "\x1b[90m");
}

static void reset_actors(void)
{
	static const char *const colors[4] = {"\x1b[41;97m", "\x1b[45;97m", "\x1b[46;30m", "\x1b[48;5;208;30m"};
	static const int house_x[4] = {13, 13, 11, 15}, waits[4] = {0, 30, 80, 130};
	px = 13;
	py = 23;
	pdir = pwant = LEFT;
	mouth = 0;
	fright = 0;
	for (int i = 0; i < 4; i++) {
		struct ghost *g = &ghosts[i];
		g->color = colors[i];
		g->home_x = house_x[i];
		g->x = house_x[i];
		g->y = i ? 14 : 11;
		g->out = i == 0;
		g->wait = waits[i];
		g->dir = LEFT;
		g->scared = 0;
	}
}

static void ghost_home(struct ghost *g, int wait)
{
	g->x = g->home_x;
	g->y = 14;
	g->out = 0;
	g->scared = 0;
	g->wait = wait;
}

static void target(int i, int scatter, int *tx, int *ty)
{
	static const int corner_x[4] = {25, 2, 27, 0}, corner_y[4] = {-3, -3, 31, 31};
	struct ghost *g = &ghosts[i];
	int ddx = g->x - px, ddy = g->y - py;
	if (scatter || (i == 3 && ddx * ddx + ddy * ddy < 64)) {
		*tx = corner_x[i];
		*ty = corner_y[i];
	} else if (i == 1) {
		*tx = px + 4 * dx[pdir];
		*ty = py + 4 * dy[pdir];
	} else if (i == 2) {
		*tx = 2 * (px + 2 * dx[pdir]) - ghosts[0].x;
		*ty = 2 * (py + 2 * dy[pdir]) - ghosts[0].y;
	} else {
		*tx = px;
		*ty = py;
	}
}

static void ghost_step(int i, int scatter)
{
	static const int order[4] = {UP, LEFT, DOWN, RIGHT};
	struct ghost *g = &ghosts[i];
	int tx, ty, pick = -1, choices[4], n = 0;
	long bestd = 0;
	target(i, scatter, &tx, &ty);
	for (int k = 0; k < 4; k++) {
		int d = order[k];
		if (d == (g->dir ^ 1) || !open_tile(g->x + dx[d], g->y + dy[d]))
			continue;
		choices[n++] = d;
		long ex = wrap(g->x + dx[d]) - tx, ey = g->y + dy[d] - ty;
		if (pick < 0 || ex * ex + ey * ey < bestd) {
			pick = d;
			bestd = ex * ex + ey * ey;
		}
	}
	if (!n)
		pick = g->dir ^ 1;
	else if (g->scared)
		pick = choices[rnd(n)];
	g->dir = pick;
	g->x = wrap(g->x + dx[pick]);
	g->y += dy[pick];
}

static int meet(struct ghost *g)
{
	if (!g->out)
		return 0;
	if (g->scared) {
		score += eat_value;
		eat_value *= 2;
		ghost_home(g, 25);
		return 0;
	}
	return 1;
}

static char best_path[512];

static void load_best(void)
{
	const char *home = getenv("HOME");
	snprintf(best_path, sizeof best_path, "%s/.pacman", home ? home : ".");
	FILE *f = fopen(best_path, "r");
	if (f) {
		if (fscanf(f, "%d", &best) != 1)
			best = 0;
		fclose(f);
	}
}

static void save_best(void)
{
	FILE *f = fopen(best_path, "w");
	if (f) {
		fprintf(f, "%d\n", best);
		fclose(f);
	}
}

static void wait_ms(int ms)
{
	flush();
	long end = now_ms() + ms;
	while (now_ms() < end)
		nap(5);
}

static int again(void)
{
	char line[64];
	const char *box = "\x1b[44m";
	int r = MH / 2 - 2, c = COLS / 2 - 15;
	for (int y = 0; y < 7; y++) {
		char row[40];
		memset(row, ' ', 30);
		row[30] = 0;
		text(r + y, c, row, y == 0 || y == 6 ? box : "\x1b[40m");
		if (y > 0 && y < 6) {
			text(r + y, c, " ", box);
			text(r + y, c + 29, " ", box);
		}
	}
	text(r + 2, c + 10, "game over", "\x1b[91m");
	snprintf(line, sizeof line, "score %d  best %d", score, best);
	text(r + 3, c + (30 - (int)strlen(line)) / 2, line, "\x1b[97m");
	text(r + 4, c + 3, "enter: again   q: quit", "\x1b[93m");
	wait_ms(500);
	while (key() != K_NONE)
		;
	for (;;) {
		int k = key();
		if (k == K_ENTER)
			return 1;
		if (k == K_QUIT)
			return 0;
		nap(10);
	}
}

static void fit(void)
{
	struct winsize ws;
	if (ioctl(1, TIOCGWINSZ, &ws) < 0 || ws.ws_col < COLS || ws.ws_row < ROWS) {
		fprintf(stderr, "pacman needs a terminal of at least %d by %d\n", COLS, ROWS);
		exit(1);
	}
	ox = (ws.ws_col - COLS) / 2;
	oy = (ws.ws_row - ROWS) / 2;
}

int main(void)
{
	if (!isatty(0) || !isatty(1)) {
		fprintf(stderr, "pacman wants a terminal\n");
		return 1;
	}
	fit();
	load_best();
	raw_mode();
	for (int play = 1; play;) {
		int lives = 3, level = 1, quit = 0, paused = 0;
		score = 0;
		memset(shown, 0, sizeof shown);
		last_status[0] = 0;
		put("\x1b[0m\x1b[2J");
		for (int newlevel = 1; lives > 0 && !quit;) {
			if (newlevel) {
				dots = 0;
				for (int y = 0; y < MH; y++)
					for (int x = 0; x < MW; x++) {
						maze[y][x] = maze_src[y][x];
						dots += maze[y][x] == '.' || maze[y][x] == 'o';
					}
				newlevel = 0;
			}
			reset_actors();
			paint_all();
			lives_row(lives);
			status(level, 0);
			text(18, 25, "READY!", "\x1b[93m");
			wait_ms(1500);
			while (key() != K_NONE)
				;
			for (int x = 11; x <= 16; x++)
				paint(x, 17);

			int step = level < 4 ? 90 - level * 10 : 50, count = 0, mode_ms = 0, died = 0;
			int fright_len = level < 6 ? 50 - level * 6 : 14;
			long next = now_ms();
			while (!died && !quit && dots > 0) {
				int k;
				while ((k = key()) != K_NONE) {
					if (k == K_QUIT)
						quit = 1;
					else if (k == K_PAUSE)
						paused ^= 1;
					else if (k <= DOWN)
						pwant = k;
				}
				status(level, paused);
				flush();
				if (quit)
					break;
				if (paused || now_ms() < next) {
					nap(3);
					continue;
				}
				next += step;
				if (next < now_ms() - 200)
					next = now_ms();
				count++;

				int old_x[5], old_y[5];
				old_x[4] = px;
				old_y[4] = py;
				for (int i = 0; i < 4; i++) {
					old_x[i] = ghosts[i].x;
					old_y[i] = ghosts[i].y;
				}

				if (open_tile(px + dx[pwant], py + dy[pwant]))
					pdir = pwant;
				if (open_tile(px + dx[pdir], py + dy[pdir])) {
					px = wrap(px + dx[pdir]);
					py += dy[pdir];
				}
				mouth ^= 1;
				char *here = &maze[py][px];
				if (*here == '.' || *here == 'o') {
					if (*here == 'o') {
						fright = fright_len;
						eat_value = 200;
						for (int i = 0; i < 4; i++)
							if (ghosts[i].out) {
								ghosts[i].scared = 1;
								ghosts[i].dir ^= 1;
							}
						score += 50;
					} else {
						score += 10;
					}
					*here = ' ';
					dots--;
				}
				for (int i = 0; i < 4 && !died; i++)
					if (ghosts[i].x == px && ghosts[i].y == py)
						died = meet(&ghosts[i]);

				if (!fright)
					mode_ms += step;
				int scatter = mode_ms < 7000 || (mode_ms >= 27000 && mode_ms < 34000) ||
					      (mode_ms >= 54000 && mode_ms < 59000);
				for (int i = 0; i < 4 && !died; i++) {
					struct ghost *g = &ghosts[i];
					if (!g->out) {
						if (g->wait-- <= 0) {
							g->out = 1;
							g->x = 13;
							g->y = 11;
							g->dir = LEFT;
						}
						continue;
					}
					if (g->scared ? count % 2 : count % 5 == 0)
						continue;
					int gx = g->x, gy = g->y;
					ghost_step(i, scatter);
					int crossed = gx == px && gy == py && g->x == old_x[4] && g->y == old_y[4];
					if ((g->x == px && g->y == py) || crossed)
						died = meet(g);
				}
				if (fright && --fright == 0)
					for (int i = 0; i < 4; i++)
						ghosts[i].scared = 0;

				for (int i = 0; i < 5; i++)
					paint(old_x[i], old_y[i]);
				for (int i = 0; i < 4; i++)
					paint(ghosts[i].x, ghosts[i].y);
				paint(px, py);
			}
			if (score > best) {
				best = score;
				save_best();
			}
			if (quit)
				break;
			if (died) {
				for (int i = 0; i < 8; i++) {
					mouth = 1;
					pdir = i % 4;
					paint(px, py);
					wait_ms(110);
				}
				lives--;
				lives_row(lives);
				continue;
			}
			for (int i = 0; i < 6; i++) {
				flash_walls = i % 2 == 0;
				paint_all();
				wait_ms(250);
			}
			flash_walls = 0;
			level++;
			newlevel = 1;
		}
		if (quit)
			break;
		play = again();
	}
	restore();
	printf("score %d, best %d\n", score, best);
	return 0;
}
