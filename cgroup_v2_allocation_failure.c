#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>

#define CGROUP_PATH "/sys/fs/cgroup"
#define TEST_CGROUP "test_reproducer"
#define TEST_CGROUP_PATH CGROUP_PATH "/" TEST_CGROUP

void write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (write(fd, val, strlen(val)) < 0) {
        fprintf(stderr, "Failed to write %s to %s: %s\n", val, path, strerror(errno));
        close(fd);
        exit(1);
    }
    close(fd);
}

int is_hugetlb_accounting_enabled() {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        perror("fopen /proc/mounts");
        return -1;
    }

    char line[1024];
    int enabled = 0;
    while (fgets(line, sizeof(line), fp)) {
        char spec[256], file[256], type[256], opts[512];
        if (sscanf(line, "%255s %255s %255s %511s", spec, file, type, opts) == 4) {
            if (strcmp(file, CGROUP_PATH) == 0 && strcmp(type, "cgroup2") == 0) {
                if (strstr(opts, "memory_hugetlb_accounting") != NULL) {
                    enabled = 1;
                }
                break;
            }
        }
    }
    fclose(fp);
    return enabled;
}

int enable_hugetlb_accounting() {
    printf("Attempting to remount cgroup2 with memory_hugetlb_accounting...\n");
    int ret = system("mount -o remount,memory_hugetlb_accounting " CGROUP_PATH);
    if (ret != 0) {
        fprintf(stderr, "Failed to remount: system() returned %d\n", ret);
        return -1;
    }
    return 0;
}

int main() {
    struct stat st;
    if (stat(CGROUP_PATH, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "cgroup v2 not mounted at %s\n", CGROUP_PATH);
        return 1;
    }

    int enabled = is_hugetlb_accounting_enabled();
    if (enabled < 0) {
        return 1;
    }
    if (!enabled) {
        if (enable_hugetlb_accounting() != 0) {
            fprintf(stderr, "Could not enable memory_hugetlb_accounting\n");
            return 1;
        }
        // Re-check
        enabled = is_hugetlb_accounting_enabled();
        if (enabled <= 0) {
            fprintf(stderr, "Failed to enable memory_hugetlb_accounting (re-check failed)\n");
            return 1;
        }
        printf("Successfully enabled memory_hugetlb_accounting\n");
    } else {
        printf("memory_hugetlb_accounting is already enabled\n");
    }

    // Enable memory controller in subtree
    int fd = open(CGROUP_PATH "/cgroup.subtree_control", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "+memory", 7) < 0) {
            // Might fail if already enabled or not supported, ignore for now
        }
        close(fd);
    }

    if (mkdir(TEST_CGROUP_PATH, 0755) != 0) {
        if (errno != EEXIST) {
            perror("mkdir test_reproducer");
            return 1;
        }
    }

    // Set memory limit to 1MB (less than 2MB hugepage)
    write_file(TEST_CGROUP_PATH "/memory.max", "1M");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        // Child
        // Move to cgroup
        write_file(TEST_CGROUP_PATH "/cgroup.procs", "0");

        printf("Child: Attempting to allocate and touch 2MB hugepage...\n");
        // Allocate 2MB hugepage
        size_t size = 2 * 1024 * 1024;
        void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (addr == MAP_FAILED) {
            perror("Child: mmap MAP_HUGETLB");
            exit(1);
        }

        printf("Child: mmap succeeded at %p, touching it now (should trigger fault)...\n", addr);
        // This should trigger the fault and call alloc_hugetlb_folio -> mem_cgroup_charge_hugetlb
        // which should fail and trigger the bug.
        *(volatile char *)addr = 1;

        printf("Child: Successfully touched page (bug not triggered?).\n");
        munmap(addr, size);
        exit(0);
    }

    // Parent
    int status;
    waitpid(pid, &status, 0);

    printf("Parent: Child exited. Cleaning up.\n");
    rmdir(TEST_CGROUP_PATH);

    if (WIFSIGNALED(status)) {
        printf("Parent: Child killed by signal %d (%s)\n",
               WTERMSIG(status), strsignal(WTERMSIG(status)));
        if (WTERMSIG(status) == SIGBUS) {
            printf("Parent: Child got SIGBUS as expected (if kernel didn't crash).\n");
        }
    } else if (WIFEXITED(status)) {
        printf("Parent: Child exited with status %d\n", WEXITSTATUS(status));
    }

    return 0;
}
