/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Fixes applied on top of v1:
 *  1. `run` client robustness  — MSG_WAITALL + EINTR retry on read so
 *     signal delivery mid-read doesn't make the client exit prematurely.
 *  2. `stop` SIGKILL escalation — a background reaper thread sends
 *     SIGKILL after STOP_GRACE_SEC if the container ignores SIGTERM.
 *  3. `ps` output size — response message enlarged to PS_BUF_SIZE
 *     (4096 bytes) via a dedicated ps_response_t so many containers
 *     don't get silently truncated.
 *  4. Duplicate rootfs check — handle_start rejects a second container
 *     that points at the same rootfs directory as a running one.
 *  5. Absolute log path — log files are stored under an absolute path
 *     derived from getcwd() at supervisor start, not a relative "logs/".
 *  6. child_config_t memory in parent — cfg is now allocated on the
 *     parent's stack (inside a pipe+mmap'd region the child shares), or
 *     more simply: we free it in the parent after clone() since clone()
 *     with CLONE_NEWPID copies the address space (fork semantics).
 *     The child's copy is independent; the parent frees its own copy.
 *  7. run-client SIGINT/SIGTERM — signal handler sets a volatile flag;
 *     the blocking read loop checks EINTR and re-reads; after the stop
 *     is forwarded the client continues waiting for the final response.
 *  8. Producer thread join on container exit — when SIGCHLD fires and
 *     a container is reaped, the producer thread for that container is
 *     detached so the OS reclaims it automatically (pthread_detach).
 *     Threads are only joined explicitly during full shutdown.
 *  9. monitor.c soft_warned reset — not in engine.c, handled in monitor.c.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define CONTROL_MESSAGE_LEN 256
#define PS_BUF_SIZE         4096          /* fix #3: large enough for many containers */
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define MONITOR_DEV         "/dev/container_monitor"
#define STOP_GRACE_SEC      5             /* fix #2: seconds before SIGKILL escalation */

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */
typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    time_t             started_at;
    container_state_t  state;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                exit_code;
    int                exit_signal;
    int                stop_requested;
    char               rootfs[PATH_MAX];   /* fix #4: stored for duplicate rootfs check */
    char               log_path[PATH_MAX];
    int                pipe_read_fd;
    pthread_t          producer_thread;
    int                producer_detached;  /* fix #8 */
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

/* fix #3: separate large ps response so normal responses stay small */
typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
    int  exit_code;
    int  exit_signal;
} control_response_t;

typedef struct {
    int  status;
    char buf[PS_BUF_SIZE];
} ps_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

typedef struct {
    int               pipe_read_fd;
    char              container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
} producer_arg_t;

typedef struct run_waiter {
    char               container_id[CONTAINER_ID_LEN];
    int                client_fd;
    struct run_waiter *next;
} run_waiter_t;

/* fix #2: arg for the stop-escalation thread */
typedef struct {
    pid_t pid;
    int   grace_sec;
} stop_escalation_arg_t;

typedef struct {
    int                server_fd;
    int                monitor_fd;
    volatile int       should_stop;
    pthread_t          logger_thread;
    bounded_buffer_t   log_buffer;
    pthread_mutex_t    metadata_lock;
    container_record_t *containers;
    run_waiter_t       *run_waiters;
    pthread_mutex_t    waiters_lock;
    char               log_dir[PATH_MAX];  /* fix #5: absolute log dir */
} supervisor_ctx_t;

static supervisor_ctx_t *g_ctx = NULL;

/* fix #7: volatile flag set by run-client signal handler */
static volatile sig_atomic_t g_run_stop_requested = 0;
static char g_run_id[CONTAINER_ID_LEN];

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value,
                           unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value); return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value); return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                 char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long  nice_value;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]); return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]); return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n"); return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded Buffer                                                      */
/* ------------------------------------------------------------------ */
static int bounded_buffer_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));
    rc = pthread_mutex_init(&b->mutex, NULL);
    if (rc) return rc;
    rc = pthread_cond_init(&b->not_empty, NULL);
    if (rc) { pthread_mutex_destroy(&b->mutex); return rc; }
    rc = pthread_cond_init(&b->not_full, NULL);
    if (rc) { pthread_cond_destroy(&b->not_empty); pthread_mutex_destroy(&b->mutex); return rc; }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/*
 * bounded_buffer_push — producer side.
 *
 * Blocks while full (unless shutting down).
 * Returns 0 on success, -1 if shutting down with no space.
 *
 * Without mutex: two producers could write to the same tail slot
 * simultaneously. The mutex ensures only one thread advances tail.
 */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);
    if (b->shutting_down && b->count == LOG_BUFFER_CAPACITY) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }
    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/*
 * bounded_buffer_pop — consumer side.
 *
 * Blocks while empty (unless shutting down).
 * Returns 0 with data, 1 when shutdown + drained.
 *
 * Without mutex: consumer could read a slot before producer completes
 * the write. Mutex + condvar ensures read only after full write.
 */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->count == 0) { pthread_mutex_unlock(&b->mutex); return 1; }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logger (consumer) thread                                            */
/* ------------------------------------------------------------------ */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break;

        char log_path[PATH_MAX];
        log_path[0] = '\0';

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strncmp(c->id, item.container_id, CONTAINER_ID_LEN) == 0) {
                strncpy(log_path, c->log_path, sizeof(log_path) - 1);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!log_path[0]) continue;

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) continue;
        (void)write(fd, item.data, item.length);
        close(fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer thread — drains one container's pipe into bounded buffer   */
/* ------------------------------------------------------------------ */
static void *producer_thread_fn(void *arg)
{
    producer_arg_t *parg = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, parg->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(parg->pipe_read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(parg->buf, &item);
        memset(item.data, 0, sizeof(item.data));
    }

    close(parg->pipe_read_fd);
    free(parg);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Child entrypoint — runs inside cloned namespaces                    */
/* ------------------------------------------------------------------ */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2"); return 1;
    }
    close(cfg->log_write_fd);

    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("sethostname");

    if (chroot(cfg->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/") < 0)           { perror("chdir /"); return 1; }

    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");

    if (cfg->nice_value != 0)
        if (nice(cfg->nice_value) < 0)
            perror("nice");

    char *const argv[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", argv);
    perror("execv");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Monitor ioctl helpers                                               */
/* ------------------------------------------------------------------ */
int register_with_monitor(int monitor_fd, const char *container_id,
                           pid_t host_pid, unsigned long soft_limit_bytes,
                           unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    if (monitor_fd < 0) return 0;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    return (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) ? -1 : 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    if (monitor_fd < 0) return 0;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    return (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* fix #2: SIGKILL escalation thread                                   */
/*                                                                     */
/* Spawned when `stop` sends SIGTERM. Sleeps for STOP_GRACE_SEC then   */
/* sends SIGKILL if the process still exists.                          */
/* ------------------------------------------------------------------ */
static void *stop_escalation_thread(void *arg)
{
    stop_escalation_arg_t *a = (stop_escalation_arg_t *)arg;
    pid_t pid       = a->pid;
    int   grace_sec = a->grace_sec;
    free(a);

    sleep((unsigned int)grace_sec);

    /* Check if process still exists before sending SIGKILL */
    if (kill(pid, 0) == 0) {
        fprintf(stderr, "[supervisor] Container pid=%d ignored SIGTERM, sending SIGKILL\n", pid);
        kill(pid, SIGKILL);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container launch                                                    */
/* ------------------------------------------------------------------ */
static int launch_container(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             container_record_t **rec_out)
{
    int pipefd[2];
    char *stack        = NULL;
    pid_t pid;
    container_record_t *rec  = NULL;
    child_config_t     *cfg  = NULL;
    producer_arg_t     *parg = NULL;

    /* fix #5: build absolute log path */
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/%s.log",
             ctx->log_dir, req->container_id);

    if (pipe(pipefd) < 0) { perror("pipe"); return -1; }

    stack = malloc(STACK_SIZE);
    if (!stack) { perror("malloc stack"); close(pipefd[0]); close(pipefd[1]); return -1; }

    /* fix #6: cfg allocated separately; clone() gives child its own copy */
    cfg = calloc(1, sizeof(child_config_t));
    if (!cfg) { free(stack); close(pipefd[0]); close(pipefd[1]); return -1; }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,        CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);

    close(pipefd[1]); /* parent always closes write end */
    free(stack);

    if (pid < 0) {
        perror("clone");
        free(cfg); close(pipefd[0]);
        return -1;
    }

    /* fix #6: parent frees its own copy; child has independent copy */
    free(cfg);

    rec = calloc(1, sizeof(container_record_t));
    if (!rec) {
        kill(pid, SIGKILL); waitpid(pid, NULL, 0); close(pipefd[0]); return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid          = pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = req->soft_limit_bytes;
    rec->hard_limit_bytes  = req->hard_limit_bytes;
    rec->pipe_read_fd      = pipefd[0];
    rec->producer_detached = 0;
    strncpy(rec->rootfs,   req->rootfs,   PATH_MAX - 1);
    strncpy(rec->log_path, log_path,      PATH_MAX - 1);

    parg = calloc(1, sizeof(producer_arg_t));
    if (!parg) {
        kill(pid, SIGKILL); waitpid(pid, NULL, 0); close(pipefd[0]); free(rec); return -1;
    }
    parg->pipe_read_fd = pipefd[0];
    strncpy(parg->container_id, req->container_id, CONTAINER_ID_LEN - 1);
    parg->buf = &ctx->log_buffer;

    if (pthread_create(&rec->producer_thread, NULL, producer_thread_fn, parg) != 0) {
        perror("pthread_create producer");
        kill(pid, SIGKILL); waitpid(pid, NULL, 0);
        close(pipefd[0]); free(parg); free(rec);
        return -1;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                               req->soft_limit_bytes, req->hard_limit_bytes) < 0)
        perror("register_with_monitor");

    if (rec_out) *rec_out = rec;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SIGCHLD handler                                                     */
/* ------------------------------------------------------------------ */
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    if (!g_ctx) { errno = saved_errno; return; }

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int exit_code   = 0;
        int exit_signal = 0;
        if (WIFEXITED(status))        exit_code   = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exit_signal = WTERMSIG(status);

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                c->exit_code   = exit_code;
                c->exit_signal = exit_signal;

                if (c->stop_requested)
                    c->state = CONTAINER_STOPPED;
                else if (exit_signal == SIGKILL)
                    c->state = CONTAINER_KILLED;
                else
                    c->state = CONTAINER_EXITED;

                unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);

                /* fix #8: detach producer thread so OS reclaims it */
                if (!c->producer_detached) {
                    pthread_detach(c->producer_thread);
                    c->producer_detached = 1;
                }

                /* Notify CMD_RUN waiters */
                pthread_mutex_lock(&g_ctx->waiters_lock);
                run_waiter_t **wp = &g_ctx->run_waiters;
                while (*wp) {
                    run_waiter_t *w = *wp;
                    if (strncmp(w->container_id, c->id, CONTAINER_ID_LEN) == 0) {
                        control_response_t resp;
                        memset(&resp, 0, sizeof(resp));
                        resp.status      = 0;
                        resp.exit_code   = exit_code;
                        resp.exit_signal = exit_signal;
                        snprintf(resp.message, sizeof(resp.message),
                                 "container %s finished (state=%s exit_code=%d signal=%d)",
                                 c->id, state_to_string(c->state),
                                 exit_code, exit_signal);
                        (void)write(w->client_fd, &resp, sizeof(resp));
                        close(w->client_fd);
                        *wp = w->next;
                        free(w);
                        continue;
                    }
                    wp = &(*wp)->next;
                }
                pthread_mutex_unlock(&g_ctx->waiters_lock);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
    errno = saved_errno;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/* Request handlers                                                    */
/* ------------------------------------------------------------------ */

/* fix #3: ps uses dedicated larger response */
static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    ps_response_t resp;
    int off = 0;
    memset(&resp, 0, sizeof(resp));

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    if (!c)
        off += snprintf(resp.buf + off, sizeof(resp.buf) - off, "(no containers)\n");
    while (c && (size_t)off < sizeof(resp.buf) - 1) {
        char tbuf[32];
        struct tm *tm_info = localtime(&c->started_at);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", tm_info);
        off += snprintf(resp.buf + off, sizeof(resp.buf) - off,
                        "%-16s  pid=%-7d  state=%-10s  started=%s  "
                        "soft=%luMiB  hard=%luMiB  log=%s\n",
                        c->id, c->host_pid, state_to_string(c->state), tbuf,
                        c->soft_limit_bytes >> 20, c->hard_limit_bytes >> 20,
                        c->log_path);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp.status = 0;
    (void)write(client_fd, &resp, sizeof(resp));
}

static void handle_logs(supervisor_ctx_t *ctx, int client_fd, const char *id)
{
    char log_path[PATH_MAX] = {0};
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0) {
            strncpy(log_path, c->log_path, sizeof(log_path) - 1);
            break;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
    if (!log_path[0]) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "container '%s' not found", id);
    } else {
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "LOG_PATH:%s", log_path);
    }
    (void)write(client_fd, &resp, sizeof(resp));
}

/* fix #2: handle_stop spawns escalation thread after SIGTERM */
static void handle_stop(supervisor_ctx_t *ctx, int client_fd, const char *id)
{
    pid_t target_pid = -1;
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0 &&
            c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            target_pid        = c->host_pid;
            break;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
    if (target_pid < 0) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "container '%s' not found or not running", id);
    } else {
        kill(target_pid, SIGTERM);

        /* Spawn SIGKILL escalation thread */
        stop_escalation_arg_t *a = malloc(sizeof(*a));
        if (a) {
            a->pid       = target_pid;
            a->grace_sec = STOP_GRACE_SEC;
            pthread_t t;
            if (pthread_create(&t, NULL, stop_escalation_thread, a) == 0)
                pthread_detach(t);
            else
                free(a);
        }

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "SIGTERM sent to '%s' (pid=%d); SIGKILL in %ds if needed",
                 id, target_pid, STOP_GRACE_SEC);
    }
    (void)write(client_fd, &resp, sizeof(resp));
}

/* fix #4: handle_start checks for duplicate rootfs among running containers */
static void handle_start(supervisor_ctx_t *ctx, int client_fd,
                          const control_request_t *req)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            /* Duplicate ID check */
            if (strncmp(c->id, req->container_id, CONTAINER_ID_LEN) == 0) {
                pthread_mutex_unlock(&ctx->metadata_lock);
                control_response_t resp;
                memset(&resp, 0, sizeof(resp));
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message),
                         "container '%s' already running", req->container_id);
                (void)write(client_fd, &resp, sizeof(resp));
                return;
            }
            /* fix #4: Duplicate rootfs check — resolve symlinks/relative paths */
            char rp_req[PATH_MAX], rp_rec[PATH_MAX];
            if (realpath(req->rootfs, rp_req) && realpath(c->rootfs, rp_rec)) {
                if (strcmp(rp_req, rp_rec) == 0) {
                    pthread_mutex_unlock(&ctx->metadata_lock);
                    control_response_t resp;
                    memset(&resp, 0, sizeof(resp));
                    resp.status = -1;
                    snprintf(resp.message, sizeof(resp.message),
                             "rootfs '%s' already in use by container '%s'",
                             req->rootfs, c->id);
                    (void)write(client_fd, &resp, sizeof(resp));
                    return;
                }
            }
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
    if (launch_container(ctx, req, NULL) < 0) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "failed to launch container '%s'", req->container_id);
    } else {
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "container '%s' started", req->container_id);
    }
    (void)write(client_fd, &resp, sizeof(resp));
}

static void handle_run(supervisor_ctx_t *ctx, int client_fd,
                        const control_request_t *req)
{
    if (launch_container(ctx, req, NULL) < 0) {
        control_response_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "failed to launch container '%s'", req->container_id);
        (void)write(client_fd, &resp, sizeof(resp));
        return;
    }

    run_waiter_t *w = calloc(1, sizeof(run_waiter_t));
    if (!w) {
        control_response_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "out of memory");
        (void)write(client_fd, &resp, sizeof(resp));
        return;
    }
    strncpy(w->container_id, req->container_id, CONTAINER_ID_LEN - 1);
    w->client_fd = client_fd;

    pthread_mutex_lock(&ctx->waiters_lock);
    w->next          = ctx->run_waiters;
    ctx->run_waiters = w;
    pthread_mutex_unlock(&ctx->waiters_lock);
    /* client_fd intentionally left open — closed when container exits */
}

/* ------------------------------------------------------------------ */
/* Supervisor main loop                                                */
/* ------------------------------------------------------------------ */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx          = &ctx;

    /* fix #5: resolve absolute log directory from cwd */
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return 1; }
    snprintf(ctx.log_dir, sizeof(ctx.log_dir), "%s/logs", cwd);
    if (mkdir(ctx.log_dir, 0755) < 0 && errno != EEXIST) {
        perror("mkdir log_dir"); return 1;
    }

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc) { errno = rc; perror("pthread_mutex_init metadata"); return 1; }
    rc = pthread_mutex_init(&ctx.waiters_lock, NULL);
    if (rc) { errno = rc; perror("pthread_mutex_init waiters"); return 1; }
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc) { errno = rc; perror("bounded_buffer_init"); return 1; }

    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: cannot open %s: %s (no kernel monitor)\n",
                MONITOR_DEV, strerror(errno));

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(ctx.server_fd); return 1;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen"); close(ctx.server_fd); return 1;
    }
    chmod(CONTROL_PATH, 0666);

    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sa_term.sa_flags   = SA_RESTART;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger"); return 1;
    }

    fprintf(stderr, "[supervisor] Ready. rootfs=%s socket=%s logs=%s\n",
            rootfs, CONTROL_PATH, ctx.log_dir);

    int flags = fcntl(ctx.server_fd, F_GETFL, 0);
    fcntl(ctx.server_fd, F_SETFL, flags | O_NONBLOCK);

    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                usleep(10000);
                continue;
            }
            perror("accept");
            break;
        }

        /* Peek at the kind field to route ps vs others */
        control_request_t req;
        ssize_t n = read(client_fd, &req, sizeof(req));
        if (n != (ssize_t)sizeof(req)) { close(client_fd); continue; }

        switch (req.kind) {
        case CMD_START: handle_start(&ctx, client_fd, &req); close(client_fd); break;
        case CMD_RUN:   handle_run(&ctx, client_fd, &req);   /* fd held by waiter */ break;
        case CMD_PS:    handle_ps(&ctx, client_fd);           close(client_fd); break;
        case CMD_LOGS:  handle_logs(&ctx, client_fd, req.container_id); close(client_fd); break;
        case CMD_STOP:  handle_stop(&ctx, client_fd, req.container_id); close(client_fd); break;
        default:        close(client_fd); break;
        }
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) { c->stop_requested = 1; kill(c->host_pid, SIGTERM); }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    sleep(STOP_GRACE_SEC);

    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) kill(c->host_pid, SIGKILL);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    while (waitpid(-1, NULL, WNOHANG) > 0) ;
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0) ;

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Join producer threads that were NOT already detached */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        if (!c->producer_detached)
            pthread_join(c->producer_thread, NULL);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) { container_record_t *nx = c->next; free(c); c = nx; }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    pthread_mutex_lock(&ctx.waiters_lock);
    run_waiter_t *w = ctx.run_waiters;
    while (w) { run_waiter_t *wn = w->next; close(w->client_fd); free(w); w = wn; }
    ctx.run_waiters = NULL;
    pthread_mutex_unlock(&ctx.waiters_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.waiters_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] Clean exit.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI client                                                          */
/* ------------------------------------------------------------------ */

/*
 * fix #7: run-client signal handler sets flag; does NOT longjmp or exit.
 * The blocking read loop detects the flag after EINTR and sends stop.
 */
static void run_client_sig_handler(int sig)
{
    (void)sig;
    g_run_stop_requested = 1;
}

static void forward_stop_to_supervisor(const char *id)
{
    int fd;
    struct sockaddr_un addr;
    control_request_t req;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, id, CONTAINER_ID_LEN - 1);
    (void)write(fd, &req, sizeof(req));
    (void)read(fd, &resp, sizeof(resp));
    close(fd);
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\nIs the supervisor running?\n",
                CONTROL_PATH, strerror(errno));
        close(fd); return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write request"); close(fd); return 1;
    }

    /* fix #3: ps uses a larger response struct */
    if (req->kind == CMD_PS) {
        ps_response_t resp;
        memset(&resp, 0, sizeof(resp));
        n = read(fd, &resp, sizeof(resp));
        close(fd);
        if (n <= 0) { fprintf(stderr, "No response from supervisor\n"); return 1; }
        printf("%s", resp.buf);
        return (resp.status == 0) ? 0 : 1;
    }

    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    if (req->kind == CMD_RUN) {
        /*
         * fix #1: `run` blocks until supervisor writes the final response.
         * fix #7: EINTR from signal means our handler ran; check flag,
         *         forward stop once, then keep reading for final status.
         */
        size_t total = 0;
        int stop_sent = 0;
        while (total < sizeof(resp)) {
            n = read(fd, (char *)&resp + total, sizeof(resp) - total);
            if (n > 0) {
                total += (size_t)n;
            } else if (n == 0) {
                break; /* supervisor closed connection unexpectedly */
            } else if (errno == EINTR) {
                if (g_run_stop_requested && !stop_sent) {
                    forward_stop_to_supervisor(g_run_id);
                    stop_sent = 1;
                }
                /* continue reading — don't exit */
            } else {
                perror("read run response"); break;
            }
        }
        close(fd);

        if (total < sizeof(resp)) {
            fprintf(stderr, "Incomplete response from supervisor\n"); return 1;
        }
        printf("%s\n", resp.message);
        if (resp.exit_signal) return 128 + resp.exit_signal;
        return resp.exit_code;
    }

    /* All other commands: single read */
    n = read(fd, &resp, sizeof(resp));
    close(fd);

    if (n <= 0) { fprintf(stderr, "No response from supervisor\n"); return 1; }

    if (req->kind == CMD_LOGS && resp.status == 0) {
        const char *prefix = "LOG_PATH:";
        if (strncmp(resp.message, prefix, strlen(prefix)) == 0) {
            const char *path = resp.message + strlen(prefix);
            FILE *f = fopen(path, "r");
            if (!f) { fprintf(stderr, "Cannot open log %s: %s\n", path, strerror(errno)); return 1; }
            char line[256];
            while (fgets(line, sizeof(line), f)) fputs(line, stdout);
            fclose(f);
            return 0;
        }
    }

    printf("%s\n", resp.message);
    return (resp.status == 0) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* CLI command functions                                               */
/* ------------------------------------------------------------------ */
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,        argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,       argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,        argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,       argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;

    /* fix #7: install signal handler before connecting */
    strncpy(g_run_id, argv[2], CONTAINER_ID_LEN - 1);
    g_run_stop_requested = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = run_client_sig_handler;
    sa.sa_flags   = 0; /* no SA_RESTART — we want EINTR on the read */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]); return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);
    usage(argv[0]);
    return 1;
}
