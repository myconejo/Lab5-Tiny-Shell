 //tiny shell program with job control
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline)
{
    char *argv[MAXARGS]; 	// list of arguments
    char buf[MAXLINE]; 		// command line
    pid_t pid; 			// process ID
    int jid; 			// job ID
    int bg;                     // bg = 1 when & is the last character (see parseline)
    sigset_t set; 		// set of blocked signals

    /*
     * In eval, the parent must intially block SIGCHLD so...
     * at child process (fork() = 0), SIGCHLD is unblocked before execve so blocked can be inherited.
     * at parent process (fork() > 0), SIGCHLD is unblocked after addjob() so deletejob() is not prior to addjob().
    */

    // create an empty signal set 'set' and add signal number (SIGCHLD).
    sigemptyset(&set); 		// create an empty set
    sigaddset(&set, SIGCHLD); 	// add signal number to the set
    strcpy(buf, cmdline); 	// copy the string into buf
    bg = parseline(buf, argv); 	// adding child process to the jobs list as BG?

    // empty lines are ignored.
    if (argv[0] == NULL) return;

    // if a built-in command is given, then do as builtin_cmd()
    // if an argument is not a built-in command (Ex: /bin/ls, ./myspin, ...)
    if (!builtin_cmd(argv))
    {
        // 1) block SIGCHLD before forking
        sigprocmask(SIG_BLOCK, &set, NULL);
	    
	// 2) fork to create a child process
	pid = fork();

        // fork error (fork() = -1)
        if (pid < 0)
        {
            unix_error("fork error");
            return;
        }
        
	// 3) child process (fork() = 0)
        if (pid == 0)
        {
            // setpgid() so future children of this process join the new process group
            if (setpgid(0,0) < 0) unix_error("setpigd error");
	
	    // unblock the SIGCHLD before execv for signal inheritance
            sigprocmask(SIG_UNBLOCK, &set, NULL);
            
	    // run by execve()
            if (execvp(argv[0], argv) < 0) // error when there is no such command
            {
                printf("%s: Command not found\n" , argv[0]);
                exit(0); // terminate the process
            }
        }

        // 4) parent process (fork() = pid_child)
        addjob(jobs, pid, bg ? BG : FG, cmdline); // add pid to the jobs list as BG if bg, FG otherwise. 
        sigprocmask(SIG_UNBLOCK, &set, NULL); // unblock the SIGCHLD after addjob()
	if (!bg) {waitfg(pid);} // Parent process waits until FG process to be finished.
        else
        {
            jid = pid2jid(pid); // get JID
            printf("[%d] (%d) %s", jid, pid, cmdline); // print BG process
        }
    }
    return;
}
/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv)
{
    // Built-in commands include: quit, jobs, bg, fg
    char *arg1 = argv[0];
    if (strcmp(arg1, "quit") == 0) {
        exit(0); // exit from the shell.
    }
    else {
        if (strcmp(arg1, "&") == 0) {
            return 1; // ignore singleton.
        }
        else if (strcmp(arg1, "jobs") == 0) {
            listjobs(jobs); // show the list of running commands.
            return 1;
        }
        else if (strcmp(arg1, "bg") == 0) {
            do_bgfg(argv); // change job/process into BG.
            return 1;
        }
        else if (strcmp(arg1, "fg") == 0) {
            do_bgfg(argv); // change job/process into FG.
            return 1;
        }
    }
    return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv)
/* due to parseline, argv[k] are words separated by space
 * Ex) fg %1
 * argv[0] = arg1 = 'fg'
 * argv[1] = arg2 = %1
 * arg2[0] = '%' // thus we can know that arg2 refers to JID
*/ 
{
    struct job_t *do_job; // job for do_bgfg
    char *arg1 = argv[0]; // 1st argument
    char *arg2 = argv[1]; // 2nd argument

    // Check is arg2 isn't missing
    if (arg2 == NULL)
    {
	printf("%s command requires PID or %%jobid argument\n", arg1);
	return;
    }

    int isJID = (arg2[0] == '%'); // JID starts with %
    int isPID = ((arg2[0] >= 0x30) && (arg2[0] <= 0x39)); // PID starts with integer
    
    int i = 1;
    while(arg2[i] > 0x20) // checks all characters in the arg2, breaks when <space> is present.
    {
	// printf("%d\n", arg2[i]);
	if ((arg2[i] < 0x30) || (arg2[i] > 0x39))
        {
	    isJID = 0; // invalid PID
	    isPID = 0; // invalid JID
	}
	i++; 
    }

    // if given command has PID
    if (isPID)
    {
        // get the job of the called pid
        pid_t pid = (pid_t)(atoi(arg2));
        do_job = getjobpid(jobs, pid);
        // check if there's such job
        if (do_job == NULL)
        {
            printf("(%d): No such process\n", (int)pid);
            return;
        }
    }
    // if given command has JID
    else if (isJID)
    {
        // get the job of the called jid
        int jid = atoi(&arg2[1]);
        do_job = getjobjid(jobs, jid);
        if (do_job == NULL)
        {
            printf("%s: No such job\n", arg2);
            return;
        }
    }
    // neither JID nor PID
    else
    {
        printf("%s: argument must be a PID or %%jobid\n", arg1);
        return;
    }

    // change the state according to arg1
    if (strcmp(arg1, "bg") == 0)
    {
        (*do_job).state = BG; // change the job into BG
        printf("[%d] (%d) %s", (*do_job).jid, (*do_job).pid, (*do_job).cmdline); // print BG
	/*
	 * int kill (pid_t pid, int sig);
	 * sends signal 'sig' to the process (pid)
	 * send SIGCONT so that the process can continue after its state has been changed. 
	*/
        kill(-(*do_job).pid, SIGCONT); // sends SIGCONT to continue as BG process.
    }
    else if (strcmp(arg1, "fg") == 0)
    {
        (*do_job).state = FG; // change the job into FG
        kill(-(*do_job).pid, SIGCONT); // sends SIGCONT to continue as FG process.
        waitfg((*do_job).pid); // wait until pid (now FG) is finished. 
    }
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    // in waitfg, wait only with sleep(), and let sigchld_handler do the reaping. 
    if (!pid) return; // is pid valid?
    struct job_t *job = getjobpid(jobs, pid); // fetch the job of pid
    while ((*job).state == FG) {sleep(1);} // LOOOOOOOOOOOP when pid is FG
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig)
{
    pid_t pid_chld;
    int jid_chld;
    int status;
    sigset_t mask_all; // Mask with all signals
    sigset_t prev_all; // Mask with previous blocked[]

    sigfillset(&mask_all); // Add every signal number so we can block all signals

    // WNOHANG: return 0 if no child in the wait set is terminated or stopped
    // WUNTRACED: return pid if any child in wait set is signaled or stopped
    while((pid_chld = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0)
    {
        // child terminated normally, WIFEXITED = 1
        if (WIFEXITED(status))
        {
	    sigprocmask(SIG_BLOCK, &mask_all, &prev_all); // Synchronize by blocking all signals to avoid races
            deletejob(jobs, pid_chld); // delete the child process
	    sigprocmask(SIG_SETMASK, &prev_all, NULL); // restore previous blocked[]
        }
        // child terminated by signal. WIFSIGNALED = 1
        else if (WIFSIGNALED(status))
        {
	    sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
            jid_chld = pid2jid(pid_chld);
   	    deletejob(jobs, pid_chld); // delete the child process
	    printf("Job [%d] (%d) terminated by signal %d\n", jid_chld, (int)pid_chld, WTERMSIG(status));
	    sigprocmask(SIG_SETMASK, &prev_all, NULL);
        }
        // if stop signal arrived to child, WIFSTOPPED = 1 (distinguish stopped and terminated childs)
        else if (WIFSTOPPED(status))
        {
	    sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
            jid_chld = pid2jid(pid_chld); // get jid
            (*getjobpid(jobs, pid_chld)).state = ST; // set the state as STOPPED
            printf("Job [%d] (%d) stopped by signal %d\n", jid_chld, (int)pid_chld, WSTOPSIG(status));
	    sigprocmask(SIG_SETMASK, &prev_all, NULL);
        }
    }
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig)
{
    pid_t pid_fg = fgpid(jobs); // current FG process in the jobs list
    if (pid_fg != 0) kill(-pid_fg, sig); // SIGINT sent to FG process group
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig)
{
    pid_t pid_fg = fgpid(jobs); // current FG process in the jobs list
    if (pid_fg != 0) kill(-pid_fg, sig); // SIGTSTP sent to FG process group
    return;
}
/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}




