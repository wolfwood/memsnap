#include<stdio.h>
#include<signal.h>
#include<unistd.h>
#include<stdlib.h>
#include<stdbool.h>
#include<limits.h>
#include<fcntl.h>
#include<inttypes.h>
#include<semaphore.h>
#include<getopt.h>

#include<sys/ptrace.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<sys/mman.h>
#include<sys/stat.h>
#define __USE_POSIX199309
#include<time.h>
#include<sys/time.h>

#include "region_list.h"

/* memsnap defines */
#define BUFFER_SIZE 4096  /* Must align with system page size */

struct piditem
{
    pid_t pid;
    struct piditem * next;
};
struct piditem * head;

void print_usage();
void alrm_hdlr(int useless);
void err_msg(char * msg);
void ptrace_all_pids(int cmd);
void free_pid_list(struct piditem * ele);

/* External data */
extern char * optarg;
extern int optind;      /* Index to first non-arg parameter */

/* I love the smell of global variables at 5 in the morning. */
struct region_list * rl;
struct region_list * cur;

/* More globals */
int snap = 1;
timer_t timer;
struct itimerspec t;
sem_t sem;

/* Options */
bool OPT_H = false;
bool OPT_T = false;
bool OPT_M = false;
bool OPT_U = false;
bool OPT_P = false;
bool OPT_S = false;
bool OPT_G = false;
bool OPT_D = false;
bool OPT_F = false;
bool OPT_L = false;

int termsnap;
int interval;

void print_usage()
{
    fprintf(stderr, "Usage: memsnap [options] -p pid\n");
    fprintf(stderr, "\t-h Print usage\n");
    fprintf(stderr, "\t-p <pid> Attach to <pid>\n");
    fprintf(stderr, "\t-t <sec> Specify time interval between snapshots in seconds\n");
    fprintf(stderr, "\t-m <ms> Specify time interval between snapshots in milliseconds\n");
    fprintf(stderr, "\t-u <us> Specify time interval between snapshots in microseconds\n");
    fprintf(stderr, "\t-f <snaps> Finish after taking <snaps> number of snapshots\n");
    fprintf(stderr, "\t-l Snap live, without pausing the process being snapshotted\n");
}

int main(int argc, char * argv[])
{
    struct piditem * curitem;
    /* piditem setup */
    head = calloc(1, sizeof(struct piditem));
    head->next = NULL;

    /* Timer setup */
    timer_create(CLOCK_MONOTONIC, NULL, &timer);
    t.it_value.tv_sec = 1;
    t.it_value.tv_nsec = 0;
    t.it_interval.tv_sec = 0;
    t.it_interval.tv_nsec = 0;

    /* Argument parsing */
    char opt;
    char * strerr = NULL;
    long arg;
    while((opt = getopt(argc, argv, "+ht:m:u:p:sglad:f:")) != -1)
    {
        switch(opt)
        {
            case 'p':
                OPT_P = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                    err_msg("Unable to parse -p argument correctly, should be a pid\n\n");
                curitem = head;
                while(curitem->next != NULL)
                    curitem = curitem->next; // Go to last list item
                curitem->pid = (pid_t) arg;
                curitem->next = calloc(1, sizeof(struct piditem));
                curitem = curitem->next;
                curitem->next = NULL;
                optarg = NULL;
                break;
            case 't':
            case 'm':
            case 'u':
                if(OPT_T || OPT_M || OPT_U)
                    err_msg("-t -m -u mutally exclusive\nPlease specify only one\n\n");
                if(opt == 't')
                    OPT_T = true;
                else if(opt == 'm')
                    OPT_M = true;
                else
                    OPT_U = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                {
                    if(opt == 't')
                        err_msg("Unable to parse -t argument correctly, should be number of seconds\n\n");
                    else if(opt == 'm')
                        err_msg("Unable to parse -m argument correctly, should be number of milliseconds\n\n");
                    else
                        err_msg("Unable to parse -u argument correctly, should be number of microseconds\n\n");
                }
                if(opt == 'm')
                {
                    t.it_value.tv_nsec = (arg % 1000) * 1000000;
                    t.it_value.tv_sec = arg / 1000; /* truncates per Section 2.5 of K&R 2nd ed */
                }
                else if(opt == 'u')
                {
                    t.it_value.tv_nsec = (arg % 1000000) * 1000;
                    t.it_value.tv_sec = arg / 1000000;
                }
                else
                    t.it_value.tv_sec = arg;
                optarg = NULL;
                break;
            case 'f':
                if(OPT_F)
                    err_msg("-f specified two or more times\n\n");
                OPT_F = true;
                arg = strtol(optarg, &strerr, 10);
                if(strerr[0] != 0)
                    err_msg("Unable to parse -f argument correctly, should be number of snapshots\n\n");
                if(arg < 0)
                    err_msg("Number of snapshots specified for -f argument is negative.\n\n");
                termsnap = arg;
                optarg = NULL;
                break;
            case 'l':
                OPT_L = true;
                break;
            case 'h':
            default:
                print_usage();
                return 0;
        }
    }
    /* Option validity checks */
    if(!OPT_P)
        err_msg("memsnap requires a pid\n\n");
    //exit(0); // Option testing

    sem_init(&sem, 0, 0);

    // Call the handler to set up the signal handling and post to the sem for the first time
    alrm_hdlr(0);

    /* Timer set */
    timer_settime(timer, 0, &t, NULL);

retry_sem:
    while(sem_wait(&sem) == 0)
    {
        ptrace_all_pids(PTRACE_ATTACH);
        curitem = head;
        while(curitem->next != NULL)
        {
            pid_t pid;
            int i, j, chk;
            char * buffer;
            int mem_fd;
            int seg_fd;
            int seg_len;
            off_t offset;

            pid = curitem->pid;

            buffer = calloc(1, 4096);
            snprintf(buffer, 24, "%s%d%s", "/proc/", (int) pid, "/mem");

            mem_fd = open(buffer, O_RDONLY);
            rl = new_region_list(pid, RL_FLAG_RWANON);
            cur = rl;
            for(i=0; cur != NULL; i++)
            {
                seg_len = (int)((intptr_t) cur->end - (intptr_t) cur->begin);
                snprintf(buffer, 4096, "%s%d%s%d%s%d", "pid", pid, "_snap", snap, "_seg", i);
                seg_fd = open(buffer, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);

                offset = 0;
                lseek(mem_fd, (intptr_t) cur->begin, SEEK_SET);
                for(j=0; j<seg_len; j+=BUFFER_SIZE)
                {
                    offset = read(mem_fd, buffer, BUFFER_SIZE);
                    err_chk(offset == -1);
                    while(offset != BUFFER_SIZE)
                    {
                        chk = read(mem_fd, buffer + offset, BUFFER_SIZE - offset);
                        err_chk(chk == -1);
                        offset += chk;
                    }
                    offset = write(seg_fd, buffer, BUFFER_SIZE);
                    err_chk(offset == -1);
                    while(offset != BUFFER_SIZE)
                    {
                        chk = write(seg_fd, buffer + offset, BUFFER_SIZE - offset);
                        err_chk(chk == -1);
                        offset += chk;
                    }
                }

                close(seg_fd);

                cur = cur->next;
            }
            close(mem_fd);
            free_region_list(rl);
            printf("snap: %d, pid: %d\n", snap, pid);
            curitem = curitem->next;
        }
        timer_settime(timer, 0, &t, NULL);

        ptrace_all_pids(PTRACE_DETACH);

        if(OPT_F && snap == termsnap)
            return 0;
        snap++;
    }
    goto retry_sem; // Yes, yes, I know.

    free_pid_list(head);

    return 0;

err:
    perror("main");
    ptrace_all_pids(PTRACE_DETACH);
    return -1;
}

void alrm_hdlr(int __attribute__((unused)) useless)
{
    signal(SIGALRM, &alrm_hdlr);

    sem_post(&sem);
    return;
}

void err_msg(char * msg)
{
    fprintf(stderr, msg);
    print_usage();
    exit(-1);
}

void ptrace_all_pids(int cmd)
{
    int status;
    if(OPT_L)
        return;
    struct piditem * cur = head;
    while(cur->next != NULL)
    {
        ptrace(cmd, cur->pid, NULL, NULL);
        if(cmd == PTRACE_ATTACH)
            wait(&status);
        cur = cur->next;
    }
}

/* Frees all piditems subsequent in the list */
void free_pid_list(struct piditem * ele)
{
    if(ele->next != NULL)
        free_pid_list(ele->next);
    free(ele->next);
    ele->next = NULL;
}
