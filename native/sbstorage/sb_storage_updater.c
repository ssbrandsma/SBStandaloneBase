#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LEB_SIZE 129024LL
#define FIXED_LEBS 258LL

static const char *states[] = {
    "idle", "backup_written", "backup_verified", "updater_armed",
    "destructive_started", "production_removed", "production_created",
    "image_written", "restore_started", "restore_verified", "complete"
};

static int state_index(const char *state) {
    size_t i;
    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++)
        if (!strcmp(state, states[i])) return (int)i;
    return -1;
}

static int read_state(const char *path, char *state, size_t size) {
    FILE *f = fopen(path, "r");
    char line[256];
    int schema = 0, found = 0;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!strncmp(line, "schema=", 7)) schema = atoi(line + 7);
        if (!strncmp(line, "state=", 6)) {
            snprintf(state, size, "%s", line + 6);
            found = 1;
        }
    }
    fclose(f);
    return schema == 1 && found && state_index(state) >= 0 ? 0 : -1;
}

static int write_state(const char *path, const char *state) {
    char tmp[512];
    FILE *f;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    f = fopen(tmp, "w");
    if (!f) return -1;
    if (fprintf(f, "schema=1\nstate=%s\n", state) < 0 || fflush(f) || fsync(fileno(f))) {
        fclose(f); unlink(tmp); return -1;
    }
    if (fclose(f) || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

static int plan(int argc, char **argv) {
    long long total, available, production, current_backup, backup, reserve, user_budget, target;
    if (argc != 8) return 64;
    total = atoll(argv[2]); available = atoll(argv[3]); production = atoll(argv[4]);
    current_backup = atoll(argv[5]); backup = atoll(argv[6]); reserve = atoll(argv[7]);
    if (total <= 0 || available < 0 || production <= 0 || current_backup < 0 || backup <= 0 || reserve < 0)
        return 65;
    user_budget = FIXED_LEBS + available + production + current_backup;
    target = user_budget - FIXED_LEBS - backup - reserve;
    printf("schema=1\nmode=offline_plan\ntotal_lebs=%lld\nfixed_lebs=%lld\n"
           "user_budget_lebs=%lld\navailable_lebs=%lld\nproduction_lebs=%lld\n"
           "current_backup_lebs=%lld\nbackup_lebs=%lld\n"
           "reserve_lebs=%lld\nmax_target_lebs=%lld\nmax_target_bytes=%lld\n"
           "destructive_execution_enabled=false\nearly_boot_environment_required=true\n",
           total, FIXED_LEBS, user_budget, available, production, current_backup, backup, reserve, target,
           target > 0 ? target * LEB_SIZE : 0);
    return target > 0 && FIXED_LEBS + backup + reserve + target <= total ? 0 : 2;
}

int main(int argc, char **argv) {
    char state[64];
    int current, next;
    if (argc >= 2 && !strcmp(argv[1], "plan")) return plan(argc, argv);
    if (argc == 3 && !strcmp(argv[1], "status")) {
        if (read_state(argv[2], state, sizeof(state))) {
            printf("schema=1\nvalid=false\nreason=invalid_or_missing_state\n");
            return 2;
        }
        printf("schema=1\nvalid=true\nstate=%s\ndestructive_execution_enabled=false\n", state);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "advance")) {
        if (!getenv("SB_STORAGE_SIMULATION")) {
            fprintf(stderr, "state transitions are enabled only in offline simulation\n");
            return 78;
        }
        if (read_state(argv[2], state, sizeof(state)) || strcmp(state, argv[3])) return 3;
        current = state_index(argv[3]); next = state_index(argv[4]);
        if (current < 0 || next != current + 1) return 4;
        if (write_state(argv[2], argv[4])) { perror("state update"); return 74; }
        printf("schema=1\ntransitioned=true\nstate=%s\n", argv[4]);
        return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "execute") || !strcmp(argv[1], "install"))) {
        fprintf(stderr, "physical migration is disabled pending an approved early-boot environment\n");
        return 78;
    }
    fprintf(stderr, "usage: sb-storage-updater plan TOTAL AVAILABLE PRODUCTION CURRENT_BACKUP TARGET_BACKUP RESERVE | status FILE | advance FILE EXPECTED NEXT | execute\n");
    return 64;
}
