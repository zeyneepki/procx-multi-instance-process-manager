#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHM_NAME "/procx_shm"
#define SEM_NAME "/procx_sem"
#define KEY_FILE "msgfile"
#define KEY_PROJ_ID 65

#define MAX_INSTANCES 16
#define MAX_PROCESSES 50
#define CMD_MAX 256

typedef enum { MODE_ATTACHED = 0, MODE_DETACHED = 1 } ProcessMode;
typedef enum { STATUS_RUNNING = 0, STATUS_TERMINATED = 1 } ProcessStatus;

// process tablosundaki her bir girdiyi tutan yapı
typedef struct {
    pid_t pid;
    pid_t owner_pid;
    char command[CMD_MAX];
    ProcessMode mode;
    ProcessStatus status;
    time_t start_time;
    int is_active;
} ProcessInfo;

typedef struct {
    ProcessInfo processes[MAX_PROCESSES];
    int process_count;
    int initialized;

    int active_instances;
    pid_t instance_pids[MAX_INSTANCES];
} SharedData;

// message queue komutları
typedef enum { MSG_START = 1, MSG_TERMINATE = 2 } MsgCommand;

typedef struct {
    long msg_type;
    int command;
    pid_t sender_pid;
    pid_t target_pid;
} Message;

static volatile sig_atomic_t g_stop = 0;

static SharedData *g_data;
static sem_t *g_sem;
static int g_shm_fd = -1;
static int g_mq = -1;

static pthread_t g_tid_mon, g_tid_ipc;

//hata fonksiyonu
static void die(const char *m) { perror(m); exit(1); }

// ctrl+c veya sigtermde programı düzgün kapatmak için
static void on_term(int sig) { (void)sig; g_stop = 1; }

// message queue için gerekli keyfile
static void ensure_keyfile(void) {
    int fd = open(KEY_FILE, O_CREAT | O_RDWR, 0666);
    if (fd == -1) die("open msgfile");
    close(fd);
}

// semaphore kilidi aç-kapa
static void lock_sem(void) {
    while (sem_wait(g_sem) == -1) {
        if (errno == EINTR) continue;
        die("sem_wait");
    }
}
static void unlock_sem(void) {
    if (sem_post(g_sem) == -1) die("sem_post");
}
static int pid_alive(pid_t p) {
    return (p > 0 && (kill(p, 0) == 0 || errno != ESRCH));
}

//aktif instance güncelleme
static void refresh_instances_locked(void) {
    int cnt = 0;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        pid_t p = g_data->instance_pids[i];
        if (pid_alive(p)) g_data->instance_pids[cnt++] = p;
    }
    for (int i = cnt; i < MAX_INSTANCES; i++)
        g_data->instance_pids[i] = 0;

    g_data->active_instances = cnt;
}

// yeni açılan procxi shared memory listesine ekler
static void add_instance_locked(pid_t me) {
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (g_data->instance_pids[i] == me) return;

    for (int i = 0; i < MAX_INSTANCES; i++)
        if (g_data->instance_pids[i] == 0) {
            g_data->instance_pids[i] = me;
            g_data->active_instances++;
            return;
        }
}
static void remove_instance_locked(pid_t me) {
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (g_data->instance_pids[i] == me) {
            g_data->instance_pids[i] = 0;
            break;
        }
    refresh_instances_locked();
}

// shared memory içinde boş yer bulur
static int find_free_slot_locked(void) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!g_data->processes[i].is_active) return i;
    return -1;
}

// pid'e karşılık gelen tablo indexini döner
static int find_pid_slot_locked(pid_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (g_data->processes[i].is_active && g_data->processes[i].pid == pid)
            return i;
    return -1;
}

static int read_line(char *buf, size_t n) {
    if (!fgets(buf, n, stdin)) return -1;
    return 0;
}

static int read_int(const char *prompt, int *out) {
    char line[64];
    for (;;) {
        if (g_stop) return -1;
        printf("%s", prompt);
        fflush(stdout);

        if (read_line(line, sizeof(line)) != 0) return -1;

        char *end = NULL;
        long v = strtol(line, &end, 10);
        if (end == line) continue;
        *out = (int)v;
        return 0;
    }
}

// tüm procxlere bildirimi gönderme
static void send_ipc(int command, pid_t target_pid) {
    if (g_mq == -1) return;

    pid_t pids[MAX_INSTANCES];
    int n = 0;

    lock_sem();
    refresh_instances_locked();
    n = g_data->active_instances;
    if (n > MAX_INSTANCES) n = MAX_INSTANCES;

    for (int i = 0; i < n; i++) pids[i] = g_data->instance_pids[i];
    unlock_sem();

    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0) continue;

        Message m = {0};
        m.msg_type   = (long)pids[i];
        m.command    = command;
        m.sender_pid = getpid();
        m.target_pid = target_pid;

        msgsnd(g_mq, &m, sizeof(Message) - sizeof(long), IPC_NOWAIT);
    }
}

// shared memory, semaphore ve message queue başlatma
static void ipc_init(void) {
    g_shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (g_shm_fd == -1) die("shm_open");

    if (ftruncate(g_shm_fd, sizeof(SharedData)) == -1) die("ftruncate");
    g_data = (SharedData*)mmap(NULL, sizeof(SharedData),
                               PROT_READ|PROT_WRITE,
                               MAP_SHARED, g_shm_fd, 0);
    if (g_data == MAP_FAILED) die("mmap");

    g_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (g_sem == SEM_FAILED) die("sem_open");

    lock_sem();
    if (g_data->initialized != 1) {
        memset(g_data, 0, sizeof(SharedData));
        g_data->initialized = 1;
    }

    refresh_instances_locked();
    add_instance_locked(getpid());
    unlock_sem();

    ensure_keyfile();
    key_t key = ftok(KEY_FILE, KEY_PROJ_ID);
    if (key == -1) die("ftok");

    g_mq = msgget(key, 0666 | IPC_CREAT);
    if (g_mq == -1) die("msgget");
}

//diğer ProcX instancelarından gelen mesajları dinler
static void* ipc_thread(void *arg) {
    (void)arg;

    while (!g_stop) {
        Message m;
        ssize_t r = msgrcv(g_mq, &m,
                           sizeof(Message) - sizeof(long),
                           (long)getpid(),
                           IPC_NOWAIT);

        if (r == -1) {
            if (errno == ENOMSG) { usleep(150000); continue; }
            if (errno == EIDRM || errno == EINVAL) break;
            if (errno == EINTR) continue;
            usleep(150000);
            continue;
        }

        if (m.sender_pid == getpid()) continue;

        if (m.command == MSG_START)
            printf("\n[IPC] yeni process başlatıldı: PID %d\n", (int)m.target_pid);
        else if (m.command == MSG_TERMINATE)
            printf("\n[IPC] process sonlandırıldı: PID %d\n", (int)m.target_pid);

        fflush(stdout);
    }
    return NULL;
}

//processlerin ölüp ölmediğini iki saniyede bir kontrol eder
static void* monitor_thread(void *arg) {
    (void)arg;

    while (!g_stop) {
        sleep(2);

        pid_t died[MAX_PROCESSES];
        int dn = 0;

        lock_sem();
        for (int i = 0; i < MAX_PROCESSES; i++) {
            ProcessInfo *p = &g_data->processes[i];
            if (!p->is_active || p->status != STATUS_RUNNING) continue;

            int terminated = 0;

            if (p->owner_pid == getpid()) {
                int st = 0;
                if (waitpid(p->pid, &st, WNOHANG) == p->pid)
                    terminated = 1;

            } else {
                if (kill(p->pid, 0) == -1 && errno == ESRCH)
                    terminated = 1;
            }

            if (terminated) {
                pid_t dead = p->pid;

                p->status = STATUS_TERMINATED;
                p->is_active = 0;

                if (g_data->process_count > 0)
                    g_data->process_count--;

                if (dn < MAX_PROCESSES)
                    died[dn++] = dead;
            }
        }
        unlock_sem();

        for (int k = 0; k < dn; k++) {
            printf("\n[MONITOR] process %d sonlandı\n", (int)died[k]);
            fflush(stdout);
            send_ipc(MSG_TERMINATE, died[k]);
        }
    }
    return NULL;
}

// shared memoryde çalışan process listesini yazdırır
static void list_processes(void) {
    lock_sem();

    printf("\nÇALIŞAN PROGRAMLAR\n");
    printf("%-8s | %-20s | %-10s | %-8s | %-8s\n",
           "PID", "Command", "Mode", "Owner", "Süre");

    time_t now = time(NULL);
    int total = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        ProcessInfo *p = &g_data->processes[i];
        if (!p->is_active) continue;

        int sec = (int)difftime(now, p->start_time);

        char cmd_short[21];
        snprintf(cmd_short, sizeof(cmd_short), "%.20s", p->command);

        printf("%-8d | %-20s | %-10s | %-8d | %-8d\n",
               (int)p->pid,
               cmd_short,
               (p->mode == MODE_DETACHED) ? "Detached" : "Attached",
               (int)p->owner_pid,
               sec);

        total++;
    }

    printf("\ntoplam: %d process\n\n", total);
    unlock_sem();
}

static int parse_argv(char *cmdline, char ***argv_out) {
    int cap = 16, n = 0;
    char **argv = (char**)calloc(cap, sizeof(char*));
    if (!argv) return -1;

    cmdline[strcspn(cmdline, "\n")] = '\0';

    char *save = NULL;
    char *tok = strtok_r(cmdline, " ", &save);

    while (tok) {
        if (n + 1 >= cap) {
            cap *= 2;
            char **tmp = (char**)realloc(argv, cap * sizeof(char*));
            if (!tmp) { free(argv); return -1; }
            argv = tmp;
        }
        argv[n++] = tok;
        tok = strtok_r(NULL, " ", &save);
    }

    argv[n] = NULL;
    if (n == 0) { free(argv); return -1; }

    *argv_out = argv;
    return 0;
}

// yeni process başlatma
static void start_program(void) {
    char cmdline[CMD_MAX];
    int mode_int = 0;

    printf("çalıştırılacak komutu girin: ");
    fflush(stdout);

    if (read_line(cmdline, sizeof(cmdline)) != 0)
        return;

    cmdline[strcspn(cmdline, "\n")] = '\0';

    int num = 0;
    if (!(sscanf(cmdline, "sleep %d", &num) == 1 && num > 0)) {
        printf("[HATA] Geçersiz format. Sadece 'sleep <saniye>' şeklinde girin.\n");
        return;
    }

    if (read_int("mod seçin (0: attached, 1: detached): ", &mode_int) != 0)
        return;

    ProcessMode mode = (mode_int == 1) ? MODE_DETACHED : MODE_ATTACHED;

    char cmd_copy[CMD_MAX];
    strncpy(cmd_copy, cmdline, CMD_MAX-1);
    cmd_copy[CMD_MAX-1] = '\0';

    char **argv = NULL;
    if (parse_argv(cmd_copy, &argv) != 0) return;

    pid_t pid = fork();
    if (pid == -1) {
        free(argv);
        die("fork");
    }

    if (pid == 0) {
        if (mode == MODE_DETACHED) {
            setsid();
            int dn = open("/dev/null", O_RDWR);
            if (dn != -1) {
                dup2(dn, 0);
                dup2(dn, 1);
                dup2(dn, 2);
                if (dn > 2) close(dn);
            }
        }

        execvp(argv[0], argv);
        _exit(1);
    }

    lock_sem();
    int slot = find_free_slot_locked();
    if (slot == -1) {
        unlock_sem();
        free(argv);
        printf("liste dolu\n");
        return;
    }

    ProcessInfo *p = &g_data->processes[slot];
    p->pid = pid;
    p->owner_pid = getpid();

    strncpy(p->command, cmdline, CMD_MAX-1);

    p->mode = mode;
    p->status = STATUS_RUNNING;
    p->start_time = time(NULL);
    p->is_active = 1;
    g_data->process_count++;

    unlock_sem();

    printf("[SUCCESS] process %d başlatıldı.\n", (int)pid);
    send_ipc(MSG_START, pid);

    free(argv);
}

// program sonlandirma
static void terminate_program(void) {
    char line[64];
    char *end = NULL;

    printf("sonlandırılacak process pid: ");
    fflush(stdout);

    if (read_line(line, sizeof(line)) != 0)
        return;

    long v = strtol(line, &end, 10);
    if (end == line) {
        printf("[HATA] Geçersiz PID.\n");
        return;
    }

    while (*end == ' ' || *end == '\t' || *end == '\n') end++;
    if (*end != '\0') {
        printf("[HATA] Geçersiz PID.\n");
        return;
    }

    pid_t pid = (pid_t)v;

    lock_sem();
    int idx = find_pid_slot_locked(pid);
    unlock_sem();

    if (idx == -1) {
        printf("[HATA] Böyle bir PID yok.\n");
        return;
    }

    kill(pid, SIGTERM);

    lock_sem();
    int s = find_pid_slot_locked(pid);
    if (s != -1) {
        g_data->processes[s].is_active = 0;
        g_data->processes[s].status = STATUS_TERMINATED;
        if (g_data->process_count > 0)
            g_data->process_count--;
    }
    unlock_sem();

    printf("[INFO] process %d sonlandırıldı.\n", (int)pid);
    send_ipc(MSG_TERMINATE, pid);
}

// procx kapanırken tüm kaynaklar temizlenir
static void cleanup(void) {
    int is_last = 0;

    pid_t to_reap[MAX_PROCESSES];
    int reap_n = 0;

    lock_sem();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        ProcessInfo *p = &g_data->processes[i];
        if (!p->is_active) continue;

        if (p->owner_pid == getpid() && p->mode == MODE_ATTACHED) {
            kill(p->pid, SIGTERM);
            if (reap_n < MAX_PROCESSES)
                to_reap[reap_n++] = p->pid;

            p->is_active = 0;
            p->status = STATUS_TERMINATED;
            if (g_data->process_count > 0) g_data->process_count--;
        }
    }

    refresh_instances_locked();
    remove_instance_locked(getpid());
    refresh_instances_locked();

    if (g_data->active_instances == 0)
        is_last = 1;

    unlock_sem();

    for (int i = 0; i < reap_n; i++) {
        pid_t c = to_reap[i];
        if (c <= 0) continue;

        int collected = 0;

        for (int t = 0; t < 10; t++) {
            int st = 0;
            pid_t r = waitpid(c, &st, WNOHANG);
            if (r == c) { collected = 1; break; }
            if (r == 0) { usleep(100000); continue; }
            if (r == -1) { collected = 1; break; }
        }

        if (!collected) {
            kill(c, SIGKILL);
            waitpid(c, NULL, 0);
        }
    }

    if (g_data) munmap(g_data, sizeof(SharedData));
    if (g_shm_fd != -1) close(g_shm_fd);
    if (g_sem && g_sem != SEM_FAILED) sem_close(g_sem);

    if (is_last) {
        if (g_mq != -1) msgctl(g_mq, IPC_RMID, NULL);
        sem_unlink(SEM_NAME);
        shm_unlink(SHM_NAME);
    }
}

static void print_menu(void) {
    printf("\n");
    printf("+-----------------------------+\n");
    printf("|          ProcX v1.0         |\n");
    printf("+-----------------------------+\n");
    printf("|  1. Yeni Program Çalıştır   |\n");
    printf("|  2. Çalışan Programları Listele |\n");
    printf("|  3. Program Sonlandır       |\n");
    printf("|  0. Çıkış                   |\n");
    printf("+-----------------------------+\n\n");
    printf("Seçiminiz: ");
    fflush(stdout);
}

int main(void) {
    ipc_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (pthread_create(&g_tid_ipc, NULL, ipc_thread, NULL) != 0)
        die("pthread_create ipc");

    if (pthread_create(&g_tid_mon, NULL, monitor_thread, NULL) != 0)
        die("pthread_create monitor");

    while (!g_stop) {
        print_menu();

        char line[64];
        if (read_line(line, sizeof(line)) != 0) {
            g_stop = 1;
            break;
        }

        int choice = (int)strtol(line, NULL, 10);

        if      (choice == 1) start_program();
        else if (choice == 2) list_processes();
        else if (choice == 3) terminate_program();
        else if (choice == 0) {
            printf("\n[INFO] procx kapandı.\n");
            fflush(stdout);
            g_stop = 1;
            break;
        }
        else printf("geçersiz seçim.\n");
    }

    g_stop = 1;
    pthread_join(g_tid_mon, NULL);
    pthread_join(g_tid_ipc, NULL);

    cleanup();
    return 0;
}
