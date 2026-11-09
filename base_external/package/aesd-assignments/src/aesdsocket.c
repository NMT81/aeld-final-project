#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/queue.h>
#include <errno.h>
#include "aesd_ioctl.h"

#define USE_AESD_CHAR_DEVICE 1
#define BUFFER_SIZE 1024
#if USE_AESD_CHAR_DEVICE
#define TMP_DATA "/dev/aesdchar"
#else
#define TMP_DATA "/var/tmp/aesdsocketdata"
#endif
#define CHILD_PIDFILE "/var/tmp/aesdsocket.pid"

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar) \
    for ((var) = SLIST_FIRST((head)); \
        (var) && ((tvar) = SLIST_NEXT((var), field), 1); \
        (var) = (tvar))
#endif

struct thread_node {
    pthread_t thread_id;
    int log_fd;
    int client_fd;
    bool finished;
    char ip[INET6_ADDRSTRLEN];
    SLIST_ENTRY(thread_node) entries;
};
SLIST_HEAD(thread_list_head, thread_node) thread_head = SLIST_HEAD_INITIALIZER(thread_head);

volatile bool signal_captured = false;
bool tmp_data_opened = false;
#if !USE_AESD_CHAR_DEVICE
bool enable_time_worker = false;
#endif
int tmp_data_size = 0;
int tmp_data_fd = 0;

pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t node_mutex = PTHREAD_MUTEX_INITIALIZER;

void handle_signal(int signal_num) {

    if ((signal_num == SIGINT) || (signal_num == SIGTERM))
    {
        signal_captured = true;
    }
}

#if !USE_AESD_CHAR_DEVICE
void* time_worker(void* arg) {
    int tm_fd = *(int*)arg;
    char time_str[31] = {0};
    char i;
    time_t now;
    struct tm *tm_info;
    struct timespec ts;
    ts.tv_sec = 1;
    ts.tv_nsec = 0;

    while (!signal_captured) {
        for(i=0; ((i<10) && (!signal_captured)); i++){
            nanosleep(&ts, NULL);
        }
        if (signal_captured) break;

        now = time(NULL);
        tm_info = localtime(&now);
        strftime(time_str, sizeof(time_str), "timestamp:%Y-%m-%d %H:%M:%S\n", tm_info);
        if (enable_time_worker){
            pthread_mutex_lock(&file_mutex);
            write(tm_fd, time_str, sizeof(time_str));
            tmp_data_size += sizeof(time_str);
            pthread_mutex_unlock(&file_mutex);
        }
    }

    return NULL;
}
#endif

void* client_worker(void* arg) {
    struct thread_node* node = (struct thread_node*)arg;
    ssize_t bytes_received;
    char buf[BUFFER_SIZE];
    #if USE_AESD_CHAR_DEVICE
    ssize_t bytes_read;
    char buffer[BUFFER_SIZE];
    struct aesd_seekto seekto;
    #endif

    pthread_mutex_lock(&file_mutex);
    while(!signal_captured)
    {
        bytes_received = recv(node->client_fd, buf, BUFFER_SIZE - 1, 0);
        if (bytes_received == -1) {         // error or signal arrived
            break;
        } else {
            #if USE_AESD_CHAR_DEVICE
            if (strncmp(buf, "AESDCHAR_IOCSEEKTO:", 19) == 0) {
                seekto.write_cmd = buf[19] - '0';
                seekto.write_cmd_offset = buf[21] - '0';
                if (ioctl(node->log_fd, AESDCHAR_IOCSEEKTO, &seekto) == 0) {
                    while ((bytes_read = read(node->log_fd, buffer, BUFFER_SIZE)) > 0) {
                        write(node->client_fd, buffer, bytes_read);
                    }
                    break;
                }
            } else
            #endif
            {
                write(node->log_fd, buf, bytes_received);
                tmp_data_size += bytes_received;
                if (buf[bytes_received - 1] == '\n')
                {
                    #if USE_AESD_CHAR_DEVICE
                    lseek(node->log_fd, 0, SEEK_SET);
                    while ((bytes_read = read(node->log_fd, buffer, BUFFER_SIZE)) > 0) {
                        write(node->client_fd, buffer, bytes_read);
                    }
                    #else
                    off_t offset = 0;
                    sendfile(node->client_fd, node->log_fd, &offset, tmp_data_size);
                    #endif
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&file_mutex);

    close(node->client_fd);
    node->finished = true;
    syslog(LOG_DEBUG, "Closed connection from %s", node->ip);

    return NULL;
}

void reap_worker(void) {
    struct thread_node *np, *tmp;
    pthread_mutex_lock(&node_mutex);
    SLIST_FOREACH_SAFE(np, &thread_head, entries, tmp) {
        if (np->finished) {
            pthread_join(np->thread_id, NULL);
            SLIST_REMOVE(&thread_head, np, thread_node, entries);
            free(np);
        }
    }
    pthread_mutex_unlock(&node_mutex);
}

int main (int argc, char *argv[])
{
    struct sigaction sa;
    int server_fd, client_fd;
    struct addrinfo hints, *res, *p;
    struct sockaddr_storage client_addr;
    socklen_t addr_size;
    int yes = 1;
    pid_t pid;
    #if !USE_AESD_CHAR_DEVICE
    pthread_t tm_tid;
    #endif
    // Setup stream socket binding on port 9000
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, "9000", &hints, &res) != 0) {
        printf("fail getaddrinfo\n\r");
        return 2;
    }
    for (p = res; p != NULL; p = p->ai_next) {
        server_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (server_fd == -1) {
            continue;
        }

        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(int));

        if (bind(server_fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        close(server_fd);
    }
    freeaddrinfo(res);
    if (p == NULL) {
        printf("Fail setting socket\n\r");
        return 3;
    }

    // check argument, create deamon
    if ((argc > 1) && (strcmp(argv[1],"-d") == 0)){
        pid = fork();
        if (pid > 0){           //parent process
            int pid_fd = open(CHILD_PIDFILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
            char pid_buf[20] = {0};
            snprintf(pid_buf, sizeof(pid_buf), "%d", pid);
            write(pid_fd, pid_buf, sizeof(pid_buf));
            close(pid_fd);
            return 0;
        } else if (pid == 0){   //child process
            setsid();           // start new session
            umask(0);           // Reset file permissions mask
            close(STDIN_FILENO);// detech session controls 
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        }
    }

    // Configure the sigaction structure
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;    // No special flags

    // Register handler for SIGINT
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return 1;
    }

    // Register handler for SIGTERM
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return 1;
    }

    if (listen(server_fd, 10) == -1) {
        close(server_fd);
        return 4;
    }

    // Setup logging
    openlog(argv[0], LOG_PID, LOG_USER);    
    
    #if !USE_AESD_CHAR_DEVICE
    pthread_create(&tm_tid, NULL, time_worker, &tmp_data_fd);
    #endif
    tmp_data_size = 0;
    // loop accept, receive, send
    while(!signal_captured)
    {
        reap_worker();
        addr_size = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_size);
        if (client_fd == -1) {
            break;
        }
        struct thread_node* new_node = malloc(sizeof(struct thread_node));
        if (!new_node) {
            close(client_fd);
            continue;
        }        
        getnameinfo((struct sockaddr *)&client_addr, addr_size,
                      new_node->ip, sizeof(new_node->ip), 
                      NULL, 0,
                      NI_NUMERICHOST);
        syslog(LOG_DEBUG, "Accepted connection from %s", new_node->ip);
        if(!tmp_data_opened){
            tmp_data_opened = true;
            #if USE_AESD_CHAR_DEVICE
            tmp_data_fd = open(TMP_DATA, O_RDWR);
            #else
            tmp_data_fd = open(TMP_DATA, O_RDWR | O_CREAT | O_TRUNC, 0644);
            #endif            
        }
        new_node->client_fd = client_fd;
        new_node->finished = false;
        new_node->log_fd = tmp_data_fd;
        #if !USE_AESD_CHAR_DEVICE
        enable_time_worker = true;
        #endif
        pthread_mutex_lock(&node_mutex);
        SLIST_INSERT_HEAD(&thread_head, new_node, entries);
        pthread_mutex_unlock(&node_mutex);

        if (pthread_create(&new_node->thread_id, NULL, client_worker, new_node) != 0) {
            pthread_mutex_lock(&node_mutex);
            SLIST_REMOVE(&thread_head, new_node, thread_node, entries);
            pthread_mutex_unlock(&node_mutex);            
            close(client_fd);
            free(new_node);
        }
    }
    if (signal_captured){
        syslog(LOG_DEBUG, "Caught signal, exiting");
    }
    while (!SLIST_EMPTY(&thread_head)) {
        reap_worker();
    }
    #if !USE_AESD_CHAR_DEVICE
    pthread_join(tm_tid, NULL);
    remove(TMP_DATA);
    #endif
    close(server_fd);
    close(tmp_data_fd);
    pthread_mutex_destroy(&node_mutex);
    pthread_mutex_destroy(&file_mutex);
    closelog();
    remove(CHILD_PIDFILE);
    
    return 0;
}
