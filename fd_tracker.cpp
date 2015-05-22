#include "fd_tracker.h"
#include <assert.h>
#include <strings.h>
#include <runtime.h>
#include <cutils/hashmap.h>
#include <stdlib.h>

#define TRACK_THRESHHOLD 0.8
volatile tracking_mode g_tracking_mode = DISABLED;

int g_rlimit_nofile = -1;
char** g_hash_array = NULL;
Hashmap * g_hash_map = NULL;
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
struct entry_points g_entry_points;

// FIXME: what if setrlimit or prlimit is invoked ?
// FIXME: consider std::atomic for performance ?
// FIXME: hard/soft rlimit
// FIXME: doesn't work for setuid/setgid

__attribute__((constructor))
void setup() {
#define ENTRYPOINT_ENUM(name, rettype, ...)                             \
    typedef rettype (*FUNC_##name)(__VA_ARGS__);                        \
    g_entry_points.p_##name = (FUNC_##name) dlsym(RTLD_NEXT, #name);    \

#include "entry_points.h"
    ENTRYPOINT_LIST(ENTRYPOINT_ENUM);

#undef ENTRYPOINT_LIST
#undef ENTRYPOINT_ENUM

    struct rlimit limit;
    int ret = getrlimit(RLIMIT_NOFILE, &limit);
    if (ret) {
        ALOGE("FD_TRACKER: getrlimit failed, errno: %d", errno);
        return;
    }
    if (limit.rlim_cur == RLIM_INFINITY) {
        ALOGE("FD_TRACKER: RLIM_NOFILE is INFINITY, skip fd_tracker");
        return;
    }

    g_rlimit_nofile = limit.rlim_cur;
    g_hash_array = (char**) malloc(sizeof(char*) * (g_rlimit_nofile));
    bzero(g_hash_array, sizeof(char*) * g_rlimit_nofile);

    assert(TRACK_THRESHHOLD > 0 && TRACK_THRESHHOLD < 1);
    
    limit.rlim_cur = (rlim_t) (g_rlimit_nofile * TRACK_THRESHHOLD);
    ret = setrlimit(RLIMIT_NOFILE, &limit);
    if (ret) {
        ALOGE("FD_TRACKER: setrlimit failed, errno: %d", errno);
        return;
    }
    g_tracking_mode = NOT_TRIGGERED;
    g_hash_map = hashmapCreate(g_rlimit_nofile, pred_str_hash, pred_str_equals);
}

void do_track(int fd) {
    AutoLock lock(&g_mutex);
    if (g_tracking_mode != TRIGGERED) {
        return;
    };
    assert(fd >= 0);
    if (fd >= g_rlimit_nofile) {
        ALOGE("FD_TRACKER: fd: %d exceed rlimit: %d?", fd, g_rlimit_nofile);
        return;
    }
    
    struct rlimit limit;
    int ret = getrlimit(RLIMIT_NOFILE, &limit);
    int orig_limit = limit.rlim_cur;
    limit.rlim_cur = orig_limit + 1;
    ret = setrlimit(RLIMIT_NOFILE, &limit);
    
    android::CallStack stack;
    stack.update(4);

    std::ostringstream java_stack;
    art::Runtime::DumpJavaStack(java_stack);

    limit.rlim_cur = orig_limit;
    ret = setrlimit(RLIMIT_NOFILE, &limit);
    

    assert(g_hash_array[fd] == NULL);

    char* md5_sum = md5((char*)stack.toString("").string(), (char*)java_stack.str().c_str());

    trace_info * _trace_info = (trace_info *) hashmapGet(g_hash_map,md5_sum);
    if (_trace_info == NULL) {
        _trace_info = (trace_info *) malloc(sizeof(trace_info));
        _trace_info->count = 1;
        _trace_info->native_stack_trace = strdup(stack.toString("").string());
        _trace_info->java_stack_trace = strdup(java_stack.str().c_str());
        hashmapPut(g_hash_map, md5_sum, _trace_info);
    } else {
        _trace_info->count++;
    }
    g_hash_array[fd] = md5_sum;
}

void do_trigger() {
    AutoLock lock(&g_mutex);
    if (g_tracking_mode != NOT_TRIGGERED) {
        return;
    };
    struct rlimit limit;
    int ret = getrlimit(RLIMIT_NOFILE, &limit);
    if (ret) {
        ALOGE("FD_TRACKER: getrlimit failed, errno: %d", errno);
        g_tracking_mode = DISABLED;
        return;
    }
    if (limit.rlim_cur == RLIM_INFINITY) {
        ALOGE("FD_TRACKER: RLIM_NOFILE is INFINITY, skip fd_tracker");
        g_tracking_mode = DISABLED;
        return;
    }
    limit.rlim_cur = (rlim_t) (g_rlimit_nofile);
    ret = setrlimit(RLIMIT_NOFILE, &limit);
    if (ret) {
        ALOGE("FD_TRACKER: setrlimit failed, errno: %d", errno);
        g_tracking_mode = DISABLED;
        return;
    }
    ALOGE("FD_TRACKER: reset RLIM_NOFILE to %d", limit.rlim_cur);
    g_tracking_mode = TRIGGERED;
}

void do_report() {
    AutoLock lock(&g_mutex);
    ALOGE("FD_TRACKER: ****** dump begin ******");
    // hashmapForEach(g_hash_map, dump_trace, NULL);
    
    int hash_size = hashmapSize(g_hash_map);
    trace_info * traces [hash_size];
    bzero(traces, sizeof (trace_info *) * hash_size);

    int context [2] = {(int)traces, 0};
    hashmapForEach(g_hash_map, pred_collect_map_value, (void *) context);

    qsort(traces, hash_size, sizeof(trace_info *), pred_sort_trace);

    for (int i = 0; i < hash_size; i++) {
        trace_info * _trace_info = traces[i];
        ALOGE("FD_TRACKER: ------ dump trace ------");
        ALOGE("FD_TRACKER: repetition: %d", _trace_info->count);
        ALOGE("FD_TRACKER: java trace:\n%s", _trace_info->java_stack_trace);
        ALOGE("FD_TRACKER: native trace:\n%s", _trace_info->native_stack_trace);
    }

    ALOGE("FD_TRACKER: ****** dump end ******");
    g_tracking_mode = DISABLED;
}

extern "C" {
    int close(int fd) {
        int ret = (*g_entry_points.p_close)(fd);
        if (g_tracking_mode != TRIGGERED) {
            return ret;
        }
        if (fd < 0 || fd >= g_rlimit_nofile) {
            return ret;
        }
        
        {
            AutoLock lock(&g_mutex);
            if (g_tracking_mode != TRIGGERED) {
                return ret;
            }
            if (g_hash_array[fd] != NULL) {
                char * md5_sum = g_hash_array[fd];
                assert(md5_sum != NULL);
                trace_info * _trace_info = (trace_info *) hashmapGet(g_hash_map, md5_sum);
                if (_trace_info != NULL) {
                    _trace_info->count--;
                }
                if (_trace_info->count <= 0) {
                    free(_trace_info->native_stack_trace);
                    free(_trace_info->java_stack_trace);
                    hashmapRemove(g_hash_map, md5_sum);
                }
            
                free(g_hash_array[fd]);
                g_hash_array[fd] = NULL;
            }
        }
        return ret;
    }

    int open(const char *pathname, int flags) {
        TRACK_RET(open, pathname, flags);
    }

    // opendir/closedir is not tracked, since they are implemented
    // using open/close

    int socket(int domain, int type, int protocol) {
        TRACK_RET (socket, domain, type, protocol);
    }
    
    int socketpair(int domain, int type, int protocol, int array[2]) {
        TRACK_ARRAY(socketpair, domain, type, protocol, array);
    }
    
    int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
        TRACK_RET (accept, sockfd, addr, addrlen);
    }

    int dup(int oldfd) {
        TRACK_RET(dup, oldfd);
    }

    int dup2 (int oldfd, int newfd) {
        TRACK_RET(dup2, oldfd, newfd);
    }

    int dup3 (int oldfd, int newfd, int flag) {
        TRACK_RET(dup3, oldfd, newfd, flag);
    }
    
    int pipe (int array[2]) {
        TRACK_ARRAY(pipe, array);
    }
    
    int pipe2 (int fd[2], int flags) {
        TRACK_RET(pipe2, fd, flags);
    }

    int creat(const char *path, mode_t mod) {
        TRACK_RET(creat, path, mod);
    }
}
