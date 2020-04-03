/*
 * Declarations for file handle and file table management.
 */

#ifndef _FILE_H_
#define _FILE_H_

/*
 * Contains some file-related maximum length constants
 */
#include <limits.h>
#include <proc.h>

/*
 * Put your function declarations and data types here ...
 */


//Individual Open File struct (OF), contains:
    //Pointer to vnode
    //Number of processes opening that file
    //Access flags
    //The current offset
struct OF {
    struct vnode *vn_ptr;
    int num_open;
    int flags;
    off_t offset;
};


//Individual File Descriptor Struct (FD), contains:
    //Pointer to corresponding OF struct
    //Designated fd number
struct FD {
    struct OF *of_ptr;
    int fd;
};

//OF table struct, contains:
    //A lock, for concurrency
    //An array containing pointers to OF structs
struct OF_table { //Global
    struct lock *of_lock;
    struct OF *open_file[MAXOPEN];
};

//FD table struct, contains:
    //An array contianing pointers to FD structs 
struct FD_table { //Local
    struct FD *fd_table[MAXOPEN];
};



                //Prototypes of functions

//An 'init' function, used to initialise local FD table and OF table (only if not already initialised)
int create_tables(const char *stdin, const char *stdout, const char *stderr);

//A dedicated function to open (in order to open files using char*)
int init_open(char *filename, int flags, mode_t mode, int *retval);

//Implementation of standard comments
int sys_open(userptr_t filename, int flags, mode_t mode, int *retval);
int sys_close(int fd);
int sys_read(int fd, void *buf, size_t buflen, int *retval);
int sys_write(int fd, void *buf, size_t nbytes, int *retval);
int sys_dup2(int oldfd, int newfd, int *retval);
off_t sys_lseek(int fd, off_t pos, int whence, off_t *retval);

//Declaring global OF_table
struct OF_table *of_table;


#endif /* _FILE_H_ */