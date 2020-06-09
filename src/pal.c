#include <stdlib.h> 
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stddef.h>

#include "pal.h"
#include "errors.h"


struct pal_file_handle{ 
    int fd;
};

const char* get_file_name(file_handle_t* handle){
    return ((char*)handle + sizeof(file_handle_t)); 
}

size_t get_file_handle_size(const char* path, const char* name) { 
    if(!path || !name)
        return 0;

    size_t path_len = strlen(path);
    size_t name_len = strlen(name);

    if(!path_len || !name_len)
        return 0;

    return sizeof(file_handle_t) + path_len + 1 /* slash */ + name_len + 1 /* null terminating*/;
}

static char* set_file_name(const char* path, const char* name, file_handle_t* handle){

    // we rely on the fact that foo/bar == foo//bar on linux
    size_t path_len = strlen(path);
    size_t name_len = strlen(name);

    char* filename = (char*)handle + sizeof(file_handle_t);

    memcpy(filename, path, path_len); 
    *(filename + path_len) = '/';
    memcpy(filename + path_len + 1, name, name_len + 1); 
    return filename;
}

static bool fsync_parent_directory(char* file){
    char* last = strrchr(file, '/');
    int fd;
    if(!last){
        fd = open(".", O_RDONLY);
    }
    else{
        *last = 0;
        fd = open(file, O_RDONLY);
        *last = '/';
    }
    if(fd == -1){
        push_error(errno, "Unable to open parent directory of: %s", file);
        return false;
    }
    bool res = true;
    if(fsync(fd)){
        push_error(errno, "Failed to fsync parent directory of: %s", file);
        res = false;
    }
    if(close(fd)){
        push_error(errno, "Failed to close (after fsync) parent directory of: %s", file);
        res = false;
    }
    return res;

}

static bool ensure_file_path(char* file) {
    // already exists?
    struct stat st;
    if(stat(file, &st)){
        if(S_ISDIR (st.st_mode)){
            push_error(EISDIR, "The path '%s' is a directory, expected a file", path);
            return false;
        }
        return true; // file exists, so we are good
    }

    char* cur = path;
    while(*cur){
        char* next_sep = strchr(cur, '/')
        if(!next_sep)
            return true; // not more directories
        
        *next_sep = 0; // add null sep to cut the string

        if(stat(file, &st)){ // now we are checking the directory!
            if(!S_ISDIR(st.st_mode)){
                push_error(ENOTDIR, "The path '%s' is a file, but expected a directory", path);
                *next_sep = '/';
                return false;
            }
        }
        else { // probably does not exists
            if (mkdir(file, S_IRWXU) == -1 && errno != EEXIST){
                push_error(errno, "Unable to create directory: %s", file);
                *next_sep = '/';
                return false;
            }
            if(!fsync_parent_directory(file)){
                mark_error();
                *next_sep = '/';
                return false;   
            }
        }

        cur = next_sep + 1;
    }
}

bool create_file(const char* path, const char* name, file_handle_t* handle) { 

    char* filename = set_file_name(path, name, handle);
    struct stat st;
    bool isNew = !stat(filename, &st);
    if(isNew){
        if(!ensure_file_path(filename)){
            mark_error();
            return false;
        }
    }
    else{
         if(S_ISDIR (st.st_mode)){
            push_error(EISDIR, "The path '%s' is a directory, expected a file", path);
            return false;
        }
    }

    int fd = open(filename, O_CLOEXEC  | O_CREAT | O_RDWR , S_IRUSR | S_IWUSR);
    if (fd == -1){
        push_error(errno, "Unable to open file %s", filename);
        return false; 
    }
    if(isNew) {
        if(!fsync_parent_directory(filename)) {
            push_error(EIO, "Unable to fsync parent directory after creating new file: %s", filename);
            if(!close(fd)){
                push_error(errno, "Unable to close file (%i) %s", fd, filename);
            }
            return false;
        }
    }
    handle->fd = fd;
    return true;
}

bool get_file_size(file_handle_t* handle, uint64_t* size){
    struct stat st;
    int res = fstat(handle->fd, &st);
    if(res != -1){
        *size = (uint64_t)st.st_size;
        return true;
    }
    push_error(errno, "Unable to stat(%s)", get_file_name(handle));
    return false;
}

bool map_file(file_handle_t* handle, uint64_t size, void** address){
    void* addr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
    if(addr == MAP_FAILED){
        push_error(errno, "Unable to map file %s with size %lu", get_file_name(handle), size);
        *address = 0;
        return false;
    }
    *address = addr;
    return true;
}

bool unmap_file(void* address, uint64_t size){
    if(munmap(address, size) == -1){
        push_error(EINVAL, "Unable to unmap!");
        return false;
    }
    return true;
}

bool close_file(file_handle_t* handle){
    if(!handle)
        return true;

    if(close(handle->fd) == -1){
        push_error(errno, "Failed to close file %s (%i)", get_file_name(handle), handle->fd);
        return false;
    }

    return true;
}

bool ensure_file_minimum_size(file_handle_t* handle, uint64_t minimum_size){
    int res = fallocate (handle->fd, 0, 0, (int64_t)minimum_size);
    if(res != -1)
        return true;
    
    push_error(errno, "Unable to extend file to size %s to %lu", get_file_name(handle), minimum_size);
    return false;
}
 