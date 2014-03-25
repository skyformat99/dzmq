#include <unistd.h>
#include <execinfo.h>
#include <string.h>
#include "dzcommon.h"
#include "dzlog.h"
#include "dzutil.h"
#include "dzbroker.h"

extern struct setting settings;

static void settings_init(const char *broker_name, const bool log);

static void settings_init(const char *broker_name, const bool log) {
    char temp[32] = "";
    sprintf(temp, "./log/%s.log", broker_name);
    strcpy(settings.log_name, temp);
    settings.log = log;
}

void segfault_sigaction(int signal, siginfo_t *si, void *arg) {
    void *array[20];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 20);

    // print out all the frames to stderr
    fprintf(stderr, "Error: signal %d:\n", signal);
    backtrace_symbols_fd(array, size, STDERR_FILENO);

    exit(0);
}


void test_broker(int argc, char **argv) {
    int verbose = 0;
    if (argc < 2) {
        printf ("syntax: main me {you}...\n");
        return;
    }
    const char *local = argv[1];
    int rlen = argc - 2;
    char **remote = (char **)malloc(rlen * sizeof(char *));
    /*memcpy(remote, argv+2*sizeof(char *), rlen * sizeof(char *));*/
    memcpy(remote, argv+2, rlen * sizeof(char *));

    dz_broker *broker = dz_broker_new(local, remote, rlen);
    dz_broker_sim_worker(broker, NBR_WORKERS, verbose);
    dz_broker_sim_client(broker, NBR_CLIENTS, verbose);
    dz_broker_main_loop_mdp(broker);

    free(remote);
    dz_broker_destory(&broker);
}

int main(int argc, char **argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = segfault_sigaction;
    sa.sa_flags   = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    _init_path = getcwd(NULL, 0);

    int c;
    const char *optstr = "l:h";
    while (-1 != (c = getopt(argc, argv, optstr)) ) {
        switch(c) {
            case 'l':
                settings.log = true;
                strcpy(settings.log_name, optarg);
                break;
            case 'h':
                printf("Useage .\n");
                exit(1);
            default:
                fprintf(stderr, "Illegal argument\n");
                return 1;
        }
    }

    if (argc < 2) {
        printf ("syntax: main me {you}...\n");
        return 0;
    }
    const char *broker_name = argv[1];
    settings_init(broker_name, false);
    if (settings.log) {
        const char *log_path = "./log";
        if (is_dir(log_path) != 1) {
            if (mk_dir(log_path) != 1) {
                LOG_PRINT(LOG_ERROR, "log_path[%s] Create Failed!", log_path);
                return -1;
            }
        }
        log_init();
    }

    srandom ((unsigned) time (NULL));
    test_broker(argc, argv);
    LOG_PRINT(LOG_INFO, "main return %s", _init_path);
}
