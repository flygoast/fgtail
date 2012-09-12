/**********************************************************
 * DESCRIPTION: It's written occording to coreutils's tail.
 *  Just for modifying it to watch the log file.
 * AUTHOR: flygoast(flygoast@126.com)
 * FIRST CREATED: 2012-04-02 
 * LAST MODIFIED: 2012-04-13
 **********************************************************/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <assert.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* True if negative values of the signed integer type T uses two's
 * complement, one's complement, or signed magnitude representation,
 * respectively. */
#define TYPE_TWOS_COMPLEMENT(t) ((t) ~(t)0 == (t)-1)
#define TYPE_ONES_COMPLEMENT(t) ((t) ~(t)0 == 0)
#define TYPE_SIGNED_MAGNITUDE(t) ((t) ~(t)0 < (t)-1)

#define TYPE_SIGNED(t) (!((t)0 < (t)-1))
#define TYPE_MAXIMUM(t) \
    ((t)(! TYPE_SIGNED(t) \
        ? (t)-1 \
        : ((((t)1 <<(sizeof(t) * CHAR_BIT - 2)) - 1) * 2 + 1)))
#define TYPE_MINIMUM(t) \
    ((t)(! TYPE_SIGNED(t) \
        ? (t)0 \
        : TYPE_SIGNED_MAGNITUDE(t) \
        ? ~(t)0 \
        : ~ TYPE_MAXIMUM(t)))
#define OFF_T_MAX TYPE_MAXIMUM(off_t)

#define VERSION     "0.0.1"
#define AUTHORS     "flygoast(flygoast@126.com)"

/* Number of items to tail. */
#define DEFAULT_N_LINES     10

/* Special values for dump_remainder's N_BYTES parameter. */
#define COPY_TO_EOF         UINTMAX_MAX
#define COPY_A_BUFFER       (UINTMAX_MAX - 1)

#define HM_MULTIPLE_FILES       0
#define HM_ALWAYS               1
#define HM_NEVER                2

/* Follow the name of each file: if the file is renamed, 
   try to reopen that name and track the end of the new
   file if/when it's recreated. This is useful for tracking
   logs that are occasionally rotated. */
#define FOLLOW_NAME 1

/* follow each descriptor obtained upon opening a file.
   that means we'll continue to follow the end of a file
   even after it has been renamed or unlinked. */
#define FOLLOW_DESCRIPTOR 2

#define DEFAULT_FOLLOW_MODE FOLLOW_DESCRIPTOR

/* The types of files for which tail works. */
#define IS_TAILABLE_FILE_TYPE(mode) \
    (S_ISREG(mode) || S_ISFIFO(mode) || S_ISSOCK(mode) || S_ISCHR(mode))

struct File_spec {
    /* The actual file name, or "-" for stdin. */
    char *name;

    /* Attributes of the file the last time we checked. */
    off_t   size;
    time_t  mtime;
    dev_t   dev;
    ino_t   ino;
    mode_t  mode;

    /* The specified name initially referred to a directory 
       or some other type for which tail isn't meaningful.
       Unlike for a permission problem(tailable, below) once
       this is set, the name is not checked ever again. */
    int     ignore;
    
    /* A file is tailable if it exists, is readable, and is of 
       type IS_TAILABLE_FILE_TYPE. */
    int     tailable;

    /* File descriptor on which the file is open; 
       -1 if it's not open. */
    int     fd;

    /* The value of errno seen last time we checked this file. */
    int     errnum;

    /* 1 if O_NONBLOCK is clear, 0 if set, -1 if not known. */
    int     blocking;

    /* See description of DEFAULT_MAX_N_... below. */
    uintmax_t n_unchanged_stats;
};

/* Keep trying to open a file even if it is inaccessible when 
   tail starts or if it becomes inaccessible later -- useful
   only with -f. */
static int reopen_inaccessible_files;

/* If true, interpret the numberic argument as the number of lines.
   Otherwise, interpret it as the number of bytes. */
static int count_lines;

/* Whether we follow the name of each file or the file descriptor
   that is initially associated with each name. */
static int follow_mode = DEFAULT_FOLLOW_MODE;

/* If true, read from the ends of all specified files until killed. */
static int forever;

/* If true, count from start of file instead of end. */
static int from_start;

/* If true, print filename headers. */
static int print_headers;

/* When tailing a file by name, if there have been this many 
   consecutive iterations for which the file has not changed,
   then open/fstat the file to determine if that file name is
   still associated with the same device/inode-number pair as
   before. This option is meaningful only when following by 
   name. --max-unchanged-stats=N */
#define DEFAULT_MAX_N_UNCHANGED_STATS_BETWEEN_OPENS 5
static uintmax_t max_n_unchanged_stats_between_opens = 
    DEFAULT_MAX_N_UNCHANGED_STATS_BETWEEN_OPENS;

/* The process ID of the process (presumably on the current host)
   that is writing to all followed files. */
static pid_t pid;

/* True if we have ever read standard input. */
static int have_read_stdin;

/* For long options that have no equivalent short option, 
   use a non-character as a pseudo short option, starting
   with CHAR_MAX + 1. */
#define RETRY_OPTION                (CHAR_MAX + 1)
#define MAX_UNCHANGED_STATS_OPTION  (CHAR_MAX + 2)
#define PID_OPTION                  (CHAR_MAX + 3)
#define LONG_FOLLOW_OPTION          (CHAR_MAX + 4)
#define HELP_OPTION                 (CHAR_MAX + 5)
#define VERSION_OPTION              (CHAR_MAX + 6)

static struct option const long_options[] = {
    {"bytes", required_argument, NULL, 'c'},
    {"follow", optional_argument, NULL, LONG_FOLLOW_OPTION},
    {"lines", required_argument, NULL, 'n'},
    {"max-unchanged-stats", required_argument, NULL, 
        MAX_UNCHANGED_STATS_OPTION},
    {"pid", required_argument, NULL, PID_OPTION},
    {"quiet", no_argument, NULL, 'q'},
    {"retry", no_argument, NULL, RETRY_OPTION},
    {"silent", no_argument, NULL, 'q'},
    {"sleep-interval", required_argument, NULL, 's'},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, HELP_OPTION},
    {"version", no_argument, NULL, VERSION_OPTION},
    {NULL, 0, NULL, 0}
};

static const char *program_name;

static void set_progname(char *argv0) {
    const char *base;
    base = strrchr(argv0, '/');
    program_name = base ? (base + 1) : argv0;
}

static void usage(int status) {
    if (status != EXIT_SUCCESS) {
        fprintf(stderr, "Try `%s --help` for more information.\n",
            program_name);
    } else {
        printf("\
Usage: %s [OPTION]...[FILE]...\n", program_name);
        printf("\
Print the last %d lines of each FILE to standard output.\n\
With more than one FILE, precede each with a header \
giving the file name.\n\
With no FILE, or when FILE is -, read standard input.\n",
        DEFAULT_N_LINES);
        fputs("\
Mandatory arguments to long options are mandatory for \
short options too.\n", stdout);
        fputs("\
    -c, --bytes=K       output the last K bytes; alternatively, use -c +K\n\
                        to output bytes starting the Kth of each file\n", 
            stdout);
        fputs("\
    -f, --follow[={name|descriptor}]\n\
                        output appended data as the file grows;\n\
                        -f, --follow, and --flow=descriptor are\n\
                        equivalent\n\
    -F                  same as --follow=name --retry\n", stdout);
        printf("\
    -n, --lines=K       output the last K lines, instead of the last %d;\n\
                        or use -n +K to output lines starting with the Kth\n\
    --max-unchanged-stats=N\n\
                        with --follow=name, reopen a FILE which has not\n\
                        changed size after N (default %d) iterations\n\
                        to see if it has been unlinked or renamed\n\
                        (this is usual case of rotated log files).\n\
                        With inotify, this option is rarely useful. \n",
                    DEFAULT_N_LINES,
                    DEFAULT_MAX_N_UNCHANGED_STATS_BETWEEN_OPENS);
        fputs("\
    --pid=PID           with -f, terminate after process ID, PID dies\n\
    -q, --quiet, --silent\n\
                        never output headers giving file names\n\
    --retry             keep trying to open a file even when it is or\n\
                        becomes inaccessible; useful when following by\n\
                        name, i.e., with --follow=name\n", stdout);
        fputs("\
    -s, --sleep-interval=N\n\
                        with -f, sleep for approximately N seconds\n\
                        (default 1.0) between iterations.\n\
    -v, --verbose       always output headers giving file names\n", 
            stdout);
        fputs("\
    --help              display this help and exit\n\
    --version           output version information and exit\n", stdout);
    }
    exit(status);
}
        
static int valid_file_spec(struct File_spec const *f) {
    /* Exactly one of the following subexpressions must be true. */
    return ((f->fd == -1) ^ (f->errnum == 0));
}

static char const * pretty_name(struct File_spec const *f) {
    return (!strcmp(f->name, "-") ? "standard input" : f->name);
}

static void write_header(const char *pretty_filename) {
    static int first_file = 1;
    printf("%s==> %s <==\n", (first_file ? "" : "\n"), pretty_filename);
    first_file = 0;
}

static void xwrite_stdout(char const *buffer, size_t n_bytes) {
    if (n_bytes > 0 && fwrite(buffer, 1, n_bytes, stdout) == 0) {
        fprintf(stderr, "fwrite error:%s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static struct timespec dtotimespec(double sec) {
#define BILLION     (1000 * 1000 * 1000)
    double min_representable = TYPE_MINIMUM(time_t);
    double max_representable = 
        ((TYPE_MAXIMUM(time_t) * (double)BILLION + (BILLION - 1)) 
        / BILLION);
    struct timespec r;

    if (!(min_representable < sec)) {
        r.tv_sec = TYPE_MINIMUM(time_t);
        r.tv_nsec = 0;
    } else if (!(sec < max_representable)) {
        r.tv_sec = TYPE_MAXIMUM(time_t);
        r.tv_nsec = BILLION - 1;
    } else {
        time_t s = (time_t)sec;
        double frac = BILLION * (sec - s);
        long ns = frac;
        ns += ns < frac;
        s += ns / BILLION;
        ns %= BILLION;

        if (ns < 0) {
            s--;
            ns += BILLION;
        }
        r.tv_sec = s;
        r.tv_nsec = ns;
    }

    return r;
}

static int xnanosleep(double seconds) {
    struct timespec ts_sleep = dtotimespec(seconds);
    for ( ;; ) {
        errno = 0;
        if (nanosleep(&ts_sleep, NULL) == 0) {
            break;
        }

        if (errno != EINTR && errno != 0) {
            return -1;
        }
    }

    return 0;
}

/* Record a file F with descriptor FD, size SIZE, status ST, and
   blocking status BLOCKING. */
static void record_open_fd(struct File_spec *f, int fd,
        off_t size, struct stat const *st,
        int blocking) {
    f->fd = fd;
    f->size = size;
    f->mtime = st->st_mtime;
    f->dev = st->st_dev;
    f->ino = st->st_ino;
    f->mode = st->st_mode;
    f->blocking = blocking;
    f->n_unchanged_stats = 0;
    f->ignore = 0;
}

/* Close the file with descriptor FD and name FILENAME. */
static void close_fd(int fd, const char *filename) {
    if (fd != -1 && fd != STDIN_FILENO && close(fd)) {
        fprintf(stderr, "close %s failed:%s\n", filename, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

/* Process the interrupted read() system call. */
static size_t safe_read(int fd, void *buf, size_t count) {
    for ( ; ; ) {
        ssize_t result = read(fd, buf, count);
        if (0 <= result) {
            return result;
        } else if (errno == EINTR) {
            continue;
        } else {
            return result;
        }
    }
}

/* Read and output N_BYTES of file PRETTY_FILENAME starting at the
   current position in FD. If N_BYTES is COPY_TO_EOF, then copy until
   end of file. If N_BYTES is COPY_A_BUFFER, then copy at most one 
   buffer's worth. Return the number of bytes read from the file. */
static uintmax_t dump_remainder(const char *pretty_filename, int fd,
        uintmax_t n_bytes) {
    uintmax_t n_written;
    uintmax_t n_remaining = n_bytes;

    n_written = 0;
    while (1) {
        char buffer[BUFSIZ];
        size_t n = MIN(n_remaining, BUFSIZ);
        size_t bytes_read = safe_read(fd, buffer, n);
        if (bytes_read == -1) {
            if (errno != EAGAIN) {
                fprintf(stderr, "read failed:%s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        xwrite_stdout(buffer, bytes_read);
        n_written += bytes_read;
        if (n_bytes != COPY_TO_EOF) {
            n_remaining -= bytes_read;
            if (n_remaining == 0 || n_bytes == COPY_A_BUFFER) {
                break;
            }
        }
    }
    return n_written;
}

/* Print the last N_LINES lines from the end of file FD.
 * Go backward through the file, reading 'BUFSIZ' bytes 
 * at a time(except probably the first), until we hit
 * the start of the file or have read N_LINES newlines.
 * START_POS is the starting position of the read pointer
 * for the file associated with FD(may be nonzero).
 * END_POS is the file offset of EOF(one larger than offset
 * of last byte).
 * Return 0 if successful. */
static int file_lines(const char *pretty_filename, int fd, 
        uintmax_t n_lines, off_t start_pos, off_t end_pos,
        uintmax_t *read_pos) {
    char buffer[BUFSIZ];
    size_t bytes_read;
    off_t pos = end_pos;

    if (n_lines == 0) {
        return 0;
    }

    /* Set 'bytes_read' to the size of the last, probably partial, buffer;
       0 < 'bytes_read' <= 'BUFSIZ'. */
    bytes_read = (pos - start_pos) % BUFSIZ;
    if (bytes_read == 0) {
        bytes_read = BUFSIZ;
    }

    /* Make 'pos' a multiple of 'BUFSIZ' (0 if file is short),
       so that all reads will be on block boundaries, which
       might increase efficiency. */
    pos -= bytes_read;
    if ((off_t)-1 == lseek(fd, pos, SEEK_SET)) {
        fprintf(stderr, "lseek %s failed:%s\n", 
                pretty_filename, strerror(errno));
        exit(EXIT_FAILURE);
    }
    bytes_read = safe_read(fd, buffer, bytes_read);
    if (bytes_read < 0) {
        fprintf(stderr, "read %s failed:%s\n", 
                pretty_filename, strerror(errno));
        exit(EXIT_FAILURE);
    }

    *read_pos = pos + bytes_read;

    /* Count the incomplete line on files that don't 
       end with a newline. */
    if (bytes_read && buffer[bytes_read - 1] != '\n') {
        --n_lines;
    }

    do {
        /* Scan backward, counting the newlines in the bufferfull. */
        size_t n = bytes_read;
        while (n) {
            char const *nl;
            nl = memrchr(buffer, '\n', n);
            if (nl == NULL) {
                break;
            }
            n = nl - buffer; /* buffer left to search '\n' in it */
            if (n_lines-- == 0) {
                /* If this newline isn't the last character in the buffer,
                   output the part that is after it. */
                if (n != bytes_read - 1) {
                    xwrite_stdout(nl + 1, bytes_read - (n + 1));
                }

                *read_pos += dump_remainder(pretty_filename, fd,
                        end_pos - (pos + bytes_read));
                return 0;
            }
        }

        /* Not enough newlines in that bufferfull. */
        if (pos == start_pos) {
            /* Not enough lines in the file; print everything from 
               start_pos to the end. */
            lseek(fd, start_pos, SEEK_SET);
            *read_pos = start_pos + 
                dump_remainder(pretty_filename, fd, end_pos);
            return 0;
        }
        pos -= BUFSIZ;
        lseek(fd, pos, SEEK_SET);

        bytes_read = safe_read(fd, buffer, BUFSIZ);
        if (bytes_read == -1) {
            fprintf(stderr, "read failed:%s\n", strerror(errno));
            return -1;
        }
        *read_pos = pos + bytes_read;
    } while (bytes_read > 0);
    return 0;
}

/* Print the last N_LINES lines from the end of the standard input,
   open for reading as pipe FD.
   Buffer the text as a linked list of LBUFFERs, adding them as needed.
   Return 0 if successful. */
static int pipe_lines(const char *pretty_filename, int fd, 
        uintmax_t n_lines, uintmax_t *read_pos) {
    struct linebuffer {
        char buffer[BUFSIZ];
        size_t nbytes;
        size_t nlines;
        struct linebuffer *next;
    };

    typedef struct linebuffer LBUFFER;
    LBUFFER *first, *last, *tmp;
    size_t total_lines = 0; /* Total number of newlines in all buffers. */
    int ret = 0;
    size_t n_read; /* Size in bytes of most recent read */

    first = last = malloc(sizeof(LBUFFER));
    if (!first) {
        fprintf(stderr, "Out of memory:%s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    first->nbytes = first->nlines = 0;
    first->next = NULL;
    tmp = malloc(sizeof(LBUFFER));
    if (!tmp) {
        fprintf(stderr, "Out of memory:%s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Input is always read into a fresh buffer. */
    while (1) {
        n_read = safe_read(fd, tmp->buffer, BUFSIZ);
        if (n_read == 0 || n_read == -1) {
            break;
        }
        tmp->nbytes = n_read;
        *read_pos += n_read;
        tmp->nlines = 0;
        tmp->next = NULL;

        /* count the number of newlines just read */
        {
            char const *buffer_end = tmp->buffer + n_read;
            char const *p = tmp->buffer;
            while ((p = memchr(p, '\n', buffer_end - p))) {
                ++p;
                ++tmp->nlines;
            }
        }
        total_lines += tmp->nlines;

        /* If there is enough room in the last buffer read, 
           just append the new one to it. This is bacause 
           when reading from a pipe, 'n_read' can often be
           very small. */
        if (tmp->nbytes + last->nbytes < BUFSIZ) {
            memcpy(&last->buffer[last->nbytes], tmp->buffer, tmp->nbytes);
            last->nbytes += tmp->nbytes;
            last->nlines += tmp->nlines;
        } else {
            /* If there's not enough room, link the new buffer
               onto the end of the list, then either free up
               the oldest buffer for the next read if that would
               leave enough lines, or else malloc a new one.
               Some compaction mechanism is possible but probably
               not worthwhile. */
            last->next = tmp;
            last = last->next;
            if (total_lines - first->nlines > n_lines) {
                tmp = first;
                total_lines -= first->nlines;
                first = first->next;
            } else {
                tmp = malloc(sizeof(LBUFFER));
            }
        }
    }
    free(tmp);

    if (n_read == -1) {
        fprintf(stderr, "reading failed:%s\n", strerror(errno));
        ret = -1;
        goto free_lbuffers;
    }

    /* If the file is empty, then bail out. */
    if (last->nbytes == 0) {
        goto free_lbuffers;
    }

    /* This prevents a core dump when the pipe contains no newlines. */
    if (n_lines == 0) {
        goto free_lbuffers;
    }

    /* Count the incomplete line on files that don't 
       end with a newline. */
    if (last->buffer[last->nbytes - 1] != '\n') {
        ++last->nlines;
        ++total_lines;
    }

    /* Run through the list, printing lines. First, skip over
       unneeded buffers. */
    for (tmp = first; total_lines - tmp->nlines > n_lines; 
            tmp = tmp->next) {
        total_lines -= tmp->nlines;
    }

    /* Find the correct beginning, then print the rest of the file. */
    {
        char const *beg = tmp->buffer;
        char const *buffer_end = tmp->buffer + tmp->nbytes;
        if (total_lines > n_lines) {
            /* Skip 'total_lines' - 'n_lines' newlines. We made sure
               that 'total_lines' - 'n_lines' <= 'tmp->nlines' */
            size_t j;
            for (j = total_lines - n_lines; j; --j) {
                beg = memchr(beg, '\n', buffer_end - beg);
                assert(beg);
                ++beg;
            }
        }
        xwrite_stdout(beg, buffer_end - beg);
    }

    for (tmp = tmp->next; tmp; tmp = tmp->next) {
        xwrite_stdout(tmp->buffer, tmp->nbytes);
    }

free_lbuffers:
    while (first) {
        tmp = first->next;
        free(first);
        first = tmp;
    }
    return ret;
}

static int start_lines(const char *pretty_filename, int fd, 
        uintmax_t n_lines, uintmax_t *read_pos) {
    if (n_lines == 0) {
        return 0;
    }

    while (1) {
        char buffer[BUFSIZ];
        char *p = buffer;
        size_t bytes_read = safe_read(fd, buffer, BUFSIZ);
        char *buffer_end = buffer + bytes_read;

        if (bytes_read == 0) { /* EOF */
            return -1;
        }

        if (bytes_read < 0) {
            fprintf(stderr, "read %s failed:%s\n", 
                    pretty_filename, strerror(errno));
            exit(EXIT_FAILURE);
        }

        *read_pos += bytes_read;
        while ((p = memchr(p, '\n', buffer_end - p))) {
            ++p;
            if (--n_lines == 0) {
                if (p < buffer_end) {
                    xwrite_stdout(p, buffer_end - p);
                }
                return 0;
            }
        }
    }
}

/* Print the last N_BYTES characters from the end of pipe FD.
 * This is a stripped down version of pipe_lines. 
 * Return 0 if successful. */
static int pipe_bytes(const char *pretty_filename, int fd, 
        uintmax_t n_bytes, uintmax_t *read_pos) {
    struct charbuffer {
        char buffer[BUFSIZ];
        size_t nbytes;
        struct charbuffer *next;
    };
    typedef struct charbuffer CBUFFER;
    CBUFFER *first, *last, *tmp;
    size_t i; /* Index into buffers. */
    size_t total_bytes = 0; /* Total characters in all buffers. */
    int ret = 0;
    size_t n_read;
    
    first = last = malloc(sizeof(CBUFFER));
    assert(last);
    first->nbytes = 0;
    first->next = NULL;
    tmp = malloc(sizeof(CBUFFER));
    assert(tmp);

    /* Input is always read into a fresh buffer. */
    while (1) {
        n_read = safe_read(fd, tmp->buffer, BUFSIZ);
        if (n_read <= 0) {
            break;
        }

        *read_pos += n_read;
        tmp->nbytes = n_read;
        tmp->next = NULL;

        total_bytes += tmp->nbytes;
        /* If there is enough room in the last buffer read, just
         * append the new one to it. This is because when reading
         * from a pipe, 'nbytes' can often be very small. */
        if (tmp->nbytes + last->nbytes < BUFSIZ) {
            memcpy(&last->buffer[last->nbytes], tmp->buffer, 
                    tmp->nbytes);
            last->nbytes += tmp->nbytes;
        } else {
            /* If there's not enough room, link the new buffer onto
             * the end of the list, then either free up the oldest
             * buffer for the next read if that would leave enough
             * characters, or else malloc a new one. Some compaction
             * mechanism is possible but probably not worthwhile. */
            last = last->next = tmp;
            if (total_bytes - first->nbytes > n_bytes) {
                tmp = first;
                total_bytes -= first->nbytes;
                first = first->next;
            } else {
                tmp = malloc(sizeof(CBUFFER));
            }
        }
    }
    free(tmp);

    if (n_read == -1) {
        fprintf(stderr, "read %s failed:%s\n",
                pretty_filename, strerror(errno));
        ret = -1;
        goto free_cbuffers;
    }

    /* Run through the list, printing characters. First, skip over
     * unneeded buffers. */
    for (tmp = first; total_bytes - tmp->nbytes > n_bytes; 
            tmp = tmp->next) {
        total_bytes -= tmp->nbytes;
    }

    /* Find the correct beginning, then print the rest of the file.
     * We made sure that 'total_bytes' - 'n_bytes' <= 'tmp->nbytes' */
    if (total_bytes > n_bytes) {
        i = total_bytes - n_bytes;
    } else {
        i = 0;
    }

    xwrite_stdout(&tmp->buffer[i], tmp->nbytes - i);
    for (tmp = tmp->next; tmp; tmp = tmp->next) {
        xwrite_stdout(tmp->buffer, tmp->nbytes);
    }

free_cbuffers:
    while (first) {
        tmp = first->next;
        free(first);
        first = tmp;
    }
    return ret;
}

/* Skip N_BYTES characters from the start of pipe FD, and print
 * any extra characters that were read beyond that.
 * Return 1 on error, 0 if ok, -1 if EOF. */
static int start_bytes(const char *pretty_filename, int fd,
        uintmax_t n_bytes, uintmax_t *read_pos) {
    char buffer[BUFSIZ];

    while (0 < n_bytes) {
        size_t bytes_read = safe_read(fd, buffer, BUFSIZ);
        if (bytes_read == 0) {
            return -1;
        }

        if (bytes_read < 0) {
            fprintf(stderr, "read %s failed:%s\n", 
                    pretty_filename, strerror(errno));
            exit(EXIT_FAILURE);
        }
        *read_pos += bytes_read;
        if (bytes_read <= n_bytes) {
            n_bytes -= bytes_read;
        } else {
            size_t n_remaining = bytes_read - n_bytes;
            if (n_remaining) {
                xwrite_stdout(&buffer[n_bytes], n_remaining);
            }
            break;
        }
    }
    return 0;
}

/* Output the last N_BYTES bytes of file FILENAME open for reading
 * in FD. Return 0 if successful. */
static int tail_bytes(const char *pretty_filename, int fd, 
        uintmax_t n_bytes, uintmax_t *read_pos) {
    struct stat     stats;
    if (fstat(fd, &stats)) {
        fprintf(stderr, "fstat %s failed:%s\n",
                pretty_filename, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (from_start) {
        if (S_ISREG(stats.st_mode) && n_bytes <= OFF_T_MAX) {
            lseek(fd, n_bytes, SEEK_CUR);
            *read_pos += n_bytes;
        } else {
            int t = start_bytes(pretty_filename, fd, 
                    n_bytes, read_pos);
            
            if (t < 0) { /* EOF */
                return 0;
            }
        }
        *read_pos += dump_remainder(pretty_filename, fd, COPY_TO_EOF);
    } else {
        if (S_ISREG(stats.st_mode) && n_bytes <= OFF_T_MAX) {
            off_t current_pos = lseek(fd, 0, SEEK_CUR);
            off_t end_pos = lseek(fd, 0, SEEK_END);
            off_t diff = end_pos - current_pos;

            /* Be careful here. The current position may actually be
             * beyond the end of the file. */
            off_t bytes_remaining = diff < 0 ? 0 : diff;
            off_t nb = n_bytes;

            if (bytes_remaining <= nb) {
                /* From the current position to end of file, there
                 * are no more bytes than have been requested. So
                 * reposition the file pointer to the incoming 
                 * current position and print everything after that. */
                *read_pos = lseek(fd, current_pos, SEEK_SET);
            } else {
                /* There are more bytes remaining than were requested.
                 * Back up. */
                *read_pos = lseek(fd, -nb, SEEK_END);
            }
            *read_pos += dump_remainder(pretty_filename, fd, n_bytes);
        } else {
            return pipe_bytes(pretty_filename, fd, n_bytes, read_pos);
        }
    }
    return 0;
}

/* Output the last N_LINES lines of file FILENAME open for reading
 * in FD. Return 0 if successful. */
static int tail_lines(const char *pretty_filename, int fd,
        uintmax_t n_lines, uintmax_t *read_pos) {
    struct stat     stats;
    if (fstat(fd, &stats)) {
        fprintf(stderr, "fstat %s failed:%s\n", 
                pretty_filename, strerror(errno));
        return -1;
    }

    if (from_start) {
        int t = start_lines(pretty_filename, fd, n_lines, read_pos);
        if (t == -1) { /* EOF */
            return 0;
        }
        *read_pos += dump_remainder(pretty_filename, fd, COPY_TO_EOF); 
    } else {
        off_t start_pos = -1;
        off_t end_pos;

        /* Use file_lines() only if FD refers to a regular file for
         * which lseek(... SEEK_END) works. */
        if (S_ISREG(stats.st_mode) 
                && (start_pos = lseek(fd, 0, SEEK_CUR)) != -1
                && start_pos < (end_pos = lseek(fd, 0, SEEK_END))) {
            *read_pos = end_pos;
            if (end_pos != 0 
                    && file_lines(pretty_filename, fd, n_lines,
                            start_pos, end_pos, read_pos) < 0) {
                return -1;
            }
        } else {
            /* Under very unlikely circumstances, it is possible to
             * reach this point after positioning the file pointer
             * to end of file via the 'lseek(...SEEK_END)' above.
             * In that case, reposition the file pointer back to 
             * start_pos before calling pipe_lines. */
            if (start_pos != -1) {
                lseek(fd, start_pos, SEEK_SET);
            }
            return pipe_lines(pretty_filename, fd, n_lines, read_pos);
        }
    }
    return 0;
}

/* Display the last N_UNITS units of file FILENAME, open for reading
 * via FD. Set *READ_POS to the position of the input stream pointer.
 * *READ_POS is usually the number of bytes read and corresponds to
 * an offset from the beginning of a file. However, it may be larger
 * than OFF_T_MAX(as for an input pipe), and may also be larger than
 * the number of bytes read (when an input pointer initially not at
 * beginning of file), and may be far greater than the number of bytes
 * actually read for an input file that is seekable.
 * Return 0 if successful. */
static int tail(const char *filename, int fd, uintmax_t n_units,
        uintmax_t *read_pos) {
    *read_pos = 0;
    if (count_lines) {
        return tail_lines(filename, fd, n_units, read_pos);
    } else {
        return tail_bytes(filename, fd, n_units, read_pos);
    }
}

/* Display the last N_UNITS units of the file described by pfs.
 * Return 0 if successful. */
static int tail_file(struct File_spec *f, uintmax_t n_units) {
    int fd;
    int ret;
    int is_stdin = (!strcmp(f->name, "-"));
    if (is_stdin) {
        have_read_stdin = 1;
        fd = STDIN_FILENO;
    } else {
        fd = open(f->name, O_RDONLY);
    }

    f->tailable = !(reopen_inaccessible_files && fd == -1);

    if (fd == -1) {
        if (forever) {
            f->fd = -1;
            f->errnum = errno;
            f->ignore = 0;
            f->ino = 0;
            f->dev = 0;
        }
        fprintf(stderr, "cannot open %s for reading\n", pretty_name(f));
        ret = -1;
    } else {
        uintmax_t read_pos;
        if (print_headers) {
            write_header(pretty_name(f));
        }
        ret = tail(pretty_name(f), fd, n_units, &read_pos);
        if (forever) {
            struct stat stats;
            f->errnum = (ret == 0) ? 0 : -1;
            if (fstat(fd, &stats) < 0) {
                ret = -1;
                f->errnum = errno;
                fprintf(stderr, "fstat %s failed\n", 
                        pretty_name(f), strerror(errno));
            } else if (!IS_TAILABLE_FILE_TYPE(stats.st_mode)) {
                fprintf(stderr, "Cannot follow end of this type"
                        " of file; giving up on this name\n",
                        pretty_name(f));
                ret = -1;
                f->errnum = -1;
                f->ignore = 1;
            }

            if (ret != 0) {
                close_fd(fd, pretty_name(f));
                f->fd = -1;
            } else {
                /* Note: we must use read_pos here, not stats.st_size,
                 * to avoid a race condition described by Ken Raeburn:
                 * http://mail.gnu.org/archive/html/bug-textutils/
                 * 2003-05/msg00007.html */
                record_open_fd(f, fd, read_pos, &stats, 
                        (is_stdin ? -1 : 1));
            }
        } else {
            if (!is_stdin && close(fd)) {
                fprintf(stderr, "close fd of %s failed:%s\n",
                        pretty_name(f), strerror(errno));
                ret = -1;
            }
        }
    }
    return ret;
}

/* Return 1 if any of the N_FILES files in F are live, i.e., 
 * have open file descriptors. */
static int any_live_files(const struct File_spec *f, size_t n_files) {
    size_t i;
    for (i = 0; i < n_files; ++i) {
        if (0 <= f[i].fd) {
            return 1;
        }
    }
    return 0;
}

static void recheck(struct File_spec *f, int blocking) {
    /* open/fstat the file and announce if dev/ino have changed */
    struct stat new_stats;
    int ok = 1;
    int is_stdin = (!strcmp(f->name, "-"));
    int was_tailable = f->tailable;
    int prev_errnum = f->errnum;
    int new_file;
    int fd = (is_stdin ? STDIN_FILENO : 
            open(f->name, O_RDONLY | (blocking ? 0 : O_NONBLOCK)));

    assert(valid_file_spec(f));

    /* If the open fails because the file doesn't exist, then
       mark the file as not tailable. */
    f->tailable = !(reopen_inaccessible_files && fd == -1);

    if (fd == -1 || fstat(fd, &new_stats) < 0) {
        ok = 0;
        f->errnum = errno;
        if (!f->tailable) {
            if (was_tailable) {
                /* FIXME-maybe: detect the case in which the file
                   first becomes unreadable(perms), and later
                   becomes readable again and can be seen to be
                   the same file(dev/ino). Otherwise, tail prints
                   the entire contents of the file when it becomes
                   readable. */
                fprintf(stderr, "%s has become inaccessible\n",
                        pretty_name(f));
            } else {
                /* Say nothing... it's still not tailable */
            }
        } else if (prev_errnum != errno) {
            fprintf(stderr, "%s:%s\n", pretty_name(f), strerror(errno));
        }
    } else if (!IS_TAILABLE_FILE_TYPE(new_stats.st_mode)) {
        ok = 0;
        f->errnum = -1;
        fprintf(stderr, "%s has been replaced with an untailable file;"
                "giving up on this name", pretty_name(f));
        f->ignore = 1;
    } else {
        f->errnum = 0;
    }

    new_file = 0;
    if (!ok) {
        close_fd(fd, pretty_name(f));
        close_fd(f->fd, pretty_name(f));
        f->fd = -1;
    } else if (prev_errnum && prev_errnum != ENOENT) {
        new_file = 1;
        assert(f->fd == -1);
        fprintf(stderr, "%s has become accessible\n", pretty_name(f));
    } else if (f->ino != new_stats.st_ino || 
            f->dev != new_stats.st_dev) {
        new_file = 1;
        if (f->fd == -1) {
            fprintf(stderr, "%s has appeared; following end of new file",
                    pretty_name(f));
        } else {
            /* Close the old one. */
            close_fd(f->fd, pretty_name(f));

            /* File has been replaced(e.g., via log rotation) --
               tail the new one. */
            fprintf(stderr, "%s has been replaced;"
                    "following end of new file", pretty_name(f));
        }
    } else {
        if (f->fd == -1) {
            /* This happens when one iteration finds the file missing,
               then the preceding <dev, inode> pair is reused as the
               file is recreated. */
            new_file = 1;
        } else {
            close_fd(fd, pretty_name(f));
        }
    }

    if (new_file) {
        /* Start at the beginning of the file. */
        record_open_fd(f, fd, 0, &new_stats, 
                (is_stdin ? -1 : blocking));
        lseek(fd, 0, SEEK_SET);
    }
}

/* Tail N_FILES files forever, or until killed.
   The pertinent information for each file is stored
   in an entry of F. Loop over each of them, doing
   an fstat to see if they have changed size,
   and an occasional open/fstat to see if any /dev/ino
   pair has changed. If none of them have changed size
   in one iteration, sleep for a while and try again.
   Continue until the user interrupts us. */
static void tail_forever(struct File_spec *f, size_t n_files,
        double sleep_interval) {
    /* Use blocking I/O as an optimization, when it's easy. */
    int blocking = (pid == 0 && follow_mode == FOLLOW_DESCRIPTOR
            && n_files == 1 && !S_ISREG(f[0].mode));
    size_t last = n_files - 1;
    int writer_is_dead = 0;

    while (1) {
        size_t i;
        int any_input = 0;
        for (i = 0; i < n_files; ++i) {
            int fd;
            char const *name;
            mode_t  mode;
            struct stat stats;
            uintmax_t bytes_read;

            if (f[i].ignore) {
                continue;
            }

            if (f[i].fd < 0) {
                recheck(&f[i], blocking);
                continue;
            }

            fd = f[i].fd;
            name = pretty_name(&f[i]);
            mode = f[i].mode;

            if (f[i].blocking != blocking) {
                int old_flags = fcntl(fd, F_GETFL);
                int new_flags = old_flags | (blocking ? 0 : O_NONBLOCK);
                if (old_flags < 0 || 
                        (new_flags != old_flags 
                         && fcntl(fd, F_SETFL, new_flags) == -1)) {
                    /* Don't update f[i].blocking if fcntl fails. */
                    if (S_ISREG(f[i].mode) && errno == EPERM) {
                        /* This happens when using tail -f on a file
                         * with the append-only attribute. */
                        assert(0);
                    } else {
                        fprintf(stderr, 
                                "Cannot change %s nonblocking mode:%s\n",
                                name, strerror(errno));
                        exit(EXIT_FAILURE);
                    }
                } else {
                    f[i].blocking = blocking;
                }
            }

            if (!f[i].blocking) {
                if (fstat(fd, &stats) != 0) {
                    f[i].fd = -1;
                    f[i].errnum = errno;
                    fprintf(stderr, "%s:%s\n", name, strerror(errno));
                    continue;
                }

                if (f[i].mode == stats.st_mode 
                        && (!S_ISREG(stats.st_mode) 
                            || f[i].size == stats.st_size)
                        && f[i].mtime == stats.st_mtime) {
                    if ((max_n_unchanged_stats_between_opens
                                <= f[i].n_unchanged_stats++) 
                            && follow_mode == FOLLOW_NAME) {
                        recheck(&f[i], f[i].blocking);
                        f[i].n_unchanged_stats = 0;
                    }
                    continue;
                }

                /* This file has changed. Print out what we can,
                 * and then keep looping. */
                f[i].mtime = stats.st_mtime;
                f[i].mode = stats.st_mode;

                /* reset counter */
                f[i].n_unchanged_stats = 0;

                if (S_ISREG(mode) && stats.st_size < f[i].size) {
                    fprintf(stderr, "file %s truncated", name);
                    last = i;
                    lseek(fd, stats.st_size, SEEK_SET);
                    f[i].size = stats.st_size;
                    continue;
                }

                if (i != last) {
                    if (print_headers) {
                        write_header(name);
                    }
                    last = i;
                }
            }
            bytes_read = dump_remainder(name, fd, 
                    f[i].blocking ? COPY_A_BUFFER : COPY_TO_EOF);
            any_input |= (bytes_read != 0);
            f[i].size += bytes_read;
        }

        if (!any_live_files(f, n_files) 
                && !reopen_inaccessible_files) {
            fprintf(stderr, "no files remaining\n");
            break;
        }

        if ((!any_input || blocking) && fflush(stdout) != 0) {
            fprintf(stderr, "write error\n");
            exit(EXIT_FAILURE);
        }

        /* If nothing was read, sleep and/or check for dead writers. */
        if (!any_input) {
            if (writer_is_dead) {
                break;
            }

            /* Once the writer is dead, read the files once more to
             * avoid a race condition. */
            writer_is_dead = (pid != 0
                    && kill(pid, 0) != 0
                    /* Handle the case in which you cannot send a
                     * signal to the writer, so kill fails and 
                     * sets errno to EPERM. */
                    && errno != EPERM);
            if (!writer_is_dead && xnanosleep(sleep_interval)) {
                fprintf(stderr, "cannot read realtime clock\n");
                exit(EXIT_FAILURE);
            }
        }
    }
}

/* Mark as '.ignore' each member of 'pfs' that corresonds to 
 * a pipe or fifo, and return the number of non-ignored members. */
static size_t ignore_fifo_and_pipe(struct File_spec *pfs, 
        size_t n_files) {
    size_t n_viable = 0;

    size_t i;
    for (i = 0; i < n_files; ++i) {
        if (!strcmp(pfs[i].name, "-") && S_ISFIFO(pfs[i].mode)) {
            pfs[i].ignore = 1;
        } else {
            ++n_viable;
        }
    }
    return n_viable;
}

static void parse_options(int argc, char **argv, 
        uintmax_t *n_units, int *header_mode, double *sleep_interval) {
    int c;
    while ((c = getopt_long(argc, argv, "c:n:fFqs:v0123456789",
            long_options, NULL)) != -1) {
        switch (c) {
            case 'F':
                forever = 1;
                follow_mode = FOLLOW_NAME;
                reopen_inaccessible_files = 1;
                break;
            case 'c':
            case 'n':
                count_lines = (c == 'n');
                if (*optarg == '+') {
                    from_start = 1;
                } else if (*optarg == '-') {
                    ++optarg;
                }
                *n_units = strtol(optarg, NULL, 10);
                break;

            case 'f':
            case LONG_FOLLOW_OPTION:
                forever = 1;
                if (optarg == NULL) {
                    follow_mode = DEFAULT_FOLLOW_MODE;
                } else {
                    if (!strcmp(optarg, "descriptor")) {
                        follow_mode = FOLLOW_DESCRIPTOR;
                    } else if (!strcmp(optarg, "name")) {
                        follow_mode = FOLLOW_NAME;
                    } else {
                        fprintf(stderr, "Invalid option argument:%s\n",
                                optarg);
                        exit(EXIT_FAILURE);
                    }
                }
                break;
            case RETRY_OPTION:
                reopen_inaccessible_files = 1;
                break;
            case MAX_UNCHANGED_STATS_OPTION:
                /* --max-unchanged-stats=N */
                max_n_unchanged_stats_between_opens = 
                    strtol(optarg, NULL, 10);
                break;
            case PID_OPTION:
                pid = strtol(optarg, NULL, 10);
                break;
            case 'q':
                *header_mode = HM_NEVER;
                break;
            case 's':
                *sleep_interval = strtod(optarg, NULL);
                break;
            case 'v':
                *header_mode = HM_ALWAYS;
                break;
            case HELP_OPTION:
                usage(EXIT_SUCCESS);
                break;
            case VERSION_OPTION:
                printf("%s: version:%s\nAuthors:%s\n", 
                        program_name, VERSION, AUTHORS);
                exit(EXIT_SUCCESS);
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                fprintf(stderr, "option used in invalid context -- %c",
                        c);
                exit(EXIT_FAILURE);
            default:
                usage(EXIT_FAILURE);
                exit(1);
        }
    }

    if (reopen_inaccessible_files && follow_mode != FOLLOW_NAME) {
        fprintf(stderr, "warning: --retry is useful mainly when"
                " following by name");
    }

    if (pid && !forever) {
        fprintf(stderr, "warning: PID ignored; --pid=PID is useful"
                " only when following");
    } else if (pid && kill(pid, 0) != 0 && errno == ENOSYS) {
        fprintf(stderr, "warning: --pid=PID is not supported"
                " on this system");
        pid = 0;
    }
}

int main(int argc, char **argv) {
    int ret = 0;
    /* If from_start, the number of items to skip before printing;
       otherwise, the number of items at the end of the file to
       print. */
    uintmax_t n_units = DEFAULT_N_LINES;
    int header_mode = HM_MULTIPLE_FILES;
    double sleep_interval = 1.0;

    size_t n_files;
    char **file;
    struct File_spec *pfs;
    size_t i;
    /* The number of seconds to sleep between iterations.
       During one iteration, every file name or descriptor is
       checked to see if it has changed. */

    set_progname(argv[0]);

    /* Initialize the global flags. */
    have_read_stdin = 0;
    count_lines = 1;
    forever = from_start = print_headers = 0;

    parse_options(argc, argv, &n_units, &header_mode, &sleep_interval);

    /* To start printing with item N_UNITS from the start of
       the file, skip (N_UNITS - 1) items. 'tail -n +0' is 
       actually meaningless, but for Unix compatibility it's
       treated the same as 'tail -n +1'. */
    if (from_start) {
        if (n_units) {
            --n_units;
        }
    }

    if (optind < argc) {
        n_files = argc - optind;
        file = argv + optind;
    } else {
        static char *dummy_stdin = (char *)"-";
        n_files = 1;
        file = &dummy_stdin;
    }

    {
        int found_hyphen = 0;
        for (i = 0; i < n_files; ++i) {
            if (!strcmp(file[i], "-")) {
                found_hyphen = 1;
                break;
            }
        }

        /* When following by name, there must be a name. */
        if (found_hyphen && follow_mode == FOLLOW_NAME) {
            fprintf(stderr, "cannot follow \"-\" by name\n");
            exit(EXIT_FAILURE);
        }

        /* When following forever, warn if any file is '-'.
           This is only a warning, since tail's output(
           before a failing seek, and that from any non-stdin
           files) might still be useful. */
        if (forever && found_hyphen && isatty(STDIN_FILENO)) {
            fprintf(stderr, "warning: following standard input"
                    " indefinitely is ineffective");
        }
    }

    pfs = malloc(n_files * sizeof(*pfs));
    if (!pfs) {
        fprintf(stderr, "Out of memory:%s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < n_files; ++i) {
        pfs[i].name = file[i];
    }

    if (header_mode == HM_ALWAYS || 
            (header_mode == HM_MULTIPLE_FILES && n_files > 1)) {
        print_headers = 1;
    }

    for (i = 0; i < n_files; ++i) {
        ret |= tail_file(&pfs[i], n_units);
    }

    if (forever && ignore_fifo_and_pipe(pfs, n_files)) {
        tail_forever(pfs, n_files, sleep_interval);
    }

    if (have_read_stdin && close(STDIN_FILENO) < 0) {
        fprintf(stderr, "close STDIN_FILENO failed:%s\n", 
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    exit(ret != 0 ? EXIT_FAILURE : EXIT_SUCCESS);
}
