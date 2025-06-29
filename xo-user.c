#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void draw_board(uint32_t bits)
{
    for (int r = 0; r < 4; ++r) {
        printf("|");
        for (int c = 0; c < 4; ++c) {
            uint32_t v = (bits >> ((r * 4 + c) * 2)) & 3;
            char ch = v == 1 ? 'X' : v == 2 ? 'O' : ' ';
            printf(" %c |", ch);
        }
        printf("\r\n-----------------\r\n");
    }
}

static bool read_attr, end_attr;
static char moves[128][4];
static int n_moves;
static char game_lines[64][256];
static int n_games;

static void game_over_flush(void);
static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16:
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);

            break;
        case 17:
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            game_over_flush();
            end_attr = true;
            write(attr_fd, buf, 6);

            break;
        }
    }
    close(attr_fd);
}

static void draw_time(void)
{
    time_t now = time(NULL);
    const struct tm *tm = localtime(&now);
    printf("\x1b[H");
    printf("Time: %02d:%02d:%02d\x1b[K", tm->tm_hour, tm->tm_min, tm->tm_sec);
    fflush(stdout);
}

static void redraw_board(uint32_t bits, bool clear)
{
    if (clear)
        printf("\x1b[2J\x1b[H");
    else
        printf("\x1b[H");
    draw_time();
    putchar('\n');
    draw_board(bits);
    fflush(stdout);
}
static void record_move(uint32_t oldb, uint32_t newb)
{
    if (n_moves >= 128)
        return;
    for (int i = 0; i < 16; ++i) {
        uint32_t o = (oldb >> (i * 2)) & 3;
        uint32_t n = (newb >> (i * 2)) & 3;
        if (o == 0 && n != 0) {
            moves[n_moves][0] = 'A' + (i % 4);
            moves[n_moves][1] = '1' + (i / 4);
            moves[n_moves][2] = '\0';
            ++n_moves;
            break;
        }
    }
}

static void game_over_flush(void)
{
    if (!n_moves)
        return;
    int len = 0;
    len += snprintf(game_lines[n_games] + len, sizeof(game_lines[0]) - len,
                    "Moves:");
    for (int i = 0; i < n_moves; ++i)
        len += snprintf(game_lines[n_games] + len, sizeof(game_lines[0]) - len,
                        " %s%s", moves[i], (i + 1 == n_moves) ? "" : " -> ");
    n_games++;
    n_moves = 0;
}

static inline int occupied_cells(uint32_t b)
{
    int cnt = 0;
    for (int i = 0; i < 16; ++i)
        if ((b >> (i * 2)) & 3)
            ++cnt;
    return cnt;
}


int main(int argc, char *argv[])
{
    if (!status_check())
        exit(1);
    setvbuf(stdout, NULL, _IONBF, 0);
    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);


    fd_set readset;
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY | O_NONBLOCK);
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    read_attr = true;
    end_attr = false;

    printf("\x1b[2J");
    printf("\x1b[?25l");

    uint32_t last_bits = 0;
    bool need_full_redraw = true;
    int last_occ = 0;
    n_games = 0;
    n_moves = 0;

    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(device_fd, &readset);

        struct timeval tv = {1, 0};
        int ret = select(max_fd + 1, &readset, NULL, NULL, &tv);
        if (ret < 0) {
            perror("select");
            break;
        }

        if (ret == 0) {
            draw_time();
            continue;
        }

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            listen_keyboard_handler();
            if (read_attr)
                need_full_redraw = true;
        } else if (FD_ISSET(device_fd, &readset)) {
            uint32_t bits;
            if (read(device_fd, &bits, 4) == 4) {
                int occ = occupied_cells(bits);
                if (occ < last_occ && last_occ)
                    game_over_flush();

                last_occ = occ;
                if (bits)
                    record_move(last_bits, bits);
                last_bits = bits;
                need_full_redraw = true;
            }
        }


        if (need_full_redraw && read_attr) {
            redraw_board(last_bits, true);
            need_full_redraw = false;
        } else {
            draw_time();
        }
    }
    redraw_board(last_bits, false);
    game_over_flush();
    putchar('\n');
    for (int g = 0; g < n_games; ++g)
        printf("%s\n", game_lines[g]);

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);
    close(device_fd);
    printf("\x1b[?25h");
    return 0;
}
