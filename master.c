#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* pid_t fork(); */
/* int wait(int *); */
/* int ftruncate(int, size_t); */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
/* int shm_open(const char* name, int oflag, mode_t mode); */
/* int shm_unlink(const char *name); */
/* void *mmap(void *, size_t, int prot, int flags, int fd, off_t); */
/* int munmap(void *, size_t); */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
/* sem_t; */
#include <semaphore.h>
/* time_t; */
#include <time.h>

#define MAX_LINE_LEN 512
#define MAX_CHILDREN 20
#define MAX_EXECUTION_TIME_IN_SEC 200
#define CHILD_ARGS_COUNT 5

/**
 * Simple structure to store the numbers
 * in a linked list. removes the
 * necessity to read the whole file twice.
 */
typedef struct number_node_t {
  int number;
  struct number_node_t *next;
} number_node;

/**
 * Global variables to be used at different stages of
 * the program like signal handler.
 */
const char *shared_number_buffer_name =
    "/numbers-2342342";   // shared memory block path for numbers
int numbers_fd = -1;      // shared numbers file descriptor
int *numbers = NULL;      // shared numbers array
size_t number_count = 0;  // total integers to be stored

const char *shared_pids_name =
    "/pids-234234";               // child pid buffer to keep track of children
int pids_fd = -1;                 // shared child pid file descriptor
pid_t *pids = NULL;               // shared child pids
size_t pid_count = MAX_CHILDREN;  // total children spawned or active plus
                                  // parent and timer process holder

const char *shared_sem_name = "/sem-234234235";  // shared semaphore name
int sem_fd = -1;                                 // shared sem file descriptor
sem_t *sem = NULL;                               // shared semaphore memory

FILE *log_fp = NULL;  // log file pointer
pid_t parent_pid = -1;
char **child_args = NULL;
number_node *numlist =
    NULL;  // a copy of numbers in a linked list, not shared with children

/**
 * Creates and adds a new node to the list.
 * If root argument is NULL a new node is created
 * and returned.
 */
number_node *add_node(number_node *root, int number) {
  number_node *node = (number_node *)malloc(sizeof(struct number_node_t));
  number_node *cur = NULL;
  if (node == NULL) {
    perror("could not allocate memory for list");
    exit(EXIT_FAILURE);
  }
  node->number = number;
  node->next = NULL;
  if (root == NULL) {
    return node;
  }
  cur = root;
  while (cur->next) {
    cur = cur->next;
  }
  cur->next = node;
  return root;
}

/**
 * Frees up the allocated list.
 */
void free_nodes(number_node *root) {
  number_node *cur = root;
  number_node *tmp;
  while (cur) {
    tmp = cur->next;
    free(cur);
    cur = tmp;
  }
}

/**
 * Counts and returns the size of node list
 */
size_t count_nodes(number_node *root) {
  size_t rv = 0;
  number_node *cur = root;
  while (cur) {
    cur = cur->next;
    rv++;
  }
  return rv;
}

/**
 * Help menu.
 * Prints a simple usage help to stdout.
 */
void help_menu(char **argv) {
  printf("Usage: %s OPTIONS\n", argv[0]);
  printf("\n");
  printf("OPTIONS -\n");
  printf(
      "\t-i input file path that contains the numbers. one number "
      "in each line.\n");
  printf("\t-h print this menu and exit\n");
  printf("\n");
  exit(EXIT_SUCCESS);
}

/**
 * Function to allocate shared memories.
 */
void allocate_shared_memory() {
  if ((numbers_fd = shm_open(shared_number_buffer_name, O_RDWR | O_CREAT,
                             S_IRUSR | S_IWUSR)) < 0) {
    perror("could not open shared numbers memory file");
    exit(EXIT_FAILURE);
  }
  if (ftruncate(numbers_fd, sizeof(int) * number_count) < 0) {
    perror("could not allocate shared numbers memory");
    exit(EXIT_FAILURE);
  }
  if ((numbers =
           (int *)mmap(NULL, sizeof(int) * number_count, PROT_READ | PROT_WRITE,
                       MAP_SHARED, numbers_fd, 0)) == MAP_FAILED) {
    perror("could not map shared numbers memory");
    exit(EXIT_FAILURE);
  }

  if ((pids_fd = shm_open(shared_pids_name, O_RDWR | O_CREAT,
                          S_IRUSR | S_IWUSR)) < 0) {
    munmap(numbers, sizeof(int) * number_count);
    shm_unlink(shared_number_buffer_name);
    perror("could not open shared child pid memory");
    exit(EXIT_FAILURE);
  }
  if (ftruncate(pids_fd, sizeof(pid_t) * pid_count) < 0) {
    munmap(numbers, sizeof(int) * number_count);
    shm_unlink(shared_number_buffer_name);
    perror("could not allocate shared child pid memory");
    exit(EXIT_FAILURE);
  }
  if ((pids = (pid_t *)mmap(NULL, sizeof(pid_t) * pid_count,
                            PROT_READ | PROT_WRITE, MAP_SHARED, pids_fd, 0)) ==
      MAP_FAILED) {
    munmap(numbers, sizeof(int) * number_count);
    shm_unlink(shared_number_buffer_name);
    perror("could not map shared child pid memory");
    exit(EXIT_FAILURE);
  }
  for (int i = 0; i < pid_count; i++) pids[i] = -1;

  if ((sem_fd = shm_open(shared_sem_name, O_RDWR | O_CREAT,
                         S_IRUSR | S_IWUSR)) < 0) {
    munmap(numbers, sizeof(int) * number_count);
    shm_unlink(shared_number_buffer_name);
    munmap(pids, sizeof(pid_t) * pid_count);
    shm_unlink(shared_pids_name);
    perror("could not open shared semaphore memory");
    exit(EXIT_FAILURE);
  }
  if (ftruncate(sem_fd, sizeof(sem_t)) < 0) {
    munmap(numbers, sizeof(int) * number_count);
    shm_unlink(shared_number_buffer_name);
    munmap(pids, sizeof(pid_t) * pid_count);
    shm_unlink(shared_pids_name);
    perror("could not allocate shared semaphore memory");
    exit(EXIT_FAILURE);
  }
  if ((sem = (sem_t *)mmap(NULL, sizeof(sem_t), PROT_READ | PROT_WRITE,
                           MAP_SHARED, sem_fd, 0)) == MAP_FAILED) {
    munmap(numbers, sizeof(int) * number_count);
    shm_unlink(shared_number_buffer_name);
    munmap(pids, sizeof(pid_t) * pid_count);
    shm_unlink(shared_pids_name);
    perror("could not map shared semaphore memory");
    exit(EXIT_FAILURE);
  }
  // initialize semaphore to 1
  if (sem_init(sem, 1, 1) < 0) {
    munmap(numbers, sizeof(int) * number_count);
    shm_unlink(shared_number_buffer_name);
    munmap(pids, sizeof(pid_t) * pid_count);
    shm_unlink(shared_pids_name);
    munmap(sem, sizeof(sem_t));
    shm_unlink(shared_sem_name);
    perror("could not initialize semaphore");
    exit(EXIT_FAILURE);
  }
}

/**
 * Frees up shared memories.
 *
 * Additionaly frees the local numbers list.
 */
static void cleanup_memories(int childproc) {
  munmap(numbers, sizeof(int) * number_count);
  munmap(pids, sizeof(pid_t) * pid_count);
  munmap(sem, sizeof(sem_t));

  // if it is called from child process
  // we want to avoid unlinking shared memories
  // as they may be needed in other processes.
  // only unlinking the kernel paths when it is
  // called from parent process.
  if (!childproc) {
    sem_destroy(sem);  // destroy the semaphore too
    shm_unlink(shared_number_buffer_name);
    shm_unlink(shared_pids_name);
    shm_unlink(shared_sem_name);

    /* close file handlers */
    if (log_fp != NULL) {
      fflush(log_fp);
      fclose(log_fp);
    }
  }
  /* free up numbers list */
  free_nodes(numlist);
  if (child_args != NULL) {
    for (int i = 0; i < (CHILD_ARGS_COUNT - 1); i++) {
      free(child_args[i]);
    }
    free(child_args);
  }
}

/**
 * Signal handler function.
 *
 * When a signal is thrown, this will be called as
 * the interrupt/signal handler.
 */
static void signal_handler(int sig) {
  pid_t pid = getpid();
  fprintf(stderr, "pid: %d, signal caught. exiting ...\n", pid);
  fflush(stderr);

  if (pid == parent_pid) {
    // kill all running child process
    mlock(pids, sizeof(int) * pid_count);
    for (int i = 0; i < pid_count; i++) {
      if (pids[i] > 0) {
        fprintf(stderr, "sending kill signal to child %d\n", pids[i]);
        fflush(stderr);
        kill((pid_t)pids[i], SIGKILL);
      }
    }
    munlock(pids, sizeof(int) * pid_count);

    sleep(1);
    // clean up memories and file handles
    cleanup_memories(0);
  } else {
    cleanup_memories(1);
  }
  exit(EXIT_SUCCESS);
}

/**
 * Initializes signal handlers
 */
void init_signal_handlers() {
  struct sigaction sig_action;
  sig_action.sa_handler = signal_handler;
  sig_action.sa_flags = 0;
  sigemptyset(&sig_action.sa_mask);

  sigaction(SIGINT, &sig_action, NULL);
}

int main(int argc, char **argv) {
  /* local variables */
  char *input_filepath = NULL;
  char *line = NULL;
  char *number_buffer = NULL;
  int number = 0;
  FILE *input_fp = NULL;
  number_node *tmp = NULL;
  size_t count = 0;
  struct timespec start_time, algo_start_time, algo_end_time;
  int cur_segment_count = 0;
  int cur_segment_index = 0;
  int segment_size = 2;
  int cur_segment_size = 0;
  int result_found = 0;
  int numbers_left = 0;
  int child_count = 0;
  int child_stat = 0;
  time_t seed;
  int wait_sec = 0;

  /* parse arguments */
  if (argc < 2) {
    help_menu(argv);
  }
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0) {
      help_menu(argv);
    } else if (strcmp(argv[i], "-i") == 0) {
      input_filepath = argv[++i];
    }
  }

  /* collect starting time */
  clock_gettime(CLOCK_REALTIME, &start_time);

  /* read the input file */
  if ((line = (char *)malloc(MAX_LINE_LEN)) == NULL) {
    perror("could not allocate memory for input buffer");
    exit(EXIT_FAILURE);
  }
  for (int i = 0; i < MAX_LINE_LEN; i++) line[i] = 0;
  if ((number_buffer = (char *)malloc(MAX_LINE_LEN)) == NULL) {
    perror("could not allocate memory for number buffer");
    free(line);
    exit(EXIT_FAILURE);
  }
  for (int i = 0; i < MAX_LINE_LEN; i++) number_buffer[i] = 0;
  if (input_filepath != NULL &&
      (input_fp = fopen(input_filepath, "r")) == NULL) {
    free(line);
    free(number_buffer);
    perror("could not open input file to read from");
    exit(EXIT_FAILURE);
  }
  while (fgets(line, MAX_LINE_LEN, input_fp) != NULL) {
    // remove leading and trailing whitespace from the line
    int start_pos = 0;
    int end_pos = strlen(line);
    for (; start_pos < end_pos; start_pos++) {
      if (!isspace(line[start_pos])) break;
    }
    end_pos--;
    for (; end_pos >= 0; end_pos--) {
      if (!isspace(line[end_pos])) {
        end_pos++;
        break;
      }
    }
    // check for empty line, invalid char
    if (start_pos == end_pos || end_pos < 0 || !isdigit(line[start_pos])) {
      //   printf("empty line\n");
      continue;
    }
    // copy the number part from line to number buffer
    // and add a terminating null char for number conversion simplicity
    memcpy(number_buffer, &line[start_pos], (size_t)(end_pos - start_pos));
    number_buffer[end_pos] = 0;

    number = atoi(number_buffer);
    numlist = add_node(numlist, number);
    printf("%d\n", number);  // print the number
  }
  number_count = count_nodes(numlist);
  printf("Total %ld numbers are read.\n", number_count);

  /* close file pointer and free up memories */
  fclose(input_fp);
  free(number_buffer);
  free(line);

  /* allocate child argument buffer */
  if ((child_args = (char **)malloc(sizeof(char *) * CHILD_ARGS_COUNT)) ==
      NULL) {
    perror("could not allocate child argument array");
    exit(EXIT_FAILURE);
  }
  for (int i = 0; i < (CHILD_ARGS_COUNT - 1); i++) {
    child_args[i] = (char *)malloc(sizeof(char) * 32);
    child_args[i][0] = '\0';
  }
  sprintf(child_args[0], "./bin_adder");
  sprintf(child_args[3], "%ld", number_count);
  child_args[4] = NULL;

  /* open log file */
  if ((log_fp = fopen("adder_log.log", "w")) == NULL) {
    free_nodes(numlist);
    perror("could not open log file to write");
    exit(EXIT_FAILURE);
  }
  printf("Log file initialized.\n");

  /* initialize singal handlers */
  parent_pid = getpid();  // cache parent pid for all children
  fprintf(log_fp, "Parent process id: %d\n", getpid());
  fprintf(log_fp, "Input file: %s\n", input_filepath);
  fprintf(log_fp, "Total numbers found: %ld\n", number_count);
  fflush(log_fp);
  init_signal_handlers();

  /* allocate shared memory */
  allocate_shared_memory();
  fprintf(log_fp, "Shared memories allocated.\n");
  fflush(log_fp);

  /*  run binary adder. in every iteration the
      numbers are divided into N/2 groups and summed up in
      different processes. result of a segment will be written in the
      first position of the respective segment by the child process.
   */

  /* print some info to output streams about the current algorithm */
  fprintf(log_fp, "Starting summation using binary addition algorithm.\n");
  fflush(log_fp);
  printf("Starting summation using binary addition algorithm.\n");
  clock_gettime(CLOCK_REALTIME, &algo_start_time);

  /* copy numbers to the shared memory */
  count = 0;
  tmp = numlist;
  while (tmp) {
    numbers[count++] = tmp->number;
    tmp = tmp->next;
  }
  numbers_left = number_count;

  /* divide and iterate each group until the result is found */
  while (!result_found) {
    cur_segment_count =
        (numbers_left % 2) ? ((numbers_left / 2) + 1) : (numbers_left / 2);
    cur_segment_index = 0;

    // log current number of segments
    sem_wait(sem);
    fprintf(log_fp, "Current segment count: %d\n", cur_segment_count);
    fflush(log_fp);
    sem_post(sem);

    // fork cur_segment_count number of children
    child_count = 0;
    seed = time(NULL);
    srand((unsigned int)seed);
    for (int i = 0; i < cur_segment_count; i++) {
      // calculate segment size, can be 2 or 1
      cur_segment_size =
          ((cur_segment_index + segment_size) > numbers_left) ? 1 : 2;

      wait_sec = rand() % 4;

      // fork a child
      if (fork() == 0) {
        // store child pid
        pid_t pid = getpid();
        mlock(pids, sizeof(pid_t) * MAX_CHILDREN);
        for (int i = 0; i < MAX_CHILDREN; i++) {
          if (pids[i] < 0) {
            pids[i] = pid;
            break;
          }
        }
        munlock(pids, sizeof(pid_t) * MAX_CHILDREN);

        // log child id
        sem_wait(sem);
        // wait for random seconds (between 0-3)
        sleep(wait_sec);
        fprintf(log_fp, "Child PID: %d, Index: %d, Size: %d, Slept: %d sec\n",
                pid, cur_segment_index, cur_segment_size, wait_sec);
        fflush(log_fp);
        sem_post(sem);

        // prepare child arguments
        sprintf(child_args[1], "%d", cur_segment_index);
        sprintf(child_args[2], "%d", cur_segment_size);

        // exec
        execv("./bin_adder", child_args);

        // exit child process
        cleanup_memories(1);
        exit(EXIT_SUCCESS);
      }

      child_count++;
      cur_segment_index += cur_segment_size;

      // if maximum number of children are already created
      // wait for a child to finish up
      if (child_count > MAX_CHILDREN) {
        pid_t child_pid = wait(&child_stat);
        child_count--;

        mlock(pids, sizeof(pid_t) * MAX_CHILDREN);
        for (int i = 0; i < MAX_CHILDREN; i++) {
          if (pids[i] == child_pid) {
            pids[i] = -1;
            break;
          }
        }
        munlock(pids, sizeof(pid_t) * MAX_CHILDREN);
      }
    }

    // wait for currently active adders to finish up
    pid_t child_pid;
    while ((child_pid = wait(&child_stat)) > 0) {
      printf("Child PID: %d is done working.\n", child_pid);
      sem_wait(sem);
      fprintf(log_fp, "Child PID: %d is done working.\n", child_pid);
      fflush(log_fp);
      sem_post(sem);
      // mark the child as done
      mlock(pids, sizeof(pid_t) * MAX_CHILDREN);
      for (int i = 0; i < MAX_CHILDREN; i++) {
        if (pids[i] == child_pid) {
          pids[i] = -1;
          break;
        }
      }
      munlock(pids, sizeof(pid_t) * MAX_CHILDREN);
    }

    // collect results and remove the distances between them
    mlock(numbers, sizeof(int) * number_count);
    cur_segment_index = 0;  // using this for indexing purpose, avoiding
                            // creation of a new variable
    for (int i = 0; i < cur_segment_count; i++) {
      numbers[cur_segment_index++] = numbers[i * segment_size];
    }
    munlock(numbers, sizeof(int) * number_count);

    // update the numbers left to be summed up
    numbers_left = cur_segment_count;

    // break the loop if numbers left is 1
    if (numbers_left == 1) {
      result_found = 1;

      clock_gettime(CLOCK_REALTIME, &algo_end_time);

      // print out the result
      printf("Result: %d using binary addition.\n", numbers[0]);
      printf("Algorithm took %ld sec.\n",
             algo_end_time.tv_sec - algo_start_time.tv_sec);
      sem_wait(sem);
      fprintf(log_fp, "Result: %d using binary addition.\n", numbers[0]);
      fprintf(log_fp, "Algorithm took %ld sec.\n",
              algo_end_time.tv_sec - algo_start_time.tv_sec);
      fflush(log_fp);
      sem_post(sem);
    }
  }

  // check if execution time is larger than the max duration specified
  if ((algo_end_time.tv_sec - start_time.tv_sec) >= MAX_EXECUTION_TIME_IN_SEC) {
    printf("Execution took longer than %d sec, took %ld sec. Exiting.\n",
           MAX_EXECUTION_TIME_IN_SEC,
           (algo_end_time.tv_sec - start_time.tv_sec));
    sem_wait(sem);
    fprintf(
        log_fp, "Execution took longer than %d sec, took %ld sec. Exiting.\n",
        MAX_EXECUTION_TIME_IN_SEC, (algo_end_time.tv_sec - start_time.tv_sec));
    fprintf(log_fp, "Cleaning up memories\n");
    sem_post(sem);
    cleanup_memories(0);
    exit(EXIT_FAILURE);
  }

  /*  run logarithmic addition. in every iteration the
      numbers are divided into log(N) (base e) groups
      and summed up in different processes. result is
      handled just like the binary addition.
  */

  /* print some info to output streams about the current algorithm */
  fprintf(log_fp, "Starting summation using logarithmic algorithm.\n");
  fflush(log_fp);
  printf("Starting summation using logarithmic algorithm.\n");
  clock_gettime(CLOCK_REALTIME, &algo_start_time);

  /* copy numbers to the shared memory */
  count = 0;
  tmp = numlist;
  while (tmp) {
    numbers[count++] = tmp->number;
    tmp = tmp->next;
  }
  numbers_left = number_count;

  /* main loop */
  result_found = 0;
  while (!result_found) {
    if (log((double)numbers_left) < 0.0) {
      cur_segment_count = 1;
      segment_size = numbers_left;
    } else {
      double actual_segment_size =
          (double)numbers_left / log((double)numbers_left);
      segment_size = ceil(actual_segment_size);
      segment_size =
          (segment_size > numbers_left) ? numbers_left : segment_size;

      cur_segment_count = (numbers_left % segment_size)
                              ? ((numbers_left / segment_size) + 1)
                              : (numbers_left / segment_size);
    }
    cur_segment_index = 0;

    // log current number of segments
    sem_wait(sem);
    fprintf(log_fp, "Current segment count: %d\n", cur_segment_count);
    fflush(log_fp);
    sem_post(sem);

    // fork cur_segment_count number of children
    child_count = 0;
    seed = time(NULL);
    srand((unsigned int)seed);
    for (int i = 0; i < cur_segment_count; i++) {
      // calculate segment size, can be 2 or 1
      cur_segment_size = ((cur_segment_index + segment_size) > numbers_left)
                             ? (numbers_left - cur_segment_index)
                             : segment_size;

      wait_sec = rand() % 4;

      // fork a child
      if (fork() == 0) {
        // store child pid
        pid_t pid = getpid();
        mlock(pids, sizeof(pid_t) * MAX_CHILDREN);
        for (int i = 0; i < MAX_CHILDREN; i++) {
          if (pids[i] < 0) {
            pids[i] = pid;
            break;
          }
        }
        munlock(pids, sizeof(pid_t) * MAX_CHILDREN);

        // log child id
        sem_wait(sem);
        // wait for random seconds (between 0-3)
        sleep(wait_sec);
        fprintf(log_fp, "Child PID: %d, Index: %d, Size: %d, Slept: %d sec\n",
                pid, cur_segment_index, cur_segment_size, wait_sec);
        fflush(log_fp);
        sem_post(sem);

        // prepare child arguments
        sprintf(child_args[1], "%d", cur_segment_index);
        sprintf(child_args[2], "%d", cur_segment_size);

        // exec
        execv("./bin_adder", child_args);

        // exit child process
        cleanup_memories(1);
        exit(EXIT_SUCCESS);
      }

      child_count++;
      cur_segment_index += cur_segment_size;

      // if maximum number of children are already created
      // wait for a child to finish up
      if (child_count > MAX_CHILDREN) {
        pid_t child_pid = wait(&child_stat);
        child_count--;

        mlock(pids, sizeof(pid_t) * MAX_CHILDREN);
        for (int i = 0; i < MAX_CHILDREN; i++) {
          if (pids[i] == child_pid) {
            pids[i] = -1;
            break;
          }
        }
        munlock(pids, sizeof(pid_t) * MAX_CHILDREN);
      }
    }

    // wait for currently active adders to finish up
    pid_t child_pid;
    while ((child_pid = wait(&child_stat)) > 0) {
      printf("Child PID: %d is done working.\n", child_pid);
      sem_wait(sem);
      fprintf(log_fp, "Child PID: %d is done working.\n", child_pid);
      fflush(log_fp);
      sem_post(sem);
      // mark the child as done
      mlock(pids, sizeof(pid_t) * MAX_CHILDREN);
      for (int i = 0; i < MAX_CHILDREN; i++) {
        if (pids[i] == child_pid) {
          pids[i] = -1;
          break;
        }
      }
      munlock(pids, sizeof(pid_t) * MAX_CHILDREN);
    }

    // collect results and remove the distances between them
    mlock(numbers, sizeof(int) * number_count);
    cur_segment_index = 0;  // using this for indexing purpose, avoiding
                            // creation of a new variable
    for (int i = 0; i < cur_segment_count; i++) {
      numbers[cur_segment_index++] = numbers[i * segment_size];
    }
    munlock(numbers, sizeof(int) * number_count);

    // update the numbers left to be summed up
    numbers_left = cur_segment_count;

    // break the loop if numbers left is 1
    if (numbers_left == 1) {
      result_found = 1;

      clock_gettime(CLOCK_REALTIME, &algo_end_time);

      // print out the result
      printf("Result: %d using logarithmic algorithm.\n", numbers[0]);
      printf("Algorithm took %ld sec.\n",
             algo_end_time.tv_sec - algo_start_time.tv_sec);
      sem_wait(sem);
      fprintf(log_fp, "Result: %d using logarithmic algorithm.\n", numbers[0]);
      fprintf(log_fp, "Algorithm took %ld sec.\n",
              algo_end_time.tv_sec - algo_start_time.tv_sec);
      fflush(log_fp);
      sem_post(sem);
    }
  }

  /* free up shared memory */
  sem_wait(sem);
  fprintf(log_fp, "Cleaning up memories\n");
  sem_post(sem);
  cleanup_memories(0);

  return 0;
}
