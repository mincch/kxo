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

static void redraw_board(uint32_t bits)
{
    printf("\x1b[2J\x1b[H");
    draw_time();
    putchar('\n');
    draw_board(bits);
    fflush(stdout);
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
                last_bits = bits;
                need_full_redraw = true;
            }
        }


        if (need_full_redraw && read_attr) {
            redraw_board(last_bits);
            need_full_redraw = false;
        } else {
            draw_time();
        }
    }



    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);
    printf("\x1b[?25h");
    return 0;
}
