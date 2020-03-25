#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* int shm_open(const char* name, int oflag, mode_t mode); */
/* void *mmap(void *, size_t, int prot, int flags, int fd, off_t); */
/* int munmap(void *, size_t); */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

/**
 * Global variables to be used at different stages of
 * the program like signal handler.
 */
const char *shared_number_buffer_name =
    "/numbers-2342342";   // shared memory block path for numbers
int numbers_fd = -1;      // shared numbers file descriptor
int *numbers = NULL;      // shared numbers array
size_t number_count = 0;  // total integers to be stored

/**
 * Function to allocate shared memories.
 */
void allocate_shared_memory() {
  if ((numbers_fd = shm_open(shared_number_buffer_name, O_RDWR | O_CREAT,
                             S_IRUSR | S_IWUSR)) < 0) {
    perror("could not open shared numbers memory file");
    exit(EXIT_FAILURE);
  }
  if ((numbers =
           (int *)mmap(NULL, sizeof(int) * number_count, PROT_READ | PROT_WRITE,
                       MAP_SHARED, numbers_fd, 0)) == NULL) {
    perror("could not map shared numbers memory");
    exit(EXIT_FAILURE);
  }
}

/**
 * Frees up shared memories.
 *
 * Additionaly frees the local numbers list.
 */
static void cleanup_memories() {
  // unmap shared memories
  munmap(numbers, sizeof(int) * number_count);
}

int main(int argc, char **argv) {
  int start_index = 0;
  int size = 0;
  int sum = 0;
  /* read command line arguments */
  if (argc < 4) {
    printf("usage: ./bin_adder start_index size total_numbers\n");
    exit(EXIT_SUCCESS);
  }
  start_index = atoi(argv[1]);
  size = atoi(argv[2]);
  number_count = (size_t)atoi(argv[3]);

  // load in the numbers
  allocate_shared_memory();

  sum = 0;
  for (int i = start_index; i < (start_index + size); i++) {
    mlock(numbers, sizeof(int) * number_count);
    sum += numbers[i];
    munlock(numbers, sizeof(int) * number_count);
  }
  mlock(numbers, sizeof(int) * number_count);
  numbers[start_index] = sum;
  // printf("%d %d %d\n", start_index, size, sum);
  munlock(numbers, sizeof(int) * number_count);

  // free up memory
  cleanup_memories();

  return 0;
}
