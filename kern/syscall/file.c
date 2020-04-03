#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <lib.h>
#include <uio.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>
#include <copyinout.h>
#include <proc.h>

/*
 * Add your file-related functions here ...
 */

int create_tables(const char *stdin, const char *stdout, const char *stderr) {

//Initialise of_table
    if (of_table == NULL) {
        of_table = kmalloc(sizeof(struct OF_table));
        if (of_table == NULL) {
            return ENOMEM;
        }

        for (int i = 0; i < MAXOPEN; i++) {
            of_table->open_file[i] = NULL;
        }

        struct lock *new_of_lock = lock_create("of_lock");
        of_table->of_lock = new_of_lock;
    }


//Initialise local FD_table
    curproc->FD_table = kmalloc(sizeof(struct FD_table));
    if (curproc->FD_table == NULL) {
        return ENOMEM;
    }

    for (int i = 0; i < MAXOPEN; i++) {
        curproc->FD_table->fd_table[i] = NULL;
    }

    int retval, fd;
    char address[PATH_MAX];

    strcpy(address, stdin);

    //connecting stdin
    retval = init_open(address, O_RDONLY, 0, &fd);
    if (retval) {
        kfree(curproc->FD_table);
        return retval;
    }

    //connecting stdout
    strcpy(address, stdout);
    retval = init_open(address, O_WRONLY, 0, &fd);
    if (retval) {
        kfree(curproc->FD_table);
        return retval;
    }

    //connecting stderr
    strcpy(address, stderr);
    retval = init_open(address, O_WRONLY, 0, &fd);
    if (retval) {
        kfree(curproc->FD_table);
        return retval;
    }
    return 0;
}

//Separate open function for init only
int init_open(char *filename, int flags, mode_t mode, int *retval) {
    int result, fd = 0;
    struct vnode *v;

    if (filename == NULL) {
        return EBADF;
    }

    lock_acquire(of_table->of_lock);

    result = vfs_open(filename, flags, mode, &v);
    if (result) {
        lock_release(of_table->of_lock);
        return result;
    }

    //Create OF and FD structs
    struct OF *of = kmalloc(sizeof(struct OF));
    struct FD *file_descriptor = kmalloc(sizeof(struct FD));

    //Setup OF and FD
    file_descriptor->of_ptr = of;
    of->vn_ptr = v;
    of->num_open = 1;
    of->offset = 0;
    of->flags = flags;

    //Find open spot in of_table
    for(int i = 0; i < MAXOPEN; i++) {
        if (of_table->open_file[i] == NULL) {
            of_table->open_file[i] = of;
            break;
        }
    }

    ///Find open spot in fd_table
    for(int i = 0; i < MAXOPEN; i++) {
        if (curproc->FD_table->fd_table[i] == NULL) {
            curproc->FD_table->fd_table[i] = file_descriptor;
            file_descriptor->fd = i;
            fd = i;
            break;
        }
    }

    *retval = fd;
    //Release lock
    lock_release(of_table->of_lock);
    return 0;   
}

/*
 * SYS_OPEN
 * parameters: userptr_t filename, int flags, mode_t mode, int *retval
 * returns: EBADF if error
 * open opens the file, device, or other kernel object named by the pathname filename.
 * handles different flags (O_RDONLY, WRONLY,RDWR)
 */ 

//Standard open function
int sys_open(userptr_t filename, int flags, mode_t mode, int *retval) {
    int result, fd = 0;
    struct vnode *v;
    //Check for bad filename
    if (filename == NULL) {
        return EBADF;
    }
    //Get lock
    lock_acquire(of_table->of_lock);

    char *newFileName = kmalloc(sizeof(char) * __PATH_MAX);
    //Copy to kernel space
    result = copyinstr(filename, newFileName, __PATH_MAX, NULL);
    if (result) {
        lock_release(of_table->of_lock);
        return result;
    }    

    result = vfs_open(newFileName, flags, mode, &v);
    if (result) {
        lock_release(of_table->of_lock);    
        return result;
    }
    //Create OF and FD structs
    struct OF *of = kmalloc(sizeof(struct OF));
    struct FD *file_descriptor = kmalloc(sizeof(struct FD));


    //Setup OF and FD structs
    file_descriptor->of_ptr = of;
    of->vn_ptr = v;
    of->num_open = 1;
    of->offset = 0;
    of->flags = flags;

    //Find open spot in of_table
    for(int i = 0; i < MAXOPEN; i++) {
        if (of_table->open_file[i] == NULL) {
            of_table->open_file[i] = of;
            break;
        }
    }

    //Find open spot in FD_table
    for(int i = 0; i < MAXOPEN; i++) {
        if (curproc->FD_table->fd_table[i] == NULL) {
            curproc->FD_table->fd_table[i] = file_descriptor;
            file_descriptor->fd = i;
            fd = i;
            break;
        }
    }

    *retval = fd;
    //Release lock
    lock_release(of_table->of_lock);
    return 0;
}

/*
 * SYS_CLOSE
 * parameters: int fd
 * returns: EBADF if error
 * uses lock for concurrency 
 * The file handle fd is closed.
 */ 

 int sys_close(int fd) {
    //Check for sane fd
    if (fd < 0 || fd > MAXOPEN) {
        return EBADF;
    }
    //Get FD struct from FD_table
    struct FD *fd_ptr = curproc->FD_table->fd_table[fd];
    if (fd_ptr == NULL) {
        return EBADF;
    }

    //Get lock
    lock_acquire(of_table->of_lock);

    //Get pointer to OF struct from fd_ptr
    struct OF *of_ptr = fd_ptr->of_ptr;
    if (of_ptr == NULL) {
        lock_release(of_table->of_lock);
        return EBADF;
    }


    //Free FD struct
    kfree(fd_ptr);
    curproc->FD_table->fd_table[fd] = NULL;

    //If last process opening file, remove file. Otherwise, decrement num_open
    if (of_ptr->num_open == 1) {
        vfs_close(of_ptr->vn_ptr);
        for (int i = 0; i < MAXOPEN; i++) {
            if (of_table->open_file[i] == of_ptr) {
                of_table->open_file[i] = NULL;
            }
        }
        kfree(of_ptr);
    } else {
        of_ptr->num_open--;
    }

    //Release lock
    lock_release(of_table->of_lock);
    return 0;
 }

/*
 * SYS_READ
 * parameters: int fd, void *buf, size_t buflen, int *retval
 * returns: EBADF if error
 * uses lock for concurrency 
 * The current seek position of the file is advanced by the number of bytes read.
 * read reads up to buflen bytes from the file specified by fd,
 */ 

int sys_read(int fd, void *buf, size_t buflen, int *retval){
    //Check sane inputs
    if (fd < 0 || fd > MAXOPEN) {
         return EBADF;
     }

    //Get FD struct from fd_table
    struct FD *fd_ptr = curproc->FD_table->fd_table[fd];
    if (fd_ptr == NULL) {
        return EBADF;
    }

    //Acquire lock
    lock_acquire(of_table->of_lock);

    //Get pointer to OF struct from fd_ptr
    struct OF *of_ptr = fd_ptr->of_ptr;
    if (of_ptr == NULL || of_ptr->flags == O_WRONLY) {
        lock_release(of_table->of_lock);
        return EBADF;
    }

    //Initialise iovec and uio structs
    int result;
    struct iovec iovec_n;
    struct uio uio_n;

    //Read data
    uio_uinit(&iovec_n, &uio_n, buf, buflen, of_ptr->offset, UIO_READ);
    result = VOP_READ(of_ptr->vn_ptr, &uio_n);
    if (result) {
        lock_release(of_table->of_lock);    
        return result;
    }

    //Calculate offset
    *retval = uio_n.uio_offset - of_ptr->offset;

    of_ptr->offset = uio_n.uio_offset;

    //Release lock
    lock_release(of_table->of_lock);
    return 0;

}

/*
 * SYS_WRITE
 * parameters: int fd, void *buf, size_t nbytes, int *retval
 * returns: EBADF if error
 * uses lock for concurrency 
 * The current seek position of the file is advanced by the number of bytes written.
 * write writes up to buflen bytes to the file specified by fd,
 */ 

int sys_write(int fd, void *buf, size_t nbytes, int *retval){
    //Check sane input
    if (fd < 0 || fd > MAXOPEN) {
         return EBADF;
    }

    //Get FD from fd_table
    struct FD *fd_ptr = curproc->FD_table->fd_table[fd];
    if (fd_ptr == NULL) {
        return EBADF;
    }

    //Acquire lock
    lock_acquire(of_table->of_lock);

    //Get OF struct from fd_ptr
    struct OF *of_ptr = fd_ptr->of_ptr;
    if (of_ptr == NULL || of_ptr->flags == O_RDONLY) {
        lock_release(of_table->of_lock);
        return EBADF;
    }

    //Setup iovec and uio structs
    int result;
    struct iovec iovec_n;
    struct uio uio_n;

    //Write data
    uio_uinit(&iovec_n, &uio_n, buf, nbytes, of_ptr->offset, UIO_WRITE);
    result = VOP_WRITE(of_ptr->vn_ptr, &uio_n);
    if (result) {
        lock_release(of_table->of_lock);
        return result;
    }

    //Calculate offsets
    *retval = uio_n.uio_offset - of_ptr->offset;

    of_ptr->offset = uio_n.uio_offset;
   
    lock_release(of_table->of_lock);
    return 0;
}

/*
 * SYS_DUP2
 * parameters: int oldfd, int newfd, int *retval
 * returns: EBADF, ENOMEM if error (-1 returned)
 * uses lock for concurrency 
 * clones the file handle oldfd onto the file handle newfd
 * If newfd names an already-open file, that file is closed.
 */ 


int sys_dup2(int oldfd, int newfd, int *retval){
    //Check both oldfd and newfd
    int result;

    if (oldfd < 0 || oldfd > MAXOPEN) {
        return EBADF;        
    }

    if (newfd < 0 || newfd > MAXOPEN) {
        return EBADF;
    }

    if (newfd == oldfd) {
        return EBADF;
    }

    //If newfd spot in FD_table is occupied, close that FD
    if (curproc->FD_table->fd_table[newfd] != NULL) {
        result = sys_close(newfd);
        if (result) {
            return ENOMEM;
        }
    }

    //Acquire lock
    lock_acquire(of_table->of_lock);

    //Setup old and new fd structs
    struct FD *old_fd = curproc->FD_table->fd_table[oldfd];
    struct FD *new_fd = kmalloc(sizeof(struct FD));
    new_fd->of_ptr = old_fd->of_ptr;

    //Find spot for new FD
    for (int i = 0; i < MAXOPEN; i++) {
        if (curproc->FD_table->fd_table[i] == NULL) {
            curproc->FD_table->fd_table[i] = new_fd;
            new_fd->fd = i;
        }
    }

    //Get pointer to OF struct
    struct OF *of = old_fd->of_ptr;

    //Increment num_open
    of->num_open++;

    *retval = new_fd->fd;

    //Release lock
    lock_release(of_table->of_lock);
    return new_fd->fd;
}

/*
 * SYS_LSEEK
 * parameters: int fd, off_t pos, int whence, off_t *retval
 * returns: EBADF, EINVAL if error 
 * uses lock for concurrency 
 * passes to VOP_STAT on SEEK_END
 * calculates new offset depending on SEEK_SET, CUR or END.
 */ 

off_t sys_lseek(int fd, off_t pos, int whence, off_t *retval) {
    //Initialise values
    *retval = 0;
    int result;
    off_t lseekval = 0;

    //Check sanity
    if (fd < 0 || fd > MAXOPEN) {
        return EBADF;
    }

    //Get FD struct
    struct FD *fd_ptr = curproc->FD_table->fd_table[fd];
    if (fd_ptr == NULL) {
        return EBADF;
    }

    //Acquire lock
    lock_acquire(of_table->of_lock);

    //Get OF struct
    struct OF *of_ptr = fd_ptr->of_ptr;
    if (of_ptr == NULL) {
        lock_release(of_table->of_lock);
        return EBADF;
    }

    //If whence < 0, abort
    if (whence < 0) {
        lock_release(of_table->of_lock);
        return EINVAL;
    }

    //If whence == SEEK_SET, pos given is the value
    if (whence == SEEK_SET) {
        lseekval = pos;
    }

    //If whence == SEEK_CUR, add pos given on top of seekval
    if (whence == SEEK_CUR) {

        lseekval += pos;
    }

    //If whence == SEEK_END, add pos on top of EOF
    if (whence == SEEK_END) {
        struct stat stat_n;
        result = VOP_STAT(of_ptr->vn_ptr, &stat_n);
        if (result) {
            lock_release(of_table->of_lock);
            return result;
        }
        lseekval = stat_n.st_size + pos; 
    }

    
    of_ptr->offset = lseekval;

    //Release lock
    lock_release(of_table->of_lock);
    return 0;
}