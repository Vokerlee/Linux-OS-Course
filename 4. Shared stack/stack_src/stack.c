#include "stack.h"
#include "sys_err.h"

const size_t N_ATTEMPTS_IN_INITIALIZATION = 3;

struct timespec MAX_OP_TIME = {.tv_sec = 10};

int attach_sem_set(key_t key)
{
    errno = 0;
    int sem_id = semget(key, N_STACK_SEMAPHORES, IPC_CREAT | IPC_EXCL | 0666); // check for semaphore set existense
    if (sem_id != -1) // successfully created sem set => set didn't exist
    {
        int error_state1 = semctl(sem_id, INIT_SEM,     SETVAL, 1);
        //int error_state2 = semctl(sem_id, N_PROC_SEM,   SETVAL, 0);
        int error_state3 = semctl(sem_id, DESTRUCT_SEM, SETVAL, 0);

        if (error_state1 == -1 || error_state3 == -1) // initialization
        {   
            error_msg("semctl() error in semaphores set initialization");
            semctl(key, 0, IPC_RMID);

            return -1;
        }

        struct sembuf sop = {.sem_num = MUTEX_SEM, .sem_flg = 0, .sem_op = +1};
        if (semop(sem_id, &sop, 1) == -1) // just to change sem_otime to say other processes that it has been initialized
        {
            error_msg("semop() error in semaphores set initialization");
            semctl(key, 0, IPC_RMID);

            return -1;
        }
    }
    else
    {
        const size_t n_tries_to_get_set = N_ATTEMPTS_IN_INITIALIZATION;
        union semun arg;
        struct semid_ds ds;

        sem_id = semget(key, N_STACK_SEMAPHORES, 0666); // retrieve ID of existing set
        if (sem_id == -1)
        {
            error_msg("semget() error in semaphores set attachment");
            return -1;
        }

        // Wait until another process has called semop() for initialization

        arg.buf = &ds;
        for (size_t j = 0; j < n_tries_to_get_set; j++)
        {
            if (semctl(sem_id, 0, IPC_STAT, arg) == -1)
            {
                error_msg("semctl() error");
                return -1;
            }

            if (ds.sem_otime != 0) // has semop() performed?
                break;
            
            sleep(1); // if not, wait and retry
        }

        if (ds.sem_otime == 0) // initialization has not been completed => some error has happened;
        {
            error_msg("initialization error");
            return -1;
        }
    }

    return sem_id;
}

int delete_sem_set(key_t key)
{
    errno = 0;
    int sem_id = semget(key, N_STACK_SEMAPHORES, 0666);
    if (sem_id == -1)
    {
        perror("erorr in semget (delete_sem_set call)");
        return -1;
    }
        
    errno = 0;
    int error_state = semctl(sem_id, 0, IPC_RMID);
    if (error_state == -1)
    {
        perror("error in semctl (delete_sem_set call)");
        return -1;
    }
        
    return 0;
}

int delete_shmem(key_t key, int size)
{
    errno = 0;
    int shmem_id = shmget(key, (size + 2) * sizeof(void *), 0666);
    if (shmem_id == -1)
    {
        perror("error in shmget (delete_shmem call)");
        return -1;
    }

    errno = 0;
    int error_state = shmctl(shmem_id, IPC_RMID, NULL);
    if (error_state == -1)
    {
        perror("error in shmctl (delete_shmem call)");
        return -1;
    }

    return 0;
}

stack_t* attach_stack(key_t key, int size)
{
    if (size <= 0)
        return NULL;
    if (key == -1)
        return NULL;

    //printf("Attach begins\n");

    int sem_removed = 0;

    int sem_id = attach_sem_set(key); // attach semaphore set
    if (sem_id == -1)
        return NULL;

    struct sembuf init_sem_lock = {.sem_num = INIT_SEM, .sem_flg = SEM_UNDO, .sem_op = -1};
    int error_init_lock = semtimedop(sem_id, &init_sem_lock, 1, &MAX_OP_TIME);
    if (error_init_lock == -1 && errno != EAGAIN)
    {
        sem_id = attach_sem_set(key); // attach semaphore set
        if (sem_id == -1)
            return NULL;

        sem_removed = 1;
    }
    else if (errno == EAGAIN)
    {
        sem_id = attach_sem_set(key); // attach semaphore set
        if (sem_id == -1)
            return NULL;

        sem_removed = 1;
    }

    if (sem_removed == 1)
    {
        int error_init_lock = semtimedop(sem_id, &init_sem_lock, 1, &MAX_OP_TIME);
        if (error_init_lock == -1)
        {
            perror("Everything is bad");
        }
    }

    stack_t* stack = (stack_t*) calloc(1, sizeof(stack_t)); // to return from the function
    if (stack == NULL)
        return NULL;

    stack->wait_type = 0;

    int shmem_id = shmget(key, (size + 2) * sizeof(void *), IPC_CREAT | IPC_EXCL | 0666); // try to create new shared memory
    if (shmem_id == -1) // it has already been created => just attach it
    {
        //fprintf(stdout, "Stack exists, attaching the stack\n");

        shmem_id = shmget(key, (size + 2) * sizeof(void *), 0666);
        if (shmem_id == -1)
        {
            perror("error in shmget() while attaching stack (attach_stack() call)");
            free(stack);
            return NULL;
        }

        stack->memory = (void *) shmat(shmem_id, NULL, 0);
        if (stack->memory == (void *) -1)
        {
            perror("error in shmat() while attaching stack (attach_stack() call)");
            free(stack);
            return NULL;
        }

        stack->shmem_id = shmem_id;
        stack->sem_id   = sem_id;

        stack->capacity = *((int *) (stack->memory));
        stack->size     = *((int *) (stack->memory + sizeof(void*)));
    }
    else
    {
        //fprintf(stdout, "No stack exists, creating new stack\n");
        
        shmem_id = shmget(key, (size + 2) * sizeof(void *), 0666);
        if (shmem_id == -1)
        {
            perror("error in shmget() while creating stack");
            delete_sem_set(key);
            free(stack);

            return NULL;
        }

        stack->memory =  (void *) shmat(shmem_id, NULL, 0);
        if (stack->memory == (void *) -1)
        {
            perror("error in shmat() while creating stack (attach_stack() call)");
            delete_sem_set(key);
            delete_shmem(key, size);
            free(stack);

            return NULL;
        }

        int max_size = size;
        int initial_size = 0;

        int res_op1 = semctl(sem_id, FULL_SEM,  SETVAL, max_size);
        int res_op2 = semctl(sem_id, EMPTY_SEM, SETVAL, 0);

        if (res_op1 == -1 || res_op2 == -1)
        {
            perror("error in semctl while setting initial values to semaphores set (attach_stack() call)");
            delete_sem_set(key);
            delete_shmem(key, size);
            free(stack);

            printf("1 = %d\n", res_op1);
            printf("2 = %d\n", res_op2);

            return NULL;
        }
            
        memcpy((void *) stack->memory, &max_size, sizeof(int));
        memcpy((void *) stack->memory + sizeof(void *), &initial_size, sizeof(int));

        stack->shmem_id = shmem_id;
        stack->sem_id   = sem_id;

        stack->capacity = *((int *) (stack->memory));
        stack->size     = *((int *) (stack->memory + sizeof(void *)));
    }

    // struct sembuf add_sem_proc = {.sem_num = N_PROC_SEM, .sem_flg = SEM_UNDO, .sem_op = +1};
    // int error_add_proc = semtimedop(sem_id, &add_sem_proc, 1, &MAX_OP_TIME);
    // if (error_add_proc == -1)
    // {
    //     perror("error in semtimedop() while attaching stack (attach_stack() call)");
    //     free(stack);
    //     return NULL;
    // }

    struct sembuf init_sem_unlock = {.sem_num = INIT_SEM, .sem_flg = SEM_UNDO, .sem_op = +1};
    int error_init_unlock = semtimedop(sem_id, &init_sem_unlock, 1, &MAX_OP_TIME);
    if (error_init_unlock == -1)
    {
        perror("error in semtimedop() while attaching stack (attach_stack() call)");
        free(stack);
        return NULL;
    }

    stack->stack_key = key;

    //printf("Attach was: %d, %d\n", shmem_id, sem_id);

    return stack;
}

int get_size(stack_t* stack)
{
    if (stack == NULL)
        return -1;

    stack->size = *((int *)(stack->memory + sizeof(void *))); // updating size

    return stack->capacity;
}

int get_count(stack_t* stack)
{
    if (stack == NULL)
        return -1;
    
    stack->size = *((int *)(stack->memory + sizeof(void *))); // updating size
   
    return stack->size;
}

int detach_stack(stack_t* stack)
{
    if (stack == NULL)
        return -1;

    //printf("Detaching: %d %d\n", stack->shmem_id, stack->sem_id);

    int return_value = 0;

    int sem_id = attach_sem_set(stack->stack_key);
    if (sem_id == -1)
        return -1;

    struct sembuf init_sem_lock = {.sem_num = INIT_SEM, .sem_flg = SEM_UNDO, .sem_op = -1};
    int error_init_lock = semtimedop(sem_id, &init_sem_lock, 1, &MAX_OP_TIME);
    if (error_init_lock == -1 && errno != EAGAIN)
    {
        perror("error in semtimedop() while deattaching stack (detach_stack() call)");
        return -1;
    }
    else if (errno == EAGAIN)
    {
        perror("operation timed out, abort from function (detach_stack() call)");
        errno = 0;
        return -1;
    }
    
    errno = 0;
    struct sembuf mutex_lock = {.sem_num = MUTEX_SEM, .sem_flg = SEM_UNDO, .sem_op = -1};
    int error_mutex_lock = semtimedop(sem_id, &mutex_lock, 1, &MAX_OP_TIME);
    if (error_mutex_lock == -1 && errno != EAGAIN)
    {
        perror("error in semtimedop() while deattaching stack (detach_stack() call)");
        return -1;
    }
    else if (errno == EAGAIN)
    {
        perror("operation timed out, abort from function (detach_stack() call)");
        errno = 0;
        return -1;
    }

    errno = 0;
    int destruct_value = semctl(sem_id, DESTRUCT_SEM, GETVAL);
    if (destruct_value == -1)
    {
        perror("semctl() error (detach_stack() call)");
        return_value = -1;
    }

    struct shmid_ds ds = {0};
    
    errno = 0;
    int n_processes = shmctl(stack->shmem_id, IPC_STAT, &ds);
    if (n_processes == -1)
    {
        perror("shmctl() error (detach_stack() call)");
        return_value = -1;
    }

    n_processes = ds.shm_nattch;
    //printf("n_proc = %d, dest = %d\n", n_processes, destruct_value);

    if (destruct_value > 0 && n_processes == 1)
    {
        int error = delete_sem_set(stack->stack_key);

        errno = 0;
        int error_shmdt = shmdt(stack->memory);
        if (error_shmdt == -1)
            perror("error while detaching the stack (shmdt() call)");

        free(stack);

        if (error == -1)
            return -1;
        else
            return 0;
    }

    // struct sembuf sub_sem_proc = {.sem_num = N_PROC_SEM, .sem_flg = SEM_UNDO, .sem_op = -1};
    // int error_sub_proc = semtimedop(sem_id, &sub_sem_proc, 1, &MAX_OP_TIME);
    // if (error_sub_proc == -1)
    // {
    //     perror("error in semtimedop() (detach_stack() call)");
    //     free(stack);
    //     return_value = -1;
    // }

    errno = 0;
    int error_shmdt = shmdt(stack->memory);
    if (error_shmdt == -1)
        perror("error while detaching the stack (shmdt() call)");

    struct sembuf mutex_unlock = {.sem_num = MUTEX_SEM, .sem_flg = SEM_UNDO, .sem_op = +1};
    int error_mutex_unlock = semop(sem_id, &mutex_unlock, 1);
    if (error_mutex_unlock == -1)
    {
        perror("error in semtimedop() (detach_stack() call)");
        free(stack);
        return -1;
    }

    struct sembuf init_sem_unlock = {.sem_num = INIT_SEM, .sem_flg = SEM_UNDO, .sem_op = +1};
    int error_init_unlock = semtimedop(sem_id, &init_sem_unlock, 1, &MAX_OP_TIME);
    if (error_init_unlock == -1)
    {
        perror("error in semtimedop() while deattaching stack (detach_stack() call)");
        free(stack);
        return -1;
    }

    free(stack);

    return return_value;
}

int mark_destruct(stack_t* stack)
{
    if (stack == NULL)
        return -1;

    if (delete_shmem(stack->stack_key, stack->capacity) == -1)
        return -1;

    struct sembuf destruct_on = {.sem_num = DESTRUCT_SEM, .sem_flg = 0, .sem_op = +1};
    int error_destr_sem = semop(stack->sem_id, &destruct_on, 1);
    if (error_destr_sem == -1)
    {
        perror("error in semtimedop() while attaching stack (mark_destruct() call)");
        return -1;
    }

    return 0;
}

int push(stack_t* stack, void* value)
{
    if (stack == NULL || value == NULL)
        return -1;

    int error_state = 0;

    // Attaching semaphore set

    errno = 0;
    struct sembuf full_sem_op = {.sem_num = FULL_SEM, .sem_op = -1, .sem_flg = 0};
    if (stack->wait_type == -1)
    {
        full_sem_op.sem_flg = IPC_NOWAIT;
        error_state = semop(stack->sem_id, &full_sem_op, 1);
    }
    else if (stack->wait_type == 0)
        error_state = semop(stack->sem_id, &full_sem_op, 1);
    else
        error_state = semtimedop(stack->sem_id, &full_sem_op, 1, &stack->timeout);

    if (error_state == -1 && errno != EAGAIN)
    {
        perror("error in semtimedop() while pushing to stack (push() call)");
        return -1;
    }
    else if (errno == EAGAIN)
    {
        perror("operation timed out, abort from function (push() call)");
        errno = 0;
        return -1;
    }

    struct sembuf mutex_lock = {.sem_num = MUTEX_SEM, .sem_op = -1, .sem_flg = SEM_UNDO};
    errno = 0;
    error_state = semtimedop(stack->sem_id, &mutex_lock, 1, &MAX_OP_TIME);
    if (error_state == -1 && errno != EAGAIN)
    {
        perror("error in semtimedop() while pushing to stack (push() call)");
        return -1;
    }
    else if (errno == EAGAIN)
    {
        perror("operation timed out, abort from function (push() call)");
        errno = 0;
        return -1;
    }

    // Main operations start

    stack->size = *((int *)(stack->memory + sizeof(void *))); // updating size
    memcpy((void *)(stack->memory + (stack->size + 1) * sizeof(void *)), &value, sizeof(void *)); // placing data to memory

    int new_size = stack->size + 1;
    stack->size  = new_size;
    memcpy((void *) (stack->memory + sizeof(void *)), &new_size, sizeof(int));

    // Main operations end

    struct sembuf mutex_unlock = {.sem_num = MUTEX_SEM, .sem_op = +1, .sem_flg = SEM_UNDO};
    error_state = semop(stack->sem_id, &mutex_unlock, 1);
    if (error_state == -1)
    {
        perror("error in semop() while pushing to stack (push() call)");
        return -1;
    }

    struct sembuf empty_sem_op = {.sem_num = EMPTY_SEM, .sem_op = +1, .sem_flg = 0};
    error_state = semop(stack->sem_id, &empty_sem_op, 1);
    if (error_state == -1)
    {
        perror("error in semtimedop() while pushing to stack (push() call)");
        return -1;
    }

    return 0;
}

int pop(stack_t* stack, void** value)
{
    if (stack == NULL || value == NULL)
        return -1;

    int error_state = 0;

    // Attaching semaphore set

        //printf("%d\n", stack->sem_id);

    errno = 0;
    struct sembuf empty_sem_op = {.sem_num = EMPTY_SEM, .sem_op = -1, .sem_flg = 0};
    if (stack->wait_type == -1)
    {
        empty_sem_op.sem_flg = IPC_NOWAIT;
        error_state = semop(stack->sem_id, &empty_sem_op, 1);
    }
    else if (stack->wait_type == 0)
        error_state = semop(stack->sem_id, &empty_sem_op, 1);
    else
        error_state = semtimedop(stack->sem_id, &empty_sem_op, 1, &stack->timeout);

    if (error_state == -1 && errno != EAGAIN)
    {
        perror("error in semtimedop() while pushing to stack (pop() call)");
        return -1;
    }
    else if (errno == EAGAIN)
    {
        perror("operation timed out, abort from function (pop() call)");
        errno = 0;
        return -1;
    }

    struct sembuf mutex_lock = {.sem_num = MUTEX_SEM, .sem_op = -1, .sem_flg = SEM_UNDO};
    errno = 0;
    error_state = semtimedop(stack->sem_id, &mutex_lock, 1, &MAX_OP_TIME);
    if (error_state == -1 && errno != EAGAIN)
    {
        perror("error in semtimedop() while pushing to stack (push() call)");
        return -1;
    }
    else if (errno == EAGAIN)
    {
        perror("operation timed out, abort from function (push() call)");
        errno = 0;
        return -1;
    }
   
    // Main operations start

    stack->size = *((int *)(stack->memory + sizeof(void *))); // updating size

    void** pop_value = stack->memory + (1 + stack->size) * sizeof(void *);
    *value = *pop_value;

    int new_size = stack->size - 1;
    stack->size = new_size;
    memcpy((void *)(stack->memory + sizeof(void *)), &new_size, sizeof(int));

    // Main operations end

    struct sembuf mutex_unlock = {.sem_num = MUTEX_SEM, .sem_op = +1, .sem_flg = SEM_UNDO};
    error_state = semop(stack->sem_id, &mutex_unlock, 1);
    if (error_state == -1)
    {
        perror("error in semop() while pushing to stack (push() call)");
        return -1;
    }

    struct sembuf full_sem_op = {.sem_num = FULL_SEM, .sem_op = +1, .sem_flg = 0};
    error_state = semop(stack->sem_id, &empty_sem_op, 1);
    if (error_state == -1)
    {
        perror("error in semtimedop() while pushing to stack (push() call)");
        return -1;
    }

    return 0;
}

int set_wait(stack_t* stack, int val, struct timespec* timeout)
{
    if (stack == NULL)
        return -1;

    if (val == -1 || val == 0 || val == 1)
        stack->wait_type = val;
    else
        return -1;

    if (val == 1)
    {
        if (timeout == NULL)
            return -1;

        stack->timeout = *timeout;
    }

    return 0;
}

void print_stack(stack_t* stack, FILE* fout)
{
    if (stack == NULL || fout == NULL)
        return;

    int size = *((int *)(stack->memory + sizeof(void *)));

    // BASE INFO
    fprintf(fout, "\n\n\nStack[%p]:\n", stack);
    fprintf(fout, "Stack max size: %d\n", stack->capacity);
    fprintf(fout, "Stack cur size: %d\n", size);
    fprintf(fout, "Stack shared memory beginning: %p\n", stack->memory);

    // MEMORY INFO
    if (stack->memory)
    {
        fprintf(fout, "Stack shared memory[0]: %d\n", *((int *) stack->memory));
        fprintf(fout, "Stack shared memory[1]: %d\n", *((int *)(stack->memory + sizeof(void *))));

        for (size_t i = 0; i < size; ++i)
            fprintf(fout, "Stack [%d] = %d\n", i, *((int *)(stack->memory + (2 + i) * sizeof(void *))));
    }
    
    fprintf(fout, "\n\n\n");
}
